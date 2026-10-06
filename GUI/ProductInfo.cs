namespace RSMods
{
	internal static class ProductInfo
	{
		// Product name. Internal names nobody sees (pipes, shared memory, registry keys, resource names) keep
		// "RSModsPlus"; user-visible folders use the product name (%LOCALAPPDATA%\Rocksmith Audio Bridge,
		// Videos\Rocksmith Audio Bridge). The runtime files are RocksmithAudioBridge.dll and RocksmithAudioBridgeAsio.dll.
		public const string PRODUCT_NAME = "Rocksmith Audio Bridge";
		// Keep VERSION in step with DLL/ProductVersion.hpp VERSION.
		public const string VERSION = "4.0";
		public const string DISPLAY_NAME = PRODUCT_NAME + " " + VERSION;

		// Title of the desktop audio bridge window. This is a cross-process contract: the game DLL finds the
		// running bridge by this exact title (DLL/Keybindings.cpp AUDIO_BRIDGE_WINDOW_TITLE) to auto-launch
		// it and to deliver the recording hotkey, and the GUI uses it to focus an existing instance.
		// Change both copies together.
		public const string AUDIO_BRIDGE_WINDOW_TITLE = DISPLAY_NAME + " · Desktop mixer";
	}
}
