#pragma once

// The popout's content around one PedalView: a black stage with a spotlight, a top bar (previous, a picker and
// next to step through pedals, or just the pedal's name), and the pedal centred and scaled to the window with its
// reflection on the floor.
// The stage owns no audio state: whoever fills it (the host's popout, or the PedalGallery prototype) gives it the
// pedal names and builds the PedalView for the chosen one.

#include "PedalLookAndFeel.hpp"
#include "PedalView.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace PedalUi
{
	class PedalStage final : public juce::Component, private juce::Timer
	{
	public:
		PedalStage();
		~PedalStage() override;

		void SetTitles(const juce::StringArray& titles, int selected);
		std::function<void(int index)> onSelect;   // the top bar picked another pedal: build it and SetPedal
		std::function<void(int delta)> onStep;     // the left or right arrow key (the gallery; unset in the game)
		// The top bar shows only this name, no picker. SetTitles brings the picker back (the host's window lists what
		// can replace its pedal there, PedalEditor::UpdateTitles; the gallery lists its pedals).
		void SetTitle(const juce::String& title);

		void SetPedal(std::unique_ptr<PedalView> pedal);
		PedalView* Pedal() const { return pedal.get(); }

		// Renders the reflection now rather than on the next timer tick (for snapshots taken straight away).
		void RefreshReflection();

		// When the stage is a window of its own (the host's popout): the top bar drags the window, a close button
		// sits on its right (onClose), and a corner grip resizes it. Off in a window that has its own frame.
		void SetWindowControls(bool on);
		std::function<void()> onClose;
		std::function<void()> onMovedOrResized;   // the player dragged or resized the window (remember where)

		// The window takes the size the pedal needs: the pedal at `scale` with its bar, margins and floor, and no
		// empty space at the sides. Each pedal (and MORE) resizes the window, keeping its top centre where it was;
		// the corner grip changes only the scale. Kept within 90% of the window's display.
		void SetFitWindow(bool on, float scale);
		float FitScale() const { return fitScale; }

		void paint(juce::Graphics&) override;
		void resized() override;
		void mouseDown(const juce::MouseEvent&) override;
		void mouseDrag(const juce::MouseEvent&) override;
		void mouseUp(const juce::MouseEvent&) override;
		bool keyPressed(const juce::KeyPress&) override;

	private:
		PedalLookAndFeel lookAndFeel;
		std::unique_ptr<PedalView> pedal;
		juce::TextButton previous{ "<" }, next{ ">" }, close{ "X" };
		juce::ComboBox picker;
		juce::Label title;
		juce::ComponentDragger dragger;
		// Snaps a corner resize to the fitted size of the scale it asks for (fit mode); plain limits otherwise.
		class FitConstrainer final : public juce::ComponentBoundsConstrainer
		{
		public:
			explicit FitConstrainer(PedalStage& stage) : stage(stage) {}
			void checkBounds(juce::Rectangle<int>& bounds, const juce::Rectangle<int>& previous, const juce::Rectangle<int>& limits,
				bool top, bool left, bool bottom, bool right) override;
		private:
			PedalStage& stage;
		};
		FitConstrainer constrainer{ *this };
		bool fitWindow = false;
		float fitScale = 1.0f;
		std::unique_ptr<juce::ResizableCornerComponent> corner;
		bool windowControls = false, dragging = false;
		juce::Image reflection;
		juce::Rectangle<float> pedalArea;
		float scale = 1.0f;
		bool reflectionDirty = false;

		void Fit();
		void FitWindow();
		juce::Rectangle<int> FittedSize(float pedalScale) const;
		float ScaleFor(int width, int height) const;
		float MaxFitScale() const;
		void Step(int delta);
		void timerCallback() override;
	};
}
