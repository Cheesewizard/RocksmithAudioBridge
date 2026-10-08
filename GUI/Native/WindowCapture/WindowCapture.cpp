#include <windows.h>
#include <unknwn.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <strmif.h>
#include <codecapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// Recording pipeline. Two problems with the first version (30 fps, CPU readback, Baseline 12 Mbps) made
// takes look choppy and soft next to the game:
//  - Frames were kept when 33 ms had passed since the last kept frame, measured from its arrival time, so a
//    60 Hz source with normal jitter alternated 2- and 3-frame gaps (judder).
//  - Every frame was mapped to the CPU right after the copy (GPU stall), copied into a fresh buffer and
//    colour-converted in software before encoding.
// Now each frame lands on a fixed output clock (slot = elapsed / interval); a slot the game skipped repeats
// the previous image so the file is constant rate. On the GPU path the frame is converted BGRA -> NV12 by the
// D3D11 video processor (BT.709, studio range) and handed to the encoder as a DXGI surface through the
// sink writer's device manager, so the hardware encoder reads it without a CPU round trip. If any part of
// the GPU path cannot be set up, the CPU path (RGB32 readback) is used instead with the same clock.
namespace
{
	constexpr UINT32 MaximumInFlight = 16;
	constexpr LONGLONG MaximumRepeatedSlots = 30;

	// NV12 surfaces handed to the encoder. A tracked sample calls back when the encoder releases it, which
	// returns it here, so a surface is never overwritten while the encoder may still read it.
	struct SurfacePool
	{
		std::mutex lock;
		std::vector<com_ptr<IMFSample>> free;
		UINT32 created = 0;
	};

	struct SampleReturn : implements<SampleReturn, IMFAsyncCallback>
	{
		std::weak_ptr<SurfacePool> pool;

		explicit SampleReturn(std::weak_ptr<SurfacePool> owner) : pool(std::move(owner)) {}

		HRESULT __stdcall GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }

