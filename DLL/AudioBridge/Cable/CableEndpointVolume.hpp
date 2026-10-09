#pragma once

#include <endpointvolume.h>
#include <wrl.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace Audio::PersistentInput
{
	class CableVolume final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IAudioEndpointVolume, Microsoft::WRL::FtmBase>
	{
	public:
		float GetGain() const { return muted ? 0.0f : level.load(); }
		HRESULT STDMETHODCALLTYPE RegisterControlChangeNotify(IAudioEndpointVolumeCallback* callback) override
		{
			if (!callback) return E_POINTER;
			std::lock_guard<std::mutex> guard(callbackMutex);
			for (const auto& item : callbacks) if (item.Get() == callback) return S_OK;
			try { callbacks.emplace_back(callback); } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE UnregisterControlChangeNotify(IAudioEndpointVolumeCallback* callback) override
		{
			if (!callback) return E_POINTER;
			std::lock_guard<std::mutex> guard(callbackMutex);
			for (auto item = callbacks.begin(); item != callbacks.end(); ++item)
			{
				if (item->Get() == callback) { callbacks.erase(item); return S_OK; }
			}
			return E_INVALIDARG;
		}
		HRESULT STDMETHODCALLTYPE GetChannelCount(UINT* count) override { if (!count) return E_POINTER; *count = 2; return S_OK; }
		HRESULT STDMETHODCALLTYPE SetMasterVolumeLevel(float value, LPCGUID context) override
		{
			if (!std::isfinite(value) || value < -96.0f || value > 0) return E_INVALIDARG;
			return SetMasterVolumeLevelScalar(value == -96.0f ? 0.0f : std::pow(10.0f, value / 20.0f), context);
		}
		HRESULT STDMETHODCALLTYPE SetMasterVolumeLevelScalar(float value, LPCGUID context) override
		{
			if (!std::isfinite(value) || value < 0 || value > 1) return E_INVALIDARG;
			level = value; return Notify(context);
		}
		HRESULT STDMETHODCALLTYPE GetMasterVolumeLevel(float* value) override
		{
			if (!value) return E_POINTER;
			const float scalar = level; *value = scalar > 0 ? std::max(-96.0f, 20.0f * std::log10(scalar)) : -96.0f; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetMasterVolumeLevelScalar(float* value) override { if (!value) return E_POINTER; *value = level; return S_OK; }
		HRESULT STDMETHODCALLTYPE SetChannelVolumeLevel(UINT channel, float value, LPCGUID context) override { return channel < 2 ? SetMasterVolumeLevel(value, context) : E_INVALIDARG; }
		HRESULT STDMETHODCALLTYPE SetChannelVolumeLevelScalar(UINT channel, float value, LPCGUID context) override { return channel < 2 ? SetMasterVolumeLevelScalar(value, context) : E_INVALIDARG; }
		HRESULT STDMETHODCALLTYPE GetChannelVolumeLevel(UINT channel, float* value) override { return channel < 2 ? GetMasterVolumeLevel(value) : E_INVALIDARG; }
		HRESULT STDMETHODCALLTYPE GetChannelVolumeLevelScalar(UINT channel, float* value) override { return channel < 2 ? GetMasterVolumeLevelScalar(value) : E_INVALIDARG; }
		HRESULT STDMETHODCALLTYPE SetMute(BOOL value, LPCGUID context) override { muted = value != FALSE; return Notify(context); }
		HRESULT STDMETHODCALLTYPE GetMute(BOOL* value) override { if (!value) return E_POINTER; *value = muted; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetVolumeStepInfo(UINT* step, UINT* count) override
		{
			if (!step || !count) return E_POINTER;
			*step = static_cast<UINT>(level.load() * 100); *count = 101; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE VolumeStepUp(LPCGUID context) override { return SetMasterVolumeLevelScalar(std::min(1.0f, level.load() + 0.01f), context); }
		HRESULT STDMETHODCALLTYPE VolumeStepDown(LPCGUID context) override { return SetMasterVolumeLevelScalar(std::max(0.0f, level.load() - 0.01f), context); }
		HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD* mask) override { if (!mask) return E_POINTER; *mask = 0; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetVolumeRange(float* minimum, float* maximum, float* increment) override
		{
			if (!minimum || !maximum || !increment) return E_POINTER;
			*minimum = -96; *maximum = 0; *increment = 1; return S_OK;
		}

	private:
		HRESULT Notify(LPCGUID context)
		{
			std::vector<Microsoft::WRL::ComPtr<IAudioEndpointVolumeCallback>> snapshot;
			try { std::lock_guard<std::mutex> guard(callbackMutex); snapshot = callbacks; }
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
			alignas(AUDIO_VOLUME_NOTIFICATION_DATA) BYTE storage[sizeof(AUDIO_VOLUME_NOTIFICATION_DATA) + sizeof(float)]{};
			auto* notification = reinterpret_cast<AUDIO_VOLUME_NOTIFICATION_DATA*>(storage);
			notification->guidEventContext = context ? *context : GUID_NULL;
			notification->bMuted = muted;
			notification->fMasterVolume = level;
			notification->nChannels = 2;
			notification->afChannelVolumes[0] = notification->afChannelVolumes[1] = level;
			for (const auto& callback : snapshot) callback->OnNotify(notification);
			return S_OK;
		}

		std::atomic<float> level{ 1.0f };
		std::atomic<bool> muted{ false };
		std::mutex callbackMutex;
		std::vector<Microsoft::WRL::ComPtr<IAudioEndpointVolumeCallback>> callbacks;
	};

	inline Microsoft::WRL::ComPtr<CableVolume> GetCableVolume()
	{
		static auto volume = Microsoft::WRL::Make<CableVolume>();
		return volume;
	}
}
