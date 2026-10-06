#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <atomic>
#include <memory>
#include <mutex>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

namespace
{
	struct Capture
	{
		com_ptr<ID3D11Device> device;
		com_ptr<ID3D11DeviceContext> context;
		com_ptr<ID3D11Texture2D> staging;
		com_ptr<IMFSinkWriter> writer;
		IDirect3DDevice captureDevice{ nullptr };
		GraphicsCaptureItem item{ nullptr };
		Direct3D11CaptureFramePool pool{ nullptr };
		GraphicsCaptureSession session{ nullptr };
		Direct3D11CaptureFramePool::FrameArrived_revoker frameArrived;
		std::mutex writing;
		DWORD stream = 0;
		UINT width = 0;
		UINT height = 0;
		LONGLONG frequency = 0;
		LONGLONG startCounter = 0;
		UINT64 startFileTime = 0;
		UINT32 minimumInterval = 0;
		LONGLONG lastWritten = -1;
		std::atomic<UINT64> frames{ 0 };
		std::atomic<HRESULT> failure{ S_OK };
		bool mediaFoundationStarted = false;
	};

	std::mutex guard;
	Capture* active = nullptr;

	void EnsureApartment()
	{
		HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (result != RPC_E_CHANGED_MODE && FAILED(result))
			throw_hresult(result);
	}

