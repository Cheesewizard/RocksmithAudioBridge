#include "stdafx.h"
#include "TakeRecorder.hpp"
#include "SharedOutput.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <shlobj.h>

namespace Audio::Takes
{
	namespace
	{
		std::mutex g_mutex;
		bool g_ownsTake = false;
		VideoPhase g_phase = VideoPhase::Off;
		std::string g_message;
		bool g_messageIsError = false;
		std::wstring g_token;
		HANDLE g_process = nullptr;
		HANDLE g_ready = nullptr;
		HANDLE g_stop = nullptr;

		std::filesystem::path GameFolder()
		{
			wchar_t exe[MAX_PATH]{};
			GetModuleFileNameW(nullptr, exe, MAX_PATH);
			return std::filesystem::path(exe).parent_path();
		}

		std::filesystem::path SettingsFile() { return GameFolder() / L"RSMods.ini"; }

		std::filesystem::path TempFile(const std::wstring& token, const wchar_t* extension)
		{
			wchar_t temp[MAX_PATH]{};
			GetTempPathW(MAX_PATH, temp);
			return std::filesystem::path(temp) / (L"RSModsVideoTake-" + token + extension);
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
			std::string out(size > 0 ? size - 1 : 0, '\0');
			if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
			return out;
		}

		std::string FileName(const std::wstring& path) { return Utf8(std::filesystem::path(path).filename().wstring()); }

		void Say(const std::string& message, bool error)
		{
			g_message = message;
			g_messageIsError = error;
			if (error) LOG_ERROR("[Takes] " << message << std::endl);
			else LOG_INFO("[Takes] " << message << std::endl);
		}

		ControlResponse Control(uint32_t op, const std::wstring& value = L"")
		{
			ControlRequest request;
			request.operation = op;
			wcsncpy_s(request.value, value.c_str(), _TRUNCATE);
			return SharedOutput::DispatchControl(request);
		}

		void CloseHelper()
		{
			for (HANDLE* handle : { &g_process, &g_ready, &g_stop })
				if (*handle) { CloseHandle(*handle); *handle = nullptr; }
		}

