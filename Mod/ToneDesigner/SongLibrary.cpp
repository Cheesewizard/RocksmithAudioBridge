#include "stdafx.h"
#include "SongLibrary.hpp"
#include "../Core/HelperRun.hpp"

#include <ShlObj.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace SongLibrary
{
	namespace
	{
		std::mutex g_lock;
		std::shared_ptr<const Library> g_library = std::make_shared<Library>();
		std::atomic<State> g_state{ State::NotRead };
		std::atomic<bool> g_started{ false };

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
			return text;
		}

		std::wstring CacheFolder()
		{
			PWSTR local = nullptr;
			std::wstring folder;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local))) folder = local;
			CoTaskMemFree(local);
			return folder + L"\\Rocksmith Audio Bridge\\Cache\\";
		}

		std::wstring IndexPath() { return CacheFolder() + L"song-index.tsv"; }
		std::wstring TonesPath() { return CacheFolder() + L"song-index.tones.tsv"; }   // ToneDesignerTools.cs TonesPath

		std::vector<std::string> SplitTabs(const std::string& line)
		{
			std::vector<std::string> fields;
			std::stringstream parts(line);
			std::string field;
			while (std::getline(parts, field, '\t')) fields.push_back(field);
			return fields;
		}

		// Tab separated, UTF-8: key, title, artist, album, year, arrangements, file.
		bool Load()
		{
			std::ifstream in(IndexPath(), std::ios::binary);
			if (!in) return false;
			auto library = std::make_shared<Library>();
			std::string line;
			while (std::getline(in, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const std::vector<std::string> fields = SplitTabs(line);
				if (fields.size() < 7 || fields[0].empty()) continue;
				Song song;
				song.key = fields[0];
				song.title = fields[1];
				song.artist = fields[2];
				song.album = fields[3];
				song.year = atoi(fields[4].c_str());
				song.arrangements = fields[5];
				song.file = fields[6];
				library->songs.push_back(std::move(song));
			}
			std::sort(library->songs.begin(), library->songs.end(), [](const Song& a, const Song& b) {
				const int artist = _stricmp(a.artist.c_str(), b.artist.c_str());
				return artist != 0 ? artist < 0 : _stricmp(a.title.c_str(), b.title.c_str()) < 0;
			});
			for (size_t i = 0; i < library->songs.size(); ++i) library->byKey[Lower(library->songs[i].key)] = i;
			// Tab separated, UTF-8: song key, arrangement, slot, tone key, tone name. Missing before the first index
			// that read tones: the songs then list none.
			std::ifstream tones(TonesPath(), std::ios::binary);
			while (std::getline(tones, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				const std::vector<std::string> fields = SplitTabs(line);
				if (fields.size() < 5 || fields[3].empty()) continue;
				const auto song = library->byKey.find(Lower(fields[0]));
				if (song == library->byKey.end()) continue;
				library->songs[song->second].tones.push_back({ fields[1], fields[2], fields[3], fields[4] });
			}
			std::lock_guard<std::mutex> lock(g_lock);
			g_library = std::move(library);
			return true;
		}
	}

	const Song* Library::Find(const std::string& key) const
	{
		const auto it = byKey.find(Lower(key));
		return it == byKey.end() ? nullptr : &songs[it->second];
	}

	void Refresh()
	{
		if (g_started.exchange(true)) return;
		g_state = State::Reading;
		std::thread([]
		{
			const bool hadIndex = Load();   // last session's, at once
			if (hadIndex) g_state = State::Ready;
			// A big library takes a minute or two the first time; later runs only read new or changed files.
			const int code = AudioBridge::HelperRun::Run(L"--song-index " + AudioBridge::HelperRun::Quote(AudioBridge::HelperRun::GameFolder().substr(0, AudioBridge::HelperRun::GameFolder().size() - 1))
				+ L" " + AudioBridge::HelperRun::Quote(IndexPath()), 10 * 60 * 1000);
			const bool loaded = code == 0 && Load();
			LOG_INFO("(SONG LIBRARY) index " << (loaded ? "updated" : "not updated") << " (helper exit " << code << "), "
				<< Get()->songs.size() << " songs" << std::endl);
			g_state = loaded || hadIndex ? State::Ready : State::Failed;
		}).detach();
	}

	State GetState() { return g_state.load(); }

	std::wstring TonesFile() { return TonesPath(); }

	std::shared_ptr<const Library> Get()
	{
		std::lock_guard<std::mutex> lock(g_lock);
		return g_library;
	}
}
