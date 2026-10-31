#include "PedalStage.hpp"

namespace PedalUi
{
	using namespace juce;

	namespace
	{
		constexpr int kBarHeight = 40;
		constexpr float kMaxScale = 1.6f;      // in a window of fixed size (the gallery)
		constexpr float kMinFitScale = 0.4f;
		constexpr int kMargin = 24;            // left and right of the pedal
		constexpr int kTopGap = 18;            // between the bar and the pedal
		constexpr int kBottomGap = 6;
		constexpr float kFloor = 0.32f;        // the reflection under the pedal, as a share of its height
	}

	PedalStage::PedalStage()
	{
		setLookAndFeel(&lookAndFeel);
		setOpaque(true);

		previous.onClick = [this] { Step(-1); };
		next.onClick = [this] { Step(1); };
		picker.onChange = [this] { if (onSelect && picker.getSelectedItemIndex() >= 0) onSelect(picker.getSelectedItemIndex()); };
		picker.setColour(ComboBox::textColourId, Colour(0xffe9ebef));
		for (auto* c : std::initializer_list<Component*>{ &previous, &next, &picker }) addAndMakeVisible(c);
		title.setJustificationType(Justification::centred);
		title.setFont(Font(FontOptions(16.0f, Font::bold)));
		title.setColour(Label::textColourId, Colour(0xffe9ebef));
		title.setInterceptsMouseClicks(false, false);   // the bar under it drags the window
		addChildComponent(title);
		close.setTooltip("Close the pedal window");
		close.onClick = [this] { if (onClose) onClose(); };
		addChildComponent(close);
		constrainer.setMinimumSize(420, 300);
		setWantsKeyboardFocus(true);
		startTimerHz(30);
	}

	void PedalStage::SetWindowControls(bool on)
	{
		windowControls = on;
		close.setVisible(on);
		if (on && !corner)
		{
			corner = std::make_unique<ResizableCornerComponent>(this, &constrainer);
			addAndMakeVisible(*corner);
		}
		else if (!on) corner.reset();
		resized();
	}

	void PedalStage::FitConstrainer::checkBounds(Rectangle<int>& bounds, const Rectangle<int>& previous, const Rectangle<int>& limits,
		bool top, bool left, bool bottom, bool right)
	{
		if (!stage.fitWindow || !stage.pedal)
		{
			ComponentBoundsConstrainer::checkBounds(bounds, previous, limits, top, left, bottom, right);
			return;
		}
		const float s = jlimit(kMinFitScale, jmax(kMinFitScale, stage.MaxFitScale()), stage.ScaleFor(bounds.getWidth(), bounds.getHeight()));
		bounds.setSize(stage.FittedSize(s).getWidth(), stage.FittedSize(s).getHeight());   // the grip's corner: top-left stays
	}

	Rectangle<int> PedalStage::FittedSize(float pedalScale) const
	{
		const auto natural = pedal ? pedal->NaturalSize() : Rectangle<int>(400, 300);
		return { roundToInt(natural.getWidth() * pedalScale) + kMargin * 2,
			kBarHeight + kTopGap + roundToInt(natural.getHeight() * pedalScale * (1.0f + kFloor)) + kBottomGap };
	}

	float PedalStage::ScaleFor(int width, int height) const
	{
		const auto natural = pedal ? pedal->NaturalSize() : Rectangle<int>(400, 300);
		return jmin(static_cast<float>(width - kMargin * 2) / natural.getWidth(),
			static_cast<float>(height - kBarHeight - kTopGap - kBottomGap) / (natural.getHeight() * (1.0f + kFloor)));
	}

