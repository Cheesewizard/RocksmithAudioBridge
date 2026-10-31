#include "stdafx.h"
#include "ToneBackup.hpp"
#include "../Core/HelperRun.hpp"
#include "VstHostLink.hpp"
#include "../Core/ShellOpenAsync.hpp"

#include <ShlObj.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <thread>

namespace ToneBackup
{
	namespace
	{
		enum class Step { Idle, Picking, ImportChosen, Working, StoppingHost, Restoring, RestartHost };   // Picking: a file dialog is open
		std::atomic<Step> g_step{ Step::Idle };
		std::mutex g_lock;
		std::string g_status;
		ULONGLONG g_statusAt = 0;
		std::wstring g_restoreZip;
		bool g_hostWasRunning = false;
		bool g_hostWasStandby = false;   // it was off with its plugins loaded: it comes back that way
		ULONGLONG g_stopAskedAt = 0;
		bool g_autoDone = false;
		bool g_isImport = false;         // the restore steps run an import (Import a setup), not a restore
		std::wstring g_reportPath;       // the import's plugin report (helper --tone-import)
		Result g_result;
		bool g_hasResult = false;

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
		void RunInBackground(std::wstring arguments, std::string done, std::string failed, Step after, std::function<void(int)> finished = {})
		{
			std::thread([arguments, done, failed, after, finished]
			{
				const int code = AudioBridge::HelperRun::Run(arguments, 5 * 60 * 1000);
				LOG_INFO("(TONE BACKUP) helper " << Utf8(arguments) << " -> " << code << std::endl);
				SetStatus(code == 0 ? done : failed);
				if (finished) finished(code);
				g_step = after;
			}).detach();
		}

		void SetResult(Result result)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_result = std::move(result);
			g_hasResult = true;
		}

