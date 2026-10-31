#include "GuitarTonePage.hpp"

#include "StompboxPage.hpp"
#include "ToneDesignerPage.hpp"
#include "../GameTones.hpp"
#include "../SongLibrary.hpp"
#include "../StompboxLink.hpp"
#include "../VstHostLink.hpp"
#include "../../ExternalAmp/ExternalAmp.hpp"
#include "../../Shell/OverlayUi.hpp"
#include "../../Shell/ShellPages.hpp"
#include "../../Shell/Pages/PageCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// The Guitar Tone page, ported from 4.x's OverlayShell.cpp as it was: the Game amp, Tone designer, VST rig and
// External app tabs.
namespace Overlay
{
	namespace
	{
		using AudioBridge::Pages::InfoRow;
		using AudioBridge::Pages::MeterFraction;
		using AudioBridge::Pages::WarnCaption;

		// ---- Guitar Tone -------------------------------------------------------------------------------------
		// One page, one choice: where the guitar's sound comes from. A tab for each, and the tab chosen is what plays:
		//   Game amp: Rocksmith's own amps and tones;
		//   Tone designer: the built-in engine (StompboxPage.cpp) in the VST host;
		//   VST rig: VST3 plugins in the same host (VstHostLink.hpp);
		//   External app: an app on the Rocksmith Audio Bridge ASIO driver (AmpliTube standalone, TONE3000, a DAW).
		// The last three play over the amp link (AsioProxy/AmpLink.hpp), which carries one app at a time. Its settings
		// (what is connected, latency, late buffers, the safety buffer) fold into a one-line bar on the host's tabs
		// (click it to open the card); External app keeps the card open. The choice is saved (ExternalAmp::GuitarTone);
		// until a player picks one, the page opens on the tab of whatever plays.
		enum class AmpTab { GameAmp, ToneDesigner, VstRig, ExternalApp };   // ExternalAmp::GuitarTone's values, in tab order
		constexpr int AMP_TAB_COUNT = 4;
		AmpTab g_ampTab = AmpTab::ToneDesigner;
		bool g_ampLinkOpen = false;   // the host's tabs: the Amp link card shown in full (session only)

		// What the VST host plays, for the amp link's line: its built-in engine or the plugin chain. The host is one
		// program for both tabs, so its name on the link says nothing.
		std::string HostEngineName() { return StompboxLink::GetSnapshot().isWanted ? "Tone designer" : "VST rig"; }

		// Makes a tab's mode the one that plays, and saves it. Each mode sounds as it did before the tabs came:
		//   Game amp: what the A/B switch did (ExternalAmp mixes no amp sim in and leaves the game's amp unmuted, now
		//     saved), plus what the power button did (the host in standby: it stops processing, its plugins stay loaded
		//     so the other tabs come back at once);
		//   Tone designer, VST rig: the host turned back on from standby, playing the engine or the chain
		//     (StompboxLink::SetWanted). A closed host stays closed: its tab shows the Start button, as before;
		//   External app: the host in standby, so the amp link is free for the app. The app's own switch is untouched.
		void SelectGuitarTone(AmpTab tab)
		{
			g_ampTab = tab;
			ExternalAmp::SetGuitarTone(static_cast<ExternalAmp::GuitarTone>(tab));
			const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();
			switch (tab) {
			case AmpTab::GameAmp:
			case AmpTab::ExternalApp:
				if (vst.hostRunning && !vst.standby) VstHostLink::TurnOff();
				break;
			case AmpTab::ToneDesigner:
			case AmpTab::VstRig:
				StompboxLink::SetWanted(tab == AmpTab::ToneDesigner);
				if (vst.hostRunning && vst.standby) VstHostLink::StartHost();
				break;
			}
		}

