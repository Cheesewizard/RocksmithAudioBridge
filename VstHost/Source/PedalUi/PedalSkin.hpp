#pragma once

// The pedal editors' artwork: photographic knobs, footswitch and surface textures (generated with ChatGPT's image
// generation, see VstHost/PedalSkin/README.md), loaded once from a folder of images. Each piece is optional: the
// drawing code asks for it and falls back to its own vector drawing when it is missing, so the editors work (plainer)
// without the folder.
//
// Pieces (file name without .webp, .png or .jpg):
//   knob-amp      a chicken-head amp knob, pointer up (the whole image turns with the value)
//   knob-stomp    a black pedal knob, no pointer (stays still; the pointer is drawn over it)
//   knob-chrome   a chrome pedal knob, no pointer (likewise)
//   footswitch    a chrome stomp switch in its hex nut
//   enclosure     neutral grey pedal paint, tinted to each pedal's colour (Tinted)
//   tolex, cab-tolex, faceplate, grille, rack-face, plate   surfaces, drawn to cover an area (Cover)
// Shells, whole bodies with their areas measured in skin.json (fractions of the image: x, y, width, height):
//   amp-modern, amp-british, amp-vintage   amp heads: "panel" (blank control panel), "grille" (logo area)
//   cab-front                              a 4x12 front: "grille"
//   rack-unit                              a rack faceplate: "inner" (between the ears; the ears keep their shape)
//   pedal-shell                            a grey pedal enclosure, tinted per pedal: "inner" (the corners keep shape)
// Message thread only.

#include <juce_gui_basics/juce_gui_basics.h>

namespace PedalUi
{
	namespace Skin
	{
		// Loads every piece found in `folder` (the others stay missing). Safe to call again (a later folder replaces).
		void Load(const juce::File& folder);
		bool IsLoaded();

		// The piece, or an invalid image when it is missing.
		const juce::Image& Get(const char* name);

		// Draws `image` over `area`, scaled to cover it (cropped to its aspect, centred).
		void Cover(juce::Graphics&, const juce::Image& image, juce::Rectangle<float> area, float opacity = 1.0f);

		// The neutral grey `enclosure` tinted to `colour` (cached per colour); invalid when the piece is missing.
		juce::Image Tinted(juce::Colour colour);
		// Any neutral grey piece tinted to `colour`, its transparency kept (the pedal shell).
		juce::Image TintedPiece(const char* name, juce::Colour colour);

		// A whole-body piece and its measured areas (fractions of the image).
		struct Shell
		{
			juce::Image image;
			juce::Rectangle<float> panel, grille, inner;
			float Aspect() const { return image.isValid() ? static_cast<float>(image.getWidth()) / image.getHeight() : 1.0f; }
		};
		const Shell* GetShell(const char* name);   // nullptr when the image or its areas are missing

		// Draws `image` into `dest` as a 9-slice: the margins around `inner` (fractions of the image) keep their
		// proportions at the scale the destination's height sets (the corners and ears stay round and square); the
		// middle stretches.
		void DrawSliced(juce::Graphics&, const juce::Image& image, juce::Rectangle<float> dest, juce::Rectangle<float> inner);
	}
}