		std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L'\\') + 1); }

		// The plugin report --tone-import writes: "missing" or "moved", tab, plugin, tab, the tones or the new path.
		void ReadReport(Result& result)
		{
			std::ifstream file(std::filesystem::path(g_reportPath), std::ios::binary);
			std::string line;
			while (std::getline(file, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);   // a byte order mark
				std::vector<std::string> parts;
				std::stringstream fields(line);
				for (std::string part; std::getline(fields, part, '\t');) parts.push_back(part);
				if (parts.size() < 3) continue;
				if (parts[0] == "missing") result.missing.push_back(parts[1] + ": used by " + parts[2]);
				else if (parts[0] == "moved") result.moved.push_back(parts[1] + ": found in " + parts[2]);
			}
		}

		// Restore and import both close Tone designer first: it autosaves its chain and holds the files.
		void BeginHostStop(const std::wstring& zip, bool isImport)
		{
			g_restoreZip = zip;
			g_isImport = isImport;
			g_hostWasRunning = VstHostLink::HostProcessRunning();
			g_hostWasStandby = VstHostLink::GetSnapshot().standby;
			g_stopAskedAt = GetTickCount64();
			if (g_hostWasRunning) VstHostLink::StopHost();
			SetStatus(isImport ? "Importing: closing Tone designer first..." : "Restoring: closing Tone designer first...");
		}

		// A Windows file dialog on its own thread (the game keeps drawing meanwhile), as the VST rig's Add folder does.
		// Returns the chosen path, or empty when cancelled.
		std::wstring AskForFile(bool save, const std::wstring& suggestedName)
		{
			std::wstring chosen;
			if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return chosen;
			IFileDialog* dialog = nullptr;
			const HRESULT created = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
			if (SUCCEEDED(created) && dialog)
			{
				const COMDLG_FILTERSPEC types[] = { { L"Rocksmith Audio Bridge setup (*.zip)", L"*.zip" } };
				dialog->SetFileTypes(1, types);
				dialog->SetDefaultExtension(L"zip");
				DWORD options = 0;
				dialog->GetOptions(&options);
				dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
				dialog->SetTitle(save ? L"Export my setup" : L"Import a setup");
				if (save) dialog->SetFileName(suggestedName.c_str());
				PWSTR documents = nullptr;
				IShellItem* start = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents))
					&& SUCCEEDED(SHCreateItemFromParsingName(documents, nullptr, IID_PPV_ARGS(&start))))
					dialog->SetFolder(start);
				CoTaskMemFree(documents);
				if (start) start->Release();
				IShellItem* item = nullptr;
				PWSTR path = nullptr;
				if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
				{
					chosen = path;
					CoTaskMemFree(path);
				}
				if (item) item->Release();
				dialog->Release();
			}
			CoUninitialize();
			return chosen;
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
		RunInBackground(L"--tone-backup " + AudioBridge::HelperRun::Quote(zip), "Backed up to " + Utf8(zip.substr(zip.find_last_of(L'\\') + 1)) + ".",
			"The backup did not work (the Audio Bridge helper; see Logs\\tone-designer-tools.log).", Step::Idle);
	}

	void Restore(const std::wstring& zip)
	{
		Step idle = Step::Idle;
		if (!g_step.compare_exchange_strong(idle, Step::StoppingHost)) return;
		BeginHostStop(zip, false);
	}

	void ExportSetup()
	{
		Step idle = Step::Idle;
		if (!g_step.compare_exchange_strong(idle, Step::Picking)) return;
		std::thread([]
		{
			const std::wstring zip = AskForFile(true, Stamp(L"My Rocksmith setup %04u-%02u-%02u") + L".zip");
			if (zip.empty()) { g_step = Step::Idle; return; }
			g_step = Step::Working;
			SetStatus("Exporting your setup...");
			const std::string name = Utf8(FileName(zip));
			RunInBackground(L"--tone-backup " + AudioBridge::HelperRun::Quote(zip), "Exported your setup to " + name + ".",
				"The export did not work (the Audio Bridge helper; see Logs\\tone-designer-tools.log).", Step::Idle, [zip](int code)
				{
					Result result;
					result.ok = code == 0;
					result.title = result.ok ? "Setup exported" : "The export did not work";
					result.text = result.ok
						? "Saved as " + Utf8(zip) + ". On the other PC, open this page and choose File, Import a setup."
						: "Nothing was saved. See Logs\\tone-designer-tools.log.";
					SetResult(std::move(result));
				});
		}).detach();
	}

	void ImportSetup()
	{
		Step idle = Step::Idle;
		if (!g_step.compare_exchange_strong(idle, Step::Picking)) return;
		std::thread([]
		{
			const std::wstring zip = AskForFile(false, {});
			if (zip.empty()) { g_step = Step::Idle; return; }
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_restoreZip = zip;
			}
			g_step = Step::ImportChosen;   // OnFrame closes the host on the render thread, then imports as a restore runs
		}).detach();
	}

	bool TakeResult(Result& out)
	{
		std::lock_guard<std::mutex> lock(g_lock);
		if (!g_hasResult) return false;
		out = g_result;
		g_hasResult = false;
		return true;
	}

	void OpenFolder()
	{
		SHCreateDirectoryExW(nullptr, Folder().c_str(), nullptr);
		ShellOpenAsync::Open(Folder());
	}

	void OnFrame(bool gameLoaded)
	{
		// Once a session, after the game has loaded: today's automatic backup (the helper skips it when it exists).
		if (!g_autoDone && gameLoaded && g_step == Step::Idle)
		{
			g_autoDone = true;
			g_step = Step::Working;
			RunInBackground(L"--tone-backup-auto " + AudioBridge::HelperRun::Quote(Folder()), "", "", Step::Idle);
		}
		switch (g_step.load())
		{
		case Step::ImportChosen:
			{
				std::wstring zip;
				{
					std::lock_guard<std::mutex> lock(g_lock);
					zip = g_restoreZip;
				}
				BeginHostStop(zip, true);
			}
			g_step = Step::StoppingHost;
			break;
		case Step::StoppingHost:
			if (VstHostLink::HostProcessRunning())
			{
				if (GetTickCount64() - g_stopAskedAt > 20000)
				{
					SetStatus(g_isImport ? "Tone designer did not close, so nothing was imported. Close it from the VST rig's File menu (Close Tone designer) and try again."
						: "Tone designer did not close, so nothing was restored. Close it from its File menu (Close Tone designer) and try again.");
					if (g_isImport)
					{
						Result result;
						result.isImport = true;
						result.title = "Nothing was imported";
						result.text = "Tone designer did not close. Close it from the VST rig's File menu (Close Tone designer) and try again.";
						SetResult(std::move(result));
					}
					g_step = Step::Idle;
				}
				break;
			}
			g_step = Step::Restoring;
			if (g_isImport)
			{
				SHCreateDirectoryExW(nullptr, Folder().c_str(), nullptr);
				const std::wstring safety = Folder() + L"\\" + Stamp(L"Before import %04u-%02u-%02u %02u%02u") + L".zip";
				wchar_t temp[MAX_PATH]{};
				GetTempPathW(MAX_PATH, temp);
				g_reportPath = std::wstring(temp) + L"Rocksmith Audio Bridge import report.txt";
				DeleteFileW(g_reportPath.c_str());
				const std::string name = Utf8(FileName(g_restoreZip));
				const std::string safetyName = Utf8(FileName(safety));
				SetStatus("Importing " + name + "...");
				RunInBackground(L"--tone-import " + AudioBridge::HelperRun::Quote(g_restoreZip) + L" " + AudioBridge::HelperRun::Quote(safety) + L" " + AudioBridge::HelperRun::Quote(g_reportPath),
					"Imported " + name + ". What you had before is in \"" + safetyName + "\".",
					"The import did not work (the Audio Bridge helper; see Logs\\tone-designer-tools.log).", Step::RestartHost, [name, safetyName](int code)
					{
						Result result;
						result.ok = code == 0;
						result.isImport = true;
						result.title = result.ok ? "Setup imported" : "The import did not work";
						result.text = result.ok
							? "Your tones, captures, cabinets, presets, song tones and settings from " + name + " are added; nothing was deleted. "
								"What you had before is in \"" + safetyName + "\" (VST rig page, File, Backups)."
							: "See Logs\\tone-designer-tools.log. If anything changed, \"" + safetyName + "\" (VST rig page, File, Backups) holds what you had before.";
						if (result.ok) ReadReport(result);
						SetResult(std::move(result));
					});
			}
			else
			{
				const std::wstring safety = Folder() + L"\\" + Stamp(L"Before restore %04u-%02u-%02u %02u%02u") + L".zip";
				const std::wstring name = g_restoreZip.substr(g_restoreZip.find_last_of(L'\\') + 1);
				SetStatus("Restoring " + Utf8(name) + "...");
				RunInBackground(L"--tone-restore " + AudioBridge::HelperRun::Quote(g_restoreZip) + L" " + AudioBridge::HelperRun::Quote(safety),
					"Restored " + Utf8(name) + ". What you had before is in \"" + Utf8(safety.substr(safety.find_last_of(L'\\') + 1)) + "\".",
					"The restore did not work; your tones are as they were (the Audio Bridge helper; see Logs\\tone-designer-tools.log).", Step::RestartHost);
			}
			break;
		case Step::RestartHost:
			VstHostLink::ReloadSettings();
			if (g_hostWasRunning) VstHostLink::StartHost(g_hostWasStandby);
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
