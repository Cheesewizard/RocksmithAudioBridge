#pragma once

#include <windows.h>
#include <algorithm>
#include <atomic>

// The block size the game's own audio engine runs at in Cable mode.
//
// The game sizes its engine (Wwise, the in-game amps and pedals, note detection) from the period its devices report.
// Our virtual cable input and game output used to report 10 ms, so the game ran 480-frame blocks and every in-game
// tone carried that in input and output buffering. Under RS_ASIO the game runs at the player's own ASIO buffer size
// (RS_ASIO-log: "host requested buffer duration: 3ms (144 frames)", "actual buffer duration: 2ms (128 frames)" on an
// interface set to 128), which is why in-game tones feel fast there.
//
// Cable mode does the same: Automatic gives the game the buffer of the output the player uses, so a player who runs
// their interface at 256 because their PC needs it gets 256, not someone else's 128.
//   - ASIO interface (Cable mode's ASIO output): its own buffer size, the player's setting in its control panel.
//   - Windows device: the period Windows granted (overlay Device buffer), at most 144; a device stuck at 480 (no low-latency
//     engine) says nothing about what the PC can do, and smaller game blocks still take delay out of the game itself.
// The game asks for its period before the output opens, so Automatic uses what the output opened with last session
// (Remember, from OutputSession); the first start uses 128.
//
// HKCU\Software\RSMods\AsioProxy: CableGameBlockFrames (0 = Automatic, or 64 / 128 / 256 / 512 / 480 = the old
// behaviour), CableOutputBufferFrames + CableOutputIsAsio (remembered). Read once when the game opens its devices at
// boot, so a change applies on the next start.
namespace Audio::CableGamePeriod
{
	namespace Detail
	{
		constexpr wchar_t kKey[] = L"Software\\RSMods\\AsioProxy";

		inline DWORD Read(const wchar_t* name, DWORD fallback)
		{
			DWORD value = fallback, size = sizeof(value), type = 0;
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return fallback;
			if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS || type != REG_DWORD) value = fallback;
			RegCloseKey(key);
			return value;
		}

		inline void Write(const wchar_t* name, DWORD value)
		{
			HKEY key = nullptr;
			if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
			RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
			RegCloseKey(key);
		}

		inline UINT32 Clamp(DWORD frames) { return static_cast<UINT32>((std::clamp)(frames, DWORD{ 64 }, DWORD{ 1024 })); }
	}

	// 0 = Automatic, else a fixed block size.
	inline DWORD Setting() { return Detail::Read(L"CableGameBlockFrames", 0); }
	inline void SetSetting(DWORD frames) { Detail::Write(L"CableGameBlockFrames", frames); }

	// What Automatic picks from the output remembered last session (or 128 before any).
	inline UINT32 Automatic()
	{
		const DWORD buffer = Detail::Read(L"CableOutputBufferFrames", 0);
		if (!buffer) return 128;
		// Windows outputs: at most 144 (3 ms, the bridge's own Windows default). 256 on a 336-frame headphone device only
		// added delay to every game tone.
		return Detail::Clamp(Detail::Read(L"CableOutputIsAsio", 0) ? buffer : (std::min)(buffer, DWORD{ 144 }));
	}

	// What the next game start will use.
	inline UINT32 NextStart()
	{
		const DWORD setting = Setting();
		return setting ? Detail::Clamp(setting) : Automatic();
	}

	// What this session uses: read at the first call (the game opening its devices at boot), and again by
	// ApplyNextStart when the game's output is reopened live (DeviceEnumeratorHook ResetGameOutput).
	inline std::atomic<UINT32>& Current()
	{
		static std::atomic<UINT32> frames{ NextStart() };
		return frames;
	}
	inline UINT32 Frames() { return Current().load(); }
	inline UINT32 ApplyNextStart() { const UINT32 next = NextStart(); Current().store(next); return next; }

	// The same as a WASAPI period (100 ns units), rounded up so it converts back to at least Frames().
	inline LONGLONG Period() { return (static_cast<LONGLONG>(Frames()) * 10000000 + 47999) / 48000; }

	// OutputSession, once the output runs: the buffer it opened with, for the next start's Automatic.
	inline void Remember(UINT32 bufferFrames, bool asio)
	{
		static DWORD lastBuffer = 0, lastAsio = 2;
		if (!bufferFrames || (bufferFrames == lastBuffer && static_cast<DWORD>(asio) == lastAsio)) return;
		lastBuffer = bufferFrames;
		lastAsio = asio;
		Detail::Write(L"CableOutputBufferFrames", bufferFrames);
		Detail::Write(L"CableOutputIsAsio", asio ? 1 : 0);
	}
}