		// The cable's guitar-to-speaker figure in two parts: input (the cable read: packet wait, cable to PC) and output
		// (the rest). Rounded once and subtracted, so the two parts always add up to the total shown.
		struct CableSplit { int total, input, output; };
		CableSplit SplitCableLatency(const ExternalAmp::Snapshot& amp)
		{
			const int total = static_cast<int>(std::lround(amp.cableLatencyMs));
			const int read = (std::min)(total, static_cast<int>(std::lround(amp.cableReadMs)));
			return { total, read, total - read };
		}

		void AmpLinkCard(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			BeginCard("Amp link");
			if (!amp.proxyLoaded) {
				WarnCaption("Needs Rocksmith Audio Bridge ASIO as the game's audio driver. It is not loaded in this session.");
				EndCard();
				return;
			}
			const bool toneDesigner = link.guestPresent && VstHostLink::IsHostGuest(link.guestName);
			const std::string name = toneDesigner ? HostEngineName() : link.guestName[0] ? link.guestName : "an amp sim";
			if (amp.gameAmp) InfoRow("Playing", "The game's amp (Game amp)", Color::Muted);
			else if (link.connected) InfoRow("Playing", name, Color::Good);
			else if (link.guestPresent) InfoRow("Playing", name + " (audio stopped)", Color::Warn);
			else InfoRow("Playing", "The game's amp (nothing connected)", Color::Muted);
			char text[128];
			// On the cable the link's own blocks are a small part of the delay (the output device and its buffer are the
			// rest), so the measured guitar-to-speaker figure is shown instead (CableAmpLink's measurement), split into
			// input, output and total (the DAW convention).
			if (link.frames > 0 && amp.cableHost && amp.cableLatencyMs > 0.0f) {
				const CableSplit split = SplitCableLatency(amp);
				sprintf_s(text, "48 kHz, %d samples, latency input %d ms, output %d ms, total %d ms", link.frames, split.input, split.output, split.total);
			}
			else if (link.frames > 0)
				sprintf_s(text, "48 kHz, %d samples, +%.1f ms", link.frames, link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / AmpLink::kSampleRate);
			InfoRow("Format", link.frames > 0 ? text : "Waiting for the game's audio");
			sprintf_s(text, "%u this session", link.lateBlocks);
			InfoRow("Late buffers", text, link.lateBlocks > 0 ? Color::Warn : Color::Text);
			if (link.lateBlocks > 0)
				Caption("Late buffers are blocks the amp sim did not finish in time; each one is a click. Try the extra safety buffer, fewer or lighter plugins (or a lighter preset), or a bigger game buffer.");
			if (amp.pedalSemitones != 0) {
				sprintf_s(text, "%+d semitones, sent to the amp sim too", amp.pedalSemitones);
				InfoRow("Drop Pedal", text);
			}
			bool safety = amp.safetyBuffer;
			char safetyCaption[96];
			sprintf_s(safetyCaption, "Adds one more buffer (+%.1f ms) so a busy PC clicks less.", (link.frames > 0 ? link.frames : 128) * 1000.0 / AmpLink::kSampleRate);
			if (ToggleRow("Extra safety buffer", safetyCaption, &safety)) ExternalAmp::SetSafetyBuffer(safety);
			// Real Tone Cable players have no ASIO buffer to set, so the link's block size is picked here
			// (CableAmpLink). With an ASIO interface it is the interface's own buffer size and this row is not shown.
			if (amp.cableHost) {
				static const char* const blocks[] = { "Automatic", "64", "128", "256" };
				static const int frames[] = { 0, 64, 128, 256 };
				int index = 0;
				for (int i = 0; i < 4; ++i) if (frames[i] == amp.cableBlockSetting) index = i;
				if (SegmentedRow("Amp sim block", &index, blocks, 4, "How many samples the amp sim processes in one go; changes at once. Smaller is less delay but more work for the PC; heavy plugins may then show late buffers. Automatic is 64 when the cable is read directly over USB, 128 otherwise. Changing it restarts Tone designer's audio for a moment."))
					ExternalAmp::SetCableBlock(frames[index]);
				if (!amp.cableDirectUsb && amp.cableBlockSetting == 64)
					Caption("64 needs the cable read directly over USB; using 128. Windows hands over the cable's input 480 samples at a time.");
			}
			EndCard();
		}

