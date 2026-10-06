#include "stdafx.h"
#include "PersistentCapture.hpp"
#include "SharedOutput.hpp"
#include "OutputTap.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#pragma comment(lib, "propsys.lib")


namespace Audio::PersistentInput
{
	using namespace Microsoft::WRL;
	class CableEnumerator;
	std::mutex enumeratorRegistryMutex;
	std::vector<CableEnumerator*> enumerators;
	std::atomic_bool cableForPlayerTwoEnabled{ false };
	constexpr PROPERTYKEY FORM_FACTOR_KEY{ { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };
	constexpr PROPERTYKEY AUDIO_FORMAT_KEY{ { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };

	constexpr wchar_t DEVICE_ID[] = L"{RSModsPlus.PersistentCable.1}";
	const PROPERTYKEY DEVICE_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 2 };
	// AMD's USB audio offload exposes the controller in property 2 and the attached USB function in 39.
	const PROPERTYKEY USB_FUNCTION_INSTANCE = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 39 };
	const PROPERTYKEY DRIVER_INSTANCE = { { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 3 };

	bool HasCableIdentity(IPropertyStore* properties)
	{
		if (!properties) return false;
		for (const PROPERTYKEY& key : { DEVICE_INSTANCE, USB_FUNCTION_INSTANCE })
		{
			PROPVARIANT value{};
			const HRESULT result = properties->GetValue(key, &value);
			bool matches = false;
			if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal)
			{
				constexpr wchar_t identity[] = L"VID_12BA&PID_00FF";
				constexpr size_t length = _countof(identity) - 1;
				const wchar_t* candidate = value.pwszVal;
				for (; *candidate; ++candidate)
				{
					if (_wcsnicmp(candidate, identity, length) != 0) continue;
					const wchar_t following = candidate[length];
					matches = following == L'\0' || following == L'&' || following == L'\\' || following == L'#';
					if (matches) break;
				}
			}
			PropVariantClear(&value);
			if (matches) return true;
		}
		return false;
	}

	// Boot-scan trace for the Player 2 cable device under RS_ASIO. Logs every call the game makes on the device and its property
	// store so the debug log names the last thing asked before the game gave up. Cheap: a handful of calls
	// per scan, and only for the Player 2 wrapper's device.
	void TraceCableCall(bool enabled, const char* what)
	{
		if (enabled) LOG_INFO("(P2 CABLE SCAN) " << what << std::endl);
	}

	bool IsCable(IMMDevice* device)
	{
		ComPtr<IPropertyStore> properties;
		return device && SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && HasCableIdentity(properties.Get());
	}

	class CableProperties final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IPropertyStore, FtmBase>
	{
	public:
		bool trace = false;
		HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override { TraceCableCall(trace, "properties GetCount"); if (!count) return E_POINTER; *count = 5; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override
		{
			if (!key) return E_POINTER;
			const PROPERTYKEY keys[] = { AUDIO_FORMAT_KEY, PKEY_Device_FriendlyName, FORM_FACTOR_KEY, DEVICE_INSTANCE, DRIVER_INSTANCE };
			if (index >= 5) return E_INVALIDARG;
			*key = keys[index]; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override
		{
			if (trace)
			{
				char text[96];
				sprintf_s(text, "properties GetValue {%08lX-%04hX-%04hX} pid %lu", key.fmtid.Data1, key.fmtid.Data2, key.fmtid.Data3, key.pid);
				TraceCableCall(true, text);
			}
			if (!value) return E_POINTER;
			PropVariantInit(value);
			if (key == PKEY_Device_FriendlyName) return InitPropVariantFromString(L"Rocksmith USB Guitar Adapter (Rocksmith Audio Bridge)", value);
			if (key == DEVICE_INSTANCE) return InitPropVariantFromString(L"{1}.RSMODSPLUS\\VID_12BA&PID_00FF\\PersistentCable1", value);
			if (key == DRIVER_INSTANCE) return InitPropVariantFromString(L"RSMODSPLUS\\VID_12BA&PID_00FF", value);
			if (key == FORM_FACTOR_KEY) { value->vt = VT_UI4; value->ulVal = LineLevel; return S_OK; }
			if (key == AUDIO_FORMAT_KEY)
			{
				WAVEFORMATEX format{ WAVE_FORMAT_PCM, 2, SAMPLE_RATE, SAMPLE_RATE * 4, 4, 16, 0 };
				value->blob.pBlobData = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(format)));
				if (!value->blob.pBlobData) return E_OUTOFMEMORY;
				std::memcpy(value->blob.pBlobData, &format, sizeof(format));
				value->blob.cbSize = sizeof(format);
				value->vt = VT_BLOB;
				return S_OK;
			}
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
		HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }
	};

