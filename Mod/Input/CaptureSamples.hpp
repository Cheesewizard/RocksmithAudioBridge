#pragma once

#include <Windows.h>
#include <mmreg.h>

#include <cstdint>
#include <string>

namespace AudioBridge::Input
{
	enum class SampleFormat
	{
		Unsupported,
		Float32,
		Int32,
		Int24,
		Int16
	};

	// The interleaved format of the game's capture packets.
	struct PacketFormat
	{
		SampleFormat sampleFormat = SampleFormat::Unsupported;
		std::uint32_t sampleRate = 0;
		std::uint32_t channelCount = 0;

		bool IsUsable() const
		{
			return sampleFormat != SampleFormat::Unsupported && sampleRate > 0 && channelCount > 0;
		}
	};

	// Unsupported when the format isn't PCM 16/24/32 or float 32, or its block alignment doesn't match.
	PacketFormat ReadPacketFormat(const WAVEFORMATEXTENSIBLE& waveFormat);
	std::string DescribePacketFormat(const PacketFormat& format);

	// The guitar is on the first channel. Reads it as float in [-1, 1].
	void ReadFirstChannel(const BYTE* packet, const PacketFormat& format, std::uint32_t frames, float* output);
	// Writes the (clamped) samples to every channel of the packet.
	void WriteAllChannels(BYTE* packet, const PacketFormat& format, std::uint32_t frames, const float* samples);
}
