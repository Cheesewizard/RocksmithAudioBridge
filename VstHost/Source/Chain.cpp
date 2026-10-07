#include "Chain.hpp"

#include "HostLog.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace VstHost
{
	namespace
	{
		constexpr char kToneMagic[8] = { 'R', 'A', 'B', 'T', 'O', 'N', 'E', '1' };

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

	bool Chain::LoadSlot(Slot& slot, const std::wstring& path, std::string& error)
	{
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
		slot->plugin->SaveState();   // the replaced plugin keeps its settings for next time
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
		removed->plugin->SaveState();
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
		std::vector<char> component, controller;
		m_slots[index]->plugin->GetStateBlobs(component, controller);
		auto slot = std::make_unique<Slot>();
		if (!LoadSlot(*slot, m_slots[index]->path, error)) return false;
		slot->plugin->SetStateBlobs(std::move(component), std::move(controller));   // before it plays: no stream thread yet
		slot->bypass = m_slots[index]->bypass.load();
		Pause();
		m_slots.insert(m_slots.begin() + index + 1, std::move(slot));
		m_structureDirty = true;
		Resume();
		Log("chain: duplicated slot %d (%d in the chain)", index + 1, Count());
		return true;
	}

	void Chain::Clear()
	{
		Pause();
		std::vector<std::unique_ptr<Slot>> old;
		old.swap(m_slots);
		m_structureDirty = true;
		Resume();
		for (auto& slot : old) slot->plugin->SaveState();
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

	// Tone file: magic "RABTONE1", input and output trim (int32 tenths of a dB), slot count (uint32), then per slot
	// the plugin path (uint32 length in UTF-16 units + text), bypass (uint8), and the plugin's component and
	// controller settings (uint32 length + bytes each).
	bool Chain::SaveTone(const std::wstring& file)
	{
		const std::wstring temp = file + L".tmp";
		FILE* out = nullptr;
		if (_wfopen_s(&out, temp.c_str(), L"wb") != 0 || !out) { Log("tone: cannot write %ls", temp.c_str()); return false; }
		fwrite(kToneMagic, 1, sizeof(kToneMagic), out);
		const int32_t levels[2] = { m_inputTenths, m_outputTenths };
		fwrite(levels, sizeof(levels), 1, out);
		const uint32_t count = static_cast<uint32_t>(m_slots.size());
		fwrite(&count, sizeof(count), 1, out);
		for (auto& slot : m_slots)
		{
			WriteBlob(out, slot->path.data(), static_cast<uint32_t>(slot->path.size() * sizeof(wchar_t)));
			const uint8_t bypass = slot->bypass ? 1 : 0;
			fwrite(&bypass, 1, 1, out);
			std::vector<char> component, controller;
			slot->plugin->GetStateBlobs(component, controller);
			WriteBlob(out, component.data(), static_cast<uint32_t>(component.size()));
			WriteBlob(out, controller.data(), static_cast<uint32_t>(controller.size()));
		}
		const bool ok = fclose(out) == 0 && MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (ok) m_structureDirty = false;
		Log("tone: %s %ls (%u plugin(s))", ok ? "saved" : "save FAILED", file.c_str(), count);
		return ok;
	}

	bool Chain::LoadTone(const std::wstring& file, std::string& error, std::string& missing)
	{
		FILE* in = nullptr;
		if (_wfopen_s(&in, file.c_str(), L"rb") != 0 || !in) { error = "The tone file could not be opened."; return false; }
		char magic[sizeof(kToneMagic)]{};
		int32_t levels[2]{};
		uint32_t count = 0;
		struct Entry { std::wstring path; bool bypass = false; std::vector<char> component, controller; };
		std::vector<Entry> entries;
		bool ok = fread(magic, 1, sizeof(magic), in) == sizeof(magic) && std::memcmp(magic, kToneMagic, sizeof(magic)) == 0
			&& fread(levels, sizeof(levels), 1, in) == 1 && fread(&count, sizeof(count), 1, in) == 1 && count <= kMaxSlots;
		for (uint32_t i = 0; ok && i < count; ++i)
		{
			Entry entry;
			std::vector<char> path;
			uint8_t bypass = 0;
			ok = ReadBlob(in, path) && fread(&bypass, 1, 1, in) == 1 && ReadBlob(in, entry.component) && ReadBlob(in, entry.controller);
			if (!ok) break;
			entry.path.assign(reinterpret_cast<const wchar_t*>(path.data()), path.size() / sizeof(wchar_t));
			entry.bypass = bypass != 0;
			entries.push_back(std::move(entry));
		}
		fclose(in);
		if (!ok) { error = "The tone file is damaged."; return false; }

		// Load the new plugins first, while the old chain keeps playing; then swap in one step.
		std::vector<std::unique_ptr<Slot>> slots;
		for (Entry& entry : entries)
		{
			auto slot = std::make_unique<Slot>();
			std::string slotError;
			if (GetFileAttributesW(entry.path.c_str()) == INVALID_FILE_ATTRIBUTES || !LoadSlot(*slot, entry.path, slotError))
			{
				const size_t slash = entry.path.find_last_of(L"\\/");
				missing += (missing.empty() ? "" : ", ") + Utf8(slash == std::wstring::npos ? entry.path : entry.path.substr(slash + 1));
				continue;
			}
			slot->plugin->SetStateBlobs(std::move(entry.component), std::move(entry.controller));
			slot->bypass = entry.bypass;
			slots.push_back(std::move(slot));
		}

		Pause();
		m_slots.swap(slots);
		Resume();
		for (auto& old : slots) old->plugin->SaveState();
		SetLevels(levels[0], levels[1]);
		m_structureDirty = false;
		Log("tone: loaded %ls (%d plugin(s)%s%s)", file.c_str(), Count(), missing.empty() ? "" : ", missing: ", missing.c_str());
		return true;
	}
}
