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

#pragma once

#include "ILiveLinkSource.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "IMessageContext.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"

class FRunnableThread;
class ILiveLinkClient;

class HOUDINILIVELINK_API FHoudiniLiveLinkSource : public ILiveLinkSource, public FRunnable
{
	public:

		FHoudiniLiveLinkSource(FIPv4Endpoint Endpoint, const float& InRefreshRate, const FString& InSubjectName);

		virtual ~FHoudiniLiveLinkSource();

		// Begin ILiveLinkSource Interface
	
		virtual void ReceiveClient(ILiveLinkClient* InClient, FGuid InSourceGuid) override;

		virtual bool IsSourceStillValid() const override;

		virtual bool RequestSourceShutdown() override;

		virtual FText GetSourceType() const override { return SourceType; };
		virtual FText GetSourceMachineName() const override { return SourceMachineName; }
		virtual FText GetSourceStatus() const override { return SourceStatus; }

		// End ILiveLinkSource Interface

		// Begin FRunnable Interface

		virtual bool Init() override { return true; }
		virtual uint32 Run() override;
		void Start();
		virtual void Stop() override;
		virtual void Exit() override { }

		// End FRunnable Interface

		bool ProcessResponseData(const FString& ReceivedData);

	private:

		ILiveLinkClient* Client;

		// Our identifier in LiveLink
		FGuid SourceGuid;

		// Source Infos
		FText SourceType;
		FText SourceMachineName;
		FText SourceStatus;

		FName SubjectName;
		
		int NumBones;
		int NumCurves;

		// Machine/Port we're connected to
		FIPv4Endpoint DeviceEndpoint;

		// Threadsafe Bool for terminating the main thread loop
		FThreadSafeBool Stopping;

		// Thread to run socket operations on
		FRunnableThread* Thread;

		// Name of the sockets thread
		FString ThreadName;

		// Indicates that the skeleton needs to be setup from houdini first
		bool SkeletonSetupNeeded;

		// Frequency update (sleep time between each update)
		float UpdateFrequency;

		// Transform scale
		// currently unused
		static const double TransformScale;
};
