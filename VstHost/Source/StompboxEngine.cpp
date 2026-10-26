#include "StompboxEngine.hpp"

#include "AudioFormat.hpp"
#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace VstHost
{
	namespace
	{
		constexpr wchar_t DLL_NAME[] = L"stompbox-capi.dll";

		float Peak(const float* samples, int count)
		{
			float peak = 0.0f;
			for (int i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
			return peak;
		}

		void RaisePeak(std::atomic<float>& peak, float value)
		{
			float current = peak.load(std::memory_order_relaxed);
			while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
		}

		std::wstring HostFolder()
		{
			wchar_t path[MAX_PATH]{};
			const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
			std::wstring folder(path, length);
			const size_t slash = folder.find_last_of(L"\\/");
			return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash + 1);
		}

		template <typename T>
		bool Resolve(HMODULE module, const char* name, T& function, std::string& error)
		{
			function = reinterpret_cast<T>(GetProcAddress(module, name));
			if (function) return true;
			error = std::string("stompbox-capi.dll has no ") + name + " (not built by VstHost\\Stompbox\\Build-Stompbox.ps1?).";
			return false;
		}
	}

	StompboxEngine::~StompboxEngine() { Unload(); }

	bool StompboxEngine::Load(const std::wstring& dataPath, std::string& error)
	{
		if (processor) return true;
		const std::wstring file = HostFolder() + DLL_NAME;
		module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module)
		{
			const DWORD code = GetLastError();
			error = "The Stompbox engine (stompbox-capi.dll) is not next to the Tone designer host (error " + std::to_string(code) + ").";
			Log("stompbox: %ls not loaded (%lu)", file.c_str(), code);
			return false;
		}
		const bool resolved = Resolve(module, "CreateProcessor", createProcessor, error)
			&& Resolve(module, "DeleteProcessor", deleteProcessor, error)
			&& Resolve(module, "InitProcessor", initProcessor, error)
			&& Resolve(module, "SetMaxAudioBufferSize", setMaxAudioBufferSize, error)
			&& Resolve(module, "StartServer", startServer, error)
			&& Resolve(module, "Process", process, error)
			&& Resolve(module, "HandleCommand", handleCommand, error)
			&& Resolve(module, "DumpConfig", dumpConfig, error)
			&& Resolve(module, "DumpProgram", dumpProgram, error)
			&& Resolve(module, "GetCurrentPreset", getCurrentPreset, error)
			&& Resolve(module, "IsPresetLoading", isPresetLoading, error)
			&& Resolve(module, "GetPresets", getPresets, error)
			&& Resolve(module, "GetStringVectorSize", getStringVectorSize, error)
			&& Resolve(module, "GetStringVectorValue", getStringVectorValue, error);
		if (!resolved)
		{
			Log("stompbox: %s", error.c_str());
			Unload();
			return false;
		}
		CreateDirectoryW(dataPath.c_str(), nullptr);
		processor = createProcessor(dataPath.c_str());
		if (!processor)
		{
			error = "The Stompbox engine did not start.";
			Log("stompbox: CreateProcessor failed for %ls", dataPath.c_str());
			Unload();
			return false;
		}
		initProcessor(processor, static_cast<float>(SAMPLE_RATE));
		Log("stompbox: loaded %ls, data in %ls", file.c_str(), dataPath.c_str());
		return true;
	}

	void StompboxEngine::Unload()
	{
		if (processor && deleteProcessor) deleteProcessor(processor);
		processor = nullptr;
		if (module) FreeLibrary(module);
		module = nullptr;
		isServerStarted = false;
	}

	void StompboxEngine::RequireLoaded(const char* operation) const
	{
		if (!processor) throw std::logic_error(std::string("StompboxEngine::") + operation + " before Load.");
	}

	std::string StompboxEngine::TakeAllocatedText(const char* text) const
	{
		if (!text) return {};
		std::string copy(text);
		CoTaskMemFree(const_cast<char*>(text));
		return copy;
	}

	void StompboxEngine::Prepare(int frameCount)
	{
		RequireLoaded("Prepare");
		frames = frameCount;
		input.assign(static_cast<size_t>(frames), 0.0f);
		pumpOutput.assign(static_cast<size_t>(frames), 0.0f);
		setMaxAudioBufferSize(processor, static_cast<size_t>(frames));
	}

	void StompboxEngine::Pump(int blocks)
	{
		RequireLoaded("Pump");
		if (frames <= 0) throw std::logic_error("StompboxEngine::Pump before Prepare.");
		for (int block = 0; block < blocks; ++block)
		{
			std::fill(input.begin(), input.end(), 0.0f);
			process(processor, input.data(), pumpOutput.data(), static_cast<size_t>(frames));
		}
	}

	void StompboxEngine::SendCommand(const std::string& command)
	{
		RequireLoaded("SendCommand");
		handleCommand(processor, command.c_str());
	}

	std::string StompboxEngine::DumpConfig()
	{
		RequireLoaded("DumpConfig");
		return TakeAllocatedText(dumpConfig(processor));
	}

	std::string StompboxEngine::DumpProgram()
	{
		RequireLoaded("DumpProgram");
		return TakeAllocatedText(dumpProgram(processor));
	}

	std::vector<std::string> StompboxEngine::ListPresets()
	{
		RequireLoaded("ListPresets");
		void* presets = getPresets(processor);
		std::vector<std::string> names;
		if (!presets) return names;
		const size_t count = getStringVectorSize(presets);
		for (size_t i = 0; i < count; ++i)
		{
			const char* name = getStringVectorValue(presets, i);
			names.emplace_back(name ? name : "");
		}
		return names;
	}

	std::string StompboxEngine::CurrentPreset()
	{
		RequireLoaded("CurrentPreset");
		const char* preset = getCurrentPreset(processor);   // Stompbox's own string: not freed
		return preset ? preset : "";
	}

	bool StompboxEngine::IsPresetLoading()
	{
		RequireLoaded("IsPresetLoading");
		return isPresetLoading(processor);
	}

	void StompboxEngine::StartRemoteServer()
	{
		RequireLoaded("StartRemoteServer");
		if (isServerStarted) return;
		startServer(processor);
		isServerStarted = true;
		Log("stompbox: remote server started on 127.0.0.1:24639");
	}

	void StompboxEngine::Process(const float* guitar, float* left, float* right, int frameCount)
	{
		if (!processor || frameCount != frames)
		{
			std::fill(left, left + frameCount, 0.0f);
			std::fill(right, right + frameCount, 0.0f);
			return;
		}
		const float gain = InputGain();
		for (int i = 0; i < frameCount; ++i) input[i] = guitar[i] * gain;
		RaisePeak(inputPeak, Peak(input.data(), frameCount));
		process(processor, input.data(), left, static_cast<size_t>(frameCount));
		std::memcpy(right, left, sizeof(float) * static_cast<size_t>(frameCount));
		RaisePeak(outputPeak, Peak(left, frameCount));
	}
}
