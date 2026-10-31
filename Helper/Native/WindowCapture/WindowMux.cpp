#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <winrt/base.h>

using namespace winrt;

namespace
{
	constexpr UINT32 AudioBitsPerSecond = 192000 / 8;

	struct Track
	{
		com_ptr<IMFSourceReader> reader;
		com_ptr<IMFSample> pending;
		DWORD sourceStream = 0;
		DWORD sinkStream = 0;
		LONGLONG offset = 0;
		bool drained = false;
	};

	HRESULT OpenVideo(const wchar_t* path, Track& track, com_ptr<IMFMediaType>& type)
	{
		HRESULT result = MFCreateSourceReaderFromURL(path, nullptr, track.reader.put());
		if (FAILED(result))
			return result;
		track.sourceStream = MF_SOURCE_READER_FIRST_VIDEO_STREAM;
		track.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		result = track.reader->SetStreamSelection(track.sourceStream, TRUE);
		if (FAILED(result))
			return result;
		return track.reader->GetCurrentMediaType(track.sourceStream, type.put());
	}

	HRESULT OpenAudio(const wchar_t* path, Track& track, com_ptr<IMFMediaType>& type)
	{
		HRESULT result = MFCreateSourceReaderFromURL(path, nullptr, track.reader.put());
		if (FAILED(result))
			return result;
		track.sourceStream = MF_SOURCE_READER_FIRST_AUDIO_STREAM;
		track.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
		result = track.reader->SetStreamSelection(track.sourceStream, TRUE);
		if (FAILED(result))
			return result;
		com_ptr<IMFMediaType> pcm;
		result = MFCreateMediaType(pcm.put());
		if (FAILED(result))
			return result;
		pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		result = track.reader->SetCurrentMediaType(track.sourceStream, nullptr, pcm.get());
		if (FAILED(result))
			return result;
		return track.reader->GetCurrentMediaType(track.sourceStream, type.put());
	}

	HRESULT AddAudioStream(IMFSinkWriter* writer, IMFMediaType* pcm, DWORD* stream)
	{
		UINT32 channels = 0, samplesPerSecond = 0, bitsPerSample = 0;
		pcm->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
		pcm->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &samplesPerSecond);
		pcm->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bitsPerSample);
		com_ptr<IMFMediaType> aac;
		HRESULT result = MFCreateMediaType(aac.put());
		if (FAILED(result))
			return result;
		aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
		aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels == 0 ? 2 : channels);
		aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, samplesPerSecond == 0 ? 48000 : samplesPerSecond);
		aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample == 0 ? 16 : bitsPerSample);
		aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, AudioBitsPerSecond);
		aac->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
		result = writer->AddStream(aac.get(), stream);
		if (FAILED(result))
			return result;
		return writer->SetInputMediaType(*stream, pcm, nullptr);
	}

	HRESULT NextSample(Track& track)
	{
		if (track.pending != nullptr || track.drained)
			return S_OK;
		DWORD flags = 0;
		com_ptr<IMFSample> sample;
		HRESULT result = track.reader->ReadSample(track.sourceStream, 0, nullptr, &flags, nullptr, sample.put());
		if (FAILED(result))
			return result;
		if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 || sample == nullptr)
		{
			track.drained = true;
			return S_OK;
		}
		track.pending = sample;
		return S_OK;
	}

	LONGLONG TimeOf(const Track& track)
	{
		LONGLONG time = 0;
		if (track.pending != nullptr)
			track.pending->GetSampleTime(&time);
		return time + track.offset;
	}

	HRESULT WritePending(IMFSinkWriter* writer, Track& track)
	{
		LONGLONG time = 0;
		track.pending->GetSampleTime(&time);
		track.pending->SetSampleTime(time + track.offset);
		HRESULT result = writer->WriteSample(track.sinkStream, track.pending.get());
		track.pending = nullptr;
		return result;
	}
}

extern "C"
{
	__declspec(dllexport) HRESULT RsCaptureMux(const wchar_t* videoPath, const wchar_t* audioPath,
		const wchar_t* outputPath, INT64 audioOffset)
	{
		if (videoPath == nullptr || audioPath == nullptr || outputPath == nullptr)
			return E_INVALIDARG;
		HRESULT result = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		bool uninitialize = SUCCEEDED(result);
		if (result == RPC_E_CHANGED_MODE)
			result = S_OK;
		if (FAILED(result))
			return result;
		result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		if (FAILED(result))
		{
			if (uninitialize) ::CoUninitialize();
			return result;
		}
		{
			Track video;
			Track audio;
			com_ptr<IMFMediaType> videoType;
			com_ptr<IMFMediaType> audioType;
			com_ptr<IMFSinkWriter> writer;
			result = OpenVideo(videoPath, video, videoType);
			if (SUCCEEDED(result))
				result = OpenAudio(audioPath, audio, audioType);
			if (SUCCEEDED(result))
				result = MFCreateSinkWriterFromURL(outputPath, nullptr, nullptr, writer.put());
			if (SUCCEEDED(result))
				result = writer->AddStream(videoType.get(), &video.sinkStream);
			if (SUCCEEDED(result))
				result = writer->SetInputMediaType(video.sinkStream, videoType.get(), nullptr);
			if (SUCCEEDED(result))
				result = AddAudioStream(writer.get(), audioType.get(), &audio.sinkStream);
			if (SUCCEEDED(result))
			{
				video.offset = audioOffset < 0 ? -audioOffset : 0;
				audio.offset = audioOffset > 0 ? audioOffset : 0;
				result = writer->BeginWriting();
			}
			while (SUCCEEDED(result))
			{
				result = NextSample(video);
				if (SUCCEEDED(result))
					result = NextSample(audio);
				if (FAILED(result) || (video.drained && audio.drained))
					break;
				if (video.pending == nullptr)
					result = WritePending(writer.get(), audio);
				else if (audio.pending == nullptr)
					result = WritePending(writer.get(), video);
				else
					result = TimeOf(video) <= TimeOf(audio)
						? WritePending(writer.get(), video)
						: WritePending(writer.get(), audio);
			}
			if (writer != nullptr)
			{
				HRESULT finalized = writer->Finalize();
				if (SUCCEEDED(result))
					result = finalized;
			}
		}
		MFShutdown();
		if (uninitialize) ::CoUninitialize();
		return result;
	}
}
