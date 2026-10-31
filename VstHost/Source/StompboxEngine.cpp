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
		constexpr DWORD CPP_EXCEPTION = 0xE06D7363;   // MSVC's SEH code for a C++ throw
		constexpr char CRASHED[] = "The Tone designer engine crashed, so the guitar plays without it. Press Rescan to start it again.";

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

		// Runs `call`, catching a crash (an access violation and the like) but letting C++ exceptions through to
		// the caller's catch. No C++ objects in here: SEH cannot share a function with object unwinding.
		template <typename Call>
		bool RunCatchingCrash(Call& call, DWORD& code)
		{
			__try
			{
				call();
				return true;
			}
			__except (code = GetExceptionCode(), code == CPP_EXCEPTION ? EXCEPTION_CONTINUE_SEARCH : EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// The stream thread's guard: anything Stompbox throws or crashes with stops here, so nothing leaves the audio
		// callback. Allocates nothing.
		bool ProcessCatchingAll(void (*process)(void*, float*, float*, size_t), void* processor, float* input, float* output, size_t frames)
		{
			__try
			{
				process(processor, input, output, frames);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		std::string LoadFailure(const std::wstring& file, DWORD code)
		{
			const std::string reinstall = " Reinstall Rocksmith Audio Bridge to restore it; the VST rig still works.";
			if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
				return "The Tone designer engine (stompbox-capi.dll) is missing from the host's folder." + reinstall;
			if (code == ERROR_BAD_EXE_FORMAT)
				return "The Tone designer engine (stompbox-capi.dll) is damaged or is not the 64-bit build." + reinstall;
			if (code == ERROR_MOD_NOT_FOUND)
				return "The Tone designer engine (stompbox-capi.dll) is missing a part it needs (error 126)." + reinstall;
			return "The Tone designer engine (stompbox-capi.dll) could not be loaded (error " + std::to_string(code) + ")." + reinstall;
		}
	}

	StompboxEngine::~StompboxEngine() { Unload(); }

	template <typename Call>
	bool StompboxEngine::Guard(const char* operation, Call&& call)
	{
		DWORD code = 0;
		try
		{
			if (RunCatchingCrash(call, code)) return true;
		}
		catch (const std::exception& exception)
		{
			lastError = std::string("Stompbox stopped on ") + operation + ": " + exception.what();
			Log("stompbox: %s threw: %s", operation, exception.what());
			return false;
		}
		catch (...)
		{
			lastError = std::string("Stompbox stopped on ") + operation + ".";
			Log("stompbox: %s threw an unknown exception", operation);
			return false;
		}
		// A crash leaves Stompbox's state unknown: stop using this processor (Process plays the guitar dry).
		faulted.store(true, std::memory_order_release);
		lastError = CRASHED;
		Log("stompbox: %s crashed (0x%08lX); the engine plays the guitar dry until it is replaced", operation, code);
		return false;
	}

	bool StompboxEngine::Load(const std::wstring& dataPath, std::string& error)
	{
		if (processor) return true;
		const std::wstring file = HostFolder() + DLL_NAME;
		module = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!module)
		{
			const DWORD code = GetLastError();
			error = LoadFailure(file, code);
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
			error = "The Tone designer engine (stompbox-capi.dll) is from a different build. Reinstall Rocksmith Audio Bridge; the VST rig still works.";
			Unload();
			return false;
		}
		CreateDirectoryW(dataPath.c_str(), nullptr);
		void* created = nullptr;
		faulted.store(false, std::memory_order_relaxed);
		const bool started = Guard("CreateProcessor", [&] { created = createProcessor(dataPath.c_str()); })
			&& created && Guard("InitProcessor", [&] { initProcessor(created, static_cast<float>(SAMPLE_RATE)); });
		if (!started)
		{
			error = "The Tone designer engine did not start on its data folder. The VST rig still works.";
			Log("stompbox: the processor did not start for %ls (%s)", dataPath.c_str(), lastError.c_str());
			if (created) Release(created, true);   // half made: never deleted
			faulted.store(false, std::memory_order_relaxed);
			Unload();
			return false;
		}
		processor = created;
		active.store(created, std::memory_order_release);
		Log("stompbox: loaded %ls, data in %ls", file.c_str(), dataPath.c_str());
		return true;
	}

	void StompboxEngine::Release(void* old, bool hasFaulted)
	{
		if (!old) return;
		DWORD code = 0;
		auto destroy = [&] { deleteProcessor(old); };
		bool deleted = false;
		if (!hasFaulted && deleteProcessor)
		{
			try { deleted = RunCatchingCrash(destroy, code); }
			catch (...) { deleted = false; }
		}
		if (deleted) return;
		// Kept, with its threads, for the life of the host: the DLL must then never unload under it.
		HMODULE pinned = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(process), &pinned);
		Log("stompbox: a %s Stompbox processor was kept, not deleted (0x%08lX)", hasFaulted ? "crashed" : "broken", code);
	}

	void StompboxEngine::Unload()
	{
		Release(incoming.exchange(nullptr), false);
		Release(retiring, retiringFaulted.load());
		retiring = nullptr;
		Release(processor, faulted.load());
		processor = nullptr;
		active.store(nullptr, std::memory_order_release);
		faulted.store(false, std::memory_order_relaxed);
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
		if (IsAdopting()) FinishAdopt(true);   // the stream is stopped: switch now
		frames = frameCount;
		const size_t size = static_cast<size_t>(frames);
		input.assign(size, 0.0f);
		incomingInput.assign(size, 0.0f);
		incomingOutput.assign(size, 0.0f);
		pumpInput.assign(size, 0.0f);
		pumpOutput.assign(size, 0.0f);
		if (!HasFaulted()) Guard("SetMaxAudioBufferSize", [&] { setMaxAudioBufferSize(processor, size); });
	}

	void StompboxEngine::Pump(int blocks)
	{
		RequireLoaded("Pump");
		if (frames <= 0) throw std::logic_error("StompboxEngine::Pump before Prepare.");
		for (int block = 0; block < blocks && !HasFaulted(); ++block)
		{
			std::fill(pumpInput.begin(), pumpInput.end(), 0.0f);
			if (!ProcessCatchingAll(process, processor, pumpInput.data(), pumpOutput.data(), static_cast<size_t>(frames)))
			{
				faulted.store(true, std::memory_order_release);
				lastError = CRASHED;
				Log("stompbox: processing crashed while a tone was being prepared");
			}
		}
	}

	bool StompboxEngine::Render(float* in, float* out)
	{
		RequireLoaded("Render");
		if (frames <= 0) throw std::logic_error("StompboxEngine::Render before Prepare.");
		if (HasFaulted()) return false;
		if (ProcessCatchingAll(process, processor, in, out, static_cast<size_t>(frames))) return true;
		faulted.store(true, std::memory_order_release);
		Log("stompbox: processing crashed on a test signal");
		return false;
	}

	bool StompboxEngine::SendCommand(const std::string& command)
	{
		RequireLoaded("SendCommand");
		if (HasFaulted())
		{
			lastError = CRASHED;
			return false;
		}
		lastError.clear();
		return Guard("a command", [&] { handleCommand(processor, command.c_str()); });
	}

	std::string StompboxEngine::DumpConfig()
	{
		RequireLoaded("DumpConfig");
		std::string text;
		if (!HasFaulted()) Guard("DumpConfig", [&] { text = TakeAllocatedText(dumpConfig(processor)); });
		return text;
	}

	std::string StompboxEngine::DumpProgram()
	{
		RequireLoaded("DumpProgram");
		std::string text;
		if (!HasFaulted()) Guard("DumpProgram", [&] { text = TakeAllocatedText(dumpProgram(processor)); });
		return text;
	}

	std::vector<std::string> StompboxEngine::ListPresets()
	{
		RequireLoaded("ListPresets");
		std::vector<std::string> names;
		if (HasFaulted()) return names;
		Guard("GetPresets", [&]
		{
			void* presets = getPresets(processor);
			if (!presets) return;
			const size_t count = getStringVectorSize(presets);
			for (size_t i = 0; i < count; ++i)
			{
				const char* name = getStringVectorValue(presets, i);
				names.emplace_back(name ? name : "");
			}
		});
		return names;
	}

	std::string StompboxEngine::CurrentPreset()
	{
		RequireLoaded("CurrentPreset");
		std::string name;
		if (!HasFaulted()) Guard("GetCurrentPreset", [&]
		{
			const char* preset = getCurrentPreset(processor);   // Stompbox's own string: not freed
			if (preset) name = preset;
		});
		return name;
	}

	bool StompboxEngine::IsPresetLoading()
	{
		RequireLoaded("IsPresetLoading");
		bool loading = false;
		if (!HasFaulted()) Guard("IsPresetLoading", [&] { loading = isPresetLoading(processor); });
		return loading;
	}

	void StompboxEngine::StartRemoteServer()
	{
		RequireLoaded("StartRemoteServer");
		if (isServerStarted) return;
		if (!Guard("StartServer", [&] { startServer(processor); })) return;
		isServerStarted = true;
		Log("stompbox: remote server started on 127.0.0.1:24639");
	}

	bool StompboxEngine::Adopt(StompboxEngine& next)
	{
		RequireLoaded("Adopt");
		if (IsAdopting() || !next.processor || next.module != module || next.HasFaulted() || next.frames != frames) return false;
		void* fresh = next.processor;
		next.processor = nullptr;
		next.active.store(nullptr, std::memory_order_release);
		retiring = processor;
		retiringFaulted.store(HasFaulted(), std::memory_order_relaxed);
		incoming.store(fresh, std::memory_order_release);
		if (frames <= 0) FinishAdopt(true);   // never prepared, so the stream has never run it
		return true;
	}

	bool StompboxEngine::FinishAdopt(bool force)
	{
		if (incoming.load(std::memory_order_acquire) && force)
		{
			// The stream is not running this engine, or is stopped: switch here. The stream holds the lock for one
			// block at most, and only when it has just started running this engine again.
			while (streamLock.exchange(true, std::memory_order_acquire)) SwitchToThread();
			if (void* next = incoming.load(std::memory_order_relaxed))
			{
				retiringFaulted.store(faulted.load(std::memory_order_relaxed), std::memory_order_relaxed);
				faulted.store(false, std::memory_order_relaxed);
				active.store(next, std::memory_order_relaxed);
				incoming.store(nullptr, std::memory_order_relaxed);
			}
			streamLock.store(false, std::memory_order_release);
		}
		if (incoming.load(std::memory_order_acquire)) return false;
		// The stream let go of the old processor before it cleared `incoming`.
		Release(retiring, retiringFaulted.load(std::memory_order_relaxed));
		retiring = nullptr;
		processor = active.load(std::memory_order_acquire);
		return true;
	}

	void StompboxEngine::Process(const float* guitar, float* left, float* right, int frameCount)
	{
		const float gain = InputGain();
		if (frameCount != frames || frames <= 0)
		{
			std::fill(left, left + frameCount, 0.0f);
			std::fill(right, right + frameCount, 0.0f);
			return;
		}
		if (streamLock.exchange(true, std::memory_order_acquire))
		{
			// The main thread is switching processors this instant: the guitar dry for one block.
			for (int i = 0; i < frameCount; ++i) left[i] = guitar[i] * gain;
			std::memcpy(right, left, sizeof(float) * static_cast<size_t>(frameCount));
			return;
		}
		void* current = active.load(std::memory_order_relaxed);
		if (!current)
		{
			std::fill(left, left + frameCount, 0.0f);
			std::fill(right, right + frameCount, 0.0f);
			streamLock.store(false, std::memory_order_release);
			return;
		}
		const size_t count = static_cast<size_t>(frameCount);
		for (int i = 0; i < frameCount; ++i) input[i] = guitar[i] * gain;
		RaisePeak(inputPeak, Peak(input.data(), frameCount));
		void* next = incoming.load(std::memory_order_acquire);
		if (next) std::memcpy(incomingInput.data(), input.data(), sizeof(float) * count);   // before Stompbox may write to it

		if (faulted.load(std::memory_order_relaxed) || !ProcessCatchingAll(process, current, input.data(), left, count))
		{
			faulted.store(true, std::memory_order_release);
			for (int i = 0; i < frameCount; ++i) left[i] = guitar[i] * gain;   // dry, never silence
		}

		if (next)
		{
			// Rescan: the adopted processor (set up and run on silence until ready) takes over, crossfaded over
			// this block.
			const bool wasFaulted = faulted.load(std::memory_order_relaxed);
			const bool ok = ProcessCatchingAll(process, next, incomingInput.data(), incomingOutput.data(), count);
			for (int i = 0; i < frameCount; ++i)
			{
				const float t = static_cast<float>(i + 1) / frameCount;
				const float fresh = ok ? incomingOutput[i] : guitar[i] * gain;
				left[i] = left[i] * (1.0f - t) + fresh * t;
			}
			retiringFaulted.store(wasFaulted, std::memory_order_relaxed);
			faulted.store(!ok, std::memory_order_relaxed);
			active.store(next, std::memory_order_relaxed);
			incoming.store(nullptr, std::memory_order_release);   // after the last use of `current`
		}

		std::memcpy(right, left, sizeof(float) * count);
		RaisePeak(outputPeak, Peak(left, frameCount));
		streamLock.store(false, std::memory_order_release);
	}
}
