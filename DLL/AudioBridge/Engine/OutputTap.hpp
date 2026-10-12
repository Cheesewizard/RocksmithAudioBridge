#pragma once

#include <Windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>

// Passive output tap: wraps the render client the game fills so the wet mix can be copied
// read-only, forwarding every call untouched. Works for both a plain WASAPI render device and
// RS_ASIO's WASAPI shim (which is how the mix reaches an ASIO driver), so it never replaces the
// endpoint and never edits RS_ASIO.ini.
namespace Audio::OutputTap
{
	// Wrap a physical (possibly RS_ASIO) enumerator so its render devices hand back a tapping client.
	HRESULT CreateTapEnumerator(IMMDeviceEnumerator* physical, IMMDeviceEnumerator** enumerator);

	// Begin/stop a paired wet game-mix and dry guitar take under directory. When idle the tap wrapper
	// just forwards, adding no latency to what the player hears.
	HRESULT StartRecording(const std::wstring& directory);
	// Outputs the finished take's frame count and start FILETIME so a video take can align/validate it.
	HRESULT StopRecording(std::wstring& savedPath, uint64_t& frames, uint64_t& started);
	bool IsRecording();
	bool IsRecordingDry();
	HRESULT RecordingError();
	uint64_t RecordedFrames();
	uint64_t RecordingStarted();

	// True once the game has opened its render client through our wrapper (the tap is live).
	bool TapClientSeen();
	// True when the Rocksmith Audio Bridge proxy ASIO driver is loaded (wet recording is available).
	bool ProxyAvailable();
	// Current proxy transport: 0 = not initialized/unavailable, 1 = virtual ASIO clock, 2 = real ASIO.
	// The virtual clock keeps the host alive while physical hardware is absent.
	int ProxyOutputMode();
	// Current proxy capture transport: 0 = physical ASIO input, 1 = virtual input waiting for
	// a Real Tone Cable, 2 = virtual input receiving the Real Tone Cable.
	int ProxyInputMode();
	// Why the proxy could not open the user's real ASIO device this launch (one readable line), or empty
	// when it bound or never tried. Explains a silent guitar that would otherwise show only as "no input".
	std::wstring ProxyBindError();
	// Ask a virtual proxy to bind its configured physical ASIO driver. Failure leaves the virtual stream live.
	bool TryPromoteProxyOutput(const std::wstring& driverName);
	// Replace a stalled real-ASIO transport with the proxy's virtual clock. Used only for automatic downgrade.
	bool TryDemoteProxyOutput();
	// Explicit re-bind of the named real ASIO driver even when the proxy already reports it bound.
	bool TryRebindProxyOutput(const std::wstring& driverName);
	// Channel routing (ticket #109). Roles: 0 output (first channel of its stereo pair), 1 Player 1, 2 Player 2,
	// 3 Mic; channels are 0-based device channels. ProxyRoute is false when RS_ASIO.ini does not send the role
	// through the bridge (or the proxy is not loaded); hostChannel is the channel RS_ASIO.ini asks for.
	bool ProxyRoute(int role, long& hostChannel, long& deviceChannel);
	// The bound device's channel names; empty while the proxy is on speaker mode.
	std::vector<std::string> ProxyChannelNames(bool input);
	// Save and apply a role's device channel (-1 = the channel RS_ASIO.ini asks for). 1 applied live, 2 saved for
	// the next bind, 0 failed. Blocks while the device's buffers are re-created: call it off the render thread.
	int SetProxyRoute(int role, long deviceChannel);
	// Input interfaces per role (DLL/AsioProxy/SplitInput.hpp): 1 Player 1, 2 Player 2, 3 Mic. Empty = the output
	// interface.
	std::wstring ProxyInputTarget(int role);
	// Saves and applies a role's input interface (empty = the output's; role -1 = every input role): 1 applied live,
	// 2 saved for the next bind, 0 failed. Blocks while the interface's buffers are re-created: call it off the render
	// thread.
	int SetProxyInputTarget(int role, const std::wstring& driver);
	// Why an input interface would not open (its inputs then come from the output interface), or empty.
	std::string ProxyInputError();
	// What the bridge's last-known-good guard did at boot (a setup that crashed the game is not reopened), or empty.
	std::string ProxySafeBootNotice();
	// A role's input on another clock: 1 input interface, 2 Real Tone Cable, 0 none. ppm, fill, margin, underruns,
	// primed as VirtualCableCapture::Stats.
	struct InputClock { int source = 0; int ppm = 0; int fill = 0; int margin = 0; unsigned underruns = 0; int primed = 0; };
	InputClock ProxyInputClock(int role);
	// A role's channel names: the output pair's interface for role 0, else that input role's own interface.
	std::vector<std::string> ProxyRoleChannelNames(int role);
	// Set the proxy's static output trim (linear gain; second arg reserved); false if the proxy is not loaded.
	bool ConfigureLimiter(float gainLinear, float reserved);
	bool ConfigureOutputGuard(bool limiterOn, float ceilingLin, bool agcOn, float targetRms);
	// Read the proxy's per-channel output peak-hold + RMS (0..1); zeros when the proxy is not loaded.
	void ReadOutputLevels(float* peak, float* rms, int maxCh);
	// Arm one latency probe injection in the proxy; returns the probe length in samples (0 if not loaded).
	int ArmLatencyProbe();
	// Total frames observed at the tap since load, for diagnostics.
	uint64_t ObservedFrames();

	// --- Alternate-device routing (ROUTE feed) support ---------------------------------------------
	// The proxy fans each output block to up to a few sinks. Recording uses one; alternate-device
	// routing registers another that renders the same block to a WASAPI device, so the mix can play on,
	// say, laptop speakers without a game restart. These wrap the proxy's AddSink/RemoveSink exports.
	using ProxySinkFn = void(__cdecl*)(const void* interleaved, long frames, long channels, long asioSampleType, double sampleRate);
	// Register a routing sink; returns false if the proxy driver is not loaded (no ASIO bridge in the chain).
	bool AddProxySink(ProxySinkFn sink);
	void RemoveProxySink(ProxySinkFn sink);
	// Mute (redirect) or unmute the proxy's forward to the real ASIO device. Muting makes routing a true
	// switch (the game's bound device falls silent) rather than an additive second output. No-op if the
	// proxy is not loaded. Returns false when it could not be applied.
	bool SetProxyForwardMuted(bool muted);
}
