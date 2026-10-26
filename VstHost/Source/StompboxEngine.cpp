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
			error = std::string(DLL_NAME, DLL_NAME + wcslen(DLL_NAME)) + " has no " + name + " (a different Stompbox build?).";
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
			error = "The Stompbox engine (stompbox-capi.dll) is not next to the Tone designer host (error " + std::to_string(GetLastError()) + ").";
			Log("stompbox: %ls not loaded (%lu)", file.c_str(), GetLastError());
			return false;
		}
		const bool resolved = Resolve(module, "CreateProcessor", createProcessor, error)
			&& Resolve(module, "DeleteProcessor", deleteProcessor, error)
			&& Resolve(module, "InitProcessor", initProcessor, error)
			&& Resolve(module, "SetMaxAudioBufferSize", setMaxAudioBufferSize, error)
			&& Resolve(module, "StartServer", startServer, error)
			&& Resolve(module, "Process", process, error)
			&& Resolve(module, "HandleCommand", handleCommand, error)
			&& Resolve(module, "GetStringVectorSize", getStringVectorSize, error)
			&& Resolve(module, "GetStringVectorValue", getStringVectorValue, error)
			&& Resolve(module, "GetAllPlugins", getAllPlugins, error)
			&& Resolve(module, "GetPresets", getPresets, error)
			&& Resolve(module, "CreatePlugin", createPlugin, error)
			&& Resolve(module, "DumpProgram", dumpProgram, error)
			&& Resolve(module, "GetPluginDescription", getPluginDescription, error)
			&& Resolve(module, "GetPluginBackgroundColor", getPluginBackgroundColor, error)
			&& Resolve(module, "GetPluginForegroundColor", getPluginForegroundColor, error)
			&& Resolve(module, "GetPluginIsUserSelectable", getPluginIsUserSelectable, error)
			&& Resolve(module, "GetPluginNumParameters", getPluginNumParameters, error)
			&& Resolve(module, "GetPluginParameter", getPluginParameter, error)
			&& Resolve(module, "GetParameterName", getParameterName, error)
			&& Resolve(module, "GetParameterDescription", getParameterDescription, error)
			&& Resolve(module, "GetParameterDisplayFormat", getParameterDisplayFormat, error)
			&& Resolve(module, "GetParameterMinValue", getParameterMinValue, error)
			&& Resolve(module, "GetParameterMaxValue", getParameterMaxValue, error)
			&& Resolve(module, "GetParameterDefaultValue", getParameterDefaultValue, error)
			&& Resolve(module, "GetParameterRangePower", getParameterRangePower, error)
			&& Resolve(module, "GetParameterType", getParameterType, error)
			&& Resolve(module, "GetParameterEnumValues", getParameterEnumValues, error)
			&& Resolve(module, "GetParameterIsAdvanced", getParameterIsAdvanced, error)
			&& Resolve(module, "GetParameterIsOutput", getParameterIsOutput, error);
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

	void StompboxEngine::Prepare(int frameCount)
	{
		if (!processor) throw std::logic_error("StompboxEngine::Prepare before Load.");
		frames = frameCount;
		input.assign(static_cast<size_t>(frames), 0.0f);
		setMaxAudioBufferSize(processor, static_cast<size_t>(frames));
	}

	void StompboxEngine::SendCommand(const std::string& command)
	{
		if (!processor) throw std::logic_error("StompboxEngine::SendCommand before Load.");
		handleCommand(processor, command.c_str());
	}

	std::vector<std::string> StompboxEngine::ReadStrings(void* strings) const
	{
		std::vector<std::string> values;
		if (!strings) return values;
		const size_t count = getStringVectorSize(strings);
		values.reserve(count);
		for (size_t i = 0; i < count; ++i)
		{
			const char* value = getStringVectorValue(strings, i);
			values.emplace_back(value ? value : "");
		}
		return values;
	}

	StompParameterInfo StompboxEngine::ReadParameter(void* parameter) const
	{
		const auto text = [](const char* value) { return std::string(value ? value : ""); };
		StompParameterInfo info;
		info.name = text(getParameterName(parameter));
		info.description = text(getParameterDescription(parameter));
		info.displayFormat = text(getParameterDisplayFormat(parameter));
		const int type = getParameterType(parameter);
		if (type < 0 || type > static_cast<int>(StompParameterType::Power))
			throw std::runtime_error("Stompbox reported an unknown parameter type " + std::to_string(type) + " for " + info.name + ".");
		info.type = static_cast<StompParameterType>(type);
		info.minimum = getParameterMinValue(parameter);
		info.maximum = getParameterMaxValue(parameter);
		info.defaultValue = getParameterDefaultValue(parameter);
		info.rangePower = getParameterRangePower(parameter);
		info.enumValues = ReadStrings(getParameterEnumValues(parameter));
		info.isAdvanced = getParameterIsAdvanced(parameter);
		info.isOutput = getParameterIsOutput(parameter);
		return info;
	}

	std::vector<StompPluginInfo> StompboxEngine::ListPlugins()
	{
		if (!processor) throw std::logic_error("StompboxEngine::ListPlugins before Load.");
		const auto text = [](const char* value) { return std::string(value ? value : ""); };
		std::vector<StompPluginInfo> plugins;
		for (const std::string& name : ReadStrings(getAllPlugins(processor)))
		{
			// The factory instance named after the plugin itself (Stompbox's own DumpConfig does the same).
			void* plugin = createPlugin(processor, name.c_str());
			if (!plugin) throw std::runtime_error("Stompbox listed " + name + " but could not create it.");
			if (!getPluginIsUserSelectable(plugin)) continue;
			StompPluginInfo info;
			info.name = name;
			info.description = text(getPluginDescription(plugin));
			info.backgroundColor = text(getPluginBackgroundColor(plugin));
			info.foregroundColor = text(getPluginForegroundColor(plugin));
			const size_t count = getPluginNumParameters(plugin);
			for (size_t i = 0; i < count; ++i) info.parameters.push_back(ReadParameter(getPluginParameter(plugin, i)));
			plugins.push_back(std::move(info));
		}
		return plugins;
	}

	void StompboxEngine::SetChain(const std::string& chainName, const std::vector<std::string>& pluginIds)
	{
		if (chainName.empty() || chainName.find(' ') != std::string::npos) throw std::invalid_argument("Stompbox chain names are one word.");
		std::string command = "SetChain " + chainName;
		for (const std::string& id : pluginIds)
		{
			if (id.empty() || id.find(' ') != std::string::npos) throw std::invalid_argument("Stompbox plugin ids are one word: '" + id + "'.");
			command += " " + id;
		}
		SendCommand(command);
	}

	std::string StompboxEngine::DumpProgram()
	{
		if (!processor) throw std::logic_error("StompboxEngine::DumpProgram before Load.");
		const char* program = dumpProgram(processor);
		if (!program) return {};
		std::string text(program);
		CoTaskMemFree(const_cast<char*>(program));   // Stompbox allocates it with CoTaskMemAlloc
		return text;
	}

	std::vector<std::string> StompboxEngine::ListPresets()
	{
		if (!processor) throw std::logic_error("StompboxEngine::ListPresets before Load.");
		return ReadStrings(getPresets(processor));
	}

	void StompboxEngine::StartRemoteServer()
	{
		if (!processor) throw std::logic_error("StompboxEngine::StartRemoteServer before Load.");
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
		std::memcpy(input.data(), guitar, sizeof(float) * static_cast<size_t>(frameCount));
		RaisePeak(inputPeak, Peak(input.data(), frameCount));
		process(processor, input.data(), left, static_cast<size_t>(frameCount));
		std::memcpy(right, left, sizeof(float) * static_cast<size_t>(frameCount));
		RaisePeak(outputPeak, Peak(left, frameCount));
	}

	void StompboxEngine::Unload()
	{
		if (processor && deleteProcessor) deleteProcessor(processor);
		processor = nullptr;
		if (module) FreeLibrary(module);
		module = nullptr;
		isServerStarted = false;
	}
}
