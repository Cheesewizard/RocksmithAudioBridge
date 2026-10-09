#pragma once

#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <string>

namespace Audio::PersistentInput
{
	enum class EnumeratorSource
	{
		Game,
		RsAsio
	};

	// Queried once when the shared input tap binds; placeholder packets are not hardware readiness.
	struct __declspec(uuid("923E6216-6077-4EC6-8F46-60AE05237BE1")) ICaptureState : IUnknown
	{
		virtual BOOL STDMETHODCALLTYPE IsPhysicalPacket() = 0;
		virtual UINT64 STDMETHODCALLTYPE GetGeneration() = 0;
	};

	// gatedByPlayerTwoToggle: the stream is the Player 2 cable under RS_ASIO; it carries the cable only
	// while "Use the Real Tone Cable for Player 2" is on and silence otherwise (see PersistentDevice.cpp).
	HRESULT CreateCaptureClient(const std::wstring& endpointId, IAudioClient** client, bool gatedByPlayerTwoToggle = false);
	HRESULT CreateEnumerator(IMMDeviceEnumerator* physical, const std::wstring& endpointId, const std::wstring& outputId,
		bool replaceCapture, bool replaceOutput, IMMDeviceEnumerator** enumerator);
	void SetCableForPlayerTwoEnabled(bool enabled);
	bool IsCableForPlayerTwoEnabled();
	bool IsCableForPlayerTwoAvailable();
	// tapMode installs the passive output tap (wrap render devices to copy the wet mix) instead of the
	// replace-the-endpoint routing. It reuses the same call-site interception; the enumerator it hands
	// the game wraps the real render device rather than substituting one. Used in passthrough.
	bool Install(const std::wstring& endpointId, const std::wstring& outputId,
		bool replaceCapture, bool replaceOutput, EnumeratorSource source, bool tapMode = false);
}
