#pragma once

// Minimal ASIO interface + types, self-declared to avoid the Steinberg SDK dependency (same approach
// RS_ASIO takes). Only what the proxy needs to forward every call to the real driver and tap output.
#include <Windows.h>

typedef long ASIOBool;
typedef long ASIOError;
typedef double ASIOSampleRate;
typedef long ASIOSampleType;

struct ASIOTimeStamp { long hi; unsigned long lo; };
struct ASIOSamples { long hi; unsigned long lo; };

struct ASIOClockSource
{
	long index;
	long associatedChannel;
	long associatedGroup;
	ASIOBool isCurrentSource;
	char name[32];
};

struct ASIOChannelInfo
{
	long channel;
	ASIOBool isInput;
	ASIOBool isActive;
	long channelGroup;
	ASIOSampleType type;
	char name[32];
};

struct ASIOBufferInfo
{
	ASIOBool isInput;
	long channelNum;
	void* buffers[2];   // filled by createBuffers: the two double-buffers for this channel
};

struct AsioInputChannel
{
	void* buffer;
	long channelNum;
	ASIOSampleType type;
};

using AsioInputObserver = void(__cdecl*)(const AsioInputChannel* channels, long channelCount,
	long frames, double sampleRate);

struct ASIOTime;   // opaque; forwarded untouched

struct ASIOCallbacks
{
	void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
	void (*sampleRateDidChange)(ASIOSampleRate sRate);
	long (*asioMessage)(long selector, long value, void* message, double* opt);
	ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
};

// ASIO drivers use their CLSID as the interface IID and are laid out as a COM vtable.
struct IAsioDriver : public IUnknown
{
	virtual ASIOBool init(void* sysHandle) = 0;
	virtual void getDriverName(char* name) = 0;
	virtual long getDriverVersion() = 0;
	virtual void getErrorMessage(char* string) = 0;
	virtual ASIOError start() = 0;
	virtual ASIOError stop() = 0;
	virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
	virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) = 0;
	virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
	virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) = 0;
	virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) = 0;
	virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) = 0;
	virtual ASIOError setClockSource(long reference) = 0;
	virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
	virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
	virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
	virtual ASIOError disposeBuffers() = 0;
	virtual ASIOError controlPanel() = 0;
	virtual ASIOError future(long selector, void* opt) = 0;
	virtual ASIOError outputReady() = 0;
};

// ASIO sample type ids we convert for the output tap.
enum
{
	ASIOSTInt16MSB = 0, ASIOSTInt24MSB = 1, ASIOSTInt32MSB = 2, ASIOSTFloat32MSB = 3, ASIOSTFloat64MSB = 4,
	ASIOSTInt32MSB16 = 8, ASIOSTInt32MSB18 = 9, ASIOSTInt32MSB20 = 10, ASIOSTInt32MSB24 = 11,
	ASIOSTInt16LSB = 16, ASIOSTInt24LSB = 17, ASIOSTInt32LSB = 18, ASIOSTFloat32LSB = 19, ASIOSTFloat64LSB = 20,
	ASIOSTInt32LSB16 = 24, ASIOSTInt32LSB18 = 25, ASIOSTInt32LSB20 = 26, ASIOSTInt32LSB24 = 27
};