	// The biggest scale whose window fits in 90% of the display the window is on.
	float PedalStage::MaxFitScale() const
	{
		const auto* display = Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds());
		if (!display) display = Desktop::getInstance().getDisplays().getPrimaryDisplay();
		if (!display) return 3.0f;
		const auto area = display->userBounds.toNearestInt();
		return ScaleFor(area.getWidth() * 9 / 10, area.getHeight() * 9 / 10);
	}

	void PedalStage::SetFitWindow(bool on, float pedalScale)
	{
		fitWindow = on;
		fitScale = pedalScale;
		FitWindow();
	}

	void PedalStage::FitWindow()
	{
		if (!fitWindow || !pedal) return;
		const float s = jlimit(kMinFitScale, jmax(kMinFitScale, MaxFitScale()), fitScale);
		const auto size = FittedSize(s);
		const auto old = getBounds();
		auto bounds = Rectangle<int>(old.getCentreX() - size.getWidth() / 2, old.getY(), size.getWidth(), size.getHeight());
		if (const auto* display = Desktop::getInstance().getDisplays().getDisplayForRect(old))
			bounds = bounds.constrainedWithin(display->userBounds.toNearestInt());
		setBounds(bounds);
		fitScale = s;
		Fit();
	}

	void PedalStage::mouseDown(const MouseEvent& e)
	{
		dragging = windowControls && e.y < kBarHeight;
		if (dragging) dragger.startDraggingComponent(this, e);
	}

	void PedalStage::mouseDrag(const MouseEvent& e)
	{
		if (dragging) dragger.dragComponent(this, e, nullptr);
	}

	bool PedalStage::keyPressed(const KeyPress& key)
	{
		if (!onStep) return false;
		if (key == KeyPress::leftKey) { onStep(-1); return true; }
		if (key == KeyPress::rightKey) { onStep(1); return true; }
		return false;
	}

	void PedalStage::mouseUp(const MouseEvent&)
	{
		if (dragging && onMovedOrResized) onMovedOrResized();
		dragging = false;
	}

	PedalStage::~PedalStage()
	{
		stopTimer();
		pedal.reset();
		setLookAndFeel(nullptr);
	}

	void PedalStage::SetTitles(const StringArray& titles, int selected)
	{
		for (auto* c : std::initializer_list<Component*>{ &previous, &next, &picker }) c->setVisible(true);
		title.setVisible(false);
		picker.clear(dontSendNotification);
		picker.addItemList(titles, 1);
		picker.setSelectedItemIndex(selected, dontSendNotification);
	}

	void PedalStage::SetTitle(const String& text)
	{
		for (auto* c : std::initializer_list<Component*>{ &previous, &next, &picker }) c->setVisible(false);
		title.setText(text, dontSendNotification);
		title.setVisible(true);
	}

	void PedalStage::Step(int delta)
	{
		const int count = picker.getNumItems();
		if (count == 0) return;
		picker.setSelectedItemIndex((picker.getSelectedItemIndex() + delta + count) % count, sendNotificationSync);
	}

	void PedalStage::SetPedal(std::unique_ptr<PedalView> view)
	{
		pedal = std::move(view);
		reflection = {};
		if (!pedal) { repaint(); return; }
		pedal->onLayoutChange = [this] { if (fitWindow) FitWindow(); else Fit(); };
		pedal->onLookChange = [this] { reflectionDirty = true; };
		addAndMakeVisible(*pedal);
		if (fitWindow) FitWindow();
		else Fit();
	}

	void PedalStage::resized()
	{
		auto bar = getLocalBounds().removeFromTop(kBarHeight).reduced(8, 7);
		const int pickerWidth = jmin(300, bar.getWidth() - 80);
		auto centre = bar.withSizeKeepingCentre(pickerWidth + 72, bar.getHeight());
		previous.setBounds(centre.removeFromLeft(30));
		next.setBounds(centre.removeFromRight(30));
		picker.setBounds(centre.reduced(6, 0));
		title.setBounds(bar.withSizeKeepingCentre(jmin(400, bar.getWidth() - 80), bar.getHeight()));
		close.setBounds(getLocalBounds().removeFromTop(kBarHeight).reduced(8, 7).removeFromRight(30));
		if (corner) corner->setBounds(getWidth() - 18, getHeight() - 18, 18, 18);
		Fit();
		// A resize from the corner grip (the window's own size follows this component's).
		if (windowControls && corner && corner->isMouseButtonDown() && onMovedOrResized) onMovedOrResized();
	}

	void PedalStage::Fit()
	{
		if (!pedal) return;
		// The pedal and its floor, as big as the room allows, centred. A fitted window is exactly this size.
		const auto natural = pedal->NaturalSize();
		scale = jmax(0.2f, ScaleFor(getWidth(), getHeight()));
		if (!fitWindow) scale = jmin(kMaxScale, scale);
		else fitScale = scale;
		const float w = natural.getWidth() * scale, h = natural.getHeight() * scale;
		const float roomTop = static_cast<float>(kBarHeight + kTopGap);
		const float roomHeight = static_cast<float>(getHeight() - kBarHeight - kTopGap - kBottomGap);
		const float x = getWidth() * 0.5f - w * 0.5f;
		const float y = roomTop + jmax(0.0f, (roomHeight - h * (1.0f + kFloor)) * 0.5f);
		pedal->setBounds(natural);
		pedal->setTransform(AffineTransform::scale(scale).translated(x, y));
		pedalArea = { x, y, w, h };
		RefreshReflection();
	}

	void PedalStage::RefreshReflection()
	{
		reflectionDirty = false;
		if (!pedal || pedalArea.isEmpty()) return;
		reflection = pedal->createComponentSnapshot(pedal->getLocalBounds(), true, scale);
		repaint();
	}

	void PedalStage::timerCallback()
	{
		if (reflectionDirty) RefreshReflection();
	}

	void PedalStage::paint(Graphics& g)
	{
		const auto bounds = getLocalBounds().toFloat();
		g.fillAll(Colour(0xff050506));

		// The top bar.
		const auto bar = bounds.withHeight(static_cast<float>(kBarHeight));
		g.setGradientFill(ColourGradient(Colour(0xff17181c), 0.0f, 0.0f, Colour(0xff0b0c0e), 0.0f, bar.getBottom(), false));
		g.fillRect(bar);
		g.setColour(Colours::white.withAlpha(0.06f));
		g.drawHorizontalLine(kBarHeight - 1, 0.0f, bounds.getWidth());

		if (pedalArea.isEmpty()) return;

		// A spotlight from above onto the pedal, fading out into the floor.
		const float floorY = pedalArea.getBottom();
		g.setGradientFill(ColourGradient(Colour(0xff26272c).withAlpha(0.9f), pedalArea.getCentreX(), pedalArea.getY() - pedalArea.getHeight() * 0.3f,
			Colours::transparentBlack, pedalArea.getCentreX() + pedalArea.getWidth() * 1.05f, pedalArea.getCentreY(), true));
		g.fillRect(bounds.withTrimmedTop(static_cast<float>(kBarHeight)));

		// The floor's horizon line, then the pedal's reflection, flipped and fading.
		ColourGradient horizon(Colours::white.withAlpha(0.0f), bounds.getX(), floorY,
			Colours::white.withAlpha(0.0f), bounds.getRight(), floorY, false);
		horizon.addColour(0.5, Colours::white.withAlpha(0.08f));
		g.setGradientFill(horizon);
		g.fillRect(bounds.getX(), floorY + 1.0f, bounds.getWidth(), 1.0f);

		// A contact shadow under the pedal.
		g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.85f), pedalArea.getCentreX(), floorY,
			Colours::transparentBlack, pedalArea.getRight() + 30.0f, floorY, true));
		g.fillEllipse(pedalArea.getX() - 20.0f, floorY - 10.0f, pedalArea.getWidth() + 40.0f, 24.0f);

		if (reflection.isValid())
		{
			const float h = pedalArea.getHeight();
			const float visible = jmin(h * 0.55f, bounds.getBottom() - floorY - 4.0f);
			if (visible > 4.0f)
			{
				Graphics::ScopedSaveState save(g);
				g.reduceClipRegion(Rectangle<float>(pedalArea.getX(), floorY + 4.0f, pedalArea.getWidth(), visible).toNearestInt());
				g.setOpacity(0.32f);
				g.drawImageTransformed(reflection, AffineTransform::scale(pedalArea.getWidth() / reflection.getWidth(),
					-h / reflection.getHeight()).translated(pedalArea.getX(), floorY + 4.0f + h));
			}
			// Fade it to the floor colour.
			g.setGradientFill(ColourGradient(Colour(0xff050506).withAlpha(0.15f), 0.0f, floorY + 4.0f,
				Colour(0xff050506), 0.0f, floorY + 4.0f + visible, false));
			g.fillRect(pedalArea.getX() - 1.0f, floorY + 4.0f, pedalArea.getWidth() + 2.0f, visible + 1.0f);
		}
	}
}
