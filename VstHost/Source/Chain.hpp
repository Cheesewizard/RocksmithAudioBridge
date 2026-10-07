#pragma once

// The plugin chain: the guitar runs through up to kMaxSlots plugins in order (gate, drive, amp, cab, reverb...),
// each of which can be bypassed, with an input and an output trim around it. A chain with all its plugins'
// settings is a "tone", saved as a file (see SaveTone for the format).
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
		void Clear();

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

		void Pause();
		void Resume();
		bool LoadSlot(Slot& slot, const std::wstring& path, std::string& error);

		std::vector<std::unique_ptr<Slot>> m_slots;
		LoadReporter m_report;
		int m_loadStep = 1, m_loadTotal = 1;   // LoadTone counts its plugins; one plugin otherwise
		int m_frames = 0;
		std::atomic<bool> m_running{ false };
		std::atomic<bool> m_inProcess{ false };
		bool m_structureDirty = false;

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
