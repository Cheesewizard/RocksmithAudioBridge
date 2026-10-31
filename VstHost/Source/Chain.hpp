#pragma once

// The plugin chain: the guitar runs through up to kMaxSlots plugins in order (gate, drive, amp, cab, reverb...),
// each of which can be bypassed, with an input and an output trim around it. A chain with all its plugins'
// settings is a "tone", saved as a file (see SaveTone for the format).
//
// Plugins that leave the chain are kept loaded for a while as spares (stopped, not processing), so a tone that uses
// them again switches in a fraction of a second instead of the seconds a heavy amp sim takes to start. How many:
// the last `recent` plus, when the player asks for it, one of each starred plugin, loaded in the background
// (PreloadOne). Every load (Add, Replace, Duplicate, LoadTone) takes a spare of the same plugin first.
//
// Threads: the stream thread calls Process; everything else is the main thread. A structural change (add,
// remove, move, load a tone) pauses the stream first (it plays silence for that moment) and waits until the
// stream thread is out of the chain, so no plugin is ever touched by both threads at once.

#include "BridgeAudio.hpp"
#include "PluginHost.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace VstHost
{
	class Chain final : public AudioSink
	{
	public:
		static constexpr int kMaxSlots = 8;

		// Main thread. `frames` is the stream's block size (0 before audio starts; slots are prepared then).
		bool Add(const std::wstring& path, std::string& error);
		bool Replace(int index, const std::wstring& path, std::string& error);
		void Remove(int index);
		void Move(int index, int delta);
		void SetBypass(int index, bool bypass);
		bool Duplicate(int index, std::string& error);   // the copy, with the same settings, goes right after it
		// Puts a plugin in at `index` with these settings (empty: its own starting point) and bypass, taking a spare
		// of it when there is one. For undo and redo (Main.cpp): a plugin that left the chain comes back as it was.
		bool Insert(int index, const std::wstring& path, const std::vector<char>& state, bool bypass, std::string& error);
		// Unloads every copy of a plugin file (chain and spares), so JUCE releases its module (ExitDll, FreeLibrary),
		// then loads the chain's copies from scratch with their settings. For a plugin that only sees something new
		// (tones imported into it, a changed library) when it starts. The rest of the chain plays on without them
		// meanwhile. A copy that does not load again leaves the chain (`error`). `sparesDropped`: how many spares of
		// it were unloaded. `slots`: each copy's slot now, in chain order, -1 for one that left.
		bool Reload(const std::wstring& path, std::string& error, int& sparesDropped, std::vector<int>& slots);
		void Clear();         // the plugins stay loaded as spares
		void ReleaseAll();    // the chain and the spares, for shutdown (each plugin's settings saved)

		// Spares. `always` are plugin files kept loaded whatever `recent` says (one each).
		void SetKeep(int recent, std::vector<std::wstring> always);
		bool IsLoaded(const std::wstring& path) const;   // in the chain or a spare
		// Loads it as a spare. `recent`: counts as just used (kept by the recent limit); else as the oldest.
		bool PreloadOne(const std::wstring& path, std::string& error, bool recent = false);
		// The plugins a tone file uses that neither the chain nor a spare covers (a plugin used twice needs two).
		static std::vector<std::wstring> TonePlugins(const std::wstring& file);
		std::vector<std::wstring> NotLoaded(const std::vector<std::wstring>& plugins) const;
		int KeepRecent() const { return m_keepRecent; }
		void DropSpares(const std::wstring& path);
		int SpareCount() const { return static_cast<int>(m_spares.size()); }
		const std::wstring& SparePath(int index) const { return m_spares[index].path; }

		bool Prepare(int frames, std::string& error);   // every slot, for a (new) stream

		// Told before each plugin loads (a heavy one takes seconds): its file, and for a tone how far it has got.
		using LoadReporter = std::function<void(const std::wstring& path, int step, int total)>;
		void SetLoadReporter(LoadReporter reporter) { m_report = std::move(reporter); }
		void Suspend();

		int Count() const { return static_cast<int>(m_slots.size()); }
		PluginHost* Plugin(int index) { return index >= 0 && index < Count() ? m_slots[index]->plugin.get() : nullptr; }
		const std::wstring& Path(int index) const { return m_slots[index]->path; }
		bool Bypassed(int index) const { return m_slots[index]->bypass.load(); }

		void SetLevels(int inputTenths, int outputTenths);
		void SetMuted(bool muted) { m_muted = muted; }
		// Every plugin skipped (the clean guitar plays); each slot keeps its own bypass. Not part of the tone.
		void SetChainBypass(bool bypass) { m_chainBypass = bypass; }
		bool ChainBypassed() const { return m_chainBypass.load(); }
		int LatencySamples();   // what the playing slots report, summed (0 while the whole chain is bypassed)
		int InputTenths() const { return m_inputTenths; }
		int OutputTenths() const { return m_outputTenths; }
		float TakeInputPeak() { return m_inputPeak.exchange(0.0f); }
		float TakeOutputPeak() { return m_outputPeak.exchange(0.0f); }

		bool TakeRestartRequest();
		bool Dirty() const;              // a plugin's settings or the chain changed since the last save
		void SavePluginSettings();       // each plugin's own settings file (its starting point next time it is added)

		// Tones. `missing` lists plugins in the tone that are not installed (they are skipped).
		bool SaveTone(const std::wstring& file);
		bool LoadTone(const std::wstring& file, std::string& error, std::string& missing);

		// AudioSink, stream thread.
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		struct Slot
		{
			std::unique_ptr<PluginHost> plugin;
			std::wstring path;
			std::atomic<bool> bypass{ false };
		};

		struct Spare
		{
			std::unique_ptr<PluginHost> plugin;
			std::wstring path;
		};

		void Pause();
		void Resume();
		bool LoadSlot(Slot& slot, const std::wstring& path, std::string& error);
		bool HasSpare(const std::wstring& path) const;
		std::unique_ptr<PluginHost> TakeSpare(const std::wstring& path);
		void Park(std::unique_ptr<PluginHost> plugin, const std::wstring& path);   // saves its settings first
		void Trim();

		std::vector<std::unique_ptr<Slot>> m_slots;
		std::vector<Spare> m_spares;              // oldest first
		int m_keepRecent = 4;
		std::vector<std::wstring> m_keepAlways;
		LoadReporter m_report;
		int m_loadStep = 1, m_loadTotal = 1;   // LoadTone counts its plugins; one plugin otherwise
		int m_frames = 0;
		std::atomic<bool> m_running{ false };
		std::atomic<bool> m_inProcess{ false };
		bool m_structureDirty = false;
		bool m_reloading = false;   // Reload has copies out of the chain

		int m_inputTenths = 0;
		int m_outputTenths = 0;
		std::atomic<bool> m_muted{ false };
		std::atomic<bool> m_chainBypass{ false };
		std::atomic<float> m_inputGain{ 1.0f };
		std::atomic<float> m_outputGain{ 1.0f };
		std::atomic<float> m_inputPeak{ 0.0f };
		std::atomic<float> m_outputPeak{ 0.0f };
		std::vector<float> m_buffers[4];   // ping-pong stereo pairs
	};
}