		// Launches RSMods.exe --video-take hidden. See GUI/Audio/VideoTakeHost.cs for the other half.
		bool LaunchVideoHelper(const std::wstring& folder)
		{
			const std::filesystem::path executable = GameFolder() / L"RSMods" / L"RSMods.exe";
			if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
				Say("Video needs RSMods.exe in the Rocksmith RSMods folder. Recording audio only.", true);
				return false;
			}
			if (!D3DHooks::hThisWnd) {
				Say("The game window is not ready for video yet. Recording audio only.", true);
				return false;
			}
			g_token = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
			std::error_code ec;
			std::filesystem::remove(TempFile(g_token, L".audio"), ec);
			std::filesystem::remove(TempFile(g_token, L".result"), ec);
			g_ready = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\RSModsVideoTake-" + g_token + L"-ready").c_str());
			g_stop = CreateEventW(nullptr, TRUE, FALSE, (L"Local\\RSModsVideoTake-" + g_token + L"-stop").c_str());
			if (!g_ready || !g_stop) {
				CloseHelper();
				Say("Video could not be prepared. Recording audio only.", true);
				return false;
			}
			std::wstring command = L"\"" + executable.wstring() + L"\" --video-take "
				+ std::to_wstring(reinterpret_cast<uintptr_t>(D3DHooks::hThisWnd)) + L" "
				+ std::to_wstring(GetCurrentProcessId()) + L" \"" + folder + L"\" " + g_token;
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			PROCESS_INFORMATION process{};
			if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
				nullptr, GameFolder().c_str(), &startup, &process)) {
				CloseHelper();
				Say("Video could not start (error " + std::to_string(GetLastError()) + "). Recording audio only.", true);
				return false;
			}
			CloseHandle(process.hThread);
			g_process = process.hProcess;
			g_phase = VideoPhase::Starting;
			return true;
		}

		// Hands the finished audio take to the helper and tells it to stop. An empty path keeps the video alone.
		void StopVideoHelper(const ControlResponse* audio)
		{
			if (!g_stop) return;
			{
				std::ofstream handoff(TempFile(g_token, L".audio"), std::ios::binary | std::ios::trunc);
				if (audio && audio->file[0])
					handoff << Utf8(audio->file) << "\n" << audio->recordingStarted << "\n" << audio->recordedFrames << "\n";
			}
			SetEvent(g_stop);
			g_phase = VideoPhase::Finishing;
		}

		// Reads the helper's result once it has exited.
		void CollectVideoResult()
		{
			std::ifstream file(TempFile(g_token, L".result"), std::ios::binary);
			std::string kind, detail, line;
			std::getline(file, kind);
			while (std::getline(file, line)) detail += (detail.empty() ? "" : "\n") + line;
			file.close();
			std::error_code ec;
			std::filesystem::remove(TempFile(g_token, L".result"), ec);
			std::filesystem::remove(TempFile(g_token, L".audio"), ec);

			const bool beforeStop = g_phase == VideoPhase::Starting || g_phase == VideoPhase::Capturing;
			if (kind == "ok") {
				g_phase = VideoPhase::Saved;
				// detail is the UTF-8 MP4 path; show just the file name.
				const size_t slash = detail.find_last_of("\\/");
				Say("Saved " + (slash == std::string::npos ? detail : detail.substr(slash + 1)), false);
			}
			else if (kind == "video") {
				g_phase = VideoPhase::Failed;
				const size_t split = detail.find('\n');
				Say("Video kept on its own: " + (split == std::string::npos ? std::string() : detail.substr(split + 1)), true);
			}
			else {
				g_phase = VideoPhase::Failed;
				Say(std::string(beforeStop ? "Video stopped: " : "Video failed: ") + (detail.empty() ? "the recorder closed unexpectedly." : detail), true);
			}
			CloseHelper();
		}

		// Moves the video helper along: capturing once it signals ready, result collected once it exits, and
		// closed if the audio take ended elsewhere (desktop bridge Stop). Called with g_mutex held.
		void Advance()
		{
			if (!g_process) return;
			if (g_phase == VideoPhase::Starting && WaitForSingleObject(g_ready, 0) == WAIT_OBJECT_0) g_phase = VideoPhase::Capturing;
			if (WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0) CollectVideoResult();
			else if ((g_phase == VideoPhase::Starting || g_phase == VideoPhase::Capturing) && !SharedOutput::IsRecording()) {
				g_ownsTake = false;
				StopVideoHelper(nullptr);
			}
		}
	}

	bool Toggle(bool video)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Advance();
		if (SharedOutput::HasTake()) {   // not IsRecording: a take that failed partway must still be stopped here
			const ControlResponse stopped = Control(3);
			const bool hadVideo = g_process && (g_phase == VideoPhase::Starting || g_phase == VideoPhase::Capturing);
			g_ownsTake = false;
			if (FAILED(stopped.result)) {
				char code[16]; sprintf_s(code, "%08X", static_cast<unsigned>(stopped.result));
				Say(std::string("Recording failed partway (0x") + code + ")"
					+ (stopped.file[0] ? ". What was recorded before that is in " + FileName(stopped.file) + "." : "."), true);
				if (hadVideo) StopVideoHelper(nullptr);
				return true;
			}
			if (hadVideo) { StopVideoHelper(&stopped); Say("Saving video...", false); }
			else Say("Saved " + FileName(stopped.file), false);
			return true;
		}

		if (g_process) {
			Say("The last video is still being saved. Try again in a moment.", true);
			return false;
		}
		const std::wstring folder = SessionFolder();
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		const ControlResponse started = Control(2, folder);
		if (FAILED(started.result)) {
			char code[16]; sprintf_s(code, "%08X", static_cast<unsigned>(started.result));
			Say(std::string("Recording could not start (0x") + code + ").", true);
			return false;
		}
		g_ownsTake = true;
		g_phase = VideoPhase::Off;
		g_message.clear();
		g_messageIsError = false;
		if (video) LaunchVideoHelper(folder);
		return true;
	}

	bool OwnsTake()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_ownsTake || g_process != nullptr;
	}

	Status GetStatus()
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		Advance();
		return { g_phase, g_message, g_messageIsError };
	}

	bool PreferVideo()
	{
		wchar_t value[16]{};
		GetPrivateProfileStringW(L"Audio Bridge", L"CaptureMode", L"Video", value, 16, SettingsFile().c_str());
		return _wcsicmp(value, L"Audio") != 0;
	}

	void SetPreferVideo(bool video)
	{
		WritePrivateProfileStringW(L"Audio Bridge", L"CaptureMode", video ? L"Video" : L"Audio", SettingsFile().c_str());
	}

	std::wstring Folder()
	{
		wchar_t value[MAX_PATH]{};
		GetPrivateProfileStringW(L"Audio Bridge", L"RecordingDirectory", L"", value, MAX_PATH, SettingsFile().c_str());
		if (value[0]) return value;
		// Same default as the settings app (UI.AudioBridgeSetup: MyVideos\Rocksmith Audio Bridge).
		PWSTR videos = nullptr;
		std::wstring folder;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &videos)) && videos)
			folder = (std::filesystem::path(videos) / L"Rocksmith Audio Bridge").wstring();
		CoTaskMemFree(videos);
		return folder;
	}

	// One subfolder per game launch, named by when the game started ("2026-10-06 21-14-05"), so takes group by
	// session instead of piling up in one folder. The desktop app's Record button derives the same name from the
	// game process's start time (AudioRoutingPanel.SessionFolder), so both land in the same folder with no IPC.
	std::wstring SessionFolder()
	{
		static const std::wstring stamp = [] {
			FILETIME created{}, exited{}, kernel{}, user{};
			SYSTEMTIME local{};
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) GetLocalTime(&local);
			else {
				SYSTEMTIME utc{};
				FileTimeToSystemTime(&created, &utc);
				SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
			}
			wchar_t text[32]{};
			swprintf_s(text, L"%04u-%02u-%02u %02u-%02u-%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
			return std::wstring(text);
		}();
		const std::wstring base = Folder();
		return base.empty() ? base : (std::filesystem::path(base) / stamp).wstring();
	}
}
