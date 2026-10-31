#include "CaptureSamples.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace AudioBridge::Input
{
	namespace
	{
		constexpr float INT16_TO_FLOAT = 1.0f / 32768.0f;
		constexpr float INT24_TO_FLOAT = 1.0f / 8388608.0f;
		constexpr float INT32_TO_FLOAT = 1.0f / 2147483648.0f;

		// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT, spelled out so no GUID library is linked.
		constexpr GUID PCM_SUBFORMAT = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
		constexpr GUID FLOAT_SUBFORMAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

		float ClampSample(float value)
		{
			if (!std::isfinite(value)) return 0.0f;
			return std::clamp(value, -1.0f, 1.0f);
		}

		template<typename SampleType, int BIT_DEPTH>
		SampleType FloatToSignedInteger(float value)
		{
			static_assert(std::is_signed_v<SampleType>, "SampleType must be signed");
			constexpr std::int64_t magnitude = std::int64_t{ 1 } << (BIT_DEPTH - 1);
			const float clamped = ClampSample(value);
			if (clamped <= -1.0f) return static_cast<SampleType>(-magnitude);
			if (clamped >= 1.0f) return static_cast<SampleType>(magnitude - 1);
			return static_cast<SampleType>(clamped * static_cast<float>(magnitude));
		}

		std::int32_t ReadInt24(const std::uint8_t* sample)
		{
			const std::uint32_t packed = static_cast<std::uint32_t>(sample[0])
				| (static_cast<std::uint32_t>(sample[1]) << 8)
				| (static_cast<std::uint32_t>(sample[2]) << 16);
			return (packed & 0x00800000u) != 0
				? static_cast<std::int32_t>(packed) - 0x01000000
				: static_cast<std::int32_t>(packed);
		}

		void WriteInt24(std::int32_t value, std::uint8_t* sample)
		{
			const std::uint32_t packed = static_cast<std::uint32_t>(value);
			sample[0] = static_cast<std::uint8_t>(packed);
			sample[1] = static_cast<std::uint8_t>(packed >> 8);
			sample[2] = static_cast<std::uint8_t>(packed >> 16);
		}

		SampleFormat IntegerFormat(WORD bitsPerSample)
		{
			switch (bitsPerSample)
			{
			case 32: return SampleFormat::Int32;
			case 24: return SampleFormat::Int24;
			case 16: return SampleFormat::Int16;
			default: return SampleFormat::Unsupported;
			}
		}
	}

	PacketFormat ReadPacketFormat(const WAVEFORMATEXTENSIBLE& waveFormat)
	{
		const WAVEFORMATEX& baseFormat = waveFormat.Format;
		PacketFormat format;
		format.sampleRate = baseFormat.nSamplesPerSec;
		format.channelCount = baseFormat.nChannels;

		if (baseFormat.wFormatTag == WAVE_FORMAT_EXTENSIBLE)
		{
			if (baseFormat.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) return {};
			if (IsEqualGUID(waveFormat.SubFormat, FLOAT_SUBFORMAT) && baseFormat.wBitsPerSample == 32)
				format.sampleFormat = SampleFormat::Float32;
			else if (IsEqualGUID(waveFormat.SubFormat, PCM_SUBFORMAT))
				format.sampleFormat = IntegerFormat(baseFormat.wBitsPerSample);
		}
		else if (baseFormat.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && baseFormat.wBitsPerSample == 32)
			format.sampleFormat = SampleFormat::Float32;
		else if (baseFormat.wFormatTag == WAVE_FORMAT_PCM)
			format.sampleFormat = IntegerFormat(baseFormat.wBitsPerSample);

		const std::uint32_t bytesPerSample = baseFormat.wBitsPerSample / 8u;
		if (!format.IsUsable() || bytesPerSample == 0 || baseFormat.nBlockAlign != format.channelCount * bytesPerSample)
			return {};
		return format;
	}

	std::string DescribePacketFormat(const PacketFormat& format)
	{
		const char* name = "unsupported";
		switch (format.sampleFormat)
		{
		case SampleFormat::Float32: name = "float32"; break;
		case SampleFormat::Int32: name = "int32"; break;
		case SampleFormat::Int24: name = "int24"; break;
		case SampleFormat::Int16: name = "int16"; break;
		case SampleFormat::Unsupported: break;
		}
		return std::string(name) + ", " + std::to_string(format.sampleRate) + " Hz, "
			+ std::to_string(format.channelCount) + " channel(s)";
	}

	void ReadFirstChannel(const BYTE* packet, const PacketFormat& format, std::uint32_t frames, float* output)
	{
		const std::size_t stride = format.channelCount;
		for (std::uint32_t frame = 0; frame < frames; ++frame)
		{
			const std::size_t index = static_cast<std::size_t>(frame) * stride;
			switch (format.sampleFormat)
			{
			case SampleFormat::Float32: output[frame] = reinterpret_cast<const float*>(packet)[index]; break;
			case SampleFormat::Int32: output[frame] = static_cast<float>(reinterpret_cast<const std::int32_t*>(packet)[index]) * INT32_TO_FLOAT; break;
			case SampleFormat::Int24: output[frame] = static_cast<float>(ReadInt24(packet + index * 3)) * INT24_TO_FLOAT; break;
			case SampleFormat::Int16: output[frame] = static_cast<float>(reinterpret_cast<const std::int16_t*>(packet)[index]) * INT16_TO_FLOAT; break;
			case SampleFormat::Unsupported: output[frame] = 0.0f; break;
			}
		}
	}

	void WriteAllChannels(BYTE* packet, const PacketFormat& format, std::uint32_t frames, const float* samples)
	{
		const std::size_t stride = format.channelCount;
		for (std::uint32_t frame = 0; frame < frames; ++frame)
		{
			const std::size_t first = static_cast<std::size_t>(frame) * stride;
			for (std::size_t channel = 0; channel < stride; ++channel)
			{
				const std::size_t index = first + channel;
				switch (format.sampleFormat)
				{
				case SampleFormat::Float32: reinterpret_cast<float*>(packet)[index] = ClampSample(samples[frame]); break;
				case SampleFormat::Int32: reinterpret_cast<std::int32_t*>(packet)[index] = FloatToSignedInteger<std::int32_t, 32>(samples[frame]); break;
				case SampleFormat::Int24: WriteInt24(FloatToSignedInteger<std::int32_t, 24>(samples[frame]), packet + index * 3); break;
				case SampleFormat::Int16: reinterpret_cast<std::int16_t*>(packet)[index] = FloatToSignedInteger<std::int16_t, 16>(samples[frame]); break;
				case SampleFormat::Unsupported: break;
				}
			}
		}
	}
}