		void ExternalAppTab(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			// While Tone designer holds the link, nothing here applies; this tab never controls Tone designer.
			if (link.guestPresent && VstHostLink::IsHostGuest(link.guestName)) {
				BeginCard();
				InfoRow("Amp link", "Busy: Tone designer is playing", Color::Warn);
				Caption("An external app can connect once Tone designer is off.");
				if (Button("Turn Tone designer off", true, "Your plugins stay loaded, so its tab turns it on again at once.")) SelectGuitarTone(AmpTab::ExternalApp);
				ImGui::SameLine();
				if (Button("Go to Tone designer")) SelectGuitarTone(StompboxLink::GetSnapshot().isWanted ? AmpTab::ToneDesigner : AmpTab::VstRig);
				EndCard();
				return;
			}

			BeginCard();
			bool enabled = amp.enabled;
			const char* state = !amp.enabled ? "Off: the game's amp plays as normal."
				: amp.gameAmpMuted ? "On: the game's amp is muted and you hear the external app."
				: amp.fallback ? "On: the game's amp plays until an app connects."
				: "On: the game's amp stays muted, waiting for an app.";
			if (ToggleRow("Use an external app", state, &enabled, "Saved. Note detection always uses your clean signal, whichever amp you hear."))
				ExternalAmp::SetEnabled(enabled);
			const std::string name = link.guestName[0] ? link.guestName : "an amp sim";
			if (link.connected) InfoRow("Connected app", name, Color::Good);
			else if (link.guestPresent) InfoRow("Connected app", name + " (audio stopped)", Color::Warn);
			else InfoRow("Connected app", "None", Color::Muted);
			if (link.guestPresent && link.guestFrames > 0 && link.frames > 0 && link.guestFrames != link.frames) {
				char frames[16];
				sprintf_s(frames, "%d", link.frames);
				WarnCaption((name + " is set to " + std::to_string(link.guestFrames) + " samples. It should switch to " + frames
					+ " by itself; if not, pick " + frames + " in its audio settings.").c_str());
			}
			if (!amp.guest64Registered)
				WarnCaption("64-bit amp sims (AmpliTube 5) cannot see the driver yet. In the Rocksmith Audio Bridge window, press Repair next to the driver (one admin prompt).");
			bool fallback = amp.fallback;
			if (ToggleRow("Fall back to the game's amp", "If the app disconnects, the game's amp comes back on.", &fallback))
				ExternalAmp::SetFallback(fallback);
			EndCard();

			BeginCard("Levels");
			Meter("Clean signal out", MeterFraction(link.inputPeak), "Your raw guitar as the app receives it, before the Input page's processing.");
			float returnDb = amp.returnTenths / 10.0f;
			bool returnSaved = false;
			if (SliderRow("App return", &returnDb, -30.0f, 6.0f, "%+.1f dB", &returnSaved, "Level of the external app in your mix (Tone designer has its own levels)."))
				ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), false);
			if (returnSaved) ExternalAmp::SetReturnTenths(static_cast<int>(std::lround(returnDb * 10.0f)), true);
			Meter("Return level", MeterFraction(link.returnPeak), "The app's output after the return level.");
			EndCard();

