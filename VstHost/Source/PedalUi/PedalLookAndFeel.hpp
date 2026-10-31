#pragma once

// How the pedal editors' controls are drawn: knobs, faders, toggle switches and the display-style pickers. All
// vector drawing, no images, so any pedal colour works and the editors scale cleanly.
//
// A control picks its style through component properties set by PedalView: "knob" ("stomp", "amp" or "chrome")
// on a Slider, and its accent colour through the usual colour ids (Slider::thumbColourId for the fader caps' glow
// and the knob's value arc).

#include <juce_gui_basics/juce_gui_basics.h>

namespace PedalUi
{
	class PedalLookAndFeel final : public juce::LookAndFeel_V4
	{
	public:
		PedalLookAndFeel();

		void drawRotarySlider(juce::Graphics&, int x, int y, int width, int height, float position,
			float startAngle, float endAngle, juce::Slider&) override;
		void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height, float position,
			float minPosition, float maxPosition, juce::Slider::SliderStyle, juce::Slider&) override;
		void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
		void drawComboBox(juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY,
			int buttonW, int buttonH, juce::ComboBox&) override;
		juce::Font getComboBoxFont(juce::ComboBox&) override;
		void positionComboBoxText(juce::ComboBox&, juce::Label&) override;
		void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted,
			bool down) override;

		// Shared drawing for the bodies (PedalView): a screw head, and the brushed-metal texture.
		static void DrawScrew(juce::Graphics&, juce::Point<float> centre, float radius, float angle);
		static void DrawBrushed(juce::Graphics&, juce::Rectangle<float> area, juce::Colour base, int seed);

	private:
		void DrawStompKnob(juce::Graphics&, juce::Rectangle<float>, float angle, juce::Colour accent, bool chrome);
		void DrawAmpKnob(juce::Graphics&, juce::Rectangle<float>, float angle);
	};
}
