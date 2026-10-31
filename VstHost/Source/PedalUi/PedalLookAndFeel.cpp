#include "PedalLookAndFeel.hpp"
#include "PedalSkin.hpp"

namespace PedalUi
{
	using namespace juce;

	PedalLookAndFeel::PedalLookAndFeel()
	{
		setColour(PopupMenu::backgroundColourId, Colour(0xff15171b));
		setColour(PopupMenu::textColourId, Colour(0xffd8dce4));
		setColour(PopupMenu::highlightedBackgroundColourId, Colour(0xff3b2a5c));
		setColour(PopupMenu::highlightedTextColourId, Colours::white);
		setColour(ComboBox::textColourId, Colour(0xffb9f5c8));
		setColour(TextButton::textColourOffId, Colour(0xffd8dce4));
		setColour(TextButton::textColourOnId, Colours::white);
	}

	void PedalLookAndFeel::DrawScrew(Graphics& g, Point<float> centre, float radius, float angle)
	{
		const auto area = Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
		g.setColour(Colours::black.withAlpha(0.45f));
		g.fillEllipse(area.translated(0.0f, radius * 0.25f).expanded(radius * 0.15f));
		g.setGradientFill(ColourGradient(Colour(0xffe2e4e8), area.getX(), area.getY(),
			Colour(0xff55595f), area.getRight(), area.getBottom(), false));
		g.fillEllipse(area);
		g.setColour(Colour(0xff2b2e33));
		const auto dir = Point<float>(std::cos(angle), std::sin(angle)) * radius * 0.7f;
		g.drawLine(Line<float>(centre - dir, centre + dir), radius * 0.28f);
	}

	void PedalLookAndFeel::DrawBrushed(Graphics& g, Rectangle<float> area, Colour base, int seed)
	{
		g.setGradientFill(ColourGradient(base.brighter(0.25f), area.getX(), area.getY(),
			base.darker(0.35f), area.getX(), area.getBottom(), false));
		g.fillRect(area);
		// Fine horizontal streaks, the same for a given seed so a repaint does not shimmer.
		Random random(seed);
		for (float y = area.getY(); y < area.getBottom(); y += 1.0f)
		{
			const float alpha = random.nextFloat() * 0.07f;
			g.setColour((random.nextBool() ? Colours::white : Colours::black).withAlpha(alpha));
			g.fillRect(area.getX(), y, area.getWidth(), 1.0f);
		}
	}

	void PedalLookAndFeel::DrawStompKnob(Graphics& g, Rectangle<float> area, float angle, Colour accent, bool chrome)
	{
		const auto centre = area.getCentre();
		const float radius = area.getWidth() * 0.5f;

		// Drop shadow, then the knurled skirt, then the cap.
		g.setColour(Colours::black.withAlpha(0.55f));
		g.fillEllipse(area.translated(0.0f, radius * 0.12f).expanded(radius * 0.06f));

		g.setGradientFill(chrome
			? ColourGradient(Colour(0xfff1f2f4), centre.x - radius, centre.y - radius, Colour(0xff4a4e55), centre.x + radius, centre.y + radius, false)
			: ColourGradient(Colour(0xff3c3f45), centre.x, centre.y - radius, Colour(0xff08090a), centre.x, centre.y + radius, false));
		g.fillEllipse(area);

		g.setColour((chrome ? Colours::black : Colours::white).withAlpha(0.18f));
		for (int i = 0; i < 48; ++i)
		{
			const float a = MathConstants<float>::twoPi * static_cast<float>(i) / 48.0f;
			const auto dir = Point<float>(std::cos(a), std::sin(a));
			g.drawLine(Line<float>(centre + dir * radius * 0.80f, centre + dir * radius * 0.98f), 1.0f);
		}

		const auto cap = area.reduced(radius * 0.24f);
		g.setGradientFill(chrome
			? ColourGradient(Colour(0xffffffff), cap.getX(), cap.getY(), Colour(0xff8b9097), cap.getRight(), cap.getBottom(), false)
			: ColourGradient(Colour(0xff2e3136), cap.getX(), cap.getY(), Colour(0xff111215), cap.getRight(), cap.getBottom(), false));
		g.fillEllipse(cap);
		g.setColour(Colours::white.withAlpha(chrome ? 0.6f : 0.08f));
		g.drawEllipse(cap.reduced(0.5f), 1.0f);

		// The pointer: a white line on black knobs, the pedal's accent on chrome ones.
		const auto dir = Point<float>(std::sin(angle), -std::cos(angle));
		g.setColour(chrome ? accent.darker(0.2f) : Colours::white);
		g.drawLine(Line<float>(centre + dir * radius * 0.22f, centre + dir * radius * 0.74f), jmax(2.0f, radius * 0.11f));
	}

