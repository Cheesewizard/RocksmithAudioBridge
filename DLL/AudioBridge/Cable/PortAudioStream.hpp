#pragma once

namespace Audio::CableInput
{
	struct PaStreamParameters
	{
		int device;
		int channelCount;
		unsigned long sampleFormat;
		double suggestedLatency;
		void* hostApiSpecificStreamInfo;
	};

	struct PaWasapiStreamInfo
	{
		unsigned long size;
		int hostApiType;
		unsigned long version;
		unsigned long flags;
		unsigned long channelMask;
		void* hostProcessorOutput;
		void* hostProcessorInput;
	};

	inline void ConfigureGameStreamTiming(PaStreamParameters& parameters, PaWasapiStreamInfo& info)
	{
		constexpr unsigned long exclusive = 1;
		constexpr unsigned long polling = 8;
		// PortAudio changes exclusive callbacks to polling when the requested latency exceeds 21.33 ms.
		// These virtual endpoints own their buffering; the physical device's default latency is irrelevant.
		parameters.suggestedLatency = 0.0;
		info.flags = (info.flags | exclusive) & ~polling;
	}
}