	class CableDevice final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDevice, IMMEndpoint, FtmBase>
	{
	public:
		// gatedByPlayerTwoToggle: this is the Player 2 cable beside an RS_ASIO input. Its stream follows the
		// "Use the Real Tone Cable for Player 2" toggle (cable audio or silence) instead of the device
		// appearing and disappearing, because the game builds its device list once at boot.
		CableDevice(std::wstring endpointId, bool gatedByPlayerTwoToggle)
			: endpointId(std::move(endpointId)), gatedByPlayerTwoToggle(gatedByPlayerTwoToggle) {}
		~CableDevice() { TraceCableCall(gatedByPlayerTwoToggle, "device destroyed"); }
		HRESULT STDMETHODCALLTYPE Activate(REFIID id, DWORD, PROPVARIANT*, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (id == __uuidof(IAudioClient))
			{
				TraceCableCall(gatedByPlayerTwoToggle, "Activate IAudioClient");
				const HRESULT result = CreateCaptureClient(endpointId, reinterpret_cast<IAudioClient**>(object), gatedByPlayerTwoToggle);
				if (gatedByPlayerTwoToggle) LOG_INFO("(P2 CABLE SCAN) Activate IAudioClient -> 0x" << std::hex << result << std::dec << std::endl);
				return result;
			}
			if (id == __uuidof(IAudioEndpointVolume))
			{
				TraceCableCall(gatedByPlayerTwoToggle, "Activate IAudioEndpointVolume");
				auto volume = GetCableVolume();
				return volume ? volume.CopyTo(id, object) : E_OUTOFMEMORY;
			}
			if (gatedByPlayerTwoToggle)
			{
				char text[64]{};
				sprintf_s(text, "{%08lX-%04hX-%04hX}", id.Data1, id.Data2, id.Data3);
				LOG_INFO("(P2 CABLE SCAN) Activate unsupported interface " << text << " -> E_NOINTERFACE" << std::endl);
			}
			return E_NOINTERFACE;
		}
		HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore** properties) override
		{
			if (gatedByPlayerTwoToggle) LOG_INFO("(P2 CABLE SCAN) OpenPropertyStore access " << access << std::endl);
			if (!properties) return E_POINTER;
			*properties = nullptr;
			if (access != STGM_READ) return STG_E_ACCESSDENIED;
			auto result = Make<CableProperties>();
			if (!result) return E_OUTOFMEMORY;
			result->trace = gatedByPlayerTwoToggle;
			return result.CopyTo(properties);
		}
		HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override
		{
			TraceCableCall(gatedByPlayerTwoToggle, "GetId");
			if (!id) return E_POINTER;
			*id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(DEVICE_ID)));
			if (!*id) return E_OUTOFMEMORY;
			std::memcpy(*id, DEVICE_ID, sizeof(DEVICE_ID)); return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override { TraceCableCall(gatedByPlayerTwoToggle, "GetState"); if (!state) return E_POINTER; *state = DEVICE_STATE_ACTIVE; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override { TraceCableCall(gatedByPlayerTwoToggle, "GetDataFlow"); if (!flow) return E_POINTER; *flow = eCapture; return S_OK; }
	private:
		std::wstring endpointId;
		bool gatedByPlayerTwoToggle;
	};

	constexpr wchar_t OUTPUT_DEVICE_ID[] = L"{RSModsPlus.Persistent.Output}";

	class OutputProperties final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IPropertyStore, FtmBase>
	{
	public:
		HRESULT STDMETHODCALLTYPE GetCount(DWORD* count) override { if (!count) return E_POINTER; *count = 3; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetAt(DWORD index, PROPERTYKEY* key) override
		{
			if (!key) return E_POINTER;
			const PROPERTYKEY keys[] = { AUDIO_FORMAT_KEY, PKEY_Device_FriendlyName, FORM_FACTOR_KEY };
			if (index >= 3) return E_INVALIDARG;
			*key = keys[index]; return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override
		{
			if (!value) return E_POINTER;
			PropVariantInit(value);
			if (key == PKEY_Device_FriendlyName) return InitPropVariantFromString(L"Rocksmith Audio Bridge", value);
			if (key == FORM_FACTOR_KEY) { value->vt = VT_UI4; value->ulVal = Speakers; return S_OK; }
			if (key == AUDIO_FORMAT_KEY)
			{
				WAVEFORMATEX format{ WAVE_FORMAT_IEEE_FLOAT, 2, SAMPLE_RATE, SAMPLE_RATE * 8, 8, 32, 0 };
				value->blob.pBlobData = static_cast<BYTE*>(CoTaskMemAlloc(sizeof(format)));
				if (!value->blob.pBlobData) return E_OUTOFMEMORY;
				std::memcpy(value->blob.pBlobData, &format, sizeof(format));
				value->blob.cbSize = sizeof(format); value->vt = VT_BLOB;
			}
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return STG_E_ACCESSDENIED; }
		HRESULT STDMETHODCALLTYPE Commit() override { return STG_E_ACCESSDENIED; }
	};

	class OutputDevice final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDevice, IMMEndpoint, FtmBase>
	{
	public:
		explicit OutputDevice(std::wstring endpointId) : endpointId(std::move(endpointId)) {}
		HRESULT STDMETHODCALLTYPE Activate(REFIID id, DWORD, PROPVARIANT*, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			return id == __uuidof(IAudioClient) ? SharedOutput::CreateClient(endpointId, reinterpret_cast<IAudioClient**>(object)) : E_NOINTERFACE;
		}
		HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD access, IPropertyStore** properties) override
		{
			if (!properties) return E_POINTER;
			*properties = nullptr;
			if (access != STGM_READ) return STG_E_ACCESSDENIED;
			auto result = Make<OutputProperties>();
			return result ? result.CopyTo(properties) : E_OUTOFMEMORY;
		}
		HRESULT STDMETHODCALLTYPE GetId(LPWSTR* id) override
		{
			if (!id) return E_POINTER;
			*id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(OUTPUT_DEVICE_ID)));
			if (!*id) return E_OUTOFMEMORY;
			std::memcpy(*id, OUTPUT_DEVICE_ID, sizeof(OUTPUT_DEVICE_ID)); return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override { if (!state) return E_POINTER; *state = DEVICE_STATE_ACTIVE; return S_OK; }
		HRESULT STDMETHODCALLTYPE GetDataFlow(EDataFlow* flow) override { if (!flow) return E_POINTER; *flow = eRender; return S_OK; }
	private:
		std::wstring endpointId;
	};

	class CableCollection final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceCollection, FtmBase>
	{
	public:
		std::vector<ComPtr<IMMDevice>> devices;
		HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override { if (!count) return E_POINTER; *count = static_cast<UINT>(devices.size()); return S_OK; }
		HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** device) override
		{
			if (!device) return E_POINTER;
			*device = nullptr;
			return index < devices.size() ? devices[index].CopyTo(device) : E_INVALIDARG;
		}
	};

	class CableNotifications final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMNotificationClient, FtmBase>
	{
	public:
		CableNotifications(IMMNotificationClient* client, bool replaceCapture, bool replaceOutput)
			: client(client), replaceCapture(replaceCapture), replaceOutput(replaceOutput) {}
		HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override
		{
			return (flow == eCapture && replaceCapture) || (flow == eRender && replaceOutput)
				? S_OK : client->OnDefaultDeviceChanged(flow, role, id);
		}
		HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override { return client->OnDeviceStateChanged(id, state); }
		HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override { return client->OnDeviceAdded(id); }
		HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override { return client->OnDeviceRemoved(id); }
		HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override { return client->OnPropertyValueChanged(id, key); }
		ComPtr<IMMNotificationClient> client;
		bool replaceCapture;
		bool replaceOutput;
	};

	class CableEnumerator final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMMDeviceEnumerator, FtmBase>
	{
	public:
		CableEnumerator(IMMDeviceEnumerator* physical, ComPtr<CableDevice> cable, ComPtr<OutputDevice> output,
			bool replaceCapture, bool replaceOutput) : physical(physical), cable(std::move(cable)), output(std::move(output)),
			replaceCapture(replaceCapture), replaceOutput(replaceOutput)
		{
			std::lock_guard<std::mutex> guard(enumeratorRegistryMutex);
			enumerators.push_back(this);
		}
		~CableEnumerator()
		{
			{
				std::lock_guard<std::mutex> guard(enumeratorRegistryMutex);
				enumerators.erase(std::remove(enumerators.begin(), enumerators.end(), this), enumerators.end());
			}
			for (const auto& notification : notifications) physical->UnregisterEndpointNotificationCallback(notification.Get());
		}
		// Not used to add or remove the Player 2 cable: the game never re-enumerates after boot, so the
		// device is always listed and the toggle gates its audio.
		void NotifyCableState(bool enabled)
		{
			if (replaceCapture) return;
			std::vector<ComPtr<IMMNotificationClient>> clients;
			{
				std::lock_guard<std::mutex> guard(notificationMutex);
				clients.reserve(notifications.size());
				for (const auto& notification : notifications) clients.push_back(notification->client);
			}
			for (const auto& client : clients)
			{
				if (enabled)
				{
					client->OnDeviceAdded(DEVICE_ID);
					client->OnDeviceStateChanged(DEVICE_ID, DEVICE_STATE_ACTIVE);
				}
				else
				{
					client->OnDeviceStateChanged(DEVICE_ID, DEVICE_STATE_DISABLED);
					client->OnDeviceRemoved(DEVICE_ID);
				}
			}
		}
		bool IsPlayerTwoCableWrapper() const { return !replaceCapture; }
		HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** devices) override
		{
			if (!devices) return E_POINTER;
			*devices = nullptr;
			if (flow != eRender && flow != eCapture && flow != eAll) return E_INVALIDARG;
			try
			{
				ComPtr<IMMDeviceCollection> actual;
				HRESULT result = S_OK;
				const bool fullyVirtual = replaceCapture && replaceOutput;
				if (!fullyVirtual)
				{
					result = physical->EnumAudioEndpoints(flow, mask, &actual);
					if (FAILED(result)) return result;
				}
				auto collection = Make<CableCollection>();
				if (!collection) return E_OUTOFMEMORY;
				// Cable mode: the cable IS the input. RS_ASIO mode: the Player 2 cable is ALWAYS listed, because
				// the game builds its device list once at boot and never sees a device added later; the Player 2
				// toggle switches the audio inside that stream instead (CaptureSession gate), so it is a hot swap.
				const bool presentCable = true;
				// Hide the physical Real Tone Cable ONLY in Cable mode, where our persistent device stands in for
				// it. In RS_ASIO mode the "physical" list is RS_ASIO's own fake devices, and RS_ASIO reports the
				// Real Tone Cable's USB identity on them on purpose (that is how the game accepts its ASIO input),
				// so the identity test matches them too. Filtering them would drop RS_ASIO's output and input from
				// the game's boot-time list ("ERROR SOUND INITIALIZATION: No audio output device").
				const bool hidePhysicalCable = replaceCapture;
				UINT count = 0;
				if (actual && !(replaceCapture && flow == eCapture)) result = actual->GetCount(&count);
				for (UINT index = 0; SUCCEEDED(result) && index < count; ++index)
				{
					ComPtr<IMMDevice> device;
					result = actual->Item(index, &device);
					if (SUCCEEDED(result) && (!hidePhysicalCable || !IsCable(device.Get())))
					{
						ComPtr<IMMEndpoint> endpoint;
						EDataFlow direction = eAll;
						result = device.As(&endpoint);
						if (SUCCEEDED(result)) result = endpoint->GetDataFlow(&direction);
						if (SUCCEEDED(result) && (!replaceOutput || direction != eRender)) collection->devices.push_back(device);
					}
				}
				if (FAILED(result)) return result;
				if (mask & DEVICE_STATE_ACTIVE)
				{
					if (flow != eRender && cable && presentCable) collection->devices.push_back(cable);
					if (replaceOutput && flow != eCapture) collection->devices.push_back(output);
				}
				if (!replaceCapture)
					LOG_INFO("(P2 CABLE SCAN) EnumAudioEndpoints flow " << flow << " mask " << mask << " -> " << collection->devices.size()
						<< " device(s), physical " << count << ", Player 2 cable " << (presentCable ? "listed" : "not listed") << std::endl);
				return collection.CopyTo(devices);
			}
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		}
		HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** device) override
		{
			if (!device) return E_POINTER;
			if (role < eConsole || role >= ERole_enum_count) return E_INVALIDARG;
			*device = nullptr;
			if (flow == eCapture && replaceCapture) return cable.CopyTo(device);
			if (flow == eRender && replaceOutput) return output.CopyTo(device);
			if (flow != eCapture && flow != eRender) return E_INVALIDARG;
			const HRESULT result = physical->GetDefaultAudioEndpoint(flow, role, device);
			if (!replaceCapture)
				LOG_INFO("(P2 CABLE SCAN) GetDefaultAudioEndpoint flow " << flow << " role " << role << " -> 0x" << std::hex << result << std::dec << std::endl);
			return result;
		}
		HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** device) override
		{
			if (!id || !device) return E_POINTER;
			if (replaceOutput && wcscmp(id, OUTPUT_DEVICE_ID) == 0) return output.CopyTo(device);
			if (cable && wcscmp(id, DEVICE_ID) == 0) return cable.CopyTo(device);
			if (replaceCapture && replaceOutput)
			{
				*device = nullptr;
				return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
			}
			ComPtr<IMMDevice> actual;
			HRESULT result = physical->GetDevice(id, &actual);
			if (FAILED(result)) { *device = nullptr; return result; }
			ComPtr<IMMEndpoint> endpoint;
			EDataFlow flow = eAll;
			result = actual.As(&endpoint);
			if (SUCCEEDED(result)) result = endpoint->GetDataFlow(&flow);
			if (FAILED(result)) { *device = nullptr; return result; }
			return flow == eRender && replaceOutput ? output.CopyTo(device) : actual.CopyTo(device);
		}
		HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* client) override
		{
			if (!replaceCapture) LOG_INFO("(P2 CABLE SCAN) RegisterEndpointNotificationCallback" << std::endl);
			if (!client) return E_POINTER;
			std::lock_guard<std::mutex> guard(notificationMutex);
			for (const auto& notification : notifications)
			{
				if (notification->client.Get() == client) return S_OK;
			}
			try
			{
				auto notification = Make<CableNotifications>(client, replaceCapture, replaceOutput);
				if (!notification) return E_OUTOFMEMORY;
				notifications.push_back(notification);
				const HRESULT result = physical->RegisterEndpointNotificationCallback(notification.Get());
				if (FAILED(result)) notifications.pop_back();
				return result;
			}
			catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
		}
		HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* client) override
		{
			if (!client) return E_POINTER;
			std::lock_guard<std::mutex> guard(notificationMutex);
			for (auto entry = notifications.begin(); entry != notifications.end(); ++entry)
			{
				if ((*entry)->client.Get() != client) continue;
				const HRESULT result = physical->UnregisterEndpointNotificationCallback(entry->Get());
				if (SUCCEEDED(result)) notifications.erase(entry);
				return result;
			}
			return E_INVALIDARG;
		}
	private:
		std::mutex notificationMutex;
		std::vector<ComPtr<CableNotifications>> notifications;
		ComPtr<IMMDeviceEnumerator> physical;
		ComPtr<CableDevice> cable;
		ComPtr<OutputDevice> output;
		bool replaceCapture;
		bool replaceOutput;
	};

	HRESULT CreateEnumerator(IMMDeviceEnumerator* physical, const std::wstring& endpointId, const std::wstring& outputId,
		bool replaceCapture, bool replaceOutput, IMMDeviceEnumerator** enumerator)
	{
		if (!physical || !enumerator) return E_POINTER;
		*enumerator = nullptr;
		try
		{
			auto device = Make<CableDevice>(endpointId, !replaceCapture);
			if (!device) return E_OUTOFMEMORY;
			auto output = Make<OutputDevice>(outputId);
			if (!output) return E_OUTOFMEMORY;
			auto result = Make<CableEnumerator>(physical, device, output, replaceCapture, replaceOutput);
			return result ? result.CopyTo(enumerator) : E_OUTOFMEMORY;
		}
		catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
	}

	// Hot swap: flips the gate every Player 2 cable stream reads per packet. Nothing is added to or removed
	// from the game's device list (it would not notice), so this works mid-song.
	void SetCableForPlayerTwoEnabled(bool enabled)
	{
		const bool previous = cableForPlayerTwoEnabled.exchange(enabled, std::memory_order_acq_rel);
		if (previous == enabled) return;
		LOG_INFO("(CABLE INPUT) Player 2 Real Tone Cable feed " << (enabled ? "on" : "off") << std::endl);
	}

	bool IsCableForPlayerTwoEnabled()
	{
		return cableForPlayerTwoEnabled.load(std::memory_order_acquire);
	}

	bool IsCableForPlayerTwoAvailable()
	{
		std::lock_guard<std::mutex> guard(enumeratorRegistryMutex);
		return std::any_of(enumerators.begin(), enumerators.end(), [](const CableEnumerator* enumerator)
			{ return enumerator->IsPlayerTwoCableWrapper(); });
	}

	std::wstring configuredEndpoint;
	std::wstring configuredOutput;
	bool configuredCapture = true;
	bool configuredOutputReplacement = true;
	bool configuredTap = false;
	using CreateObject = HRESULT(WINAPI*)(REFCLSID, IUnknown*, DWORD, REFIID, void**);
	CreateObject originalCreateObject = CoCreateInstance;

	HRESULT WINAPI CreateGameObject(REFCLSID classId, IUnknown* outer, DWORD context, REFIID id, void** object)
	{
		if (!object) return E_POINTER;
		*object = nullptr;
		if (classId != __uuidof(MMDeviceEnumerator) || id != __uuidof(IMMDeviceEnumerator) || outer)
			return originalCreateObject(classId, outer, context, id, object);
		ComPtr<IMMDeviceEnumerator> physical;
		const HRESULT result = originalCreateObject(classId, nullptr, context, IID_PPV_ARGS(&physical));
		if (!configuredCapture)
			LOG_INFO("(P2 CABLE SCAN) game created a device enumerator -> 0x" << std::hex << result << std::dec
				<< (configuredTap ? " (tap)" : "") << std::endl);
		if (FAILED(result)) return result;
		// Compose the passive render tap around the configured enumerator. This keeps the selected
		// input/output strategy intact while exposing the same normalized wet samples to recording.
		if (configuredTap)
		{
			ComPtr<IMMDeviceEnumerator> configured;
			const HRESULT configuredResult = CreateEnumerator(physical.Get(), configuredEndpoint, configuredOutput,
				configuredCapture, configuredOutputReplacement, reinterpret_cast<IMMDeviceEnumerator**>(configured.GetAddressOf()));
			if (FAILED(configuredResult)) return configuredResult;
			return OutputTap::CreateTapEnumerator(configured.Get(), reinterpret_cast<IMMDeviceEnumerator**>(object));
		}
		return CreateEnumerator(physical.Get(), configuredEndpoint, configuredOutput, configuredCapture,
			configuredOutputReplacement, reinterpret_cast<IMMDeviceEnumerator**>(object));
	}

	CreateObject ReadAsioCall(BYTE* candidate, uintptr_t moduleStart, size_t moduleSize)
	{
		if (candidate[0] != 0xe8 || candidate[5] != 0x90 || candidate[6] != 0x85 || candidate[7] != 0xc0) return nullptr;
		int32_t displacement = 0;
		std::memcpy(&displacement, candidate + 1, sizeof(displacement));
		const uintptr_t target = reinterpret_cast<uintptr_t>(candidate) + 5 + displacement;
		if (target < moduleStart || target - moduleStart >= moduleSize) return nullptr;
		return reinterpret_cast<CreateObject>(target);
	}

	bool Install(const std::wstring& endpointId, const std::wstring& outputId,
		bool replaceCapture, bool replaceOutput, EnumeratorSource source, bool tapMode)
	{
		// These two post-September-2022 call sites are independently documented by RS_ASIO.
		// Patch only a complete, unique match. Other executable layouts require their own verified profile.
		const BYTE patterns[2][8] = {
			{ 0xe8, 0x3e, 0x9d, 0x7d, 0x00, 0xcd, 0x85, 0xc0 },
			{ 0xe8, 0xc2, 0x23, 0x57, 0x00, 0x96, 0x85, 0xc0 }
		};
		auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
		const auto* header = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
		if (header->Signature != IMAGE_NT_SIGNATURE) return false;
		BYTE* sites[2]{};
		MODULEINFO asio{};
		const bool interceptRsAsio = source == EnumeratorSource::RsAsio;
		if (interceptRsAsio && !GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(L"RS_ASIO.dll"), &asio, sizeof(asio))) return false;
		CreateObject asioCreate = nullptr;
		UINT asioSites = 0;
		const auto* sections = IMAGE_FIRST_SECTION(header);
		for (UINT pattern = 0; pattern < (interceptRsAsio ? 1u : 2u); ++pattern)
		{
			for (UINT section = 0; section < header->FileHeader.NumberOfSections; ++section)
			{
				if (!(sections[section].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
				const UINT32 size = sections[section].Misc.VirtualSize;
				for (UINT32 offset = 0; offset + 8 <= size; ++offset)
				{
					auto* candidate = base + sections[section].VirtualAddress + offset;
					if (interceptRsAsio)
					{
						auto target = ReadAsioCall(candidate, reinterpret_cast<uintptr_t>(asio.lpBaseOfDll), asio.SizeOfImage);
						if (!target) continue;
						if (asioSites == 2 || (asioCreate && asioCreate != target)) return false;
						asioCreate = target;
						sites[asioSites++] = candidate;
						continue;
					}
					if (std::memcmp(candidate, patterns[pattern], 8) != 0) continue;
					if (sites[pattern]) return false;
					sites[pattern] = candidate;
				}
			}
			if (!sites[pattern]) return false;
		}
		if (interceptRsAsio && asioSites != 2) return false;
		configuredEndpoint = endpointId;
		configuredOutput = outputId;
		configuredCapture = replaceCapture;
		configuredOutputReplacement = replaceOutput;
		configuredTap = tapMode;
		originalCreateObject = interceptRsAsio ? asioCreate : CoCreateInstance;
		DWORD protection[2]{};
		for (UINT index = 0; index < 2; ++index)
		{
			if (!VirtualProtect(sites[index], 6, PAGE_EXECUTE_READWRITE, &protection[index]))
			{
				for (UINT previous = 0; previous < index; ++previous)
				{
					DWORD ignored = 0; VirtualProtect(sites[previous], 6, protection[previous], &ignored);
				}
				return false;
			}
		}
		for (UINT index = 0; index < 2; ++index)
		{
			const int32_t displacement = static_cast<int32_t>(reinterpret_cast<BYTE*>(&CreateGameObject) - (sites[index] + 5));
			sites[index][0] = 0xe8;
			std::memcpy(sites[index] + 1, &displacement, 4);
			sites[index][5] = 0x90;
			FlushInstructionCache(GetCurrentProcess(), sites[index], 6);
			DWORD ignored = 0;
			if (!VirtualProtect(sites[index], 6, protection[index], &ignored))
				LOG_ERROR("(PERSISTENT INPUT) Could not restore call-site protection" << std::endl);
		}
		LOG_INFO("(AUDIO ROUTING) " << (tapMode ? "Passive output tap installed (render device wrapped, playback unchanged)"
			: replaceOutput ? "Permanent bridge output installed" : "Permanent input wrapper installed")
			<< "; capture=" << (replaceCapture ? "Cable" : "RS_ASIO plus optional Player 2 Cable") << std::endl);
		return true;
	}
}