	void PedalLookAndFeel::DrawAmpKnob(Graphics& g, Rectangle<float> area, float angle)
	{
		// A chicken-head pointer over a black base, the classic amp knob.
		const auto centre = area.getCentre();
		const float radius = area.getWidth() * 0.5f;

		g.setColour(Colours::black.withAlpha(0.5f));
		g.fillEllipse(area.translated(0.0f, radius * 0.12f).expanded(radius * 0.04f));
		g.setGradientFill(ColourGradient(Colour(0xff34363a), centre.x, centre.y - radius,
			Colour(0xff050506), centre.x, centre.y + radius, false));
		g.fillEllipse(area.reduced(radius * 0.12f));

		Path head;
		head.startNewSubPath(0.0f, -radius * 0.98f);
		head.cubicTo(radius * 0.22f, -radius * 0.6f, radius * 0.48f, radius * 0.1f, radius * 0.40f, radius * 0.38f);
		head.cubicTo(radius * 0.25f, radius * 0.62f, -radius * 0.25f, radius * 0.62f, -radius * 0.40f, radius * 0.38f);
		head.cubicTo(-radius * 0.48f, radius * 0.1f, -radius * 0.22f, -radius * 0.6f, 0.0f, -radius * 0.98f);
		head.closeSubPath();
		head.applyTransform(AffineTransform::rotation(angle).translated(centre));

		g.setColour(Colours::black.withAlpha(0.45f));
		g.fillPath(head, AffineTransform::translation(0.0f, radius * 0.08f));
		g.setGradientFill(ColourGradient(Colour(0xfff6ecd2), centre.x - radius, centre.y - radius,
			Colour(0xffa89a78), centre.x + radius, centre.y + radius, false));
		g.fillPath(head);
		g.setColour(Colour(0xff4a4232));
		g.strokePath(head, PathStrokeType(1.0f));

		const auto dir = Point<float>(std::sin(angle), -std::cos(angle));
		g.setColour(Colour(0xff1d1a14));
		g.drawLine(Line<float>(centre + dir * radius * 0.35f, centre + dir * radius * 0.88f), jmax(1.5f, radius * 0.07f));
	}

