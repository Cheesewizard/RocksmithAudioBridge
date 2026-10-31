#pragma once

// The pedal window: a built-in (Stompbox) pedal of the Tone designer's board in a generated JUCE editor
// (Source/PedalUi, see Documentation/Pedal-editors.md), over the game like the VST rig's plugin windows.
//
// The top bar's picker, with a previous and a next button on either side, swaps the pedal for another of the same
// type in its place on the board: an amp for another amp capture, a cabinet for another IR, a pedal for another pedal
// (an effect or a pedal capture). Only the same type is offered, so a pick never turns the amp into a delay.
//
// The page opens it with its EditPedal command (StompboxService::onEditPedal). Everything it shows comes from the
// service's published catalogue (DumpConfig: which pedals exist, their controls) and program (DumpProgram: the board's
// pedals and their values), and every edit goes back as a Stompbox command through StompboxService::RunLocalCommand,
// so it is checked like the page's own and the page sees it in the next program. Knob drags are coalesced to the
// latest value per control, sent on the next tick.
//
// One window, topmost, not activated when it shows (the game keeps the keyboard until the player clicks it), no
// taskbar button. It shows while it is open and the overlay is open (Poll's `overlayShown`). It takes the size the
// pedal needs (no empty space at its sides); its top bar drags it, its corner grip scales the pedal, and where it was
// and the scale are remembered (HKCU\Software\RSMods\VstHost PedalWindowCentreX/Top/Scale).
// Main thread only.

#include "StompboxService.hpp"

#include "PedalUi/PedalCatalog.hpp"
#include "PedalUi/PedalStage.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace VstHost
{
	class PedalEditor
	{
	public:
		explicit PedalEditor(StompboxService& service);
		~PedalEditor();
		PedalEditor(const PedalEditor&) = delete;
		PedalEditor& operator=(const PedalEditor&) = delete;

		// Shows the board's pedal `pedalId` ("Delay_2"); an empty id closes the window.
		void Open(const std::string& pedalId);
		void Close();
		bool IsOpen() const { return isOpen; }

		// Every tick of the host's key timer: shows or hides the window with the overlay, sends the window's edits and
		// follows catalogue and program changes (the page's edits, a loaded tone, a Rescan).
		void Poll(bool overlayShown);

	private:
		StompboxService& service;
		std::unique_ptr<PedalUi::PedalStage> stage;
		PedalUi::Catalog catalog;
		PedalUi::CaptureLookup captures;
		std::vector<PedalUi::ProgramPedal> board;
		uint64_t catalogSeen = 0, programSeen = 0;
		juce::String shownId;          // the pedal in the window
		bool isOpen = false;
		bool isShown = false;
		bool refreshWaiting = false;   // a program arrived while the player held a control: applied on release
		std::vector<std::pair<std::string, std::string>> pending;   // key ("SetParam <id> <param>") -> command line
		// What the picker offers: a pedal kind, and for an amp, pedal capture or cabinet the file it plays.
		struct Swap
		{
			juce::String kind;
			juce::String file;
		};
		std::vector<Swap> swaps;
		juce::StringArray swapTitles;   // what the picker shows now (rebuilt only when it changes: an open list stays)
		int swapSelected = -1;
		bool titlesStale = false;       // a pick did not go through: put the picker back on the next tick

		void EnsureWindow();
		void ReadCatalog();
		void ReadProgram();
		void ShowPedal(const juce::String& id);
		void ApplyValues();
		void UpdateTitles();
		void SwapTo(int index);
		void Queue(const std::string& key, const std::string& line);
		void Flush();
		bool IsHeld() const;
		void Remember() const;
	};
}
