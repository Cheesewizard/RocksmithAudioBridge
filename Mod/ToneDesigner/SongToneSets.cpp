#include "stdafx.h"
#include "SongToneSets.hpp"
#include "SongLibrary.hpp"
#include "StompboxLink.hpp"
#include "../Core/HelperRun.hpp"

#include <ShlObj.h>
#include <bcrypt.h>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

#pragma comment(lib, "bcrypt.lib")

namespace SongToneSets
{
	namespace
	{
		using Json = nlohmann::json;

		constexpr int EXIT_REFUSED = 4;                      // --songtones-import refused the set (CONTRACTS 1)
		constexpr DWORD JOB_TIMEOUT_MS = 5 * 60 * 1000;
		constexpr ULONGLONG PLAN_DELAY_MS = 1500;           // after an assignment changes: the size line follows
		constexpr ULONGLONG DLC_RESCAN_MS = 30000;          // a .psarc not found: look again, but not every song

		enum class JobKind { Check, Plan, Export };

		struct Job
		{
			JobKind kind = JobKind::Check;
			std::string songKey;
			std::wstring psarcFile;     // the song index's file name (Check, Export)
		};

		struct Result
		{
			JobKind kind = JobKind::Check;
			std::string songKey;
			bool isDisc = false;
			std::wstring sidecar;
			bool isImported = false;    // --songtones-import ran
			int code = 0;
			Json record, oldRecord;     // null when there is none
			Json plan;                  // null when the plan did not run or did not read
			std::string notice;
		};

		struct SongEntry
		{
			View view;
			bool isCheckQueued = false;
			ULONGLONG planWantedAt = 0;     // 0: the plan is current or not wanted
			bool isPlanQueued = false;
			bool hasAssignments = false;
			std::map<std::string, std::string> assignments;   // not the game's song: read from the file, plus the page's changes
			// Imported while the host was not connected: its commands would be dropped (StompboxLink clears its queue),
			// so the set is applied once the host is there.
			bool isApplyPending = false;
			std::string pendingSongKey;
			Json pendingOldRecord;
		};

		// Shared with the worker and the dialog threads.
		std::mutex g_lock;
		std::deque<Job> g_jobs;
		std::deque<Result> g_results;
		bool g_isRunning = false;
		std::string g_runningKey;       // lower case
		std::string g_importingKey;     // lower case: --songtones-import runs for it now
		std::string g_notice;

		// Render thread.
		std::map<std::string, SongEntry> g_songs;   // lower-case song key
		std::string g_watched;
		std::string g_gameSong;
		const View g_noView;

		// Worker thread (one job at a time).
		std::unordered_map<std::wstring, std::wstring> g_dlcFiles;   // lower-case file name -> full path
		ULONGLONG g_dlcScannedAt = 0;

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return text;
		}

		std::wstring LowerW(std::wstring text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
			return text;
		}

		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
			return out;
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring FileNameOf(const std::wstring& path)
		{
			const size_t slash = path.find_last_of(L"\\/");
			return slash == std::wstring::npos ? path : path.substr(slash + 1);
		}