	void PedalLookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int width, int height, float position,
		float startAngle, float endAngle, Slider& slider)
	{
		// Room for the value arc around the knob; a photographic amp knob has none (its pointer shows the value) and
		// takes the whole space.
		const auto style = slider.getProperties()["knob"].toString();
		const bool photoAmp = style == "amp" && Skin::Get("knob-amp").isValid();
		const float size = static_cast<float>(jmin(width, height)) - (photoAmp ? 0.0f : 12.0f);
		const auto area = Rectangle<float>(size, size).withCentre(Rectangle<int>(x, y, width, height).toFloat().getCentre());
		const float angle = startAngle + position * (endAngle - startAngle);
		const auto accent = slider.findColour(Slider::thumbColourId);

		// The value arc around the knob, faint, brighter while the knob is being turned or hovered.
		const float arcRadius = size * 0.5f + 3.0f;
		Path track, value;
		track.addCentredArc(area.getCentreX(), area.getCentreY(), arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
		value.addCentredArc(area.getCentreX(), area.getCentreY(), arcRadius, arcRadius, 0.0f, startAngle, angle, true);
		if (!photoAmp)
		{
			g.setColour(Colours::black.withAlpha(0.35f));
			g.strokePath(track, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
			g.setColour(accent.withAlpha(slider.isMouseOverOrDragging() ? 0.95f : 0.55f));
			g.strokePath(value, PathStrokeType(2.0f, PathStrokeType::curved, PathStrokeType::rounded));
		}

		// The skin's photo knobs: the amp knob turns as a whole; a pedal knob stays still (its lighting with it) and
		// gets a pointer line drawn over it. Without the skin, vector knobs.
		const auto& photo = Skin::Get(style == "amp" ? "knob-amp" : (style == "chrome" ? "knob-chrome" : "knob-stomp"));
		if (photo.isValid())
		{
			g.setColour(Colours::black.withAlpha(0.5f));
			g.fillEllipse(area.reduced(size * 0.06f).translated(0.0f, size * 0.07f));
			g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
			g.setOpacity(1.0f);   // an image is drawn at the current colour's alpha: not the shadow's
			const float scale = size / jmax(photo.getWidth(), photo.getHeight());
			auto transform = AffineTransform::translation(-photo.getWidth() * 0.5f, -photo.getHeight() * 0.5f).scaled(scale);
			if (style == "amp") transform = transform.rotated(angle);
			g.drawImageTransformed(photo, transform.translated(area.getCentre()));
			if (style != "amp")
			{
				const auto centre = area.getCentre();
				const float radius = size * 0.5f;
				const auto dir = Point<float>(std::sin(angle), -std::cos(angle));
				g.setColour(style == "chrome" ? Colour(0xff1a1b1e) : Colours::white.withAlpha(0.92f));
				g.drawLine(Line<float>(centre + dir * radius * 0.30f, centre + dir * radius * 0.70f), jmax(2.0f, radius * 0.09f));
			}
			return;
		}
		if (style == "amp") DrawAmpKnob(g, area, angle);
		else DrawStompKnob(g, area, angle, accent, style == "chrome");
	}

	void PedalLookAndFeel::drawLinearSlider(Graphics& g, int x, int y, int width, int height, float position,
		float, float, Slider::SliderStyle style, Slider& slider)
	{
		if (style != Slider::LinearVertical)
		{
			LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, position, 0.0f, 0.0f, style, slider);
			return;
		}

		const auto bounds = Rectangle<int>(x, y, width, height).toFloat();
		const float capHeight = 12.0f, capWidth = jmin(30.0f, bounds.getWidth() - 4.0f);

		// The slot, sunk into the panel, with a centre (0 dB) tick and side ticks.
		const auto slot = Rectangle<float>(6.0f, bounds.getHeight() + 6.0f).withCentre(bounds.getCentre());
		g.setColour(Colours::black);
		g.fillRoundedRectangle(slot, 3.0f);
		g.setColour(Colours::white.withAlpha(0.07f));
		g.drawRoundedRectangle(slot.translated(0.0f, 1.0f), 3.0f, 1.0f);

		g.setColour(Colours::white.withAlpha(0.22f));
		for (int i = 0; i <= 10; ++i)
		{
			const float ty = bounds.getY() + bounds.getHeight() * static_cast<float>(i) / 10.0f;
			const float len = i == 5 ? 7.0f : 4.0f;
			g.drawLine(slot.getX() - 3.0f - len, ty, slot.getX() - 3.0f, ty, 1.0f);
			g.drawLine(slot.getRight() + 3.0f, ty, slot.getRight() + 3.0f + len, ty, 1.0f);
		}

		// The cap: dark plastic with a glowing line across it, like a lit graphic EQ.
		const auto accent = slider.findColour(Slider::thumbColourId);
		const auto cap = Rectangle<float>(capWidth, capHeight).withCentre({ bounds.getCentreX(), position });
		g.setColour(Colours::black.withAlpha(0.6f));
		g.fillRoundedRectangle(cap.translated(0.0f, 3.0f), 2.5f);
		g.setGradientFill(ColourGradient(Colour(0xff4b4e55), cap.getX(), cap.getY(),
			Colour(0xff17181b), cap.getX(), cap.getBottom(), false));
		g.fillRoundedRectangle(cap, 2.5f);

		const auto glow = cap.withSizeKeepingCentre(cap.getWidth() - 4.0f, 4.0f);
		for (int i = 3; i >= 1; --i)
		{
			g.setColour(accent.withAlpha(0.12f));
			g.fillRoundedRectangle(glow.expanded(static_cast<float>(i) * 2.5f), 4.0f);
		}
		g.setColour(accent.brighter(slider.isMouseOverOrDragging() ? 0.5f : 0.2f));
		g.fillRoundedRectangle(glow, 2.0f);
	}

	void PedalLookAndFeel::drawToggleButton(Graphics& g, ToggleButton& button, bool, bool)
	{
		// A mini toggle: a hex nut with a chrome bat lever, up for on.
		const auto bounds = button.getLocalBounds().toFloat();
		const float nut = jmin(bounds.getWidth(), bounds.getHeight()) * 0.42f;
		const auto centre = bounds.getCentre();

		Path hex;
		hex.addPolygon(centre, 6, nut * 0.5f, 0.0f);
		g.setGradientFill(ColourGradient(Colour(0xffd9dce0), centre.x, centre.y - nut, Colour(0xff5b5f66), centre.x, centre.y + nut, false));
		g.fillPath(hex);

		const bool on = button.getToggleState();
		const float lever = nut * 0.95f;
		const auto tip = centre + Point<float>(0.0f, on ? -lever : lever);
		g.setColour(Colours::black.withAlpha(0.4f));
		g.drawLine(Line<float>(centre.translated(1.5f, 1.5f), tip.translated(1.5f, 1.5f)), nut * 0.22f);
		g.setGradientFill(ColourGradient(Colours::white, tip.x - 3.0f, tip.y, Colour(0xff7a7f87), tip.x + 3.0f, tip.y, false));
		g.drawLine(Line<float>(centre, tip), nut * 0.22f);
		g.fillEllipse(Rectangle<float>(nut * 0.36f, nut * 0.36f).withCentre(tip));
	}

	void PedalLookAndFeel::drawComboBox(Graphics& g, int width, int height, bool, int, int, int, int, ComboBox& box)
	{
		// An LCD window: a dark bezel, a green-black screen with a faint scanline, a small arrow.
		const auto bounds = Rectangle<float>(static_cast<float>(width), static_cast<float>(height));
		g.setColour(Colour(0xff050506));
		g.fillRoundedRectangle(bounds, 4.0f);
		const auto screen = bounds.reduced(2.5f);
		g.setGradientFill(ColourGradient(Colour(0xff0f2017), screen.getX(), screen.getY(),
			Colour(0xff06100b), screen.getX(), screen.getBottom(), false));
		g.fillRoundedRectangle(screen, 3.0f);
		g.setColour(Colours::white.withAlpha(0.05f));
		g.fillRect(screen.withHeight(screen.getHeight() * 0.45f).reduced(2.0f, 1.0f));
		g.setColour(Colours::white.withAlpha(box.isMouseOver(true) ? 0.25f : 0.10f));
		g.drawRoundedRectangle(bounds.reduced(0.5f), 4.0f, 1.0f);

		if (box.getProperties()["display"]) return;   // a display (PedalView): no drop-down arrow
		Path arrow;
		const float ax = bounds.getRight() - 14.0f, ay = bounds.getCentreY();
		arrow.addTriangle(ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
		g.setColour(box.findColour(ComboBox::textColourId).withAlpha(0.8f));
		g.fillPath(arrow);
	}

	Font PedalLookAndFeel::getComboBoxFont(ComboBox& box)
	{
		return Font(FontOptions(Font::getDefaultMonospacedFontName(), jmin(15.0f, box.getHeight() * 0.55f), Font::plain));
	}

	void PedalLookAndFeel::positionComboBoxText(ComboBox& box, Label& label)
	{
		label.setBounds(8, 1, box.getWidth() - (box.getProperties()["display"] ? 16 : 26), box.getHeight() - 2);
		label.setFont(getComboBoxFont(box));
		label.setJustificationType(Justification::centredLeft);
	}

	void PedalLookAndFeel::drawButtonBackground(Graphics& g, Button& button, const Colour&, bool highlighted, bool down)
	{
		const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
		g.setColour(Colour(down ? 0xff2a2c31 : (highlighted ? 0xff24262b : 0xff1b1d21)));
		g.fillRoundedRectangle(bounds, 4.0f);
		g.setColour(Colours::white.withAlpha(button.getToggleState() ? 0.45f : 0.14f));
		g.drawRoundedRectangle(bounds, 4.0f, 1.0f);
	}
}
