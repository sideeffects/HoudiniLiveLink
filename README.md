# Houdini KineFX Live Link

LiveLink Plugin for Houdini KineFX and Unreal Engine

The Houdini KineFX Live Link plugin can be used to let you control rigs in Unreal directly from KineFX in Houdini.

After installing the plugin, a new "Houdini Live Link" source will be available.
The LiveLink plugin should be used with the "ue4_livelink" HDA, that is available in the plugin's content directory.

First, start the server on the Houdini side, directly on the live link node.
You can then add a new Live Link source in Unreal (via Windows > Live Link, Add).
The Houdini Live Link source will now be available and can be used as an animation controller.

# Installation

After downloading the plugin's release binaries, extract the archive and copy the "HoudiniLiveLink" folder to the Engine/Plugins/Animations folder in Unreal.
You can now start Unreal and enable the Houdini Live Link plugin in the "Plugins" window, under Animation.
The main LiveLink plugin needs to be enabled as well.

# Compatibility

Binaries and Source Code are available for Unreal 4.25.3.
The UE4_LiveLink HDA requires Houdini18.5 as it uses KineFX.