			BeginCard("Setup");
			Caption("1. Open AmpliTube 5, or any ASIO amp sim.");
			Caption("2. In its audio settings, choose Rocksmith Audio Bridge ASIO. Input: Rocksmith guitar. Outputs: Rocksmith out L and R.");
			Caption("3. Keep Use an external app on. The game's amp mutes as soon as the app connects.");
			Caption("For VST3 plugins without an app of their own, use the VST rig tab instead.");
			EndCard();
		}

		// The Amp link card folded to one line: what plays, the format and late buffers, and a chevron. The whole bar
		// opens and folds the card under it.
		void AmpLinkBar(const ExternalAmp::Snapshot& amp)
		{
			const AmpLink::Status& link = amp.link;
			const float width = ImGui::GetContentRegionAvail().x;
			const float height = ImGui::GetFrameHeight() + 6.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			if (ImGui::InvisibleButton("##ampLinkBar", ImVec2(width, height))) g_ampLinkOpen = !g_ampLinkOpen;
			const bool hovered = ImGui::IsItemHovered();
			Hint(g_ampLinkOpen ? "Fold the amp link settings back into this line." : "The amp link's settings: extra safety buffer, and what the numbers mean.");

			std::string summary;
			ImU32 dot = Color::Muted;
			if (!amp.proxyLoaded) { summary = "Rocksmith Audio Bridge ASIO is not loaded"; dot = Color::Warn; }
			else {
				const bool toneDesigner = link.guestPresent && VstHostLink::IsHostGuest(link.guestName);
				const std::string name = toneDesigner ? HostEngineName() : link.guestName[0] ? link.guestName : "an amp sim";
				if (amp.gameAmp) summary = "The game's amp (Game amp)";
				else if (link.connected) { summary = name; dot = Color::Good; }
				else if (link.guestPresent) { summary = name + " (audio stopped)"; dot = Color::Warn; }
				else summary = "The game's amp (nothing connected)";
				char text[128];
				if (link.frames > 0) {
					if (amp.cableHost && amp.cableLatencyMs > 0.0f) {
						const CableSplit split = SplitCableLatency(amp);
						sprintf_s(text, "   48 kHz, %d samples, in %d + out %d = %d ms", link.frames, split.input, split.output, split.total);
					}
					else sprintf_s(text, "   48 kHz, %d samples, +%.1f ms", link.frames, link.frames * (link.latencyBlocks > 1 ? 2 : 1) * 1000.0 / AmpLink::kSampleRate);
					summary += text;
				}
				if (amp.safetyBuffer) summary += ", safety buffer on";
			}

			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 end(at.x + width, at.y + height);
			list->AddRectFilled(at, end, hovered ? Color::Hover : Color::Card, 6.0f);
			list->AddRect(at, end, Color::CardEdge, 6.0f);
			ImFont* caption = GetFonts().caption ? GetFonts().caption : ImGui::GetFont();
			const float textY = at.y + (height - caption->FontSize) * 0.5f;
			float x = at.x + 12.0f;
			list->AddText(caption, caption->FontSize, ImVec2(x, textY), Color::Faint, "AMP LINK");
			x += caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, "AMP LINK").x + 14.0f;
			list->AddCircleFilled(ImVec2(x + 4.0f, at.y + height * 0.5f), 4.0f, dot, 12);
			x += 16.0f;

			// Right: late buffers (amber once there are any), then the chevron.
			float right = end.x - 12.0f;
			const float chevron = 4.5f, cy = at.y + height * 0.5f;
			const float cx = right - chevron;
			if (g_ampLinkOpen) list->AddTriangleFilled(ImVec2(cx - chevron, cy + chevron * 0.5f), ImVec2(cx + chevron, cy + chevron * 0.5f), ImVec2(cx, cy - chevron * 0.5f), Color::Muted);
			else list->AddTriangleFilled(ImVec2(cx - chevron, cy - chevron * 0.5f), ImVec2(cx + chevron, cy - chevron * 0.5f), ImVec2(cx, cy + chevron * 0.5f), Color::Muted);
			right -= 2.0f * chevron + 12.0f;
			if (amp.proxyLoaded) {
				char late[48];
				sprintf_s(late, link.lateBlocks == 1 ? "1 late buffer" : "%u late buffers", link.lateBlocks);
				const float lateW = caption->CalcTextSizeA(caption->FontSize, FLT_MAX, 0.0f, late).x;
				right -= lateW;
				list->AddText(caption, caption->FontSize, ImVec2(right, textY), link.lateBlocks > 0 ? Color::Warn : Color::Muted, late);
				right -= 16.0f;
			}
			list->PushClipRect(ImVec2(x, at.y), ImVec2(std::max(x, right), end.y), true);
			list->AddText(caption, caption->FontSize, ImVec2(x, textY), Color::Text, summary.c_str());
			list->PopClipRect();
			ImGui::SetCursorScreenPos(ImVec2(at.x, end.y));
			ImGui::Dummy(ImVec2(0, 4));
		}

		// The tabs as a segmented strip, `width` wide at the cursor; returns its height.
		float AmpTabs(float width)
		{
			static const char* const labels[AMP_TAB_COUNT] = { "Game amp", "Tone designer", "VST rig", "External app" };
			const float height = ImGui::GetFrameHeight() + 4.0f;
			const ImVec2 at = ImGui::GetCursorScreenPos();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(at, ImVec2(at.x + width, at.y + height), Color::Card, 6.0f);
			list->AddRect(at, ImVec2(at.x + width, at.y + height), Color::CardEdge, 6.0f);
			const float tabW = width / AMP_TAB_COUNT;
			for (int i = 0; i < AMP_TAB_COUNT; ++i) {
				const ImVec2 tab(at.x + tabW * i, at.y);
				ImGui::SetCursorScreenPos(tab);
				ImGui::PushID(i);
				// Also on the tab already shown: until a player picks a tab, the one shown is only what plays, not saved.
				if (ImGui::InvisibleButton("##ampTab", ImVec2(tabW, height))) SelectGuitarTone(static_cast<AmpTab>(i));
				const bool hovered = ImGui::IsItemHovered();
				ImGui::PopID();
				const bool on = g_ampTab == static_cast<AmpTab>(i);
				if (on) list->AddRectFilled(ImVec2(tab.x + 3.0f, tab.y + 3.0f), ImVec2(tab.x + tabW - 3.0f, tab.y + height - 3.0f), Color::Accent, 5.0f);
				else if (hovered) list->AddRectFilled(ImVec2(tab.x + 3.0f, tab.y + 3.0f), ImVec2(tab.x + tabW - 3.0f, tab.y + height - 3.0f), Color::Hover, 5.0f);
				const ImVec2 size = ImGui::CalcTextSize(labels[i]);
				list->AddText(ImVec2(tab.x + (tabW - size.x) * 0.5f, tab.y + (height - size.y) * 0.5f), on ? Color::Text : Color::Muted, labels[i]);
			}
			return height;
		}

		// The tone the game plays now: its key (GameTones), as "Tone_A: Crunch Lead" (the manifest's slot and the tone's
		// name) when the song index has it. Empty outside a song.
		std::string CurrentGameTone()
		{
			const std::string& key = GameTones::CurrentToneKey();
			const std::string songKey = VstHostLink::GetSnapshot().songKey;
			if (key.empty() || songKey.empty()) return {};
			SongLibrary::Refresh();   // once per session; later calls return at once
			const auto library = SongLibrary::Get();
			if (const SongLibrary::Song* song = library->Find(songKey))
				for (const SongLibrary::SongTone& tone : song->tones)
					if (_stricmp(tone.key.c_str(), key.c_str()) == 0 && !tone.name.empty() && tone.name != tone.key)
						return "Tone_" + tone.slot + ": " + tone.name;
			return key;
		}

		// Game amp: Rocksmith's own amps and tones play; the other tabs' rigs are left as they are (SelectGuitarTone).
		void GameAmpTab()
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
			ImGui::TextUnformatted("Rocksmith's own amps and tones play. Your other rigs are kept as they are.");
			ImGui::PopTextWrapPos();
			ImGui::Dummy(ImVec2(0, 6));
			BeginCard();
			Caption("Current game tone");
			const std::string tone = CurrentGameTone();
			ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + RowWidth());
			ImGui::TextUnformatted(tone.empty() ? "No song playing" : tone.c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopFont();
			Caption("Switch to Tone designer, VST rig or External app any time.");
			EndCard();
		}

		void AmpSimPage()
		{
			const ExternalAmp::Snapshot amp = ExternalAmp::GetSnapshot();
			// The first time the page opens this session: the tab the player chose last (saved), else, for a player who
			// has not chosen since the tabs came, the tab of whatever holds the link.
			static bool chosen = false;
			if (!chosen && ExternalAmp::GetGuitarTone() != ExternalAmp::GuitarTone::Unset) {
				chosen = true;
				g_ampTab = static_cast<AmpTab>(ExternalAmp::GetGuitarTone());
			}
			if (!chosen && amp.proxyLoaded) {
				chosen = true;
				if (amp.link.guestPresent && !VstHostLink::IsHostGuest(amp.link.guestName)) g_ampTab = AmpTab::ExternalApp;
				else g_ampTab = StompboxLink::GetSnapshot().isWanted ? AmpTab::ToneDesigner : AmpTab::VstRig;
			}

			// The title on the left, the tabs on the right of the same row. No description under the title: each tab
			// says what it plays.
			const ImVec2 at = ImGui::GetCursorScreenPos();
			const float width = ImGui::GetContentRegionAvail().x;
			const float tabsW = std::min(620.0f, std::max(400.0f, width * 0.5f));
			const float tabsH = ImGui::GetFrameHeight() + 4.0f;   // AmpTabs' height
			ImGui::BeginGroup();
			ImGui::PushFont(GetFonts().title ? GetFonts().title : ImGui::GetFont());
			ImGui::TextUnformatted("Guitar Tone");
			ImGui::PopFont();
			ImGui::EndGroup();
			const float headH = ImGui::GetItemRectSize().y;
			const float rowH = std::max(headH, tabsH);
			ImGui::SetCursorScreenPos(ImVec2(at.x + width - tabsW, at.y + (rowH - tabsH) * 0.5f));
			AmpTabs(tabsW);
			ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH + 8.0f));
			ImGui::Dummy(ImVec2(0, 0));

			if (g_ampTab == AmpTab::GameAmp) {
				GameAmpTab();
				return;
			}
			if (g_ampTab != AmpTab::ExternalApp) AmpLinkBar(amp);
			if (g_ampTab == AmpTab::ExternalApp || g_ampLinkOpen) AmpLinkCard(amp);
			// The host in standby (started for always-loaded plugins with Start with the game off): the engine plays nothing
			// until it is turned on. The VST rig tab has the same card of its own (ToneDesignerTab).
			const VstHostLink::Snapshot vst = VstHostLink::GetSnapshot();
			if (g_ampTab == AmpTab::ToneDesigner && vst.hostRunning && vst.standby) {
				BeginCard();
				Caption("Tone designer is off; the game's amp is playing. It turns on at once.");
				if (Button("Turn on", true, "Your guitar plays through the Tone designer again.")) SelectGuitarTone(AmpTab::ToneDesigner);
				EndCard();
			}
			else if (g_ampTab == AmpTab::ToneDesigner) {
				const float naturalHeight = DrawStompboxPage();
				if (naturalHeight > 0.0f) AudioBridge::Shell::FillWindowHeight(naturalHeight);
			}
			else if (g_ampTab == AmpTab::VstRig) ToneDesignerTab();
			else ExternalAppTab(amp);
		}
	}
}

namespace AudioBridge::GuitarTone
{
	// 4.x's sidebar led with this one entry ("Guitar Tone", the Library glyph) above the groups, and the overlay opened
	// on it until another page was picked. Order 10 keeps it first.
	void RegisterPage()
	{
		// The page needs the Audio Bridge ASIO driver in the game (ASIO mode); in Cable mode there is nothing for an
		// amp sim to connect to, so the page is not offered (4.x's IsPageAvailable).
		Shell::RegisterPage({ "guitar-tone", "Tone", Overlay::Glyph::Library, "Guitar Tone", 10, Overlay::AmpSimPage,
			[] { return ExternalAmp::GetSnapshot().proxyLoaded; } });
	}
}