		HRESULT __stdcall Invoke(IMFAsyncResult* result) noexcept override
		{
			com_ptr<::IUnknown> object;
			if (result == nullptr || FAILED(result->GetObject(object.put())))
				return S_OK;
			auto owner = pool.lock();
			if (!owner)
				return S_OK;
			auto sample = object.try_as<IMFSample>();
			if (sample)
			{
				std::lock_guard<std::mutex> guard(owner->lock);
				owner->free.push_back(sample);
			}
			return S_OK;
		}
	};

	struct Capture
	{
		com_ptr<ID3D11Device> device;
		com_ptr<ID3D11DeviceContext> context;
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
		UINT32 interval = 0;
		LONGLONG lastSlot = -1;
		std::atomic<UINT64> frames{ 0 };
		std::atomic<HRESULT> failure{ S_OK };
		bool mediaFoundationStarted = false;

		// GPU path: latest frame (BGRA) -> video processor -> pooled NV12 surface -> encoder.
		bool gpu = false;
		com_ptr<IMFDXGIDeviceManager> deviceManager;
		UINT resetToken = 0;
		com_ptr<ID3D11VideoDevice> videoDevice;
		com_ptr<ID3D11VideoContext> videoContext;
		com_ptr<ID3D11VideoProcessorEnumerator> enumerator;
		com_ptr<ID3D11VideoProcessor> processor;
		com_ptr<ID3D11Texture2D> latest;
		com_ptr<ID3D11VideoProcessorInputView> latestView;
		std::shared_ptr<SurfacePool> surfaces;
		com_ptr<IMFAsyncCallback> sampleReturn;

		// CPU path: latest frame lives in a staging texture.
		com_ptr<ID3D11Texture2D> staging;

		~Capture()
		{
			if (surfaces)
			{
				std::lock_guard<std::mutex> guard(surfaces->lock);
				surfaces->free.clear();
			}
		}
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
		// VIDEO_SUPPORT is needed for the video processor and for sharing the device with the encoder. Some
		// adapters refuse it; those still record on the CPU path.
		const UINT flags[] = { D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, D3D11_CREATE_DEVICE_BGRA_SUPPORT };
		HRESULT result = E_FAIL;
		for (UINT flag : flags)
		{
			capture.device = nullptr;
			capture.context = nullptr;
			result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flag, nullptr, 0,
				D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put());
			if (SUCCEEDED(result))
				break;
		}
		if (FAILED(result))
			result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
				D3D11_SDK_VERSION, capture.device.put(), nullptr, capture.context.put());
		if (FAILED(result))
			return result;
		// The encoder uses the device from its own threads.
		if (auto multithread = capture.device.try_as<ID3D11Multithread>())
			multithread->SetMultithreadProtected(TRUE);
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

	HRESULT CreateConverter(Capture& capture)
	{
		HRESULT result = S_OK;
		capture.videoDevice = capture.device.try_as<ID3D11VideoDevice>();
		capture.videoContext = capture.context.try_as<ID3D11VideoContext>();
		if (!capture.videoDevice || !capture.videoContext)
			return E_NOINTERFACE;
		D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
		content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
		content.InputWidth = capture.width;
		content.InputHeight = capture.height;
		content.OutputWidth = capture.width;
		content.OutputHeight = capture.height;
		content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
		result = capture.videoDevice->CreateVideoProcessorEnumerator(&content, capture.enumerator.put());
		if (FAILED(result))
			return result;
		UINT support = 0;
		if (FAILED(capture.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &support))
			|| (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0)
			return E_NOTIMPL;
		support = 0;
		if (FAILED(capture.enumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &support))
			|| (support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0)
			return E_NOTIMPL;
		result = capture.videoDevice->CreateVideoProcessor(capture.enumerator.get(), 0, capture.processor.put());
		if (FAILED(result))
			return result;

		// Game frames are full-range sRGB; the file is BT.709 studio range, which is what players assume
		// for HD video and what the output media type is tagged with.
		if (auto context1 = capture.videoContext.try_as<ID3D11VideoContext1>())
		{
			context1->VideoProcessorSetStreamColorSpace1(capture.processor.get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
			context1->VideoProcessorSetOutputColorSpace1(capture.processor.get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
		}
		else
		{
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
			input.RGB_Range = 0;
			input.YCbCr_Matrix = 1;
			D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
			output.RGB_Range = 0;
			output.YCbCr_Matrix = 1;
			output.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
			capture.videoContext->VideoProcessorSetStreamColorSpace(capture.processor.get(), 0, &input);
			capture.videoContext->VideoProcessorSetOutputColorSpace(capture.processor.get(), &output);
		}
		capture.videoContext->VideoProcessorSetStreamFrameFormat(capture.processor.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
		capture.videoContext->VideoProcessorSetStreamAutoProcessingMode(capture.processor.get(), 0, FALSE);
		RECT rectangle{ 0, 0, static_cast<LONG>(capture.width), static_cast<LONG>(capture.height) };
		capture.videoContext->VideoProcessorSetStreamSourceRect(capture.processor.get(), 0, TRUE, &rectangle);
		capture.videoContext->VideoProcessorSetStreamDestRect(capture.processor.get(), 0, TRUE, &rectangle);
		capture.videoContext->VideoProcessorSetOutputTargetRect(capture.processor.get(), TRUE, &rectangle);

		D3D11_TEXTURE2D_DESC description{};
		description.Width = capture.width;
		description.Height = capture.height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		result = capture.device->CreateTexture2D(&description, nullptr, capture.latest.put());
		if (FAILED(result))
			return result;
		D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputView{};
		inputView.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
		result = capture.videoDevice->CreateVideoProcessorInputView(capture.latest.get(), capture.enumerator.get(), &inputView, capture.latestView.put());
		if (FAILED(result))
			return result;

		result = MFCreateDXGIDeviceManager(&capture.resetToken, capture.deviceManager.put());
		if (FAILED(result))
			return result;
		result = capture.deviceManager->ResetDevice(capture.device.get(), capture.resetToken);
		if (FAILED(result))
			return result;
		capture.surfaces = std::make_shared<SurfacePool>();
		capture.sampleReturn = make<SampleReturn>(capture.surfaces);
		return S_OK;
	}

	void ReleaseConverter(Capture& capture)
	{
		capture.gpu = false;
		capture.latestView = nullptr;
		capture.latest = nullptr;
		capture.processor = nullptr;
		capture.enumerator = nullptr;
		capture.videoContext = nullptr;
		capture.videoDevice = nullptr;
		capture.deviceManager = nullptr;
		capture.sampleReturn = nullptr;
		capture.surfaces = nullptr;
	}

	// Constant-quality-ish target: ~0.15 bits per pixel per frame (about 37 Mbps at 2560x1600x60,
	// 19 Mbps at 1080p60), clamped so tiny windows still look clean. Capped at 40 Mbps: a 66 Mbps,
	// level 5.2 take (the encoder overshooting an uncapped target) stuttered in Windows Media Player,
	// and 40 Mbps still looks clean at 1440p/1600p60 and plays everywhere.
	constexpr double MAXIMUM_AUTOMATIC_BITRATE = 40000000.0;
	UINT32 AutomaticBitrate(UINT width, UINT height, UINT32 framesPerSecond)
	{
		double bits = static_cast<double>(width) * height * framesPerSecond * 0.15;
		return static_cast<UINT32>(std::clamp(bits, 8000000.0, MAXIMUM_AUTOMATIC_BITRATE));
	}

	void TagColour(IMFMediaType* type)
	{
		type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
		type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
		type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
		type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
	}

	void SetCodecValue(ICodecAPI* codec, const GUID& property, UINT32 value)
	{
		VARIANT variant{};
		variant.vt = VT_UI4;
		variant.ulVal = value;
		codec->SetValue(&property, &variant);
	}

	// Some hardware encoders (AMD's, at least) ignore MF_MT_AVG_BITRATE and run their own quality default,
	// which came out near 2x the target. Setting rate control on the encoder itself makes them honour it.
	// Best effort: an encoder that rejects a value keeps its default.
	void ConfigureEncoder(Capture& capture, UINT32 bitrate, UINT32 framesPerSecond)
	{
		com_ptr<ICodecAPI> codec;
		if (FAILED(capture.writer->GetServiceForStream(capture.stream, GUID_NULL, IID_PPV_ARGS(codec.put()))))
			return;
		VARIANT mode{};
		mode.vt = VT_UI4;
		mode.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
		if (FAILED(codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &mode)))
		{
			mode.ulVal = eAVEncCommonRateControlMode_CBR;
			codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &mode);
		}
		SetCodecValue(codec.get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
		SetCodecValue(codec.get(), CODECAPI_AVEncCommonMaxBitRate, bitrate / 2 * 3);
		// Keyframe every 2 s; the encoder default (0.5 s on AMD) spends a large share of the bits on keyframes.
		SetCodecValue(codec.get(), CODECAPI_AVEncMPVGOPSize, framesPerSecond * 2);
	}

	HRESULT CreateWriter(Capture& capture, const wchar_t* path, UINT32 bitrate, UINT32 framesPerSecond, bool highProfile)
	{
		capture.writer = nullptr;
		com_ptr<IMFAttributes> attributes;
		HRESULT result = MFCreateAttributes(attributes.put(), 3);
		if (FAILED(result))
			return result;
		attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
		attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		if (capture.gpu)
			attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, capture.deviceManager.get());
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
		if (highProfile)
			output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
		TagColour(output.get());
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
		input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		if (capture.gpu)
		{
			input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
			TagColour(input.get());
		}
		else
		{
			input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
			input->SetUINT32(MF_MT_DEFAULT_STRIDE, capture.width * 4);
		}
		MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, capture.width, capture.height);
		MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, framesPerSecond, 1);
		MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		// Rate control goes in with the input type, while the sink writer creates and configures the encoder.
		// Set later through ICodecAPI (ConfigureEncoder) AMD's encoder ignored it and ran at about 1.8x the
		// target (66 Mbps for 37). ConfigureEncoder stays as a fallback for encoders that only take it late.
		com_ptr<IMFAttributes> encoding;
		if (SUCCEEDED(MFCreateAttributes(encoding.put(), 4)))
		{
			encoding->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_PeakConstrainedVBR);
			encoding->SetUINT32(CODECAPI_AVEncCommonMeanBitRate, bitrate);
			encoding->SetUINT32(CODECAPI_AVEncCommonMaxBitRate, bitrate / 2 * 3);
			encoding->SetUINT32(CODECAPI_AVEncMPVGOPSize, framesPerSecond * 2);
		}
		result = capture.writer->SetInputMediaType(capture.stream, input.get(), encoding.get());
		if (FAILED(result) && encoding)
			result = capture.writer->SetInputMediaType(capture.stream, input.get(), nullptr);
		if (FAILED(result))
			return result;
		ConfigureEncoder(capture, bitrate, framesPerSecond);
		return capture.writer->BeginWriting();
	}

	LONGLONG Elapsed(const Capture& capture)
	{
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		return (now.QuadPart - capture.startCounter) * 10000000LL / capture.frequency;
	}

	HRESULT CreateSurface(Capture& capture, com_ptr<IMFSample>& sample)
	{
		D3D11_TEXTURE2D_DESC description{};
		description.Width = capture.width;
		description.Height = capture.height;
		description.MipLevels = 1;
		description.ArraySize = 1;
		description.Format = DXGI_FORMAT_NV12;
		description.SampleDesc.Count = 1;
		description.Usage = D3D11_USAGE_DEFAULT;
		description.BindFlags = D3D11_BIND_RENDER_TARGET;
		com_ptr<ID3D11Texture2D> texture;
		HRESULT result = capture.device->CreateTexture2D(&description, nullptr, texture.put());
		if (FAILED(result))
			return result;
		com_ptr<IMFMediaBuffer> buffer;
		result = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture.get(), 0, FALSE, buffer.put());
		if (FAILED(result))
			return result;
		if (auto planar = buffer.try_as<IMF2DBuffer>())
		{
			DWORD length = 0;
			if (SUCCEEDED(planar->GetContiguousLength(&length)))
				buffer->SetCurrentLength(length);
		}
		com_ptr<IMFTrackedSample> tracked;
		result = MFCreateTrackedSample(tracked.put());
		if (FAILED(result))
			return result;
		sample = tracked.as<IMFSample>();
		return sample->AddBuffer(buffer.get());
	}

	// Converts the latest frame into a free NV12 surface and queues it. S_FALSE = encoder is behind and every
	// surface is in flight, so this slot is skipped rather than growing memory.
	HRESULT EmitGpu(Capture& capture, LONGLONG timestamp)
	{
		com_ptr<IMFSample> sample;
		{
			std::lock_guard<std::mutex> lock(capture.surfaces->lock);
			if (!capture.surfaces->free.empty())
			{
				sample = capture.surfaces->free.back();
				capture.surfaces->free.pop_back();
			}
			else if (capture.surfaces->created >= MaximumInFlight)
				return S_FALSE;
		}
		HRESULT result = S_OK;
		if (!sample)
		{
			result = CreateSurface(capture, sample);
			if (FAILED(result))
				return result;
			std::lock_guard<std::mutex> lock(capture.surfaces->lock);
			capture.surfaces->created++;
		}
		com_ptr<IMFMediaBuffer> buffer;
		result = sample->GetBufferByIndex(0, buffer.put());
		if (FAILED(result))
			return result;
		com_ptr<ID3D11Texture2D> texture;
		result = buffer.as<IMFDXGIBuffer>()->GetResource(guid_of<ID3D11Texture2D>(), texture.put_void());
		if (FAILED(result))
			return result;
		D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputView{};
		outputView.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
		com_ptr<ID3D11VideoProcessorOutputView> view;
		result = capture.videoDevice->CreateVideoProcessorOutputView(texture.get(), capture.enumerator.get(), &outputView, view.put());
		if (FAILED(result))
			return result;
		D3D11_VIDEO_PROCESSOR_STREAM stream{};
		stream.Enable = TRUE;
		stream.pInputSurface = capture.latestView.get();
		result = capture.videoContext->VideoProcessorBlt(capture.processor.get(), view.get(), 0, 1, &stream);
		if (FAILED(result))
			return result;
		sample->SetSampleTime(timestamp);
		sample->SetSampleDuration(capture.interval);
		result = sample.as<IMFTrackedSample>()->SetAllocator(capture.sampleReturn.get(), nullptr);
		if (FAILED(result))
			return result;
		// Our reference drops when this returns; the tracked sample comes back through SampleReturn once
		// the encoder lets go of it too.
		return capture.writer->WriteSample(capture.stream, sample.get());
	}

	HRESULT EmitCpu(Capture& capture, LONGLONG timestamp)
	{
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
		sample->SetSampleDuration(capture.interval);
		return capture.writer->WriteSample(capture.stream, sample.get());
	}

	HRESULT Emit(Capture& capture, LONGLONG slot)
	{
		HRESULT result = capture.gpu ? EmitGpu(capture, slot * capture.interval) : EmitCpu(capture, slot * capture.interval);
		if (result == S_OK)
			capture.frames.fetch_add(1);
		return result;
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
		if (capture.lastSlot < 0)
		{
			LARGE_INTEGER counter{};
			QueryPerformanceCounter(&counter);
			capture.startCounter = counter.QuadPart;
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			capture.startFileTime = (static_cast<UINT64>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
		}
		// Nearest output slot, so arrival jitter of up to half a frame does not move a frame to the wrong slot.
		LONGLONG slot = (Elapsed(capture) + capture.interval / 2) / capture.interval;
		if (slot <= capture.lastSlot)
			return;
		HRESULT result = S_OK;
		// The game skipped slots (hitch, loading): repeat the previous image so the file stays constant rate.
		// Long gaps (minimised window) are left as a timestamp gap instead of flooding the encoder.
		if (capture.lastSlot >= 0 && slot - capture.lastSlot - 1 <= MaximumRepeatedSlots)
			for (LONGLONG missing = capture.lastSlot + 1; missing < slot && SUCCEEDED(result); missing++)
				result = Emit(capture, missing);
		if (SUCCEEDED(result))
		{
			capture.context->CopyResource(capture.gpu ? capture.latest.get() : capture.staging.get(), surface.get());
			result = Emit(capture, slot);
		}
		if (FAILED(result))
			capture.failure.store(result);
		else
			capture.lastSlot = slot;
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

	/// bitrate 0 = pick from the window size and frame rate (AutomaticBitrate).
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
			capture->interval = 10000000u / framesPerSecond;
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
			if (bitrate == 0)
				bitrate = AutomaticBitrate(capture->width, capture->height, framesPerSecond);

			// GPU path first; anything it cannot do falls back to the CPU path. High profile falls back to
			// the encoder default only if the encoder rejects it outright.
			capture->gpu = SUCCEEDED(CreateConverter(*capture));
			if (!capture->gpu)
				ReleaseConverter(*capture);
			result = CreateWriter(*capture, path, bitrate, framesPerSecond, true);
			if (FAILED(result) && capture->gpu)
			{
				ReleaseConverter(*capture);
				result = CreateWriter(*capture, path, bitrate, framesPerSecond, true);
			}
			if (FAILED(result))
				result = CreateWriter(*capture, path, bitrate, framesPerSecond, false);
			if (FAILED(result))
				return result;
			if (!capture->gpu)
			{
				result = CreateStaging(*capture);
				if (FAILED(result))
					return result;
			}

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
			capture->writer = nullptr;
		}
		if (startFileTime != nullptr) *startFileTime = capture->startFileTime;
		if (frames != nullptr) *frames = capture->frames.load();
		bool empty = capture->frames.load() == 0;
		bool shutdown = capture->mediaFoundationStarted;
		capture.reset();
		if (shutdown) MFShutdown();
		if (SUCCEEDED(result) && empty)
			return HRESULT_FROM_WIN32(ERROR_EMPTY);
		return result;
	}
}
