#include "PluginHost.hpp"

#include "HostLog.hpp"

#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"

#include <ShlObj.h>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace VstHost
{
	namespace
	{
		constexpr double kSampleRate = 48000.0;
		constexpr char kStateMagic[8] = { 'R', 'A', 'B', 'V', 'S', 'T', '1', 0 };

		std::string Utf8(const std::wstring& text)
		{
			if (text.empty()) return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(size), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::wstring Wide(const std::string& text)
		{
			if (text.empty()) return {};
			const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring out(static_cast<size_t>(size), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size);
			return out;
		}
	}

	PluginHost::PluginHost() : m_inChanges(1024), m_editTransfer(1024) {}

	PluginHost::~PluginHost() { Unload(); }

	tresult PLUGIN_API PluginHost::queryInterface(const TUID iid, void** obj)
	{
		if (FUnknownPrivate::iidEqual(iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual(iid, FUnknown::iid))
		{
			*obj = static_cast<IComponentHandler*>(this);
			return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(iid, IComponentHandler2::iid)) { *obj = static_cast<IComponentHandler2*>(this); return kResultOk; }
		if (FUnknownPrivate::iidEqual(iid, IPlugFrame::iid)) { *obj = static_cast<IPlugFrame*>(this); return kResultOk; }
		*obj = nullptr;
		return kNoInterface;
	}

	bool PluginHost::Load(const std::wstring& path, std::string& error)
	{
		Unload();
		m_module = VST3::Hosting::Module::create(Utf8(path), error);
		if (!m_module) { Log("load: %s", error.c_str()); return false; }

		const auto classes = m_module->getFactory().classInfos();
		const auto effect = std::find_if(classes.begin(), classes.end(),
			[](const VST3::Hosting::ClassInfo& info) { return info.category() == kVstAudioEffectClass; });
		if (effect == classes.end())
		{
			error = "This file has no audio plugin in it.";
			m_module.reset();
			return false;
		}
		m_name = effect->name();
		m_classId = effect->ID().toString();

		m_provider = std::make_unique<PlugProvider>(m_module->getFactory(), *effect, true);
		if (!m_provider->initialize())
		{
			error = "The plugin failed to start.";
			m_provider.reset();
			m_module.reset();
			return false;
		}
		m_component = m_provider->getComponentPtr();
		m_controller = m_provider->getControllerPtr();
		m_processor = FUnknownPtr<IAudioProcessor>(m_component);
		if (!m_component || !m_processor)
		{
			error = "The plugin has no audio processor.";
			Unload();
			return false;
		}
		if (m_controller) m_controller->setComponentHandler(this);

		SetupBuses();
		LoadState();
		Log("load: %s (%s), %d in / %d out channels", m_name.c_str(), m_classId.c_str(), m_inputChannels, m_outputChannels);
		return true;
	}

	void PluginHost::SetupBuses()
	{
		// Main audio buses only: the guitar into bus 0, bus 0 out to the game. Side-chains and MIDI stay off.
		for (const BusDirection dir : { kInput, kOutput })
		{
			const int32 audio = m_component->getBusCount(kAudio, dir);
			for (int32 i = 0; i < audio; ++i) m_component->activateBus(kAudio, dir, i, i == 0);
			const int32 events = m_component->getBusCount(kEvent, dir);
			for (int32 i = 0; i < events; ++i) m_component->activateBus(kEvent, dir, i, false);
		}

		const int32 inBuses = m_component->getBusCount(kAudio, kInput);
		const int32 outBuses = m_component->getBusCount(kAudio, kOutput);
		std::vector<SpeakerArrangement> ins(static_cast<size_t>(inBuses)), outs(static_cast<size_t>(outBuses));
		for (int32 i = 0; i < inBuses; ++i) m_processor->getBusArrangement(kInput, i, ins[i]);
		for (int32 i = 0; i < outBuses; ++i) m_processor->getBusArrangement(kOutput, i, outs[i]);

		// Stereo out is what the game mixes. Input: keep the plugin's own choice (guitar amps are often mono).
		if (outBuses > 0 && outs[0] != SpeakerArr::kStereo)
		{
			outs[0] = SpeakerArr::kStereo;
			if (m_processor->setBusArrangements(ins.data(), inBuses, outs.data(), outBuses) != kResultTrue)
				m_processor->getBusArrangement(kOutput, 0, outs[0]);
		}
		m_inputChannels = inBuses > 0 ? SpeakerArr::getChannelCount(ins[0]) : 0;
		m_outputChannels = outBuses > 0 ? SpeakerArr::getChannelCount(outs[0]) : 0;
	}

	bool PluginHost::Prepare(int frames, std::string& error)
	{
		Suspend();
		ProcessSetup setup{ kRealtime, kSample32, frames, kSampleRate };
		if (m_processor->setupProcessing(setup) != kResultOk)
		{
			error = "The plugin does not support 48 kHz / " + std::to_string(frames) + " samples.";
			return false;
		}
		if (!m_data.prepare(*m_component, frames, kSample32))
		{
			error = "Could not allocate the plugin's audio buffers.";
			return false;
		}
		m_data.numSamples = frames;
		m_context = {};
		m_context.sampleRate = kSampleRate;
		m_context.tempo = 120.0;
		m_context.timeSigNumerator = 4;
		m_context.timeSigDenominator = 4;
		m_context.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid;
		m_data.processContext = &m_context;
		m_data.inputParameterChanges = &m_inChanges;
		m_component->setActive(true);
		m_processor->setProcessing(true);
		m_processing = true;
		return true;
	}

	void PluginHost::Suspend()
	{
		if (!m_processor) return;
		if (m_processing.exchange(false))
		{
			m_processor->setProcessing(false);
			m_component->setActive(false);
		}
		m_data.unprepare();
	}

	void PluginHost::Process(const float* guitar, float* left, float* right, int frames)
	{
		if (!m_processing.load(std::memory_order_acquire) || frames != m_data.numSamples)
		{
			std::fill(left, left + frames, 0.0f);
			std::fill(right, right + frames, 0.0f);
			return;
		}

		if (m_data.numInputs > 0)
			for (int32 ch = 0; ch < m_data.inputs[0].numChannels; ++ch)
				std::memcpy(m_data.inputs[0].channelBuffers32[ch], guitar, sizeof(float) * frames);

		m_editTransfer.transferChangesTo(m_inChanges);
		m_processor->process(m_data);
		m_inChanges.clearQueue();
		m_context.projectTimeSamples += frames;

		if (m_data.numOutputs > 0 && m_data.outputs[0].numChannels > 0)
		{
			const AudioBusBuffers& out = m_data.outputs[0];
			std::memcpy(left, out.channelBuffers32[0], sizeof(float) * frames);
			std::memcpy(right, out.channelBuffers32[out.numChannels > 1 ? 1 : 0], sizeof(float) * frames);
		}
		else
		{
			std::fill(left, left + frames, 0.0f);
			std::fill(right, right + frames, 0.0f);
		}
	}

	tresult PLUGIN_API PluginHost::performEdit(ParamID id, ParamValue value)
	{
		m_editTransfer.addChange(id, value, 0);
		m_dirty = true;
		return kResultOk;
	}

	tresult PLUGIN_API PluginHost::restartComponent(int32 flags)
	{
		Log("plugin asked for restart, flags 0x%X", static_cast<unsigned>(flags));
		if (flags & (kReloadComponent | kIoChanged | kLatencyChanged)) m_restartRequested = true;
		if (flags & kParamValuesChanged) m_dirty = true;
		return kResultOk;
	}

	bool PluginHost::OpenEditor(HWND window, std::function<void(int, int)> onResized)
	{
		if (!m_controller) return false;
		m_window = window;
		m_onEditorResized = std::move(onResized);
		m_view = owned(m_controller->createView(ViewType::kEditor));
		if (!m_view || m_view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue)
		{
			Log("editor: the plugin has no window editor");
			m_view = nullptr;
			return false;
		}
		m_view->setFrame(this);
		if (FUnknownPtr<IPlugViewContentScaleSupport> scale = FUnknownPtr<IPlugViewContentScaleSupport>(m_view))
			scale->setContentScaleFactor(static_cast<float>(GetDpiForWindow(window)) / 96.0f);

		ViewRect size{};
		if (m_view->getSize(&size) == kResultOk) FitWindowTo(size);
		if (m_view->attached(window, kPlatformTypeHWND) != kResultOk)
		{
			Log("editor: attach failed");
			m_view->setFrame(nullptr);
			m_view = nullptr;
			return false;
		}
		Log("editor: open, %dx%d, %s", size.getWidth(), size.getHeight(), m_view->canResize() == kResultTrue ? "resizable" : "fixed size");
		return true;
	}

	void PluginHost::CloseEditor()
	{
		if (!m_view) return;
		m_view->removed();
		m_view->setFrame(nullptr);
		m_view = nullptr;
	}

	void PluginHost::FitWindowTo(const ViewRect& rect)
	{
		if (!m_window) return;
		SetWindowPos(m_window, nullptr, 0, 0, rect.getWidth(), rect.getHeight(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (m_onEditorResized) m_onEditorResized(rect.getWidth(), rect.getHeight());
	}

	tresult PLUGIN_API PluginHost::resizeView(IPlugView* view, ViewRect* newSize)
	{
		if (!view || !newSize || view != m_view.get()) return kInvalidArgument;
		FitWindowTo(*newSize);
		view->onSize(newSize);
		return kResultTrue;
	}

	std::wstring PluginHost::StatePath() const
	{
		PWSTR appData = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
		std::wstring dir = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost";
		CoTaskMemFree(appData);
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
		return dir + L"\\" + Wide(m_classId) + L".state";
	}

	// File: magic, then the component state and the controller state, each as a uint32 length plus bytes.
	bool PluginHost::SaveState()
	{
		if (!m_component) return false;
		IPtr<MemoryStream> componentState = owned(new MemoryStream());
		IPtr<MemoryStream> controllerState = owned(new MemoryStream());
		if (m_component->getState(componentState) != kResultOk) { Log("state: the plugin would not save its settings"); return false; }
		if (m_controller) m_controller->getState(controllerState);

		const std::wstring path = StatePath();
		const std::wstring temp = path + L".tmp";
		FILE* file = nullptr;
		if (path.empty() || _wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file) { Log("state: cannot write %ls", temp.c_str()); return false; }
		fwrite(kStateMagic, 1, sizeof(kStateMagic), file);
		for (MemoryStream* stream : { componentState.get(), controllerState.get() })
		{
			int64 used = 0;
			stream->tell(&used);
			const uint32_t size = static_cast<uint32_t>(used);
			fwrite(&size, sizeof(size), 1, file);
			if (size) fwrite(stream->getData(), 1, size, file);
		}
		const bool ok = fclose(file) == 0 && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (ok) m_dirty = false;
		Log("state: %s %ls", ok ? "saved" : "save FAILED", path.c_str());
		return ok;
	}

	void PluginHost::SaveIfDirty()
	{
		if (m_dirty) SaveState();
	}

	bool PluginHost::LoadState()
	{
		const std::wstring path = StatePath();
		FILE* file = nullptr;
		if (path.empty() || _wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
		std::vector<char> parts[2];
		char magic[sizeof(kStateMagic)]{};
		bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic) && std::memcmp(magic, kStateMagic, sizeof(magic)) == 0;
		for (auto& part : parts)
		{
			uint32_t size = 0;
			ok = ok && fread(&size, sizeof(size), 1, file) == 1 && size < (256u << 20);
			if (!ok) break;
			part.resize(size);
			ok = size == 0 || fread(part.data(), 1, size, file) == size;
		}
		fclose(file);
		if (!ok) { Log("state: %ls is damaged, starting from the plugin's defaults", path.c_str()); return false; }

		IPtr<MemoryStream> componentState = owned(new MemoryStream(parts[0].data(), static_cast<TSize>(parts[0].size())));
		if (m_component->setState(componentState) != kResultOk) { Log("state: the plugin refused its saved settings"); return false; }
		if (m_controller)
		{
			componentState->seek(0, IBStream::kIBSeekSet, nullptr);
			m_controller->setComponentState(componentState);
			if (!parts[1].empty())
			{
				IPtr<MemoryStream> controllerState = owned(new MemoryStream(parts[1].data(), static_cast<TSize>(parts[1].size())));
				m_controller->setState(controllerState);
			}
		}
		Log("state: restored from %ls", path.c_str());
		return true;
	}

	void PluginHost::Unload()
	{
		CloseEditor();
		Suspend();
		if (m_controller) m_controller->setComponentHandler(nullptr);
		m_processor = nullptr;
		m_controller = nullptr;
		m_component = nullptr;
		m_provider.reset();   // terminates and disconnects the plugin
		m_module.reset();
	}
}
