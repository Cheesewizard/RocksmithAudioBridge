#pragma once

// One Stompbox pedal's editor, built and laid out from its catalog entry (PedalCatalog) with no per-pedal code.
//
// The body is chosen from what the pedal has (ChooseBody):
//   Rack     three or more VSliders: a graphic EQ rack unit with lit fader caps
//   Amp      a NAM model picker, or a tone stack preset: an amp head (tolex, faceplate, chicken-head knobs)
//   Cabinet  an IR picker from the Cabinets folder: a speaker cab with grille cloth
//   Stomp    everything else: a stompbox in the pedal's own BackgroundColor, with LED and footswitch
// NAM is the capture technology, not what the player sees: a NAM pedal is named after the capture it plays
// ("5152 Lead TS9"), and a capture of a pedal (CaptureLookup's kind "pedal", from the tones manifest) gets a
// stompbox instead of an amp head. Choosing another capture can change the body; the cab is named after its IR.
// and each parameter becomes a control by its type: Knob -> knob, VSlider -> fader, Bool -> mini toggle,
// Enum and File -> a display-style picker, an output (IsOutput) -> an LED meter. Every control is always shown:
// Stompbox's advanced parameters get smaller knobs, as secondary knobs are on real pedals, rather than being hidden
// behind a switch (a pedal has at most a handful). Knob counts set the layout (one row up to 4, then rows of 3 or 4) and the
// layout sets the pedal's size (NaturalSize); PedalStage scales it to its window.
//
// Values: the view holds them and reports each change (onParamChange, onChoiceChange, onEnabledChange) in the pedal's
// own units, the ones Stompbox's "SetParam <Pedal> <Param> <value>" takes; SetValue / SetChoice / SetValueText /
// SetEnabled update it from the engine without reporting back.

#include "PedalCatalog.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace PedalUi
{
	enum class Body { Stomp, Amp, Rack, Cabinet };
	// `captureKind`: what the pedal's NAM capture is ("amp", "pedal"), when known.
	Body ChooseBody(const PluginDesc& plugin, const juce::String& captureKind = {});

	// What a file of a File parameter is, from the tones manifest (StarterTones\manifest.json in the data folder):
	// its kind ("amp", "pedal", "cabinet") and description. Empty when the file is not in it (a player's own capture).
	struct CaptureInfo
	{
		juce::String kind;
		juce::String description;
		juce::String character;   // amps: "clean", "crunch", "high-gain" (picks the amp head's look)
	};
	using CaptureLookup = std::function<CaptureInfo(const juce::String& tree, const juce::String& file)>;
	// Reads a tones manifest ("files": path, kind, description); a missing or unreadable one knows nothing.
	CaptureLookup ReadCaptureManifest(const juce::File& manifest);

	// The files a File parameter offers (Stompbox's ParameterFileTree folder, e.g. "NAM"), as Stompbox names them:
	// relative to that folder, without the extension.
	using FileLister = std::function<juce::StringArray(const juce::String& tree)>;

	class PedalView final : public juce::Component
	{
	public:
		PedalView(const PluginDesc& plugin, FileLister files, CaptureLookup captures = nullptr);
		~PedalView() override;

		std::function<void(const juce::String& param, double value)> onParamChange;
		std::function<void(const juce::String& param, const juce::String& choice)> onChoiceChange;   // Enum and File
		std::function<void(bool enabled)> onEnabledChange;
		std::function<void()> onLayoutChange;   // the size changed (another kind of capture): the stage refits it
		std::function<void()> onLookChange;     // something visible changed (the stage's reflection follows)
		std::function<void()> onNameChange;     // DisplayName changed (another capture or IR was chosen)

		// What the pedal is called on its body and in the window's title: the capture or IR it plays for NAM pedals
		// and cabinets ("JCM2000 Crunch", "4x12 V30 SM57"), else Stompbox's name ("Delay").
		juce::String DisplayName() const;

		void SetValue(const juce::String& param, double value);
		void SetChoice(const juce::String& param, const juce::String& choice);
		// A value as a program writes it (SetParam's last word): a number, or an Enum or File choice.
		void SetValueText(const juce::String& param, const juce::String& text);
		void SetEnabled(bool enabled);
		bool IsEnabled() const { return enabled; }

		const PluginDesc& Plugin() const { return plugin; }
		Body GetBody() const { return body; }
		juce::Rectangle<int> NaturalSize() const { return natural; }

		void paint(juce::Graphics&) override;
		void resized() override;

	private:
		class Control;
		class Meter;
		class FootSwitch;

		const PluginDesc plugin;
		CaptureLookup captures;
		Body body;
		juce::Colour accent;
		bool enabled = true;
		std::vector<std::unique_ptr<Control>> controls;
		std::unique_ptr<FootSwitch> footSwitch;
		juce::Rectangle<int> natural;
		juce::Rectangle<float> nameArea, panelArea, ledArea, grilleArea;
		juce::Rectangle<float> shellArea, drawerArea;   // where the shell (whole-body art) and the advanced drawer go
		juce::String shellName;                         // the PedalSkin shell this body uses; empty: drawn by code

		void Layout();
		void ApplyBody();               // the body for the current capture, its knob styles and colours
		void CaptureChanged();
		const Control* FileControl() const;   // the NAM model or cabinet IR picker, if the pedal has one
		juce::String DisplayDescription() const;
		bool NamedAfterFile() const;
		void LayoutStomp();
		void LayoutAmp();
		void LayoutRack();
		void LayoutCabinet();
		void PlaceRow(const std::vector<Control*>& row, juce::Rectangle<int> area, int knobSize);
		std::vector<Control*> Visible(bool wantAdvanced) const;
		void Changed(Control& control);

		void PaintStomp(juce::Graphics&);
		void PaintAmp(juce::Graphics&);
		void PaintRack(juce::Graphics&);
		void PaintCabinet(juce::Graphics&);
		void PaintLed(juce::Graphics&, juce::Rectangle<float> area, juce::Colour colour);
	};
}
