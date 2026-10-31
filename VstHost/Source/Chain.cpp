#include "Chain.hpp"

#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace VstHost
{
	namespace
	{
		constexpr char kToneMagic[8] = { 'R', 'A', 'B', 'T', 'O', 'N', 'E', '2' };
		constexpr char kToneMagicV1[8] = { 'R', 'A', 'B', 'T', 'O', 'N', 'E', '1' };   // before JUCE: raw VST3 states

		float DbToGain(int tenths) { return std::pow(10.0f, static_cast<float>(tenths) / 200.0f); }

		float Peak(const float* samples, int frames)
		{
			float peak = 0.0f;
			for (int i = 0; i < frames; ++i) peak = std::max(peak, std::fabs(samples[i]));
			return peak;
		}

		void RaisePeak(std::atomic<float>& peak, float value)
		{
			float current = peak.load(std::memory_order_relaxed);
			while (value > current && !peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
		}

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		void WriteBlob(FILE* file, const void* data, uint32_t size)
		{
			fwrite(&size, sizeof(size), 1, file);
			if (size) fwrite(data, 1, size, file);
		}

		bool ReadBlob(FILE* file, std::vector<char>& out)
		{
			uint32_t size = 0;
			if (fread(&size, sizeof(size), 1, file) != 1 || size > (256u << 20)) return false;
			out.resize(size);
			return size == 0 || fread(out.data(), 1, size, file) == size;
		}
	}

	void Chain::Pause()
	{
		m_running = false;
		while (m_inProcess.load()) YieldProcessor();   // at most one block
	}

	void Chain::Resume()
	{
		if (m_frames > 0) m_running = true;
	}

	bool Chain::HasSpare(const std::wstring& path) const
	{
		return std::any_of(m_spares.begin(), m_spares.end(), [&](const Spare& spare) { return _wcsicmp(spare.path.c_str(), path.c_str()) == 0; });
	}

	bool Chain::IsLoaded(const std::wstring& path) const
	{
		return HasSpare(path) || std::any_of(m_slots.begin(), m_slots.end(), [&](const std::unique_ptr<Slot>& slot) { return _wcsicmp(slot->path.c_str(), path.c_str()) == 0; });
	}

	// The newest spare of that plugin, taken out of the spares.
	std::unique_ptr<PluginHost> Chain::TakeSpare(const std::wstring& path)
	{
		for (size_t i = m_spares.size(); i-- > 0;)
			if (_wcsicmp(m_spares[i].path.c_str(), path.c_str()) == 0)
			{
				std::unique_ptr<PluginHost> plugin = std::move(m_spares[i].plugin);
				m_spares.erase(m_spares.begin() + static_cast<std::ptrdiff_t>(i));
				return plugin;
			}
		return nullptr;
	}

	// A plugin leaving the chain: its settings saved (its starting point next time), its window closed, stopped.
	void Chain::Park(std::unique_ptr<PluginHost> plugin, const std::wstring& path)
	{
		if (!plugin) return;
		plugin->SaveState();
		plugin->CloseEditor();
		plugin->Suspend();
		m_spares.push_back({ std::move(plugin), path });
		Trim();
	}

	// Keeps one spare of each `always` plugin and the newest `m_keepRecent` others; unloads the rest.
	void Chain::Trim()
	{
		std::vector<bool> keep(m_spares.size(), false);
		std::vector<std::wstring> pinned;
		int recent = 0;
		auto has = [](const std::vector<std::wstring>& list, const std::wstring& path) {
			return std::any_of(list.begin(), list.end(), [&](const std::wstring& p) { return _wcsicmp(p.c_str(), path.c_str()) == 0; });
		};
		for (size_t i = m_spares.size(); i-- > 0;)
		{
			const std::wstring& path = m_spares[i].path;
			if (has(m_keepAlways, path) && !has(pinned, path)) { keep[i] = true; pinned.push_back(path); }
			else if (recent < m_keepRecent) { keep[i] = true; ++recent; }
		}
		for (size_t i = m_spares.size(); i-- > 0;)
			if (!keep[i])
			{
				Log("spares: unloading %ls", m_spares[i].path.c_str());
				m_spares.erase(m_spares.begin() + static_cast<std::ptrdiff_t>(i));
			}
	}

	void Chain::DropSpares(const std::wstring& path)
	{
		const size_t before = m_spares.size();
		m_spares.erase(std::remove_if(m_spares.begin(), m_spares.end(), [&](const Spare& spare) { return _wcsicmp(spare.path.c_str(), path.c_str()) == 0; }), m_spares.end());
		if (m_spares.size() != before) Log("spares: unloaded %ls (asked for)", path.c_str());
	}

	void Chain::SetKeep(int recent, std::vector<std::wstring> always)
	{
		recent = std::clamp(recent, 0, 16);
		if (recent == m_keepRecent && always == m_keepAlways) return;
		m_keepRecent = recent;
		m_keepAlways = std::move(always);
		Trim();
	}

	bool Chain::PreloadOne(const std::wstring& path, std::string& error, bool recent)
	{
		auto plugin = std::make_unique<PluginHost>();
		if (!plugin->Load(path, error)) return false;
		plugin->Suspend();
		if (recent) m_spares.push_back(Spare{ std::move(plugin), path });
		else m_spares.insert(m_spares.begin(), Spare{ std::move(plugin), path });   // oldest: Trim keeps a starred one anyway
		Log("spares: %ls loaded in the background (%d kept)", path.c_str(), SpareCount());
		Trim();
		return true;
	}

	bool Chain::LoadSlot(Slot& slot, const std::wstring& path, std::string& error)
	{
		if (std::unique_ptr<PluginHost> spare = TakeSpare(path))
		{
			// Already loaded: only prepared for the stream (a fraction of a second).
			if (m_frames > 0 && !spare->Prepare(m_frames, error)) return false;
			slot.plugin = std::move(spare);
			slot.path = path;
			Log("load: %ls was kept loaded, no load needed", path.c_str());
			return true;
		}
		if (m_report) m_report(path, m_loadStep, m_loadTotal);
		auto plugin = std::make_unique<PluginHost>();
		if (!plugin->Load(path, error)) return false;
		if (m_frames > 0 && !plugin->Prepare(m_frames, error)) return false;
		slot.plugin = std::move(plugin);
		slot.path = path;
		return true;
	}

	bool Chain::Add(const std::wstring& path, std::string& error)
	{
		if (Count() >= kMaxSlots) { error = "The chain is full (" + std::to_string(kMaxSlots) + " plugins)."; return false; }
		auto slot = std::make_unique<Slot>();
		if (!LoadSlot(*slot, path, error)) return false;
		Pause();
		m_slots.push_back(std::move(slot));
		m_structureDirty = true;
		Resume();
		Log("chain: added %ls (%d in the chain)", path.c_str(), Count());
		return true;
	}

	bool Chain::Replace(int index, const std::wstring& path, std::string& error)
	{
		if (index < 0 || index >= Count()) return false;
		auto slot = std::make_unique<Slot>();
		if (!LoadSlot(*slot, path, error)) return false;
		slot->bypass = m_slots[index]->bypass.load();
		Pause();
		std::swap(m_slots[index], slot);
		m_structureDirty = true;
		Resume();
		Park(std::move(slot->plugin), slot->path);   // keeps its settings for next time, and stays loaded a while
		Log("chain: slot %d is now %ls", index + 1, path.c_str());
		return true;
	}

	void Chain::Remove(int index)
	{
		if (index < 0 || index >= Count()) return;
		Pause();
		std::unique_ptr<Slot> removed = std::move(m_slots[index]);
		m_slots.erase(m_slots.begin() + index);
		m_structureDirty = true;
		Resume();
		Park(std::move(removed->plugin), removed->path);
		Log("chain: removed slot %d", index + 1);
	}

	void Chain::Move(int index, int delta)
	{
		const int target = index + delta;
		if (index < 0 || index >= Count() || target < 0 || target >= Count()) return;
		Pause();
		std::swap(m_slots[index], m_slots[target]);
		m_structureDirty = true;
		Resume();
	}

	void Chain::SetBypass(int index, bool bypass)
	{
		if (index < 0 || index >= Count()) return;
		m_slots[index]->bypass = bypass;
		m_structureDirty = true;
	}

	bool Chain::Duplicate(int index, std::string& error)
	{
		if (index < 0 || index >= Count()) return false;
		if (Count() >= kMaxSlots) { error = "The chain is full (" + std::to_string(kMaxSlots) + " plugins)."; return false; }
		std::vector<char> state;
		m_slots[index]->plugin->GetState(state);
		auto slot = std::make_unique<Slot>();
		if (!LoadSlot(*slot, m_slots[index]->path, error)) return false;
		slot->plugin->SetState(state);   // before it plays: no stream thread yet
		slot->bypass = m_slots[index]->bypass.load();
		Pause();
		m_slots.insert(m_slots.begin() + index + 1, std::move(slot));
		m_structureDirty = true;
		Resume();
		Log("chain: duplicated slot %d (%d in the chain)", index + 1, Count());
		return true;
	}

	bool Chain::Insert(int index, const std::wstring& path, const std::vector<char>& state, bool bypass, std::string& error)
	{
		if (Count() >= kMaxSlots) { error = "The chain is full (" + std::to_string(kMaxSlots) + " plugins)."; return false; }
		auto slot = std::make_unique<Slot>();
		if (!LoadSlot(*slot, path, error)) return false;
		if (!state.empty()) slot->plugin->SetState(state);   // before it plays: no stream thread yet
		slot->bypass = bypass;
		index = std::clamp(index, 0, Count());
		Pause();
		m_slots.insert(m_slots.begin() + index, std::move(slot));
		m_structureDirty = true;
		Resume();
		Log("chain: %ls back in at slot %d (%d in the chain)", path.c_str(), index + 1, Count());
		return true;
	}

	bool Chain::Reload(const std::wstring& path, std::string& error, int& sparesDropped, std::vector<int>& slots)
	{
		sparesDropped = 0;
		slots.clear();
		if (!IsLoaded(path)) return true;

		// The copies leave the chain while they reload, and the rest of it plays on. No instance may be left, or the
		// module stays loaded. They are reloaded off to the side, like Add and Replace do, because the chain must
		// never hold a slot without a plugin: a plugin that pumps messages while it loads (a licence dialog, COM)
		// lets the host's timers, autosave, an audio reset and the page's next command run meanwhile, and they all
		// use every slot's plugin.
		std::vector<std::unique_ptr<Slot>> copies, others;
		std::vector<size_t> positions;
		Pause();
		for (size_t i = 0; i < m_slots.size(); ++i)
		{
			if (_wcsicmp(m_slots[i]->path.c_str(), path.c_str()) == 0) { positions.push_back(i); copies.push_back(std::move(m_slots[i])); }
			else others.push_back(std::move(m_slots[i]));
		}
		m_slots.swap(others);
		m_reloading = !copies.empty();
		Resume();

		std::vector<std::vector<char>> states(copies.size());
		for (size_t m = 0; m < copies.size(); ++m)
		{
			PluginHost& plugin = *copies[m]->plugin;
			plugin.GetState(states[m]);
			plugin.SaveState();   // its starting point next time it is added
			plugin.Suspend();
			copies[m]->plugin.reset();
		}
		const size_t sparesBefore = m_spares.size();
		DropSpares(path);
		sparesDropped = static_cast<int>(sparesBefore - m_spares.size());
		Log("reload: %ls unloaded (%zu in the chain, %d spare(s))", path.c_str(), copies.size(), sparesDropped);

		bool ok = true;
		auto fail = [&](const std::string& why) { ok = false; if (error.empty()) error = why.empty() ? "The plugin did not load again." : why; };
		std::vector<int> preparedFor(copies.size(), 0);
		m_loadTotal = std::max(1, static_cast<int>(copies.size()));
		for (size_t m = 0; m < copies.size(); ++m)
		{
			m_loadStep = static_cast<int>(m) + 1;
			Slot& slot = *copies[m];
			std::string slotError;
			if (LoadSlot(slot, path, slotError)) { preparedFor[m] = m_frames; slot.plugin->SetState(states[m]); }
			else fail(slotError);
		}
		m_loadStep = m_loadTotal = 1;
		// An audio reset during a load prepared the chain for the new block size, but not the copies loaded before it.
		for (size_t m = 0; m < copies.size(); ++m)
		{
			std::string slotError;
			if (copies[m]->plugin && m_frames > 0 && preparedFor[m] != m_frames && !copies[m]->plugin->Prepare(m_frames, slotError))
			{
				copies[m]->plugin.reset();
				fail(slotError);
			}
		}

		// Back at their places, in chain order so the places stay right. A copy that did not come back leaves the
		// chain, and the ones after it move up.
		Pause();
		size_t dropped = 0;
		for (size_t m = 0; m < copies.size(); ++m)
		{
			if (!copies[m]->plugin) { ++dropped; slots.push_back(-1); continue; }
			const size_t at = std::min(positions[m] - dropped, m_slots.size());
			m_slots.insert(m_slots.begin() + static_cast<std::ptrdiff_t>(at), std::move(copies[m]));
			slots.push_back(static_cast<int>(at));
		}
		if (dropped > 0) m_structureDirty = true;
		m_reloading = false;
		Resume();
		Log("reload: %ls %s", path.c_str(), ok ? "loaded again" : "did NOT load again");
		return ok;
	}

	void Chain::Clear()
	{
		Pause();
		std::vector<std::unique_ptr<Slot>> old;
		old.swap(m_slots);
		m_structureDirty = true;
		Resume();
		for (auto& slot : old) Park(std::move(slot->plugin), slot->path);
	}

	void Chain::ReleaseAll()
	{
		Pause();
		std::vector<std::unique_ptr<Slot>> old;
		old.swap(m_slots);
		for (auto& slot : old) slot->plugin->SaveState();
		old.clear();
		m_spares.clear();   // their settings were saved when they left the chain
	}

	bool Chain::Prepare(int frames, std::string& error)
	{
		Pause();
		m_frames = frames;
		for (auto& buffer : m_buffers) buffer.assign(static_cast<size_t>(std::max(frames, 1)), 0.0f);
		for (auto& slot : m_slots)
			if (!slot->plugin->Prepare(frames, error)) { error = slot->plugin->Name() + ": " + error; return false; }
		Resume();
		return true;
	}

	void Chain::Suspend()
	{
		Pause();
		for (auto& slot : m_slots) slot->plugin->Suspend();
	}

	void Chain::SetLevels(int inputTenths, int outputTenths)
	{
		m_inputTenths = std::clamp(inputTenths, -240, 240);
		m_outputTenths = std::clamp(outputTenths, -240, 240);
		m_inputGain = DbToGain(m_inputTenths);
		m_outputGain = DbToGain(m_outputTenths);
		m_structureDirty = true;
	}

	bool Chain::TakeRestartRequest()
	{
		bool any = false;
		for (auto& slot : m_slots) any = slot->plugin->TakeRestartRequest() || any;
		return any;
	}

	bool Chain::Dirty() const
	{
		if (m_structureDirty) return true;
		for (const auto& slot : m_slots) if (slot->plugin->IsDirty()) return true;
		return false;
	}

	void Chain::SavePluginSettings()
	{
		for (auto& slot : m_slots) slot->plugin->SaveState();
	}

	void Chain::Process(const float* guitar, float* left, float* right, int frames)
	{
		struct InProcess
		{
			std::atomic<bool>& flag;
			explicit InProcess(std::atomic<bool>& f) : flag(f) { flag.store(true); }
			~InProcess() { flag.store(false); }
		} inProcess(m_inProcess);

		if (!m_running.load() || frames != m_frames)
		{
			std::fill(left, left + frames, 0.0f);
			std::fill(right, right + frames, 0.0f);
			return;
		}

		float* inL = m_buffers[0].data();
		float* inR = m_buffers[1].data();
		float* outL = m_buffers[2].data();
		float* outR = m_buffers[3].data();
		const float inputGain = m_inputGain.load(std::memory_order_relaxed);
		for (int i = 0; i < frames; ++i) inL[i] = inR[i] = guitar[i] * inputGain;
		RaisePeak(m_inputPeak, Peak(inL, frames));

		// An empty chain, or every slot bypassed, passes the clean guitar through.
		const bool chainBypassed = m_chainBypass.load(std::memory_order_relaxed);
		for (auto& slot : m_slots)
		{
			if (chainBypassed) break;
			if (slot->bypass.load(std::memory_order_relaxed)) continue;
			if (!slot->plugin->Process(inL, inR, outL, outR, frames)) continue;   // not ready: skip it, keep the signal
			std::swap(inL, outL);
			std::swap(inR, outR);
		}

		const float outputGain = m_muted.load(std::memory_order_relaxed) ? 0.0f : m_outputGain.load(std::memory_order_relaxed);
		for (int i = 0; i < frames; ++i)
		{
			left[i] = inL[i] * outputGain;
			right[i] = inR[i] * outputGain;
		}
		RaisePeak(m_outputPeak, std::max(Peak(left, frames), Peak(right, frames)));
	}

	int Chain::LatencySamples()
	{
		if (m_chainBypass.load()) return 0;
		int total = 0;
		for (auto& slot : m_slots)
			if (!slot->bypass.load()) total += slot->plugin->LatencySamples();
		return total;
	}

	// Tone file: magic "RABTONE2", input and output trim (int32 tenths of a dB), slot count (uint32), then per slot
	// the plugin path (uint32 length in UTF-16 units + text), bypass (uint8), and the plugin's settings as JUCE
	// keeps them (uint32 length + bytes). "RABTONE1" files (before JUCE) have two blocks per slot instead, the raw
	// VST3 component and controller states; they load through PluginHost::StateFromVst3Parts and are written back
	// as version 2 the next time they are saved.
	bool Chain::SaveTone(const std::wstring& file)
	{
		// The reloading copies are out of the chain: a tone written now would leave them out (an autosave, or the
		// host closing with the game). The file keeps the chain as it was until the reload is done.
		if (m_reloading) { Log("tone: not saved while a plugin reloads (%ls)", file.c_str()); return false; }
		const std::wstring temp = file + L".tmp";
		FILE* out = nullptr;
		if (_wfopen_s(&out, temp.c_str(), L"wb") != 0 || !out) { Log("tone: cannot write %ls", temp.c_str()); return false; }
		fwrite(kToneMagic, 1, sizeof(kToneMagic), out);
		// The trims are the player's, not the tone's: a shared tone must not carry its author's input and
		// output levels, and switching tones must not move them. Tones are written level-neutral; the fields stay
		// for the file format; an older tone's are ignored on load.
		const int32_t levels[2] = { 0, 0 };
		fwrite(levels, sizeof(levels), 1, out);
		const uint32_t count = static_cast<uint32_t>(m_slots.size());
		fwrite(&count, sizeof(count), 1, out);
		for (auto& slot : m_slots)
		{
			WriteBlob(out, slot->path.data(), static_cast<uint32_t>(slot->path.size() * sizeof(wchar_t)));
			const uint8_t bypass = slot->bypass ? 1 : 0;
			fwrite(&bypass, 1, 1, out);
			std::vector<char> state;
			slot->plugin->GetState(state);
			WriteBlob(out, state.data(), static_cast<uint32_t>(state.size()));
		}
		const bool ok = fclose(out) == 0 && MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (ok) m_structureDirty = false;
		Log("tone: %s %ls (%u plugin(s))", ok ? "saved" : "save FAILED", file.c_str(), count);
		return ok;
	}

	std::vector<std::wstring> Chain::TonePlugins(const std::wstring& file)
	{
		std::vector<std::wstring> paths;
		FILE* in = nullptr;
		if (_wfopen_s(&in, file.c_str(), L"rb") != 0 || !in) return paths;
		char magic[sizeof(kToneMagic)]{};
		int32_t levels[2]{};
		uint32_t count = 0;
		bool ok = fread(magic, 1, sizeof(magic), in) == sizeof(magic);
		const bool v1 = ok && std::memcmp(magic, kToneMagicV1, sizeof(magic)) == 0;
		ok = ok && (v1 || std::memcmp(magic, kToneMagic, sizeof(magic)) == 0)
			&& fread(levels, sizeof(levels), 1, in) == 1 && fread(&count, sizeof(count), 1, in) == 1 && count <= kMaxSlots;
		for (uint32_t i = 0; ok && i < count; ++i)
		{
			std::vector<char> path, skip;
			uint8_t bypass = 0;
			ok = ReadBlob(in, path) && fread(&bypass, 1, 1, in) == 1 && ReadBlob(in, skip) && (!v1 || ReadBlob(in, skip));
			if (ok) paths.emplace_back(reinterpret_cast<const wchar_t*>(path.data()), path.size() / sizeof(wchar_t));
		}
		fclose(in);
		return paths;
	}

	std::vector<std::wstring> Chain::NotLoaded(const std::vector<std::wstring>& plugins) const
	{
		std::vector<bool> slotUsed(m_slots.size(), false), spareUsed(m_spares.size(), false);
		std::vector<std::wstring> missing;
		for (const std::wstring& path : plugins)
		{
			bool covered = false;
			for (size_t j = 0; j < m_slots.size() && !covered; ++j)
				if (!slotUsed[j] && _wcsicmp(m_slots[j]->path.c_str(), path.c_str()) == 0) slotUsed[j] = covered = true;
			for (size_t k = 0; k < m_spares.size() && !covered; ++k)
				if (!spareUsed[k] && _wcsicmp(m_spares[k].path.c_str(), path.c_str()) == 0) spareUsed[k] = covered = true;
			if (!covered) missing.push_back(path);
		}
		return missing;
	}

	bool Chain::LoadTone(const std::wstring& file, std::string& error, std::string& missing)
	{
		FILE* in = nullptr;
		if (_wfopen_s(&in, file.c_str(), L"rb") != 0 || !in) { error = "The tone file could not be opened."; return false; }
		char magic[sizeof(kToneMagic)]{};
		int32_t levels[2]{};
		uint32_t count = 0;
		struct Entry { std::wstring path; bool bypass = false; std::vector<char> state; };
		std::vector<Entry> entries;
		bool ok = fread(magic, 1, sizeof(magic), in) == sizeof(magic);
		const bool v1 = ok && std::memcmp(magic, kToneMagicV1, sizeof(magic)) == 0;
		ok = ok && (v1 || std::memcmp(magic, kToneMagic, sizeof(magic)) == 0)
			&& fread(levels, sizeof(levels), 1, in) == 1 && fread(&count, sizeof(count), 1, in) == 1 && count <= kMaxSlots;
		for (uint32_t i = 0; ok && i < count; ++i)
		{
			Entry entry;
			std::vector<char> path;
			uint8_t bypass = 0;
			ok = ReadBlob(in, path) && fread(&bypass, 1, 1, in) == 1;
			if (ok && v1)
			{
				std::vector<char> component, controller;
				ok = ReadBlob(in, component) && ReadBlob(in, controller);
				if (ok && !component.empty()) entry.state = PluginHost::StateFromVst3Parts(component, controller);
			}
			else if (ok) ok = ReadBlob(in, entry.state);
			if (!ok) break;
			entry.path.assign(reinterpret_cast<const wchar_t*>(path.data()), path.size() / sizeof(wchar_t));
			entry.bypass = bypass != 0;
			entries.push_back(std::move(entry));
		}
		fclose(in);
		if (!ok) { error = "The tone file is damaged."; return false; }

		// Plugins the new tone shares with the chain playing now are kept and only get the tone's settings (a
		// fraction of a second); loading a plugin from scratch takes seconds (AmpliTube, Helix). Each running slot
		// is matched at most once, by plugin file, in chain order, so a tone with two copies of a plugin keeps two.
		// Plugins kept loaded as spares are next: no load, a moment to prepare (LoadSlot takes them).
		std::vector<int> reuse(entries.size(), -1);
		std::vector<bool> taken(m_slots.size(), false);
		std::vector<bool> fromSpare(entries.size(), false);
		std::vector<bool> spareUsed(m_spares.size(), false);
		int toLoad = 0;
		for (size_t e = 0; e < entries.size(); ++e)
		{
			for (size_t j = 0; j < m_slots.size() && reuse[e] < 0; ++j)
				if (!taken[j] && _wcsicmp(m_slots[j]->path.c_str(), entries[e].path.c_str()) == 0) { reuse[e] = static_cast<int>(j); taken[j] = true; }
			if (reuse[e] >= 0) continue;
			for (size_t k = 0; k < m_spares.size() && !fromSpare[e]; ++k)
				if (!spareUsed[k] && _wcsicmp(m_spares[k].path.c_str(), entries[e].path.c_str()) == 0) { spareUsed[k] = true; fromSpare[e] = true; }
			if (!fromSpare[e]) ++toLoad;
		}

		// Load the plugins that are new, while the old chain keeps playing; then swap in one step.
		std::vector<std::unique_ptr<Slot>> loaded(entries.size());
		m_loadTotal = std::max(1, toLoad);
		m_loadStep = 0;
		for (size_t e = 0; e < entries.size(); ++e)
		{
			Entry& entry = entries[e];
			if (reuse[e] >= 0) continue;
			if (!fromSpare[e]) ++m_loadStep;
			auto slot = std::make_unique<Slot>();
			std::string slotError;
			if (GetFileAttributesW(entry.path.c_str()) == INVALID_FILE_ATTRIBUTES || !LoadSlot(*slot, entry.path, slotError))
			{
				const size_t slash = entry.path.find_last_of(L"\\/");
				missing += (missing.empty() ? "" : ", ") + Utf8(slash == std::wstring::npos ? entry.path : entry.path.substr(slash + 1));
				continue;
			}
			slot->plugin->SetState(entry.state);
			slot->bypass = entry.bypass;
			loaded[e] = std::move(slot);
		}
		m_loadStep = m_loadTotal = 1;

		// Kept plugins take the tone's settings while they play (JUCE keeps state calls and processing apart).
		for (size_t e = 0; e < entries.size(); ++e)
			if (reuse[e] >= 0) m_slots[reuse[e]]->plugin->SetState(entries[e].state);

		Pause();
		std::vector<std::unique_ptr<Slot>> slots;
		for (size_t e = 0; e < entries.size(); ++e)
		{
			if (reuse[e] >= 0)
			{
				m_slots[reuse[e]]->bypass = entries[e].bypass;
				slots.push_back(std::move(m_slots[reuse[e]]));
			}
			else if (loaded[e]) slots.push_back(std::move(loaded[e]));
		}
		m_slots.swap(slots);   // `slots` now holds the plugins the new tone does not use (and the kept ones' empty places)
		Resume();
		for (auto& old : slots) if (old && old->plugin) Park(std::move(old->plugin), old->path);
		Log("tone: kept %d plugin(s) (%d of them spares), loaded %d", static_cast<int>(entries.size()) - toLoad,
			static_cast<int>(std::count(fromSpare.begin(), fromSpare.end(), true)), toLoad);
		m_structureDirty = false;   // levels[] not applied: the trims are global (see SaveTone)
		Log("tone: loaded %ls (%d plugin(s)%s%s%s)", file.c_str(), Count(), v1 ? ", converted from the pre-JUCE format" : "",
			missing.empty() ? "" : ", missing: ", missing.c_str());
		return true;
	}
}
