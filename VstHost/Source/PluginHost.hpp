#pragma once

// One VST3 plugin, one slot of the chain (Chain.hpp): loads it, runs it on the stream thread, shows its editor in a
// window we own, and keeps its own settings per plugin (the starting point when the plugin is added to a chain).

#include "BridgeAudio.hpp"

#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace VstHost
{
	class PluginHost final :
		public Steinberg::Vst::IComponentHandler,
		public Steinberg::Vst::IComponentHandler2,
		public Steinberg::IPlugFrame
	{
	public:
		PluginHost();
		~PluginHost();

		bool Load(const std::wstring& path, std::string& error);
		void Unload();
		bool Prepare(int frames, std::string& error);   // main thread; the stream may be running (it plays silence meanwhile)
		void Suspend();                                  // main thread; returns once the stream thread is out of the plugin

		// Editor in the child window `editor`, kept at the plugin's size; `onResized` lets the owner fit its frame
		// around it (on open and whenever the plugin resizes itself).
		// An editor bigger than maxWidth x maxHeight (0 = no limit) is shrunk to fit.
		bool OpenEditor(HWND editor, std::function<void(int, int)> onResized, int maxWidth = 0, int maxHeight = 0);
		void CloseEditor();

		bool SaveState();   // this plugin's own settings file
		// The plugin's full settings in memory, for tones (Chain.hpp).
		bool GetStateBlobs(std::vector<char>& component, std::vector<char>& controller);
		bool SetStateBlobs(std::vector<char> component, std::vector<char> controller);
		bool IsDirty() const { return m_dirty; }
		void SaveIfDirty();
		bool TakeRestartRequest() { return m_restartRequested.exchange(false); }

		const std::string& Name() const { return m_name; }
		bool HasEditor() const { return m_view != nullptr; }

		// Stream thread. Stereo in and out; false (and silence) while the plugin is not ready.
		bool Process(const float* inLeft, const float* inRight, float* left, float* right, int frames);

		// FUnknown. Lives as long as the process; the reference count only satisfies the interface.
		Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
		Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
		Steinberg::uint32 PLUGIN_API release() override { return 1; }

		// IComponentHandler
		Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value) override;
		Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { m_dirty = true; return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

		// IComponentHandler2
		Steinberg::tresult PLUGIN_API setDirty(Steinberg::TBool state) override { if (state) m_dirty = true; return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString) override { return Steinberg::kResultFalse; }
		Steinberg::tresult PLUGIN_API startGroupEdit() override { return Steinberg::kResultOk; }
		Steinberg::tresult PLUGIN_API finishGroupEdit() override { return Steinberg::kResultOk; }

		// IPlugFrame
		Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

	private:
		void SetupBuses();
		bool LoadState();
		std::wstring StatePath() const;
		void FitWindowTo(const Steinberg::ViewRect& rect);

		VST3::Hosting::Module::Ptr m_module;
		std::unique_ptr<Steinberg::Vst::PlugProvider> m_provider;
		Steinberg::IPtr<Steinberg::Vst::IComponent> m_component;
		Steinberg::IPtr<Steinberg::Vst::IEditController> m_controller;
		Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> m_processor;
		Steinberg::IPtr<Steinberg::IPlugView> m_view;
		HWND m_window = nullptr;
		std::function<void(int, int)> m_onEditorResized;

		Steinberg::Vst::HostProcessData m_data;
		Steinberg::Vst::ProcessContext m_context{};
		Steinberg::Vst::ParameterChanges m_inChanges;
		Steinberg::Vst::ParameterChangeTransfer m_editTransfer;   // editor (main thread) -> stream thread
		// The stream thread keeps calling Process while a plugin is swapped in place. Suspend clears m_processing,
		// then waits for a Process already running to leave (m_inProcess), so nothing touches a plugin being
		// unloaded. Both are sequentially consistent; that ordering is what makes the handshake hold.
		std::atomic<bool> m_processing{ false };
		std::atomic<bool> m_inProcess{ false };
		std::atomic<bool> m_dirty{ false };
		std::atomic<bool> m_restartRequested{ false };
		int m_inputChannels = 0;
		int m_outputChannels = 0;

		std::string m_name;
		std::string m_classId;
	};
}