		std::wstring KnownFolder(REFKNOWNFOLDERID id)
		{
			PWSTR folder = nullptr;
			std::wstring path;
			if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &folder))) path = folder;
			CoTaskMemFree(folder);
			return path;
		}

		std::wstring DataFolder() { return KnownFolder(FOLDERID_RoamingAppData) + L"\\RocksmithAudioBridge\\Stompbox"; }

		// The song key as a file name, exactly as the host makes SongTones\<key>.txt (StompboxService::SongTonesFile),
		// so the record sits beside the assignments it belongs to.
		std::wstring SongFileName(const std::string& key)
		{
			std::wstring name = Wide(key);
			for (wchar_t& c : name)
				if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
			return name;
		}

		std::wstring RecordPath(const std::string& key) { return DataFolder() + L"\\SongTones\\" + SongFileName(key) + L".rabsong.json"; }
		std::wstring AssignmentsPath(const std::string& key) { return DataFolder() + L"\\SongTones\\" + SongFileName(key) + L".txt"; }
		std::wstring PlanPath() { return KnownFolder(FOLDERID_LocalAppData) + L"\\Rocksmith Audio Bridge\\Cache\\tones-plan.json"; }

		bool FileExists(const std::wstring& path)
		{
			const DWORD attributes = GetFileAttributesW(path.c_str());
			return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
		}

		Json ReadJson(const std::wstring& path)
		{
			std::ifstream in(std::filesystem::path(path), std::ios::binary);
			if (!in) return nullptr;
			try
			{
				return Json::parse(in);
			}
			catch (const Json::exception& error)
			{
				LOG_WARNING("(SONG TONE SETS) " << Utf8(path) << " could not be read: " << error.what() << std::endl);
				return nullptr;
			}
		}

		bool WriteJson(const std::wstring& path, const Json& json)
		{
			const std::wstring temp = path + L".tmp";
			{
				std::ofstream out(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
				out << json.dump(2);
				if (!out) return false;
			}
			return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
		}

		std::string Field(const Json& json, const char* name)
		{
			if (!json.is_object()) return {};
			const auto it = json.find(name);
			return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
		}

		std::vector<std::string> StringList(const Json& json, const char* name)
		{
			std::vector<std::string> list;
			if (!json.is_object()) return list;
			const auto it = json.find(name);
			if (it == json.end() || !it->is_array()) return list;
			for (const Json& item : *it)
				if (item.is_string()) list.push_back(item.get<std::string>());
			return list;
		}

		std::string Sha256(const std::wstring& path)
		{
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::string hex;
			if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return hex;
			std::ifstream in(std::filesystem::path(path), std::ios::binary);
			if (in && BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
			{
				std::vector<char> buffer(1 << 16);
				bool ok = true;
				while (ok && in)
				{
					in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
					const ULONG got = static_cast<ULONG>(in.gcount());
					if (got) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), got, 0) == 0;
				}
				UCHAR digest[32]{};
				if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
				{
					char text[65]{};
					for (int i = 0; i < 32; ++i) sprintf_s(text + i * 2, 3, "%02x", digest[i]);
					hex = text;
				}
				BCryptDestroyHash(hash);
			}
			BCryptCloseAlgorithmProvider(algorithm, 0);
			return hex;
		}

		// The song index keeps only the .psarc's file name; CDLC can sit in folders under dlc\. Worker thread.
		std::wstring FindPsarc(const std::wstring& fileName)
		{
			const std::wstring wanted = LowerW(fileName);
			const auto found = g_dlcFiles.find(wanted);
			if (found != g_dlcFiles.end() && FileExists(found->second)) return found->second;
			const std::wstring direct = AudioBridge::HelperRun::GameFolder() + L"dlc\\" + fileName;
			if (FileExists(direct)) return direct;
			if (g_dlcScannedAt && GetTickCount64() - g_dlcScannedAt < DLC_RESCAN_MS) return {};
			g_dlcScannedAt = GetTickCount64();
			g_dlcFiles.clear();
			std::error_code error;
			for (std::filesystem::recursive_directory_iterator it(AudioBridge::HelperRun::GameFolder() + L"dlc", std::filesystem::directory_options::skip_permission_denied, error), end;
				!error && it != end; it.increment(error))
			{
				if (it->is_regular_file(error) && _wcsicmp(it->path().extension().c_str(), L".psarc") == 0)
					g_dlcFiles[LowerW(it->path().filename().wstring())] = it->path().wstring();
			}
			const auto again = g_dlcFiles.find(wanted);
			return again == g_dlcFiles.end() ? std::wstring() : again->second;
		}

		std::wstring SidecarOf(const std::wstring& psarc)
		{
			const size_t dot = psarc.find_last_of(L'.');
			return (dot == std::wstring::npos ? psarc : psarc.substr(0, dot)) + L".rabsong";
		}

		int RunImport(const std::wstring& file, const std::string& songKey)
		{
			{
				std::lock_guard<std::mutex> lock(g_lock);
				g_importingKey = Lower(songKey);
			}
			const int code = AudioBridge::HelperRun::Run(L"--songtones-import " + AudioBridge::HelperRun::Quote(file) + L" " + AudioBridge::HelperRun::Quote(Wide(songKey)) + L" "
				+ AudioBridge::HelperRun::Quote(SongLibrary::TonesFile()), JOB_TIMEOUT_MS);
			LOG_INFO("(SONG TONE SETS) import " << Utf8(file) << " for " << songKey << " -> " << code << std::endl);
			std::lock_guard<std::mutex> lock(g_lock);
			g_importingKey.clear();
			return code;
		}

		Json RunPlan(const std::string& songKey, int& code)
		{
			const std::wstring out = PlanPath();
			DeleteFileW(out.c_str());
			code = AudioBridge::HelperRun::Run(L"--songtones-plan " + AudioBridge::HelperRun::Quote(Wide(songKey)) + L" " + AudioBridge::HelperRun::Quote(SongLibrary::TonesFile()) + L" "
				+ AudioBridge::HelperRun::Quote(out), JOB_TIMEOUT_MS);
			return code == 0 ? ReadJson(out) : Json(nullptr);
		}

		Result RunJob(const Job& job)
		{
			Result result;
			result.kind = job.kind;
			result.songKey = job.songKey;
			result.isDisc = _wcsicmp(job.psarcFile.c_str(), L"songs.psarc") == 0;
			switch (job.kind)
			{
			case JobKind::Check:
			{
				result.record = ReadJson(RecordPath(job.songKey));
				if (result.isDisc || job.psarcFile.empty()) break;
				const std::wstring psarc = FindPsarc(job.psarcFile);
				if (psarc.empty()) break;
				result.sidecar = SidecarOf(psarc);
				if (!FileExists(result.sidecar)) break;
				const std::string sha = Sha256(result.sidecar);
				// Same file and it played: nothing to do. A refused or failed one is tried again (the player may have
				// updated the mod, repaired the library or the song list may have been stale).
				if (!result.record.is_null() && !sha.empty() && _stricmp(Field(result.record, "sha256").c_str(), sha.c_str()) == 0
					&& Field(result.record, "state") == "ready") break;
				result.oldRecord = result.record;
				result.isImported = true;
				result.code = RunImport(result.sidecar, job.songKey);
				result.record = ReadJson(RecordPath(job.songKey));
				break;
			}
			case JobKind::Plan:
				result.plan = RunPlan(job.songKey, result.code);
				break;
			case JobKind::Export:
			{
				const std::wstring psarc = result.isDisc || job.psarcFile.empty() ? std::wstring() : FindPsarc(job.psarcFile);
				if (psarc.empty())
				{
					result.notice = result.isDisc ? "Rocksmith's own songs cannot carry tones: only a CDLC has a file of its own to put them beside."
						: "The song's .psarc was not found under dlc, so there is nowhere to put its tones.";
					break;
				}
				result.plan = RunPlan(job.songKey, result.code);
				result.sidecar = SidecarOf(psarc);
				result.code = AudioBridge::HelperRun::Run(L"--songtones-export " + AudioBridge::HelperRun::Quote(Wide(job.songKey)) + L" " + AudioBridge::HelperRun::Quote(SongLibrary::TonesFile())
					+ L" " + AudioBridge::HelperRun::Quote(result.sidecar), JOB_TIMEOUT_MS);
				LOG_INFO("(SONG TONE SETS) export " << job.songKey << " -> " << Utf8(result.sidecar) << " exit " << result.code << std::endl);
				if (result.code != 0)
				{
					result.notice = "The export did not work (helper exit " + std::to_string(result.code) + "; see Logs\\tone-designer-tools.log).";
					break;
				}
				result.notice = "Wrote " + Utf8(result.sidecar) + ".";
				for (const std::string& problem : StringList(result.plan, "problems")) result.notice += " " + problem;
				// The record says this file is the player's own, so it is not imported back over their tones the next
				// time the song is chosen (its hash now matches).
				Json record = ReadJson(RecordPath(job.songKey));
				if (!record.is_object()) record = Json::object();
				SYSTEMTIME now{};
				GetSystemTime(&now);
				char stamp[32]{};
				sprintf_s(stamp, "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
				Json toneKeys = Json::object();
				if (result.plan.is_object() && result.plan.contains("tones") && result.plan["tones"].is_array())
					for (const Json& tone : result.plan["tones"])
						if (!Field(tone, "toneKey").empty()) toneKeys[Field(tone, "toneKey")] = Field(tone, "preset");
				record["source"] = Utf8(result.sidecar);
				record["sha256"] = Sha256(result.sidecar);
				record["importedAt"] = stamp;
				record["state"] = "ready";
				record.erase("reason");
				record["toneKeys"] = toneKeys;
				record["fromSong"] = Json::array();
				record["useMine"] = false;
				record["exportedHere"] = true;
				if (!WriteJson(RecordPath(job.songKey), record)) LOG_WARNING("(SONG TONE SETS) the record for " << job.songKey << " could not be written" << std::endl);
				result.record = record;
				break;
			}
			}
			return result;
		}

		void Enqueue(Job job)
		{
			std::lock_guard<std::mutex> lock(g_lock);
			g_jobs.push_back(std::move(job));
		}

		void Pump()
		{
			std::lock_guard<std::mutex> lock(g_lock);
			if (g_isRunning || g_jobs.empty()) return;
			Job job = std::move(g_jobs.front());
			g_jobs.pop_front();
			g_isRunning = true;
			g_runningKey = Lower(job.songKey);
			std::thread([job]
			{
				Result result = RunJob(job);
				std::lock_guard<std::mutex> done(g_lock);
				g_results.push_back(std::move(result));
				g_isRunning = false;
				g_runningKey.clear();
			}).detach();
		}

		const SongLibrary::Song* FindSong(const std::shared_ptr<const SongLibrary::Library>& library, const std::string& key)
		{
			return library->Find(key);
		}

		// Queues the look at a song's set. False while the song list is still being read (try again later).
		bool QueueCheck(const std::string& songKey)
		{
			SongEntry& entry = g_songs[Lower(songKey)];
			if (entry.isCheckQueued) return true;
			const auto library = SongLibrary::Get();
			const SongLibrary::Song* song = FindSong(library, songKey);
			if (!song && SongLibrary::GetState() != SongLibrary::State::Ready && SongLibrary::GetState() != SongLibrary::State::Failed) return false;
			entry.isCheckQueued = true;
			Enqueue({ JobKind::Check, songKey, song ? Wide(song->file) : std::wstring() });
			return true;
		}

		void QueuePlan(const std::string& songKey)
		{
			SongEntry& entry = g_songs[Lower(songKey)];
			if (entry.isPlanQueued) return;
			entry.isPlanQueued = true;
			entry.planWantedAt = 0;
			Enqueue({ JobKind::Plan, songKey, {} });
		}

		void FillFromRecord(View& view, const Json& record)
		{
			view.state = SetState::None;
			view.toneKeys.clear();
			view.fromSong.clear();
			view.useMine = view.isExportedHere = false;
			view.author = view.reason = view.fileName = {};
			if (!record.is_object()) return;
			view.fileName = Utf8(FileNameOf(Wide(Field(record, "source"))));
			view.author = Field(record, "author");
			view.reason = Field(record, "reason");
			view.state = Field(record, "state") == "refused" ? SetState::Refused : SetState::Ready;
			view.useMine = record.value("useMine", false);
			view.isExportedHere = record.value("exportedHere", false);
			view.fromSong = StringList(record, "fromSong");
			if (record.contains("toneKeys") && record["toneKeys"].is_object())
				for (const auto& tone : record["toneKeys"].items())
					if (tone.value().is_string()) view.toneKeys[tone.key()] = tone.value().get<std::string>();
		}

		void FillFromPlan(View& view, const Json& plan)
		{
			view.hasPlan = plan.is_object();
			if (!view.hasPlan) return;
			const Json totals = plan.value("totals", Json::object());
			view.tones = totals.value("tones", 0);
			view.captures = totals.value("captures", 0);
			view.cabinets = totals.value("cabinets", 0);
			view.bytes = totals.value("bytes", 0LL);
			view.problems = StringList(plan, "problems");
			view.embeddedFiles = 0;
			if (plan.contains("files") && plan["files"].is_array())
				for (const Json& file : plan["files"])
					if (Field(file, "status") != "starter") ++view.embeddedFiles;
		}

		// Writes the page's own fields into the record; the C# side keeps fields it does not know (CONTRACTS 2).
		void SaveRecordFields(const std::string& songKey, const View& view)
		{
			const std::wstring path = RecordPath(songKey);
			Json record = ReadJson(path);
			if (!record.is_object()) return;
			record["fromSong"] = view.fromSong;
			record["useMine"] = view.useMine;
			if (!WriteJson(path, record)) LOG_WARNING("(SONG TONE SETS) the record for " << songKey << " could not be updated" << std::endl);
		}

		bool Contains(const std::vector<std::string>& list, const std::string& item) { return std::find(list.begin(), list.end(), item) != list.end(); }

		// After an import: the host reads the new presets, then every tone key the player has not made their own plays
		// the set's tone. A key counts as the song's when it is unassigned, already plays the set's tone, or still plays
		// what the previous set gave it.
		void ApplyImported(const std::string& songKey, const Json& oldRecord, View& view, bool rescan)
		{
			if (rescan) StompboxLink::Rescan();   // only after an import wrote files; a full rescan rebuilds every engine
			if (view.useMine) return;
			View previous;
			FillFromRecord(previous, oldRecord);
			const std::vector<std::string> oldFromSong = previous.fromSong.empty() ? view.fromSong : previous.fromSong;
			const std::map<std::string, std::string> assigned = Assignments(songKey);   // a copy: Assign changes it
			std::vector<std::string> fromSong;
			for (const auto& tone : view.toneKeys)
			{
				const auto current = assigned.find(tone.first);
				const std::string now = current == assigned.end() ? std::string() : current->second;
				const auto old = previous.toneKeys.find(tone.first);
				const bool isSongs = now.empty() || now == tone.second
					|| (Contains(oldFromSong, tone.first) && old != previous.toneKeys.end() && now == old->second);
				if (!isSongs) continue;
				if (now != tone.second) Assign(songKey, tone.first, tone.second);
				fromSong.push_back(tone.first);
			}
			view.fromSong = fromSong;
			SaveRecordFields(songKey, view);
		}

		void ApplyResult(Result& result)
		{
			SongEntry& entry = g_songs[Lower(result.songKey)];
			View& view = entry.view;
			switch (result.kind)
			{
			case JobKind::Plan:
				entry.isPlanQueued = false;
				FillFromPlan(view, result.plan);
				if (!view.hasPlan) LOG_WARNING("(SONG TONE SETS) no export plan for " << result.songKey << " (helper exit " << result.code << ")" << std::endl);
				return;
			case JobKind::Export:
				if (!result.notice.empty())
				{
					std::lock_guard<std::mutex> lock(g_lock);
					g_notice = result.notice;
				}
				if (result.plan.is_object()) FillFromPlan(view, result.plan);
				if (result.record.is_object()) FillFromRecord(view, result.record);
				return;
			default:
				break;
			}
			view.isKnown = true;
			view.isDisc = result.isDisc;
			FillFromRecord(view, result.record);
			if (!result.isImported) return;
			const std::string file = Utf8(FileNameOf(result.sidecar));
			if (result.code == 0 && view.state == SetState::Ready)
			{
				if (!StompboxLink::GetSnapshot().isConnected)
				{
					SongEntry& pending = g_songs[Lower(result.songKey)];
					pending.isApplyPending = true;
					pending.pendingSongKey = result.songKey;
					pending.pendingOldRecord = result.oldRecord;
					return;
				}
				ApplyImported(result.songKey, result.oldRecord, view, true);
				return;
			}
			if (result.code == EXIT_REFUSED)
			{
				view.state = SetState::Refused;
				if (view.reason.empty()) view.reason = "It was refused.";
			}
			else
			{
				view.state = SetState::Failed;
				view.reason = "The Audio Bridge helper could not read it (exit " + std::to_string(result.code) + "; see Logs\\tone-designer-tools.log).";
			}
			view.fileName = file;
		}
	}

	void OnFrame(const std::string& gameSongKey)
	{
		if (gameSongKey != g_gameSong)
		{
			g_gameSong = gameSongKey;
			// Chosen again: look again (the player may have put a set beside it since).
			if (!gameSongKey.empty()) g_songs[Lower(gameSongKey)].isCheckQueued = false;
		}
		// Only while the Tone designer plays: the song list costs a helper run, and nobody else needs the set.
		if (!g_gameSong.empty() && StompboxLink::GetSnapshot().isWanted)
		{
			SongLibrary::Refresh();   // the song list says where its .psarc is
			QueueCheck(g_gameSong);
		}
		if (!g_watched.empty())
		{
			SongEntry& watched = g_songs[g_watched];
			if (watched.planWantedAt && GetTickCount64() >= watched.planWantedAt) QueuePlan(g_watched);
		}
		std::deque<Result> results;
		{
			std::lock_guard<std::mutex> lock(g_lock);
			results.swap(g_results);
		}
		for (Result& result : results) ApplyResult(result);
		if (StompboxLink::GetSnapshot().isConnected)
			for (auto& song : g_songs)
				if (song.second.isApplyPending)
				{
					song.second.isApplyPending = false;
					ApplyImported(song.second.pendingSongKey, song.second.pendingOldRecord, song.second.view, true);
				}
		Pump();
	}

	void Watch(const std::string& songKey)
	{
		const std::string key = Lower(songKey);
		if (key == g_watched)
		{
			if (!songKey.empty() && !g_songs[key].isCheckQueued) QueueCheck(songKey);   // the song list was still being read
			return;
		}
		g_watched = key;
		if (key.empty()) return;
		SongEntry& entry = g_songs[key];
		entry.hasAssignments = false;   // read again: the host may have changed them while it was not shown
		entry.isCheckQueued = false;
		QueueCheck(songKey);
		if (!entry.view.hasPlan) QueuePlan(songKey);
	}

	const View& Get(const std::string& songKey)
	{
		const auto it = g_songs.find(Lower(songKey));
		if (it == g_songs.end()) return g_noView;
		View& view = it->second.view;
		std::lock_guard<std::mutex> lock(g_lock);
		view.isImporting = g_importingKey == it->first;
		view.isBusy = g_runningKey == it->first || std::any_of(g_jobs.begin(), g_jobs.end(), [&](const Job& job) { return Lower(job.songKey) == it->first && job.kind != JobKind::Plan; });
		return view;
	}

	const std::map<std::string, std::string>& Assignments(const std::string& songKey)
	{
		const StompboxLink::Snapshot& snapshot = StompboxLink::GetSnapshot();
		if (!snapshot.songKey.empty() && _stricmp(snapshot.songKey.c_str(), songKey.c_str()) == 0) return snapshot.songTones;
		SongEntry& entry = g_songs[Lower(songKey)];
		if (!entry.hasAssignments)
		{
			entry.hasAssignments = true;
			entry.assignments.clear();
			// SongTones\<key>.txt: tone key, tab, saved tone (StompboxService::ReadAssignments). Read only: the host owns it.
			std::ifstream file(std::filesystem::path(AssignmentsPath(songKey)), std::ios::binary);
			for (std::string line; std::getline(file, line);)
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const size_t tab = line.find('\t');
				if (tab != std::string::npos && tab > 0 && tab + 1 < line.size()) entry.assignments[line.substr(0, tab)] = line.substr(tab + 1);
			}
		}
		return entry.assignments;
	}

	void Assign(const std::string& songKey, const std::string& toneKey, const std::string& preset)
	{
		if (songKey.empty() || toneKey.empty()) return;
		const StompboxLink::Snapshot& snapshot = StompboxLink::GetSnapshot();
		// The host matches its current song by the exact key; the song list may spell it in another case.
		const bool isGameSong = !snapshot.songKey.empty() && _stricmp(snapshot.songKey.c_str(), songKey.c_str()) == 0;
		StompboxLink::AssignSongTone(isGameSong ? snapshot.songKey : songKey, toneKey, preset);
		SongEntry& entry = g_songs[Lower(songKey)];
		if (entry.hasAssignments)
		{
			if (preset.empty()) entry.assignments.erase(toneKey);
			else entry.assignments[toneKey] = preset;
		}
		entry.planWantedAt = GetTickCount64() + PLAN_DELAY_MS;
	}

	bool IsFromSong(const View& view, const std::string& toneKey, const std::string& assigned)
	{
		if (view.state != SetState::Ready || assigned.empty() || !Contains(view.fromSong, toneKey)) return false;
		const auto preset = view.toneKeys.find(toneKey);
		return preset != view.toneKeys.end() && preset->second == assigned;
	}

	void UseMine(const std::string& songKey)
	{
		SongEntry& entry = g_songs[Lower(songKey)];
		View& view = entry.view;
		if (view.state != SetState::Ready) return;
		const std::map<std::string, std::string> assigned = Assignments(songKey);
		for (const std::string& key : view.fromSong)
		{
			const auto current = assigned.find(key);
			if (current != assigned.end() && IsFromSong(view, key, current->second)) Assign(songKey, key, {});
		}
		view.useMine = true;   // fromSong stays: "Use the song's tones" puts them back
		SaveRecordFields(songKey, view);
	}

	void UseSongTones(const std::string& songKey)
	{
		SongEntry& entry = g_songs[Lower(songKey)];
		View& view = entry.view;
		if (view.state != SetState::Ready) return;
		view.useMine = false;
		ApplyImported(songKey, nullptr, view, false);   // the song's keys and the unassigned ones get the set's tones again
	}

	void Export(const std::string& songKey)
	{
		const auto library = SongLibrary::Get();
		const SongLibrary::Song* song = FindSong(library, songKey);
		Enqueue({ JobKind::Export, songKey, song ? Wide(song->file) : std::wstring() });
	}

	std::string TakeNotice()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		std::string notice;
		notice.swap(g_notice);
		return notice;
	}
}