	HRESULT CreateDevice(Capture& capture)
	{
		UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
		HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
			D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put());
		if (result == DXGI_ERROR_UNSUPPORTED)
			result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
				D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put());
		if (FAILED(result))
			return result;
		auto dxgiDevice = capture.device.as<IDXGIDevice>();
		com_ptr<::IInspectable> inspectable;
		result = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
		if (FAILED(result))
			return result;
		capture.captureDevice = inspectable.as<IDirect3DDevice>();
		return S_OK;
	}

	HRESULT CreateStaging(Capture& capture)
	{
		D3D11_TEXTURE2D_DESC description{};
		description.Width = capture.width;
		description.Height = capture.height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_STAGING;
		description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		capture.staging = nullptr;
		return capture.device->CreateTexture2D(&description, nullptr, capture.staging.put());
	}

	HRESULT CreateWriter(Capture& capture, const wchar_t* path, UINT32 bitrate, UINT32 framesPerSecond)
	{
		com_ptr<IMFAttributes> attributes;
		HRESULT result = MFCreateAttributes(attributes.put(), 2);
		if (FAILED(result))
			return result;
		attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
		attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		result = MFCreateSinkWriterFromURL(path, nullptr, attributes.get(), capture.writer.put());
		if (FAILED(result))
			return result;
		com_ptr<IMFMediaType> output;
		result = MFCreateMediaType(output.put());
		if (FAILED(result))
			return result;
		output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
		output->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
		output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, capture.width, capture.height);
		MFSetAttributeRatio(output.get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
		MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		result = capture.writer->AddStream(output.get(), &capture.stream);
		if (FAILED(result))
			return result;
		com_ptr<IMFMediaType> input;
		result = MFCreateMediaType(input.put());
		if (FAILED(result))
			return result;
		input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
		input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		input->SetUINT32(MF_MT_DEFAULT_STRIDE, capture.width * 4);
		MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, capture.width, capture.height);
		MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
		MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		result = capture.writer->SetInputMediaType(capture.stream, input.get(), nullptr);
		if (FAILED(result))
			return result;
		return capture.writer->BeginWriting();
	}

	LONGLONG Elapsed(const Capture& capture)
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return (now.QuadPart - capture.startCounter) * 10000000LL / capture.frequency;
	}

	HRESULT WriteFrame(Capture& capture, ID3D11Texture2D* surface, LONGLONG timestamp)
	{
		capture.context->CopyResource(capture.staging.get(), surface);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		HRESULT result = capture.context->Map(capture.staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
		if (FAILED(result))
			return result;
		com_ptr<IMFMediaBuffer> buffer;
		const DWORD length = capture.width * capture.height * 4;
		result = MFCreateMemoryBuffer(length, buffer.put());
		if (SUCCEEDED(result))
		{
			BYTE* target = nullptr;
			result = buffer->Lock(&target, nullptr, nullptr);
			if (SUCCEEDED(result))
			{
				result = MFCopyImage(target, capture.width * 4, static_cast<const BYTE*>(mapped.pData), mapped.RowPitch, capture.width * 4, capture.height);
				buffer->Unlock();
				buffer->SetCurrentLength(length);
			}
		}
		capture.context->Unmap(capture.staging.get(), 0);
		if (FAILED(result))
			return result;
		com_ptr<IMFSample> sample;
		result = MFCreateSample(sample.put());
		if (FAILED(result))
			return result;
		result = sample->AddBuffer(buffer.get());
		if (FAILED(result))
			return result;
		sample->SetSampleTime(timestamp);
		sample->SetSampleDuration(capture.minimumInterval);
		return capture.writer->WriteSample(capture.stream, sample.get());
	}

	void OnFrame(Capture& capture, const Direct3D11CaptureFramePool& pool)
	{
		auto frame = pool.TryGetNextFrame();
		if (!frame || FAILED(capture.failure.load()))
			return;
		auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		com_ptr<ID3D11Texture2D> surface;
		if (FAILED(access->GetInterface(guid_of<ID3D11Texture2D>(), surface.put_void())))
			return;
		D3D11_TEXTURE2D_DESC description{};
		surface->GetDesc(&description);
		if (description.Width != capture.width || description.Height != capture.height)
			return;
		std::lock_guard<std::mutex> lock(capture.writing);
		if (capture.frames.load() == 0)
		{
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			capture.startCounter = counter.QuadPart;
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			capture.startFileTime = (static_cast<UINT64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		}
		LONGLONG timestamp = Elapsed(capture);
		if (timestamp - capture.lastWritten < capture.minimumInterval && capture.frames.load() > 0)
			return;
		HRESULT result = WriteFrame(capture, surface.get(), timestamp);
		if (FAILED(result))
			capture.failure.store(result);
		else
		{
			capture.lastWritten = timestamp;
			capture.frames.fetch_add(1);
		}
	}
}

extern "C"
{
	__declspec(dllexport) BOOL RsCaptureSupported()
	{
		try
		{
			EnsureApartment();
			return GraphicsCaptureSession::IsSupported() ? TRUE : FALSE;
		}
		catch (...) { return FALSE; }
	}

	__declspec(dllexport) HRESULT RsCaptureStart(HWND window, const wchar_t* path, UINT32 bitrate, UINT32 framesPerSecond)
	{
		std::lock_guard<std::mutex> lock(guard);
		if (active != nullptr)
			return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
		if (window == nullptr || !IsWindow(window) || path == nullptr || framesPerSecond == 0)
			return E_INVALIDARG;
		auto capture = std::make_unique<Capture>();
		try
		{
			EnsureApartment();
			if (!GraphicsCaptureSession::IsSupported())
				return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
			HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
			if (FAILED(result))
				return result;
			capture->mediaFoundationStarted = true;
			capture->minimumInterval = 10000000u / framesPerSecond;
			result = CreateDevice(*capture);
			if (FAILED(result))
				return result;
			auto interop = get_activation_factory<GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
			result = interop->CreateForWindow(window, guid_of<GraphicsCaptureItem>(), reinterpret_cast<void**>(put_abi(capture->item)));
			if (FAILED(result))
				return result;
			auto size = capture->item.Size();
			capture->width = static_cast<UINT>(size.Width) & ~1u;
			capture->height = static_cast<UINT>(size.Height) & ~1u;
			if (capture->width == 0 || capture->height == 0)
				return E_INVALIDARG;
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			capture->frequency = frequency.QuadPart;
			result = CreateStaging(*capture);
			if (FAILED(result))
				return result;
			result = CreateWriter(*capture, path, bitrate, framesPerSecond);
			if (FAILED(result))
				return result;
			capture->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(capture->captureDevice,
				DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
				{ static_cast<int32_t>(capture->width), static_cast<int32_t>(capture->height) });
			Capture* raw = capture.get();
			capture->frameArrived = capture->pool.FrameArrived(auto_revoke,
				[raw](const Direct3D11CaptureFramePool& pool, const winrt::Windows::Foundation::IInspectable&) { OnFrame(*raw, pool); });
			capture->session = capture->pool.CreateCaptureSession(capture->item);
			capture->session.IsCursorCaptureEnabled(false);
			try { capture->session.IsBorderRequired(false); }
			catch (...) { }
			capture->session.StartCapture();
			active = capture.release();
			return S_OK;
		}
		catch (const hresult_error& error) { return error.code(); }
		catch (...) { return E_FAIL; }
	}

	__declspec(dllexport) HRESULT RsCaptureStop(UINT64* startFileTime, UINT64* frames)
	{
		std::lock_guard<std::mutex> lock(guard);
		if (active == nullptr)
			return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
		std::unique_ptr<Capture> capture(active);
		active = nullptr;
		try
		{
			capture->frameArrived.revoke();
			if (capture->session) capture->session.Close();
			if (capture->pool) capture->pool.Close();
		}
		catch (...) { }
		std::lock_guard<std::mutex> writing(capture->writing);
		HRESULT result = capture->failure.load();
		if (capture->writer)
		{
			HRESULT finalized = capture->writer->Finalize();
			if (SUCCEEDED(result))
				result = finalized;
		}
		if (startFileTime != nullptr) *startFileTime = capture->startFileTime;
		if (frames != nullptr) *frames = capture->frames.load();
		if (capture->mediaFoundationStarted) MFShutdown();
		if (SUCCEEDED(result) && capture->frames.load() == 0)
			return HRESULT_FROM_WIN32(ERROR_EMPTY);
		return result;
	}
}
