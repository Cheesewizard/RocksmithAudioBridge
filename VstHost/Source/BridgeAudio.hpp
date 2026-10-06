#pragma once

// Audio through the 64-bit "Rocksmith Audio Bridge ASIO" driver. In this process the driver is always the
// external amp link's guest, so the host runs on Rocksmith's clock with the game's clean guitar as input and
// its output mixed into the game, exactly like an external amp sim that picked the driver.

#include <Windows.h>
#include <atomic>
#include <string>
#include <vector>

struct IAsioDriver;

namespace VstHost
{
	class AudioSink
	{
	public:
		virtual ~AudioSink() = default;
		// Stream thread. `guitar` is Player 1's clean signal; `left`/`right` receive the plugin's output.
		virtual void Process(const float* guitar, float* left, float* right, int frames) = 0;
	};

	class BridgeAudio
	{
	public:
		~BridgeAudio() { Close(); }

		// `notifyWindow` receives kResetMessage when the driver asks for a restart (the game changed its buffer size).
		bool Open(HWND notifyWindow, AudioSink* sink, std::string& error);
		bool Start(std::string& error);
		void Stop();
		void Close();
		int Frames() const { return m_frames; }

		static constexpr UINT kResetMessage = WM_APP + 1;

		// Called from the driver's static callbacks (ASIO passes no user pointer).
		void Render(long index);
		void RequestReset();

	private:
		bool CreateDriver(std::string& error);

		IAsioDriver* m_driver = nullptr;
		HMODULE m_module = nullptr;
		HWND m_notifyWindow = nullptr;
		AudioSink* m_sink = nullptr;
		int m_frames = 0;
		bool m_buffersCreated = false;
		bool m_running = false;
		void* m_in[2][2]{};    // [channel][double-buffer half]
		void* m_out[2][2]{};
		std::vector<float> m_guitar, m_left, m_right;
	};
}
