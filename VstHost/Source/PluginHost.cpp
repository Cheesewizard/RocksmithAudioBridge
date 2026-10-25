#include "PluginHost.hpp"

#include "HostLog.hpp"
#include "AudioFormat.hpp"

#include <ShlObj.h>
#include <algorithm>
#include <cstring>

namespace VstHost
{
	namespace
	{
		constexpr char kStateMagic[8] = { 'R', 'A', 'B', 'J', 'S', 'T', '1', 0 };
		// Loading settings makes a plugin report parameter changes for a moment; those are not the player's edits.
		constexpr ULONGLONG kSettleMs = 750;

		std::unique_ptr<juce::AudioPluginFormatManager> g_formats;

		std::wstring Wide(const juce::String& text) { return text.toWideCharPointer(); }

		std::wstring StateFolder()
		{
			PWSTR appData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData))) return {};
			std::wstring dir = std::wstring(appData) + L"\\RocksmithAudioBridge\\VstHost\\PluginStates";
			CoTaskMemFree(appData);
			SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
			return dir;
		}
	}

	juce::AudioPluginFormatManager& PluginFormats()
	{
		if (!g_formats)
		{
			g_formats = std::make_unique<juce::AudioPluginFormatManager>();
			juce::addDefaultFormatsToManager(*g_formats);
		}
		return *g_formats;
	}

	void ReleasePluginFormats() { g_formats.reset(); }

	// A steady transport for plugins that ask (tempo-synced delays, LFOs): 120 BPM, 4/4, always playing.
	class PluginHost::PlayHead final : public juce::AudioPlayHead
	{
	public:
		juce::Optional<PositionInfo> getPosition() const override
		{
			PositionInfo info;
			info.setBpm(120.0);
			info.setTimeSignature(TimeSignature{ 4, 4 });
			info.setIsPlaying(true);
			info.setTimeInSamples(samples);
			info.setTimeInSeconds(static_cast<double>(samples) / SAMPLE_RATE);
			return info;
		}
		int64_t samples = 0;   // stream thread
	};

	PluginHost::PluginHost() : m_playHead(std::make_unique<PlayHead>()) {}

	PluginHost::~PluginHost() { Unload(); }

	bool PluginHost::Load(const std::wstring& path, std::string& error)
	{
		Unload();
		m_dirty = false;

		// A file can hold several plugins (a bundle with an effect and an instrument): take the first effect.
		juce::OwnedArray<juce::PluginDescription> types;
		const juce::String file(path.c_str());
		for (juce::AudioPluginFormat* format : PluginFormats().getFormats())
			if (format->fileMightContainThisPluginType(file)) format->findAllTypesForFile(types, file);
		if (types.isEmpty())
		{
			error = "This file has no plugin in it that can be loaded.";
			return false;
		}
		const juce::PluginDescription* chosen = types[0];
		for (const juce::PluginDescription* type : types)
			if (!type->isInstrument) { chosen = type; break; }

		juce::String message;
		m_plugin = PluginFormats().createPluginInstance(*chosen, SAMPLE_RATE, std::max(m_frames, 512), message);
		if (!m_plugin)
		{
			error = message.isNotEmpty() ? message.toStdString() : "The plugin failed to start.";
			Log("load: %ls failed: %s", path.c_str(), error.c_str());
			return false;
		}
		m_name = chosen->name.toStdString();
		m_identifier = chosen->createIdentifierString().toStdString();
		m_legacyUid = chosen->pluginFormatName == "VST3" ? chosen->deprecatedUid : 0;
		m_plugin->setPlayHead(m_playHead.get());
		m_plugin->addListener(this);
		ChooseBuses();
		LoadState();
		m_quietUntil = GetTickCount64() + kSettleMs;
		Log("load: %s (%s, %s by %s), %d in / %d out channels", m_name.c_str(), m_identifier.c_str(),
			chosen->pluginFormatName.toRawUTF8(), chosen->manufacturerName.toRawUTF8(), m_inputChannels, m_outputChannels);
		return true;
	}

	// Main buses only: the guitar into input bus 0, output bus 0 to the game. Side-chains and extra outputs stay
	// off. Layouts are tried in order until the plugin takes one (mono in / stereo out suits a guitar amp, then
	// stereo / stereo, mono / mono, stereo / mono); a plugin's default can have its main buses switched off
	// (Overloud TH3 opened with 0 in / 0 out), which would silence the chain. A plugin still without an output
	// is skipped by Process, so the chain passes the signal through it.
	void PluginHost::ChooseBuses()
	{
		const juce::AudioProcessor::BusesLayout start = m_plugin->getBusesLayout();
		auto with = [&](const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
		{
			juce::AudioProcessor::BusesLayout layout = start;
			for (int i = 0; i < layout.inputBuses.size(); ++i) layout.inputBuses.getReference(i) = i == 0 ? in : juce::AudioChannelSet::disabled();
			for (int i = 0; i < layout.outputBuses.size(); ++i) layout.outputBuses.getReference(i) = i == 0 ? out : juce::AudioChannelSet::disabled();
			return layout;
		};
		const juce::AudioChannelSet mono = juce::AudioChannelSet::mono(), stereo = juce::AudioChannelSet::stereo();
		const std::pair<juce::AudioChannelSet, juce::AudioChannelSet> tries[] = { { mono, stereo }, { stereo, stereo }, { mono, mono }, { stereo, mono } };
		bool set = false;
		for (const auto& [in, out] : tries)
			if (m_plugin->setBusesLayout(with(in, out))) { set = true; break; }
		if (!set)
		{
			// None taken: keep its own layout without the extra buses, with its main buses on.
			juce::AudioProcessor::BusesLayout layout = start;
			for (int i = 1; i < layout.inputBuses.size(); ++i) layout.inputBuses.getReference(i) = juce::AudioChannelSet::disabled();
			for (int i = 1; i < layout.outputBuses.size(); ++i) layout.outputBuses.getReference(i) = juce::AudioChannelSet::disabled();
			if (!m_plugin->setBusesLayout(layout)) m_plugin->enableAllBuses();
		}
		m_inputChannels = m_plugin->getMainBusNumInputChannels();
		m_outputChannels = m_plugin->getMainBusNumOutputChannels();
		if (m_outputChannels == 0) Log("load: %s has no audio output it will use; it is skipped in the chain", m_name.c_str());
	}

	bool PluginHost::Prepare(int frames, std::string& error)
	{
		Suspend();
		if (!m_plugin) { error = "No plugin loaded."; return false; }
		m_frames = frames;
		m_plugin->setRateAndBufferSizeDetails(SAMPLE_RATE, frames);
		m_plugin->prepareToPlay(SAMPLE_RATE, frames);
		const int channels = std::max({ m_plugin->getTotalNumInputChannels(), m_plugin->getTotalNumOutputChannels(), 2 });
		m_buffer.setSize(channels, frames, false, true, false);
		m_midi.ensureSize(256);
		m_processing = true;
		return true;
	}

	void PluginHost::Suspend()
	{
		if (!m_plugin) return;
		const bool wasProcessing = m_processing.exchange(false);
		while (m_inProcess.load()) YieldProcessor();   // one block at most
		if (wasProcessing) m_plugin->releaseResources();
	}

	int PluginHost::LatencySamples() const { return m_plugin ? (std::max)(0, m_plugin->getLatencySamples()) : 0; }

	bool PluginHost::Process(const float* inLeft, const float* inRight, float* left, float* right, int frames)
	{
		struct InProcess
		{
			std::atomic<bool>& flag;
			explicit InProcess(std::atomic<bool>& f) : flag(f) { flag.store(true); }
			~InProcess() { flag.store(false); }
		} inProcess(m_inProcess);

		if (!m_processing.load() || frames != m_frames || m_outputChannels == 0)
		{
			std::fill(left, left + frames, 0.0f);
			std::fill(right, right + frames, 0.0f);
			return false;
		}

		// Stereo in: left and right to the first two input channels; a mono input takes the left (the guitar is
		// mono, so both sides are the same until a stereo plugin earlier in the chain widens it).
		for (int ch = 0; ch < m_buffer.getNumChannels(); ++ch)
		{
			float* channel = m_buffer.getWritePointer(ch);
			if (ch < m_inputChannels) std::memcpy(channel, ch == 1 ? inRight : inLeft, sizeof(float) * frames);
			else std::fill(channel, channel + frames, 0.0f);
		}
		m_midi.clear();
		m_plugin->processBlock(m_buffer, m_midi);
		m_playHead->samples += frames;

		if (m_outputChannels > 0)
		{
			std::memcpy(left, m_buffer.getReadPointer(0), sizeof(float) * frames);
			std::memcpy(right, m_buffer.getReadPointer(m_outputChannels > 1 ? 1 : 0), sizeof(float) * frames);
		}
		else
		{
			std::fill(left, left + frames, 0.0f);
			std::fill(right, right + frames, 0.0f);
		}
		return true;
	}

	void PluginHost::audioProcessorParameterChanged(juce::AudioProcessor*, int, float)
	{
		if (GetTickCount64() >= m_quietUntil) m_dirty = true;
	}

	void PluginHost::audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details)
	{
		if ((details.programChanged || details.nonParameterStateChanged) && GetTickCount64() >= m_quietUntil) m_dirty = true;
	}

	bool PluginHost::OpenEditor(HWND window, std::function<void(int, int)> onResized, int maxWidth, int maxHeight)
	{
		if (!m_plugin) return false;
		CloseEditor();
		m_window = window;
		m_onEditorResized = std::move(onResized);
		juce::AudioProcessorEditor* editor = m_plugin->hasEditor() ? m_plugin->createEditorAndMakeActive() : nullptr;
		if (!editor) editor = new juce::GenericAudioProcessorEditor(*m_plugin);
		m_editor.reset(editor);
		m_editor->setTopLeftPosition(0, 0);
		m_editor->addToDesktop(0, window);   // a child window of our popout's editor area
		// The window shows only the plugin's own interface, which draws itself; JUCE 9 would draw its part with
		// Direct2D (the GPU). Software drawing leaves the GPU to the plugin, so a crash on a graphics-driver thread
		// is the plugin's (Main.cpp OnCrash), and JUCE's own GPU threads are out of the picture.
		if (juce::ComponentPeer* peer = m_editor->getPeer()) peer->setCurrentRenderingEngine(0);   // 0 = software (GDI)
		m_editor->setVisible(true);

		RECT rect{};
		GetWindowRect(static_cast<HWND>(m_editor->getWindowHandle()), &rect);
		const int width = rect.right - rect.left, height = rect.bottom - rect.top;
		if (maxWidth > 0 && maxHeight > 0 && width > 0 && height > 0 && (width > maxWidth || height > maxHeight))
		{
			// Too big for the screen (AmpliTube opens at 1710x1050): shrink it. A resizable editor takes a smaller
			// size; others get a smaller scale (JUCE passes it on to the plugin).
			const float fit = std::min(static_cast<float>(maxWidth) / width, static_cast<float>(maxHeight) / height);
			if (m_editor->isResizable())
				m_editor->setSize(static_cast<int>(m_editor->getWidth() * fit), static_cast<int>(m_editor->getHeight() * fit));
			else
				m_editor->setScaleFactor(fit);
			Log("editor: scaled to fit the screen (by %.2f)", fit);
		}
		m_editor->addComponentListener(this);
		FitWindowToEditor();
		Log("editor: open, %s%s", m_plugin->hasEditor() ? "the plugin's own" : "generic (the plugin has none)",
			m_editor->isResizable() ? ", resizable" : "");
		return true;
	}

	void PluginHost::CloseEditor()
	{
		if (!m_editor) return;
		m_editor->removeComponentListener(this);
		m_editor.reset();   // its destructor tells the plugin (editorBeingDeleted)
	}

	void PluginHost::componentMovedOrResized(juce::Component&, bool, bool wasResized)
	{
		if (wasResized) FitWindowToEditor();
	}

	// Our editor area takes the editor's size in physical pixels (the popout around it follows, onResized).
	void PluginHost::FitWindowToEditor()
	{
		if (!m_window || !m_editor) return;
		RECT rect{};
		GetWindowRect(static_cast<HWND>(m_editor->getWindowHandle()), &rect);
		const int width = rect.right - rect.left, height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0) return;
		SetWindowPos(m_window, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (m_onEditorResized) m_onEditorResized(width, height);
	}

	std::wstring PluginHost::StatePath() const
	{
		const std::wstring folder = StateFolder();
		if (folder.empty() || m_identifier.empty()) return {};
		std::wstring name = Wide(juce::String(m_identifier));
		for (wchar_t& c : name) if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
		return folder + L"\\" + name + L".state";
	}

	bool PluginHost::GetState(std::vector<char>& state)
	{
		state.clear();
		if (!m_plugin) return false;
		juce::MemoryBlock block;
		m_plugin->getStateInformation(block);
		if (block.getSize() == 0) { Log("state: %s gave no settings", m_name.c_str()); return false; }
		const char* data = static_cast<const char*>(block.getData());
		state.assign(data, data + block.getSize());
		return true;
	}

	bool PluginHost::SetState(const std::vector<char>& state)
	{
		if (!m_plugin || state.empty()) return false;
		m_quietUntil = GetTickCount64() + kSettleMs;
		m_plugin->setStateInformation(state.data(), static_cast<int>(state.size()));
		return true;
	}

	std::vector<char> PluginHost::StateFromVst3Parts(const std::vector<char>& component, const std::vector<char>& controller)
	{
		// The same XML JUCE's VST3 instance writes in getStateInformation (juce_VST3PluginFormatImpl.h).
		juce::XmlElement xml("VST3PluginState");
		if (!component.empty())
			xml.createNewChildElement("IComponent")->addTextElement(juce::MemoryBlock(component.data(), component.size()).toBase64Encoding());
		if (!controller.empty())
			xml.createNewChildElement("IEditController")->addTextElement(juce::MemoryBlock(controller.data(), controller.size()).toBase64Encoding());
		juce::MemoryBlock block;
		juce::AudioProcessor::copyXmlToBinary(xml, block);
		const char* data = static_cast<const char*>(block.getData());
		return { data, data + block.getSize() };
	}

	// File: magic, then the state as a uint32 length plus bytes.
	bool PluginHost::SaveState()
	{
		std::vector<char> state;
		if (!GetState(state)) return false;
		const std::wstring path = StatePath();
		const std::wstring temp = path + L".tmp";
		FILE* file = nullptr;
		if (path.empty() || _wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file) { Log("state: cannot write %ls", temp.c_str()); return false; }
		fwrite(kStateMagic, 1, sizeof(kStateMagic), file);
		const uint32_t size = static_cast<uint32_t>(state.size());
		fwrite(&size, sizeof(size), 1, file);
		fwrite(state.data(), 1, state.size(), file);
		const bool ok = fclose(file) == 0 && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
		if (ok) m_dirty = false;
		Log("state: %s %ls", ok ? "saved" : "save FAILED", path.c_str());
		return ok;
	}

	void PluginHost::SaveIfDirty()
	{
		if (m_dirty) SaveState();
	}

	// Before JUCE, each VST3 plugin's settings were kept as %APPDATA%\RocksmithAudioBridge\VstHost\<class id>.state
	// (magic "RABVST1", then the component and controller states, each a uint32 length plus bytes), the class id
	// as 32 hex digits in byte order. JUCE does not expose a plugin's class id, but its deprecatedUid is a hash of
	// those 16 bytes (getHashForRange in juce_VST3PluginFormatImpl.h: value * 31 + byte, bytes as signed char), so
	// the old file is found by hashing each name. Converted once; the new file then takes over.
	bool PluginHost::LoadLegacyState()
	{
		if (m_legacyUid == 0) return false;
		const std::wstring folder = StateFolder();
		if (folder.empty()) return false;
		const std::wstring legacyFolder = folder.substr(0, folder.find_last_of(L'\\'));
		WIN32_FIND_DATAW found{};
		HANDLE search = FindFirstFileW((legacyFolder + L"\\*.state").c_str(), &found);
		if (search == INVALID_HANDLE_VALUE) return false;
		std::wstring match;
		do
		{
			const std::wstring name = found.cFileName;
			if (name.size() != 32 + 6 || !std::all_of(name.begin(), name.begin() + 32, iswxdigit)) continue;
			// The name is the SDK's UID::toString in COM format (the Windows default): Data1 (4 bytes), Data2 and
			// Data3 (2 each) printed as numbers, stored little-endian in the class id; then Data4's 8 bytes in order.
			unsigned char bytes[16]{};
			const unsigned long data1 = wcstoul(name.substr(0, 8).c_str(), nullptr, 16);
			const unsigned long data2 = wcstoul(name.substr(8, 4).c_str(), nullptr, 16);
			const unsigned long data3 = wcstoul(name.substr(12, 4).c_str(), nullptr, 16);
			for (int i = 0; i < 4; ++i) bytes[i] = static_cast<unsigned char>(data1 >> (8 * i));
			for (int i = 0; i < 2; ++i) bytes[4 + i] = static_cast<unsigned char>(data2 >> (8 * i));
			for (int i = 0; i < 2; ++i) bytes[6 + i] = static_cast<unsigned char>(data3 >> (8 * i));
			for (int i = 0; i < 8; ++i) bytes[8 + i] = static_cast<unsigned char>(wcstoul(name.substr(16 + i * 2, 2).c_str(), nullptr, 16));
			uint32_t hash = 0;
			for (unsigned char byte : bytes) hash = hash * 31 + static_cast<uint32_t>(static_cast<int>(static_cast<signed char>(byte)));
			if (static_cast<int>(hash) == m_legacyUid) match = legacyFolder + L"\\" + name;
		} while (match.empty() && FindNextFileW(search, &found));
		FindClose(search);
		if (match.empty()) return false;

		FILE* file = nullptr;
		if (_wfopen_s(&file, match.c_str(), L"rb") != 0 || !file) return false;
		constexpr char kLegacyMagic[8] = { 'R', 'A', 'B', 'V', 'S', 'T', '1', 0 };
		char magic[sizeof(kLegacyMagic)]{};
		std::vector<char> parts[2];
		bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic) && std::memcmp(magic, kLegacyMagic, sizeof(magic)) == 0;
		for (auto& part : parts)
		{
			uint32_t size = 0;
			ok = ok && fread(&size, sizeof(size), 1, file) == 1 && size < (256u << 20);
			if (!ok) break;
			part.resize(size);
			ok = size == 0 || fread(part.data(), 1, size, file) == size;
		}
		fclose(file);
		if (!ok || parts[0].empty()) return false;
		SetState(StateFromVst3Parts(parts[0], parts[1]));
		SaveState();
		Log("state: converted %ls from the pre-JUCE format", match.c_str());
		return true;
	}

	bool PluginHost::LoadState()
	{
		const std::wstring path = StatePath();
		FILE* file = nullptr;
		if (!path.empty() && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return LoadLegacyState();
		if (path.empty() || _wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;
		char magic[sizeof(kStateMagic)]{};
		uint32_t size = 0;
		std::vector<char> state;
		bool ok = fread(magic, 1, sizeof(magic), file) == sizeof(magic) && std::memcmp(magic, kStateMagic, sizeof(magic)) == 0
			&& fread(&size, sizeof(size), 1, file) == 1 && size > 0 && size < (256u << 20);
		if (ok)
		{
			state.resize(size);
			ok = fread(state.data(), 1, size, file) == size;
		}
		fclose(file);
		if (!ok) { Log("state: %ls is damaged, starting from the plugin's defaults", path.c_str()); return false; }
		SetState(state);
		Log("state: restored from %ls", path.c_str());
		return true;
	}

	void PluginHost::Unload()
	{
		CloseEditor();
		Suspend();
		if (m_plugin) m_plugin->removeListener(this);
		m_plugin.reset();
		m_processing = false;
	}
}
