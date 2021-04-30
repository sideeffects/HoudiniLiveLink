/*
* Copyright (c) <2020> Side Effects Software Inc.
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*
* 1. Redistributions of source code must retain the above copyright notice,
*    this list of conditions and the following disclaimer.
*
* 2. The name of Side Effects Software may not be used to endorse or
*    promote products derived from this software without specific prior
*    written permission.
*
* THIS SOFTWARE IS PROVIDED BY SIDE EFFECTS SOFTWARE "AS IS" AND ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
* NO EVENT SHALL SIDE EFFECTS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "HoudiniLiveLinkSource.h"

#include "ILiveLinkClient.h"
#include "LiveLinkTypes.h"
#include "Roles/LiveLinkAnimationRole.h"
#include "Roles/LiveLinkAnimationTypes.h"
#include "Common/UdpSocketBuilder.h"
#include "Sockets.h"

#include "Async/Async.h"
#include "HAL/RunnableThread.h"

#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"

#define LOCTEXT_NAMESPACE "HoudiniLiveLinkSource"

#define RECV_BUFFER_SIZE 1024 * 1024

const double
FHoudiniLiveLinkSource::TransformScale = 1.0;

FHoudiniLiveLinkSource::FHoudiniLiveLinkSource(FIPv4Endpoint InEndpoint, const float& InRefreshRate, const FString& InSubjectName)
	: Stopping(false)
	, Thread(nullptr)
	, SkeletonSetupNeeded(true)
{
	// defaults
	DeviceEndpoint = InEndpoint;

	UpdateFrequency = 0.1f;
	if (InRefreshRate > 0.0f)
	{
		UpdateFrequency = 1 / InRefreshRate;
	}

	SourceStatus = LOCTEXT("SourceStatus_DeviceNotFound", "Device Not Found");
	SourceType = LOCTEXT("HoudiniLiveLinkSourceType", "Houdini LiveLink");
	SourceMachineName = LOCTEXT("HoudiniLiveLinkSourceMachineName", "localhost");

	// Default subject name
	SubjectName = TEXT("Houdini Subject");
	if (!InSubjectName.IsEmpty())
		SubjectName = FName(*InSubjectName);

	{
		Start();
		SourceStatus = LOCTEXT("SourceStatus_Receiving", "Receiving");
	}
}

FHoudiniLiveLinkSource::~FHoudiniLiveLinkSource()
{
	Stop();
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

void 
FHoudiniLiveLinkSource::ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid)
{
	Client = InClient;
	SourceGuid = InSourceGuid;
}

bool 
FHoudiniLiveLinkSource::IsSourceStillValid() const
{
	// Source is valid if we have a valid thread and socket
	bool bIsSourceValid = !Stopping && Thread != nullptr;
	return bIsSourceValid;
}

bool 
FHoudiniLiveLinkSource::RequestSourceShutdown()
{
	Stop();
	SourceStatus = LOCTEXT("SourceStatus_Stopped", "Stopped");

	return true;
}

// FRunnable interface
void 
FHoudiniLiveLinkSource::Start()
{
	SkeletonSetupNeeded = true;
	NumBones = -1;
	NumCurves = -1;

	ThreadName = "Houdini Live Link ";
	ThreadName.AppendInt(FAsyncThreadIndex::GetNext());
	
	Thread = FRunnableThread::Create(this, *ThreadName, 128 * 1024, TPri_AboveNormal, FPlatformAffinity::GetPoolThreadMask());
}

void
FHoudiniLiveLinkSource::Stop()
{
	Stopping = true;
	SkeletonSetupNeeded = true;
}

const int BUFFER_SIZE = 65536;
uint32
FHoudiniLiveLinkSource::Run()
{
	FUdpSocketBuilder builder("Houdini Live Link Receiver");
	builder.AsBlocking();
	builder.AsReusable();
	builder.BoundToAddress(FIPv4Address::Any);
	builder.BoundToPort(DeviceEndpoint.Port);
	builder.WithReceiveBufferSize(BUFFER_SIZE);

	char buf[BUFFER_SIZE];
	
	FSocket* socket = builder.Build();
	if (socket)
	{
		if (socket->GetConnectionState() != ESocketConnectionState::SCS_Connected)
			return 0;
		
		while (!Stopping)
		{
			int32 num_read;
			if (socket->Wait(ESocketWaitConditions::WaitForRead, 100))
			{
				socket->Recv((uint8*)buf, BUFFER_SIZE, num_read, ESocketReceiveFlags::None);
				
				SkeletonSetupNeeded = !ProcessResponseData(FString(num_read, buf));
			}
		}
		socket->Close();
	}
	return 0;
}

bool 
FHoudiniLiveLinkSource::ProcessResponseData(const FString& ReceivedData)
{
	// No need to process the data if we're stopping
	if(Stopping || !Thread)
		return false;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReceivedData);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		// Whatever we received is not JSON
		return false;
	}

	// Setup is done via GetSkeleton, and returns the following values:
	// parents (Int Array), vertices (FVector Array), Names (String Array)

	// Update is done via GetSkeletonPose, and has:
	// position (FVector Array), rotations (FVector Array), Names (String Array)

	// Static Data 
	bool bStaticDataUpdated = false;
	FLiveLinkStaticDataStruct StaticDataStruct = FLiveLinkStaticDataStruct(FLiveLinkSkeletonStaticData::StaticStruct());
	FLiveLinkSkeletonStaticData& StaticData = *StaticDataStruct.Cast<FLiveLinkSkeletonStaticData>();

	// Frame Data
	bool bFrameDataUpdated = false;
	FLiveLinkFrameDataStruct FrameDataStruct = FLiveLinkFrameDataStruct(FLiveLinkAnimationFrameData::StaticStruct());
	FLiveLinkAnimationFrameData& FrameData = *FrameDataStruct.Cast<FLiveLinkAnimationFrameData>();

	for (TPair<FString, TSharedPtr<FJsonValue>>& JsonField : JsonObject->Values)
	{
		const TArray<TSharedPtr<FJsonValue>>& ValueArray = JsonField.Value->AsArray();
		if (JsonField.Key.Equals(TEXT("parents"), ESearchCase::IgnoreCase))
		{
			// Parents (STATIC DATA) (GetSkeleton)
			StaticData.BoneParents.SetNumUninitialized(ValueArray.Num());
			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); BoneIdx++)
			{
				if (ValueArray[BoneIdx]->IsNull())
				{
					// Root Node
					StaticData.BoneParents[BoneIdx] = -1;
				}					
				else
				{
					StaticData.BoneParents[BoneIdx] = (int32)ValueArray[BoneIdx]->AsNumber();
				}
			}

			bStaticDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("names"), ESearchCase::IgnoreCase))
		{
			// Names (STATIC DATA) (both)
			StaticData.BoneNames.SetNumUninitialized(ValueArray.Num());

			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); BoneIdx++)
			{
				FString BoneName = ValueArray[BoneIdx]->AsString();
				StaticData.BoneNames[BoneIdx] = FName(*BoneName);
			}

			bStaticDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("vertices"), ESearchCase::IgnoreCase))
		{
			// vertices (FRAME DATA) (GetSkeleton)
			FrameData.Transforms.Init(FTransform::Identity, ValueArray.Num());
			
			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); ++BoneIdx)
			{
				const TArray<TSharedPtr<FJsonValue>>& LocationArray = ValueArray[BoneIdx]->AsArray();
				
				FVector BoneLocation = FVector::ZeroVector;
				if (LocationArray.Num() == 3)
				{
					double X = LocationArray[0]->AsNumber();
					double Y = LocationArray[1]->AsNumber();
					double Z = LocationArray[2]->AsNumber();
					
					// Houdini to Unreal: Swap Y/Z, meters to cm
					BoneLocation = FVector(X, -Y, Z) * TransformScale;
				}

				// Setup the transform using the location
				FrameData.Transforms[BoneIdx] = FTransform(FQuat::Identity, BoneLocation, FVector::OneVector);
			}

			bFrameDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("positions"), ESearchCase::IgnoreCase))
		{
			// Check the validity of the data we received
			if (!SkeletonSetupNeeded && ValueArray.Num() != NumBones)
				return false;

			// positions (FRAME DATA) (GetSkeletonPose)
			if(FrameData.Transforms.Num() <= 0)
				FrameData.Transforms.Init(FTransform::Identity, ValueArray.Num()); 

			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); ++BoneIdx)
			{
				const TArray<TSharedPtr<FJsonValue>>& LocationArray = ValueArray[BoneIdx]->AsArray();

				FVector BoneLocation = FVector::ZeroVector;
				if ( LocationArray.Num() == 3) // X, Y, Z
				{
					double X = LocationArray[0]->AsNumber();
					double Y = LocationArray[1]->AsNumber();
					double Z = LocationArray[2]->AsNumber();

					// Houdini to Unreal: Swap Y/Z, meters to cm
					BoneLocation = FVector(X, -Y, Z) * TransformScale;
				}
				FrameData.Transforms[BoneIdx].SetLocation(BoneLocation);
			}

			bFrameDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("rotations"), ESearchCase::IgnoreCase))
		{
			// Check the validity of the data we received
			if (!SkeletonSetupNeeded && ValueArray.Num() != NumBones)
			 	return false;

			// rotations (FRAME DATA) (GetSkeletonPose)
			if (FrameData.Transforms.Num() <= 0)
				FrameData.Transforms.Init(FTransform::Identity, ValueArray.Num());

			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); ++BoneIdx)
			{
				const TArray<TSharedPtr<FJsonValue>>& RotationArray = ValueArray[BoneIdx]->AsArray();

				FQuat HQuat = FQuat::Identity;
				if (RotationArray.Num() == 3)
				{
					double X = RotationArray[0]->AsNumber();
					double Y = RotationArray[1]->AsNumber();
					double Z = RotationArray[2]->AsNumber();

					// Houdini to Unreal conversion
					if (BoneIdx == 0)
						X += 90.0f;

					HQuat = FQuat::MakeFromEuler(FVector(X, -Y, -Z));
				}
				else if (RotationArray.Num() == 4)
				{
					// TODO: untested, the livelink HDA doesnot send quaternions for now
					double X = RotationArray[0]->AsNumber();
					double Y = RotationArray[1]->AsNumber();
					double Z = RotationArray[2]->AsNumber();
					double W = RotationArray[3]->AsNumber();
					HQuat = FQuat(X, Z, Y, -W);
				}

				FrameData.Transforms[BoneIdx].SetRotation(HQuat);
			}

			bFrameDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("scales"), ESearchCase::IgnoreCase))
		{
			// Check the validity of the data we received
			if (!SkeletonSetupNeeded && ValueArray.Num() != NumBones)
				return false;

			// scale (FRAME DATA) (GetSkeletonPose)
			if (FrameData.Transforms.Num() <= 0)
				FrameData.Transforms.Init(FTransform::Identity, ValueArray.Num());
		
			for (int BoneIdx = 0; BoneIdx < ValueArray.Num(); ++BoneIdx)
			{
				const TArray<TSharedPtr<FJsonValue>>& ScaleArray = ValueArray[BoneIdx]->AsArray();

				FVector BoneScale = FVector::OneVector;
				if (ScaleArray.Num() == 3) // X, Y, Z
				{
					double X = ScaleArray[0]->AsNumber();
					double Y = ScaleArray[1]->AsNumber();
					double Z = ScaleArray[2]->AsNumber();

					// Houdini to Unreal: Swap Y/Z
					BoneScale = FVector(X, Z, Y);
				}

				FrameData.Transforms[BoneIdx].SetScale3D(BoneScale);
			}

			bFrameDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("blendshape_names"), ESearchCase::IgnoreCase))
		{
			StaticData.PropertyNames.Empty(ValueArray.Num());

			for (int i = 0; i < ValueArray.Num(); ++i)
			{
				StaticData.PropertyNames.Add(FName(ValueArray[i]->AsString()));
			}

			bStaticDataUpdated = true;
		}
		else if (JsonField.Key.Equals(TEXT("blendshape_values"), ESearchCase::IgnoreCase))
		{
			// Check the validity of the data we received
			if (!SkeletonSetupNeeded && ValueArray.Num() != NumCurves)
				return false;

			FrameData.PropertyValues.Empty(ValueArray.Num());

			for (int i = 0; i < ValueArray.Num(); ++i)
			{
				FrameData.PropertyValues.Add(ValueArray[i]->AsNumber());
			}

			bFrameDataUpdated = true;
		}
	}

	// Make sure the source is still valid before attempting to update the client data
	if (!IsSourceStillValid())
		return false;

	if (bStaticDataUpdated && SkeletonSetupNeeded)
	{
		// Only update the static data if the skeleton setup is required!
		NumBones = StaticData.BoneNames.Num();
		NumCurves = StaticData.PropertyNames.Num();
		Client->PushSubjectStaticData_AnyThread({ SourceGuid, SubjectName }, ULiveLinkAnimationRole::StaticClass(), MoveTemp(StaticDataStruct));
	}

	if (bFrameDataUpdated  && !SkeletonSetupNeeded)
	{
		// Only update the frame data if where not setting up the skeleton
		Client->PushSubjectFrameData_AnyThread({ SourceGuid, SubjectName }, MoveTemp(FrameDataStruct));
	}

	return true;
}

#undef LOCTEXT_NAMESPACE
