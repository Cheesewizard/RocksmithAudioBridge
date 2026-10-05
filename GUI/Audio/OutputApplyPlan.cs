namespace RSMods.Audio
{
	/// <summary>What pressing "Apply output" should do for the currently selected device. Only the live,
	/// no-restart moves that the Rocksmith Audio Bridge proxy makes possible are decided here; everything
	/// else falls through to the existing arm/install flow. Kept pure (no UI, no I/O) so it can be tested
	/// without a running game.</summary>
	internal enum OutputApplyAction
	{
		PromoteToAsio,
		/// <summary>Send the "route to this device" command (op 21): play the game mix on the chosen Windows
		/// device live, no restart. Used when the game is up, the proxy owns the ASIO chain, and the chosen
		/// device is not the ASIO device the game is bound to.</summary>
		RouteToDevice,
		/// <summary>Send the "stop route" command (op 22): hand the mix back to the game's own ASIO device
		/// live. Used when a route is active and the ASIO device is picked again.</summary>
		StopRoute,
		/// <summary>Not a live move: use the existing flow (install/link the proxy and bind at launch, switch a
		/// WASAPI session live, or arm the bridge for the next launch).</summary>
		UseExistingFlow,
		/// <summary>Send the promote command (op 25) for the ASIO device the game is already bound to: the
		/// host tears the real driver down and binds it again. An explicit recovery for a device that is
		/// dead but still running (no automatic path exists on purpose).</summary>
		RebindAsio,
	}

	internal static class OutputApplyPlan
	{
		/// <summary>Decide the live output move for the proxy world. A live move is only possible when the game
		/// is running, the control pipe is connected, the proxy is the live output chain in the game, and the
		/// bridge is in passthrough (the proxy owns the chain, no legacy WASAPI session). Outside that, returns <see cref="OutputApplyAction.UseExistingFlow"/>.
		/// Cable mode disables RS_ASIO completely and therefore never enters this proxy path.</summary>
		/// <param name="proxyChainLive">The proxy is linked as the game's output chain, so live route commands can reach it.</param>
		/// <param name="chosenIsAsioDevice">The selected device has a matching ASIO driver, i.e. it is (or can be)
		/// the device the game is bound to directly.</param>
		/// <param name="chosenIsRoutedDevice">The selected device is already the one the live route is rendering to.</param>
		public static OutputApplyAction Plan(bool asioMode, bool gameRunning, bool connected, bool proxyChainLive,
			bool isPassthrough, bool isRouting, bool chosenIsAsioDevice, bool chosenIsRoutedDevice,
			bool proxyNeedsPromotion)
		{
			if (!asioMode || !(gameRunning && connected && proxyChainLive && isPassthrough))
				return OutputApplyAction.UseExistingFlow;
			if (chosenIsAsioDevice && proxyNeedsPromotion)
				return OutputApplyAction.PromoteToAsio;

			// The ASIO device the game is bound to: if routing elsewhere, stop and go back to it live.
			// If not routing, an explicit Apply on it re-binds the driver (release + bind), which is the
			// only way to recover a real device that went dead while still calling back.
			if (chosenIsAsioDevice)
				return isRouting ? OutputApplyAction.StopRoute : OutputApplyAction.RebindAsio;

			// A plain Windows device: route the mix there live, unless it is already the routed device.
			return chosenIsRoutedDevice ? OutputApplyAction.UseExistingFlow : OutputApplyAction.RouteToDevice;
		}
	}
}
