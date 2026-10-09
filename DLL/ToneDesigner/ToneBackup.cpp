#include "stdafx.h"
#include "ToneBackup.hpp"
#include "Core/RsModsHelper.hpp"
#include "VstHostLink.hpp"

#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>

namespace ToneBackup
{
	namespace
	{
		enum class Step { Idle, Working, StoppingHost, Restoring, RestartHost };
		std::atomic<Step> g_step{ Step::Idle };
		std::mutex g_lock;
		std::string g_status;
		ULONGLONG g_statusAt = 0;
		std::wstring g_restoreZip;
		bool g_hostWasRunning = false;
		ULONGLONG g_stopAskedAt = 0;
		bool g_autoDone = false;

		void SetStatus(const std::string& text)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_status = text;
			g_statusAt = GetTickCount64();
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring Stamp(const wchar_t* format)
		{
			SYSTEMTIME now{};
			GetLocalTime(&now);
			wchar_t text[64]{};
			swprintf_s(text, format, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
			return text;
		}

		// On a worker thread: the helper, then the result for the page.
		void RunInBackground(std::wstring arguments, std::string done, std::string failed, Step after)
		{
			std::thread([arguments, done, failed, after]
			{
				const int code = RsModsHelper::Run(arguments, 5 * 60 * 1000);
				LOG_INFO("(TONE BACKUP) RSMods.exe " << Utf8(arguments) << " -> " << code << std::endl);
				SetStatus(code == 0 ? done : failed);
				g_step = after;
			}).detach();
		}
	}

	std::wstring Folder()
	{
		PWSTR documents = nullptr;
		std::wstring folder;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents))) folder = documents;
		CoTaskMemFree(documents);
		return folder + L"\\Rocksmith Audio Bridge\\Tone designer backups";
	}

	std::vector<Entry> List()
	{
		std::vector<Entry> entries;
		std::vector<std::pair<std::filesystem::file_time_type, Entry>> found;
		std::error_code error;
		for (const auto& file : std::filesystem::directory_iterator(Folder(), error))
			if (_wcsicmp(file.path().extension().c_str(), L".zip") == 0)
				found.push_back({ file.last_write_time(error), Entry{ file.path().wstring(), Utf8(file.path().stem().wstring()) } });
		std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		for (auto& item : found) entries.push_back(std::move(item.second));
		return entries;
	}

	void BackupNow()
	{
		Step idle = Step::Idle;
		if (!g_step.compare_exchange_strong(idle, Step::Working)) return;
		SHCreateDirectoryExW(nullptr, Folder().c_str(), nullptr);
		const std::wstring zip = Folder() + L"\\" + Stamp(L"Backup %04u-%02u-%02u %02u%02u") + L".zip";
		SetStatus("Backing up your tones...");
		RunInBackground(L"--tone-backup " + RsModsHelper::Quote(zip), "Backed up to " + Utf8(zip.substr(zip.find_last_of(L'\\') + 1)) + ".",
			"The backup did not work (RSMods.exe; see Logs\\tone-designer-tools.log).", Step::Idle);
	}

	void Restore(const std::wstring& zip)
	{
		Step idle = Step::Idle;
		if (!g_step.compare_exchange_strong(idle, Step::StoppingHost)) return;
		g_restoreZip = zip;
		g_hostWasRunning = VstHostLink::HostProcessRunning();
		g_stopAskedAt = GetTickCount64();
		if (g_hostWasRunning) VstHostLink::StopHost();
		SetStatus("Restoring: closing Tone designer first...");
	}

	void OpenFolder()
	{
		SHCreateDirectoryExW(nullptr, Folder().c_str(), nullptr);
		ShellExecuteW(nullptr, L"open", Folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	}

	void OnFrame(bool gameLoaded)
	{
		// Once a session, after the game has loaded: today's automatic backup (the helper skips it when it exists).
		if (!g_autoDone && gameLoaded && g_step == Step::Idle)
		{
			g_autoDone = true;
			g_step = Step::Working;
			RunInBackground(L"--tone-backup-auto " + RsModsHelper::Quote(Folder()), "", "", Step::Idle);
		}
		switch (g_step.load())
		{
		case Step::StoppingHost:
			if (VstHostLink::HostProcessRunning())
			{
				if (GetTickCount64() - g_stopAskedAt > 20000)
				{
					SetStatus("Tone designer did not close, so nothing was restored. Close it with its power button and try again.");
					g_step = Step::Idle;
				}
				break;
			}
			g_step = Step::Restoring;
			{
				const std::wstring safety = Folder() + L"\\" + Stamp(L"Before restore %04u-%02u-%02u %02u%02u") + L".zip";
				const std::wstring name = g_restoreZip.substr(g_restoreZip.find_last_of(L'\\') + 1);
				SetStatus("Restoring " + Utf8(name) + "...");
				RunInBackground(L"--tone-restore " + RsModsHelper::Quote(g_restoreZip) + L" " + RsModsHelper::Quote(safety),
					"Restored " + Utf8(name) + ". What you had before is in \"" + Utf8(safety.substr(safety.find_last_of(L'\\') + 1)) + "\".",
					"The restore did not work; your tones are as they were (RSMods.exe; see Logs\\tone-designer-tools.log).", Step::RestartHost);
			}
			break;
		case Step::RestartHost:
			VstHostLink::ReloadSettings();
			if (g_hostWasRunning) VstHostLink::StartHost();
			g_step = Step::Idle;
			break;
		default:
			break;
		}
	}

	bool Busy() { return g_step != Step::Idle; }

	std::string Status()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (g_status.empty() || (g_step == Step::Idle && GetTickCount64() - g_statusAt > 12000)) return {};
		return g_status;
	}
}
