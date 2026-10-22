#pragma once

// One plugin, one slot of the chain (Chain.hpp), hosted through JUCE (Lib/JUCE, juce_audio_processors): loads it,
// runs it on the stream thread, shows its editor inside a window we own, and keeps its own settings per plugin (the
// starting point when the plugin is added to a chain).
//
// JUCE does the format work (VST3 component and controller, buses, parameter queues, state, editor scaling); this
// class only fits it to the host: stereo in and out at the game's block size, a stop-and-wait handshake so a plugin
// is never touched by the stream thread while it is swapped or unloaded, and an editor parented to our popout.
// Threads: everything but Process runs on the main thread, which is JUCE's message thread (Main.cpp).

#include "BridgeAudio.hpp"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace VstHost
{
	class PluginHost final : private juce::AudioProcessorListener, private juce::ComponentListener
	{
	public:
		PluginHost();
		~PluginHost() override;

		bool Load(const std::wstring& path, std::string& error);
		void Unload();
		bool Prepare(int frames, std::string& error);   // main thread; the stream may be running (it plays silence meanwhile)
		void Suspend();                                  // main thread; returns once the stream thread is out of the plugin

		// Editor in the child window `editor`, kept at the plugin's size; `onResized` lets the owner fit its frame
		// around it (on open and whenever the plugin resizes itself). An editor bigger than maxWidth x maxHeight
		// (0 = no limit) is shrunk to fit. A plugin without an editor of its own gets JUCE's generic one (sliders).
		bool OpenEditor(HWND editor, std::function<void(int, int)> onResized, int maxWidth = 0, int maxHeight = 0);
		void CloseEditor();

		bool SaveState();   // this plugin's own settings file
		// The plugin's full settings in memory, for tones (Chain.hpp): JUCE's state block (for VST3, the component
		// and controller states in a small XML).
		bool GetState(std::vector<char>& state);
		bool SetState(const std::vector<char>& state);
		// Tones saved before JUCE kept the two raw VST3 states; this wraps them the way JUCE stores them.
		static std::vector<char> StateFromVst3Parts(const std::vector<char>& component, const std::vector<char>& controller);
		bool IsDirty() const { return m_dirty; }
		void SaveIfDirty();
		bool TakeRestartRequest() { return false; }   // JUCE re-prepares the plugin itself when its layout changes

		const std::string& Name() const { return m_name; }
		// The delay the plugin reports for its processing (look-ahead, oversampling filters), in samples.
		int LatencySamples() const;
		bool HasEditor() const { return m_editor != nullptr; }

		// Stream thread. Stereo in and out; false (and silence) while the plugin is not ready.
		bool Process(const float* inLeft, const float* inRight, float* left, float* right, int frames);

	private:
		// AudioProcessorListener: any edit marks the plugin's settings as changed (autosave, tone "changed").
		void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override;
		void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override;
		// ComponentListener: the editor resized itself.
		void componentMovedOrResized(juce::Component&, bool wasMoved, bool wasResized) override;

		void ChooseBuses();
		bool LoadState();
		std::wstring StatePath() const;
		void FitWindowToEditor();

		class PlayHead;

		std::unique_ptr<juce::AudioPluginInstance> m_plugin;
		std::unique_ptr<juce::AudioProcessorEditor> m_editor;
		std::unique_ptr<PlayHead> m_playHead;
		HWND m_window = nullptr;
		std::function<void(int, int)> m_onEditorResized;

		juce::AudioBuffer<float> m_buffer;
		juce::MidiBuffer m_midi;
		int m_frames = 0;
		int m_inputChannels = 0;
		int m_outputChannels = 0;
		// The stream thread keeps calling Process while a plugin is swapped in place. Suspend clears m_processing,
		// then waits for a Process already running to leave (m_inProcess), so nothing touches a plugin being
		// unloaded. Both are sequentially consistent; that ordering is what makes the handshake hold.
		std::atomic<bool> m_processing{ false };
		std::atomic<bool> m_inProcess{ false };
		std::atomic<bool> m_dirty{ false };
		std::atomic<unsigned long long> m_quietUntil{ 0 };   // GetTickCount64: changes before this are the load's, not edits

		std::string m_name;
		std::string m_identifier;   // JUCE's PluginDescription identifier: names this plugin's settings file
		int m_legacyUid = 0;        // PluginDescription::deprecatedUid: finds its settings file from before JUCE
		bool LoadLegacyState();
	};

	// Plugin formats (VST3), shared by every slot. Main thread. Released before JUCE shuts down (Main.cpp).
	juce::AudioPluginFormatManager& PluginFormats();
	void ReleasePluginFormats();
}
