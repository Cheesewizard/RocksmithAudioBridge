#include "PedalView.hpp"
#include "PedalLookAndFeel.hpp"
#include "PedalSkin.hpp"

namespace PedalUi
{
	using namespace juce;

	namespace
	{
		constexpr float kRotaryStart = MathConstants<float>::pi * 1.2f;   // 7 o'clock to 5 o'clock
		constexpr float kRotaryEnd = MathConstants<float>::pi * 2.8f;

		Font LabelFont(float height, bool bold = true)
		{
			return Font(FontOptions(height, bold ? Font::bold : Font::plain));
		}

		bool IsDark(Colour colour) { return colour.getPerceivedBrightness() < 0.3f; }

		// A skin surface (PedalSkin) over `area`, clipped to its rounded corners. False when the piece is missing:
		// the caller draws its own surface then.
		bool SkinSurface(Graphics& g, const Image& image, Rectangle<float> area, float corner, float opacity = 1.0f)
		{
			if (!image.isValid()) return false;
			Graphics::ScopedSaveState save(g);
			Path clip;
			clip.addRoundedRectangle(area, corner);
			g.reduceClipRegion(clip);
			Skin::Cover(g, image, area, opacity);
			return true;
		}

		// Paints `count` faint flecks over `area`, the same for a given seed: metal-flake paint, tolex grain.
		void Speckle(Graphics& g, Rectangle<float> area, int seed, int count, float alpha)
		{
			Random random(seed);
			for (int i = 0; i < count; ++i)
			{
				const float x = area.getX() + random.nextFloat() * area.getWidth();
				const float y = area.getY() + random.nextFloat() * area.getHeight();
				g.setColour((random.nextBool() ? Colours::white : Colours::black).withAlpha(random.nextFloat() * alpha));
				g.fillRect(x, y, 1.2f, 1.2f);
			}
		}
	}

	Body ChooseBody(const PluginDesc& plugin, const String& captureKind)
	{
		int faders = 0;
		bool namModel = false, cabinetIr = false, presetEnum = false;
		for (auto& param : plugin.params)
		{
			if (param.type == ParamType::VSlider) ++faders;
			if (param.type == ParamType::File && param.fileTree == "NAM") namModel = true;
			if (param.type == ParamType::File && param.fileTree == "Cabinets") cabinetIr = true;
			if (param.type == ParamType::Enum && param.name == "Preset") presetEnum = true;
		}
		if (faders >= 3) return Body::Rack;
		if (namModel && captureKind == "pedal") return Body::Stomp;
		if (namModel || presetEnum) return Body::Amp;
		// ConvoReverb also picks from the Cabinets folder, but it is an effect: a stompbox.
		if (cabinetIr && !plugin.name.containsIgnoreCase("Reverb")) return Body::Cabinet;
		return Body::Stomp;
	}

	CaptureLookup ReadCaptureManifest(const File& manifest)
	{
		// "NAM/DS-1 Distortion.nam" -> key "NAM/DS-1 Distortion" (the folder, and the file as Stompbox names it).
		auto known = std::make_shared<std::map<String, CaptureInfo>>();
		const auto json = JSON::parse(manifest.loadFileAsString());
		if (auto* files = json["files"].getArray())
			for (auto& file : *files)
			{
				const auto path = file["path"].toString();
				if (path.isEmpty()) continue;
				(*known)[path.upToLastOccurrenceOf(".", false, false)] = { file["kind"].toString(), file["description"].toString(), file["character"].toString() };
			}
		return [known](const String& tree, const String& name) -> CaptureInfo
		{
			const auto found = known->find(tree + "/" + name);
			return found != known->end() ? found->second : CaptureInfo{};
		};
	}

	//==============================================================================================================
	// An output (IsOutput) parameter: a row of LEDs, green to red.

	class PedalView::Meter final : public Component, public SettableTooltipClient
	{
	public:
		void SetLevel(float level) { if (level != this->level) { this->level = level; repaint(); } }

		void paint(Graphics& g) override
		{
			const auto bounds = getLocalBounds().toFloat();
			g.setColour(Colours::black.withAlpha(0.75f));
			g.fillRoundedRectangle(bounds, 3.0f);
			constexpr int segments = 12;
			const float gap = 2.0f;
			const float w = (bounds.getWidth() - gap * (segments + 1)) / segments;
			for (int i = 0; i < segments; ++i)
			{
				const float t = static_cast<float>(i) / (segments - 1);
				const auto colour = t < 0.6f ? Colour(0xff35e06a) : (t < 0.85f ? Colour(0xfff2c230) : Colour(0xffff4433));
				const bool lit = level * segments > static_cast<float>(i) + 0.5f;
				g.setColour(lit ? colour : colour.withAlpha(0.13f));
				g.fillRoundedRectangle(bounds.getX() + gap + i * (w + gap), bounds.getY() + 2.0f, w, bounds.getHeight() - 4.0f, 1.5f);
			}
		}

	private:
		float level = 0.0f;
	};

	//==============================================================================================================
	// The bypass: a chrome footswitch on stompboxes, a power rocker on the rest.

	class PedalView::FootSwitch final : public Component, public SettableTooltipClient
	{
	public:
		FootSwitch(PedalView& owner, bool stomp) : owner(owner), stomp(stomp)
		{
			setMouseCursor(MouseCursor::PointingHandCursor);
			setTooltip("On / bypass");
		}

		bool IsStomp() const { return stomp; }

		void mouseDown(const MouseEvent&) override { pressed = true; repaint(); }
		void mouseUp(const MouseEvent& e) override
		{
			pressed = false;
			if (getLocalBounds().contains(e.getPosition()))
			{
				owner.SetEnabled(!owner.IsEnabled());
				if (owner.onEnabledChange) owner.onEnabledChange(owner.IsEnabled());
			}
			repaint();
		}

		void paint(Graphics& g) override
		{
			const auto bounds = getLocalBounds().toFloat();
			if (stomp) PaintStomp(g, bounds);
			else PaintRocker(g, bounds);
		}

	private:
		PedalView& owner;
		const bool stomp;
		bool pressed = false;

		void PaintStomp(Graphics& g, Rectangle<float> bounds)
		{
			if (const auto& image = Skin::Get("footswitch"); image.isValid())
			{
				// The skin's photo, a touch smaller while pressed, with a soft shadow under it.
				const float size = jmin(bounds.getWidth(), bounds.getHeight()) * (pressed ? 0.95f : 1.0f);
				const auto area = Rectangle<float>(size, size).withCentre(bounds.getCentre());
				g.setColour(Colours::black.withAlpha(0.35f));
				g.fillEllipse(area.reduced(size * 0.12f).translated(0.0f, size * 0.06f));
				g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
				g.setOpacity(1.0f);   // an image is drawn at the current colour's alpha: not the shadow's
				g.drawImage(image, area, RectanglePlacement::centred);
				return;
			}
			const auto centre = bounds.getCentre();
			const float r = jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;

			Path nut;
			nut.addPolygon(centre, 6, r, MathConstants<float>::pi / 6.0f);
			g.setColour(Colours::black.withAlpha(0.5f));
			g.fillPath(nut, AffineTransform::translation(0.0f, 3.0f));
			g.setGradientFill(ColourGradient(Colour(0xffc9ccd1), centre.x, centre.y - r, Colour(0xff4c5057), centre.x, centre.y + r, false));
			g.fillPath(nut);

			const float dome = r * (pressed ? 0.60f : 0.66f);
			const auto domeArea = Rectangle<float>(dome * 2.0f, dome * 2.0f).withCentre(centre.translated(0.0f, pressed ? 1.0f : 0.0f));
			g.setColour(Colours::black.withAlpha(0.45f));
			g.fillEllipse(domeArea.translated(0.0f, 2.5f));
			g.setGradientFill(ColourGradient(Colours::white, domeArea.getX(), domeArea.getY(),
				Colour(0xff6d727a), domeArea.getRight(), domeArea.getBottom(), true));
			g.fillEllipse(domeArea);
			g.setColour(Colours::white.withAlpha(0.7f));
			g.fillEllipse(domeArea.reduced(dome * 0.55f).translated(-dome * 0.25f, -dome * 0.25f));
		}

		void PaintRocker(Graphics& g, Rectangle<float> bounds)
		{
			const auto body = bounds.withSizeKeepingCentre(jmin(26.0f, bounds.getWidth()), jmin(38.0f, bounds.getHeight()));
			g.setColour(Colours::black);
			g.fillRoundedRectangle(body.expanded(2.0f), 4.0f);
			const bool on = owner.IsEnabled();
			auto top = body.withHeight(body.getHeight() * 0.5f), bottom = body.withTrimmedTop(body.getHeight() * 0.5f);
			auto& raised = on ? bottom : top;
			auto& sunk = on ? top : bottom;
			g.setGradientFill(ColourGradient(Colour(0xff4a4d53), raised.getX(), raised.getY(), Colour(0xff222326), raised.getX(), raised.getBottom(), false));
			g.fillRoundedRectangle(raised, 3.0f);
			g.setColour(Colour(0xff121315));
			g.fillRoundedRectangle(sunk, 3.0f);
			g.setColour(Colours::white.withAlpha(0.75f));
			g.setFont(LabelFont(11.0f));
			g.drawText("I", top, Justification::centred);
			g.drawText("O", bottom, Justification::centred);
		}
	};

	//==============================================================================================================
	// One parameter: its control and its name, which shows the value while the control is hovered or turned.

	class PedalView::Control final : public MouseListener
	{
	public:
		Control(PedalView& owner, const ParamDesc& desc, FileLister& files) : owner(owner), desc(desc)
		{
			label.setText(desc.name.toUpperCase(), dontSendNotification);
			label.setJustificationType(Justification::centred);
			label.setFont(LabelFont(11.0f));
			label.setInterceptsMouseClicks(false, false);
			owner.addAndMakeVisible(label);

			switch (desc.type)
			{
			case ParamType::Knob:
			case ParamType::Int:
			case ParamType::VSlider:
			{
				slider = std::make_unique<Slider>(desc.type == ParamType::VSlider ? Slider::LinearVertical : Slider::RotaryVerticalDrag,
					Slider::NoTextBox);
				const double span = desc.max > desc.min ? desc.max - desc.min : 1.0;
				slider->setNormalisableRange(NormalisableRange<double>(desc.min, desc.min + span,
					desc.type == ParamType::Int ? 1.0 : 0.0, 1.0 / desc.power));
				slider->setRotaryParameters(kRotaryStart, kRotaryEnd, true);
				slider->setDoubleClickReturnValue(true, desc.def);
				slider->setValue(desc.def, dontSendNotification);
				slider->setTooltip(desc.description);
				slider->setMouseDragSensitivity(220);
				slider->onValueChange = [this] { Report(); };
				slider->onDragStart = [this] { dragging = true; UpdateLabel(); };
				slider->onDragEnd = [this] { dragging = false; UpdateLabel(); };
				main = slider.get();
				break;
			}
			case ParamType::Bool:
				toggle = std::make_unique<ToggleButton>();
				toggle->setToggleState(desc.def >= 0.5, dontSendNotification);
				toggle->setTooltip(desc.description);
				toggle->onClick = [this] { Report(); };
				main = toggle.get();
				break;
			case ParamType::Enum:
			case ParamType::File:
			{
				combo = std::make_unique<ComboBox>();
				// Stompbox's config lists a File parameter's files; a saved catalogue without them asks `files`.
				const auto items = desc.choices.size() > 0 || desc.type == ParamType::Enum ? desc.choices
					: (files ? files(desc.fileTree) : StringArray());
				combo->addItemList(items, 1);
				if (desc.type == ParamType::Enum) combo->setSelectedItemIndex(roundToInt(desc.def), dontSendNotification);
				else if (items.size() > 0) combo->setSelectedItemIndex(0, dontSendNotification);
				combo->setTextWhenNothingSelected(desc.type == ParamType::File ? "No file" : "-");
				combo->setTextWhenNoChoicesAvailable("Nothing in " + desc.fileTree);
				// A display, not a picker: what the pedal is (its capture, its IR, its amp type) is chosen on the
				// page's board. Choosing it here looked like changing the pedal, so the window only shows it.
				combo->setTooltip(desc.description + (desc.description.isNotEmpty() ? "\n" : "") + "Change it on the Guitar Tone page.");
				combo->setInterceptsMouseClicks(false, false);
				combo->setWantsKeyboardFocus(false);
				combo->getProperties().set("display", true);
				main = combo.get();
				break;
			}
			case ParamType::Meter:
				meter = std::make_unique<Meter>();
				meter->setTooltip(desc.description);
				main = meter.get();
				SetValue(desc.def);
				break;
			}
			owner.addAndMakeVisible(*main);
			main->addMouseListener(this, true);
		}

		~Control() override { main->removeMouseListener(this); }

		PedalView& owner;
		const ParamDesc& desc;
		Label label;
		bool labelAbove = false;

		bool IsKnob() const { return desc.type == ParamType::Knob || desc.type == ParamType::Int; }
		bool IsFader() const { return desc.type == ParamType::VSlider; }
		bool IsPicker() const { return desc.type == ParamType::Enum || desc.type == ParamType::File; }
		bool IsToggle() const { return desc.type == ParamType::Bool; }
		bool IsMeter() const { return desc.type == ParamType::Meter; }

		Slider* GetSlider() const { return slider.get(); }
		ComboBox* GetCombo() const { return combo.get(); }

		void SetVisible(bool visible)
		{
			main->setVisible(visible);
			label.setVisible(visible && !IsPicker());
		}

		double Value() const
		{
			if (slider) return slider->getValue();
			if (toggle) return toggle->getToggleState() ? 1.0 : 0.0;
			if (combo) return combo->getSelectedItemIndex();
			return meterValue;
		}

		void SetValue(double value)
		{
			if (slider) slider->setValue(value, dontSendNotification);
			else if (toggle) toggle->setToggleState(value >= 0.5, dontSendNotification);
			else if (combo && desc.type == ParamType::Enum) combo->setSelectedItemIndex(roundToInt(value), dontSendNotification);
			else if (meter)
			{
				meterValue = value;
				const double span = desc.max - desc.min;
				meter->SetLevel(span > 0.0 ? static_cast<float>(std::pow(jlimit(0.0, 1.0, (value - desc.min) / span), 1.0 / desc.power)) : 0.0f);
			}
			UpdateLabel();
		}

		void SetChoice(const String& choice)
		{
			if (!combo) return;
			for (int i = 0; i < combo->getNumItems(); ++i)
				if (combo->getItemText(i) == choice) { combo->setSelectedItemIndex(i, dontSendNotification); return; }
			// An Enum written as its index.
			if (desc.type == ParamType::Enum && choice.containsOnly("0123456789") && choice.isNotEmpty())
			{
				combo->setSelectedItemIndex(choice.getIntValue(), dontSendNotification);
				return;
			}
			if (choice.isEmpty()) { combo->setSelectedId(0, dontSendNotification); return; }
			combo->addItem(choice, combo->getNumItems() + 1);   // a file the folder does not have (yet): keep showing it
			combo->setSelectedItemIndex(combo->getNumItems() - 1, dontSendNotification);
		}

		// Lays out the control in `area`: a knob `knobSize` across with its name under it (or over it, labelAbove).
		void Place(Rectangle<int> area, int knobSize)
		{
			constexpr int labelHeight = 16;
			if (IsPicker()) { main->setBounds(area); return; }
			if (IsMeter())
			{
				label.setBounds(area.removeFromTop(labelHeight));
				main->setBounds(area.withSizeKeepingCentre(jmin(area.getWidth(), 120), 12));
				return;
			}
			auto labelArea = labelAbove ? area.removeFromTop(labelHeight) : area.removeFromBottom(labelHeight);
			label.setBounds(labelArea.withSizeKeepingCentre(jmax(labelArea.getWidth(), knobSize + 24), labelHeight));
			if (IsFader()) main->setBounds(area.withSizeKeepingCentre(jmin(area.getWidth(), 34), area.getHeight()));
			else if (IsToggle()) main->setBounds(area.withSizeKeepingCentre(30, jmin(area.getHeight(), 34)));
			else main->setBounds(area.withSizeKeepingCentre(knobSize, knobSize));
		}

		void mouseEnter(const MouseEvent&) override { hovering = true; UpdateLabel(); }
		void mouseExit(const MouseEvent&) override { hovering = false; UpdateLabel(); }

	private:
		std::unique_ptr<Slider> slider;
		std::unique_ptr<ToggleButton> toggle;
		std::unique_ptr<ComboBox> combo;
		std::unique_ptr<Meter> meter;
		Component* main = nullptr;
		double meterValue = 0.0;
		bool hovering = false, dragging = false;

		void UpdateLabel()
		{
			const bool showValue = (hovering || dragging) && (slider != nullptr);
			label.setText(showValue ? desc.Format(Value()) : desc.name.toUpperCase(), dontSendNotification);
		}

		void Report()
		{
			UpdateLabel();
			owner.Changed(*this);
		}
	};

	//==============================================================================================================

	PedalView::PedalView(const PluginDesc& plugin, FileLister files, CaptureLookup captures)
		: plugin(plugin), captures(std::move(captures)), body(ChooseBody(plugin))
	{
		for (auto& param : this->plugin.params)
			controls.push_back(std::make_unique<Control>(*this, param, files));

		ApplyBody();
		Layout();
	}

	const PedalView::Control* PedalView::FileControl() const
	{
		for (auto& control : controls)
			if (control->desc.type == ParamType::File && (control->desc.fileTree == "NAM" || control->desc.fileTree == "Cabinets"))
				return control.get();
		return nullptr;
	}

	// A NAM pedal is called after its capture and the cab after its IR; an effect that loads a file (ConvoReverb's
	// impulse) keeps its own name.
	bool PedalView::NamedAfterFile() const
	{
		const auto* file = FileControl();
		return file && (file->desc.fileTree == "NAM" || body == Body::Cabinet);
	}

	String PedalView::DisplayName() const
	{
		if (const auto* file = NamedAfterFile() ? FileControl() : nullptr)
			if (file->GetCombo() && file->GetCombo()->getText().isNotEmpty())
				return file->GetCombo()->getText().fromLastOccurrenceOf("/", false, false);
		return plugin.name;
	}

	String PedalView::DisplayDescription() const
	{
		if (const auto* file = NamedAfterFile() ? FileControl() : nullptr)
			if (captures && file->GetCombo())
				if (const auto info = captures(file->desc.fileTree, file->GetCombo()->getText()); info.description.isNotEmpty())
					return info.description;
		return plugin.description;
	}

	// The body follows the chosen capture (an amp head for an amp, a stompbox for a pedal), and the controls take
	// that body's look.
	void PedalView::ApplyBody()
	{
		CaptureInfo capture;
		if (const auto* file = FileControl())
			if (captures && file->desc.fileTree == "NAM" && file->GetCombo())
				capture = captures(file->desc.fileTree, file->GetCombo()->getText());
		body = ChooseBody(plugin, capture.kind);

		// The whole-body art for it: an amp head that suits the capture (a clean amp gets the vintage head, a
		// high-gain one the modern head), the cab, the rack unit, the pedal shell. Missing art: drawn by code.
		switch (body)
		{
		case Body::Amp: shellName = capture.character == "high-gain" ? "amp-modern" : (capture.character == "clean" ? "amp-vintage" : "amp-british"); break;
		case Body::Cabinet: shellName = "cab-front"; break;
		case Body::Rack: shellName = "rack-unit"; break;
		case Body::Stomp: shellName = "pedal-shell"; break;
		}
		if (!Skin::GetShell(shellName.toRawUTF8())) shellName.clear();

		switch (body)
		{
		case Body::Rack: accent = Colour(0xffa855f7); break;
		case Body::Amp: accent = Colour(0xffffb347); break;
		case Body::Cabinet: accent = Colour(0xffd0d4da); break;
		case Body::Stomp: accent = IsDark(plugin.background) ? plugin.background.withBrightness(0.95f).withSaturation(0.6f)
			: plugin.foreground; break;
		}
		if (body == Body::Stomp && IsDark(plugin.background) && IsDark(accent)) accent = Colour(0xffd0d4da);

		for (auto& control : controls)
		{
			if (auto* slider = control->GetSlider())
			{
				slider->setColour(Slider::thumbColourId, accent);
				const bool chrome = body == Body::Cabinet || (body == Body::Stomp && IsDark(plugin.background));
				slider->getProperties().set("knob", body == Body::Amp ? "amp" : (chrome ? "chrome" : "stomp"));
			}
			const auto labelColour = body == Body::Stomp ? plugin.foreground
				: (body == Body::Amp ? Colour(0xff17171a) : Colour(0xffe9ebef));
			control->label.setColour(Label::textColourId, labelColour.withAlpha(0.9f));
			control->labelAbove = body != Body::Stomp;
		}

		const bool stomp = body == Body::Stomp;
		if (!footSwitch || footSwitch->IsStomp() != stomp)
		{
			footSwitch = std::make_unique<FootSwitch>(*this, stomp);
			addAndMakeVisible(*footSwitch);
		}
	}

	// Another capture or IR: the name changes, and a capture of another kind brings another body.
	void PedalView::CaptureChanged()
	{
		const Body before = body;
		ApplyBody();
		if (body != before)
		{
			Layout();
			if (onLayoutChange) onLayoutChange();
		}
		repaint();
		if (onNameChange) onNameChange();
	}

	PedalView::~PedalView() = default;

	void PedalView::SetValue(const String& param, double value)
	{
		for (auto& control : controls)
			if (control->desc.name == param) control->SetValue(value);
		if (onLookChange) onLookChange();
	}

	void PedalView::SetChoice(const String& param, const String& choice)
	{
		for (auto& control : controls)
			if (control->desc.name == param)
			{
				control->SetChoice(choice);
				if (control.get() == FileControl()) CaptureChanged();
			}
		if (body == Body::Cabinet) repaint();
		if (onLookChange) onLookChange();
	}

	void PedalView::SetValueText(const String& param, const String& text)
	{
		const auto* desc = plugin.Find(param);
		if (!desc) return;
		if (desc->IsChoice()) SetChoice(param, text);
		else SetValue(param, text.getDoubleValue());
	}

	void PedalView::SetEnabled(bool enabled)
	{
		this->enabled = enabled;
		repaint();
		if (onLookChange) onLookChange();
	}

	void PedalView::Changed(Control& control)
	{
		if (control.IsPicker())
		{
			if (onChoiceChange) onChoiceChange(control.desc.name, control.GetCombo()->getText());
			if (&control == FileControl()) CaptureChanged();
		}
		else if (onParamChange) onParamChange(control.desc.name, control.Value());
		if (body == Body::Cabinet) repaint();   // the grille shows the chosen IR's speakers
		if (onLookChange) onLookChange();
	}

	std::vector<PedalView::Control*> PedalView::Visible(bool wantAdvanced) const
	{
		std::vector<Control*> result;
		for (auto& control : controls)
			if (control->desc.advanced == wantAdvanced) result.push_back(control.get());
		return result;
	}

	void PedalView::PlaceRow(const std::vector<Control*>& row, Rectangle<int> area, int knobSize)
	{
		if (row.empty()) return;
		const int column = area.getWidth() / static_cast<int>(row.size());
		for (auto* control : row) control->Place(area.removeFromLeft(column), knobSize);
	}

	void PedalView::Layout()
	{
		for (auto& control : controls) control->SetVisible(true);
		switch (body)
		{
		case Body::Stomp: LayoutStomp(); break;
		case Body::Amp: LayoutAmp(); break;
		case Body::Rack: LayoutRack(); break;
		case Body::Cabinet: LayoutCabinet(); break;
		}
		setSize(natural.getWidth(), natural.getHeight());
		repaint();
	}

	// Each layout splits the controls by kind, keeping the catalog's order within each.
	void PedalView::LayoutStomp()
	{
		std::vector<Control*> knobs, pickers, toggles, meters, advKnobs, advOther;
		for (auto* c : Visible(false))
			(c->IsKnob() || c->IsFader() ? knobs : c->IsPicker() ? pickers : c->IsToggle() ? toggles : meters).push_back(c);
		for (auto* c : Visible(true))
			(c->IsKnob() || c->IsFader() ? advKnobs : advOther).push_back(c);

		const int n = static_cast<int>(knobs.size());
		const int perRow = n <= 4 ? jmax(1, n) : (n <= 6 ? 3 : 4);
		const int knobSize = n <= 2 ? 70 : (n == 3 ? 62 : (n == 4 ? 50 : 56));
		const int column = knobSize + 24;
		constexpr int small = 44, smallColumn = small + 26;   // the advanced knobs: smaller, like a pedal's trim knobs
		const int width = jmax(200, perRow * column + 40, static_cast<int>(advKnobs.size() > 4 ? 4 : advKnobs.size()) * smallColumn + 40);
		const int inner = width - 40;

		int y = 24;
		for (int i = 0; i < n; i += perRow)
		{
			std::vector<Control*> row(knobs.begin() + i, knobs.begin() + jmin(n, i + perRow));
			PlaceRow(row, { 20 + (inner - static_cast<int>(row.size()) * column) / 2, y, static_cast<int>(row.size()) * column, knobSize + 18 }, knobSize);
			y += knobSize + 26;
		}
		for (auto* c : pickers) { c->Place({ 20, y, inner, 28 }, 0); y += 36; }
		if (!toggles.empty()) { PlaceRow(toggles, { 20, y, inner, 52 }, 0); y += 58; }
		for (auto* c : meters) { c->Place({ 20, y, inner, 30 }, 0); y += 34; }

		panelArea = {};
		for (size_t i = 0; i < advKnobs.size(); i += 4)
		{
			std::vector<Control*> row(advKnobs.begin() + i, advKnobs.begin() + jmin(advKnobs.size(), i + 4));
			const int rowWidth = static_cast<int>(row.size()) * smallColumn;
			PlaceRow(row, { 20 + (inner - rowWidth) / 2, y, rowWidth, small + 18 }, small);
			y += small + 24;
		}
		std::vector<Control*> pickersAdv, rest;
		for (auto* c : advOther) (c->IsPicker() ? pickersAdv : rest).push_back(c);
		if (!rest.empty()) { PlaceRow(rest, { 20, y, inner, 52 }, 0); y += 56; }
		for (auto* c : pickersAdv) { c->Place({ 20, y, inner, 26 }, 0); y += 32; }

		nameArea = Rectangle<int>(16, y + 6, width - 32, 64).toFloat();
		y += 76;
		ledArea = Rectangle<float>(12.0f, 12.0f).withCentre({ width * 0.5f, static_cast<float>(y + 6) });
		y += 22;
		footSwitch->setBounds(width / 2 - 34, y, 68, 68);
		y += 86;
		natural = { 0, 0, width, y };
	}

	void PedalView::LayoutAmp()
	{
		std::vector<Control*> knobs, pickers, toggles, meters, adv;
		for (auto* c : Visible(false))
			(c->IsKnob() || c->IsFader() ? knobs : c->IsPicker() ? pickers : c->IsToggle() ? toggles : meters).push_back(c);
		adv = Visible(true);
		shellArea = drawerArea = {};

		if (const auto* shell = shellName.isNotEmpty() ? Skin::GetShell(shellName.toRawUTF8()) : nullptr)
		{
			// The amp head's photo at its own proportions, wide enough for the knobs on its blank panel; the power
			// switch and jewel at the panel's left, the knobs centred, MORE at its right. On the grille: the name
			// plate on the left, the capture's display on the right. Advanced settings in a drawer under the amp.
			std::vector<Control*> face(knobs);
			face.insert(face.end(), toggles.begin(), toggles.end());
			std::vector<Control*> trims, advPickers;   // the advanced: smaller knobs on the panel; their pickers on the grille
			for (auto* c : adv) (c->IsPicker() ? advPickers : (c->IsMeter() ? meters : trims)).push_back(c);
			const int faceItems = static_cast<int>(face.size()), trimItems = static_cast<int>(trims.size());
			const int width = jmax(900, roundToInt((260 + faceItems * 130 + trimItems * 96) / shell->panel.getWidth()));
			const int height = roundToInt(width / shell->Aspect());
			shellArea = Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
			const auto scaled = [&](Rectangle<float> r) { return Rectangle<float>(r.getX() * width, r.getY() * height, r.getWidth() * width, r.getHeight() * height); };
			const auto panel = scaled(shell->panel), grille = scaled(shell->grille);
			panelArea = panel;

			const int labelHeight = 15;
			const int knobSize = jlimit(36, 84, roundToInt(panel.getHeight()) - labelHeight);
			const int column = knobSize + 46;
			const auto p = panel.toNearestInt();
			footSwitch->setBounds(p.getX() + 28, p.getCentreY() - 20, 26, 40);
			ledArea = Rectangle<float>(16.0f, 16.0f).withCentre({ panel.getX() + 84.0f, panel.getCentreY() });
			const int trimSize = jmax(30, knobSize * 7 / 10), trimColumn = trimSize + 40;
			const int faceLeft = p.getX() + 120, faceRight = p.getRight() - 40;
			const int rowWidth = faceItems * column + trimItems * trimColumn;
			const int rowLeft = faceLeft + jmax(0, (faceRight - faceLeft - rowWidth) / 2);
			PlaceRow(face, { rowLeft, p.getCentreY() - (knobSize + labelHeight) / 2, faceItems * column, knobSize + labelHeight }, knobSize);
			PlaceRow(trims, { rowLeft + faceItems * column, p.getCentreY() - (trimSize + labelHeight) / 2, trimItems * trimColumn,
				trimSize + labelHeight }, trimSize);

			nameArea = Rectangle<float>(grille.getWidth() * 0.34f, jmin(64.0f, grille.getHeight() * 0.42f))
				.withPosition(grille.getX() + grille.getWidth() * 0.05f, grille.getCentreY() - jmin(64.0f, grille.getHeight() * 0.42f) * 0.5f);
			const int pickerWidth = roundToInt(grille.getWidth() * 0.36f);
			int py = roundToInt(grille.getCentreY()) - (static_cast<int>(pickers.size()) * 38 - 6) / 2;
			pickers.insert(pickers.end(), advPickers.begin(), advPickers.end());
			py = roundToInt(grille.getCentreY()) - (static_cast<int>(pickers.size()) * 38 - 6) / 2;
			for (auto* c : pickers) { c->Place({ roundToInt(grille.getRight() - grille.getWidth() * 0.05f) - pickerWidth, py, pickerWidth, 32 }, 0); py += 38; }

			int y = height;
			if (!meters.empty())
			{
				// Only what the amp reports (meters) goes in a drawer under it; every setting is on the amp.
				std::vector<Control*> row(meters), drawerPickers;
				drawerArea = Rectangle<float>(panel.getX() + 20.0f, static_cast<float>(y), panel.getWidth() - 40.0f, 86.0f);
				PlaceRow(row, { roundToInt(drawerArea.getX()) + 24, y + 12, static_cast<int>(row.size()) * 76, 40 + 22 }, 40);
				for (auto* c : drawerPickers) c->Place({ roundToInt(drawerArea.getX()) + 40 + static_cast<int>(row.size()) * 76, y + 30, 240, 26 }, 0);
				y += 92;
			}
			natural = { 0, 0, width, y };
			return;
		}

		constexpr int knobSize = 56, column = 86, top = 16, band = 96;
		const int faceItems = static_cast<int>(knobs.size() + toggles.size());
		const int width = jmax(620, 130 + faceItems * column + 40);

		// The top band: the logo plate on the left, the pickers (model, preset) as displays on the right.
		nameArea = Rectangle<int>(34, top + 22, 250, band - 40).toFloat();
		int py = top + 20;
		const int pickerHeight = pickers.size() > 1 ? 26 : 32;
		for (auto* c : pickers) { c->Place({ width - 34 - 270, py, 270, pickerHeight }, 0); py += pickerHeight + 6; }

		// The faceplate: power and jewel, then the knobs with their names over them.
		int y = top + band + 4;
		const int faceHeight = knobSize + 34 + (meters.empty() ? 0 : 30) + (adv.empty() ? 0 : 82);
		panelArea = Rectangle<int>(18, y, width - 36, faceHeight).toFloat();
		footSwitch->setBounds(36, y + 20, 34, 46);
		ledArea = Rectangle<float>(18.0f, 18.0f).withCentre({ 92.0f, static_cast<float>(y + 42) });

		std::vector<Control*> face(knobs);
		face.insert(face.end(), toggles.begin(), toggles.end());
		const int faceLeft = 120, faceRight = width - 36 - 70;   // after the jewel; clear of the MORE switch
		PlaceRow(face, { faceLeft + jmax(0, (faceRight - faceLeft - faceItems * column) / 2), y + 8, faceItems * column, knobSize + 22 }, knobSize);
		y += knobSize + 34;
		for (auto* c : meters) { c->Place({ 120, y, 200, 28 }, 0); y += 30; }
		if (!adv.empty())
		{
			std::vector<Control*> advRow, advPickers;
			for (auto* c : adv) (c->IsPicker() ? advPickers : advRow).push_back(c);
			PlaceRow(advRow, { 120, y + 4, static_cast<int>(advRow.size()) * 70, 40 + 20 }, 40);
			for (auto* c : advPickers) c->Place({ 120 + static_cast<int>(advRow.size()) * 70 + 10, y + 20, 220, 26 }, 0);
			y += 82;
		}
		natural = { 0, 0, width, y + 30 };
	}

	void PedalView::LayoutRack()
	{
		std::vector<Control*> faders, others;
		for (auto* c : Visible(false)) (c->IsFader() ? faders : others).push_back(c);
		for (auto* c : Visible(true)) (c->IsFader() ? faders : others).push_back(c);

		constexpr int left = 92, faderColumn = 46, faderHeight = 156, otherColumn = 74, top = 22;
		// The ears: the rack shell's own (at the height the unit is drawn), else 22 px.
		int ear = 22;
		shellArea = drawerArea = {};
		const auto* shell = shellName.isNotEmpty() ? Skin::GetShell(shellName.toRawUTF8()) : nullptr;
		if (shell) ear = roundToInt(shell->inner.getX() * shell->image.getWidth() * (top + 16 + faderHeight + 28) / static_cast<float>(shell->image.getHeight())) + 4;
		const int width = ear * 2 + left + static_cast<int>(faders.size()) * faderColumn
			+ static_cast<int>(others.size()) * otherColumn + 24;
		const int height = top + 16 + faderHeight + 28;

		footSwitch->setBounds(ear + 34, top + 66, 26, 40);
		ledArea = Rectangle<float>(12.0f, 12.0f).withCentre({ ear + 47.0f, top + 40.0f });
		nameArea = Rectangle<int>(ear + 8, top + 118, left - 12, 40).toFloat();

		PlaceRow(faders, { ear + left, top, static_cast<int>(faders.size()) * faderColumn, faderHeight + 16 }, 0);
		int x = ear + left + static_cast<int>(faders.size()) * faderColumn + 12;
		for (auto* c : others)
		{
			c->Place(c->IsPicker() ? Rectangle<int>(x, top + 70, otherColumn - 6, 26) : Rectangle<int>(x, top + 50, otherColumn, 44 + 18), 44);
			x += otherColumn;
		}
		panelArea = {};
		natural = { 0, 0, width, height };
		if (shell) shellArea = natural.toFloat();
	}

	void PedalView::LayoutCabinet()
	{
		std::vector<Control*> pickers, rest;
		for (auto* c : Visible(false)) (c->IsPicker() ? pickers : rest).push_back(c);
		for (auto* c : Visible(true)) (c->IsPicker() ? pickers : rest).push_back(c);

		constexpr int strip = 84, knobSize = 38;
		const int width = jmax(380, 250 + static_cast<int>(rest.size()) * 58 + 70);
		panelArea = Rectangle<int>(16, 16, width - 32, strip - 8).toFloat();

		int py = 26;
		for (auto* c : pickers) { c->Place({ 28, py, 220, pickers.size() > 1 ? 24 : 30 }, 0); py += 30; }
		PlaceRow(rest, { 260, 20, static_cast<int>(rest.size()) * 58, knobSize + 22 }, knobSize);
		footSwitch->setBounds(width - 62, 26, 26, 40);
		ledArea = {};

		shellArea = drawerArea = {};
		if (const auto* shell = shellName.isNotEmpty() ? Skin::GetShell(shellName.toRawUTF8()) : nullptr)
		{
			// The cab's photo under the control strip, at its own proportions; the name plate low on its grille.
			const float cabHeight = width / shell->Aspect();
			shellArea = Rectangle<float>(0.0f, static_cast<float>(strip), static_cast<float>(width), cabHeight);
			grilleArea = Rectangle<float>(shell->grille.getX() * width, strip + shell->grille.getY() * cabHeight,
				shell->grille.getWidth() * width, shell->grille.getHeight() * cabHeight);
			nameArea = Rectangle<float>(170.0f, 34.0f).withCentre({ grilleArea.getCentreX(), grilleArea.getBottom() - grilleArea.getHeight() * 0.1f });
			natural = { 0, 0, width, strip + roundToInt(cabHeight) };
			return;
		}
		grilleArea = Rectangle<int>(16, strip + 14, width - 32, 260).toFloat();
		nameArea = Rectangle<float>(150.0f, 30.0f).withCentre({ grilleArea.getCentreX(), grilleArea.getBottom() - 34.0f });
		natural = { 0, 0, width, strip + 14 + 260 + 18 };
	}

	void PedalView::resized() {}

	//==============================================================================================================

	void PedalView::PaintLed(Graphics& g, Rectangle<float> area, Colour colour)
	{
		if (area.isEmpty()) return;
		if (enabled)
		{
			g.setGradientFill(ColourGradient(colour.withAlpha(0.55f), area.getCentreX(), area.getCentreY(),
				colour.withAlpha(0.0f), area.getCentreX() + area.getWidth() * 1.6f, area.getCentreY(), true));
			g.fillEllipse(area.expanded(area.getWidth() * 1.6f));
		}
		g.setColour(Colours::black.withAlpha(0.7f));
		g.fillEllipse(area.expanded(1.5f));
		g.setGradientFill(ColourGradient(enabled ? colour.brighter(0.6f) : colour.darker(0.8f), area.getX(), area.getY(),
			enabled ? colour : colour.darker(1.6f), area.getRight(), area.getBottom(), true));
		g.fillEllipse(area);
		g.setColour(Colours::white.withAlpha(enabled ? 0.8f : 0.25f));
		g.fillEllipse(area.reduced(area.getWidth() * 0.3f).translated(-area.getWidth() * 0.15f, -area.getWidth() * 0.15f));
	}

	void PedalView::paint(Graphics& g)
	{
		switch (body)
		{
		case Body::Stomp: PaintStomp(g); break;
		case Body::Amp: PaintAmp(g); break;
		case Body::Rack: PaintRack(g); break;
		case Body::Cabinet: PaintCabinet(g); break;
		}
		if (!enabled)
		{
			// Bypassed: the whole unit dims a little, so it reads as off at a glance.
			g.setColour(Colours::black.withAlpha(0.28f));
			g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(4.0f), 14.0f);
		}
	}

	void PedalView::PaintStomp(Graphics& g)
	{
		const auto bodyArea = getLocalBounds().toFloat().reduced(4.0f);
		const auto bg = plugin.background, fg = plugin.foreground;
		const float corner = 16.0f;

		// The enclosure: the skin's die-cast shell in the pedal's colour (its corners and bevel kept), else its painted
		// metal texture, else a gradient with metal flake.
		if (shellName.isNotEmpty())
		{
			const auto* shell = Skin::GetShell(shellName.toRawUTF8());
			Skin::DrawSliced(g, Skin::TintedPiece(shellName.toRawUTF8(), bg), bodyArea.expanded(3.0f), shell->inner);
		}
		const bool skinned = shellName.isNotEmpty() || SkinSurface(g, Skin::Tinted(bg), bodyArea, corner);
		if (!skinned)
		{
			g.setGradientFill(ColourGradient(bg.brighter(0.22f), bodyArea.getX(), bodyArea.getY(),
				bg.darker(0.32f), bodyArea.getX(), bodyArea.getBottom(), false));
			g.fillRoundedRectangle(bodyArea, corner);
		}
		{
			Graphics::ScopedSaveState save(g);
			Path clip;
			clip.addRoundedRectangle(bodyArea, corner);
			g.reduceClipRegion(clip);
			if (skinned)
			{
				// Light from above over the photo texture: lighter at the top, darker at the bottom.
				g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.10f), bodyArea.getX(), bodyArea.getY(),
					Colours::black.withAlpha(0.22f), bodyArea.getX(), bodyArea.getBottom(), false));
				g.fillRect(bodyArea);
			}
			else Speckle(g, bodyArea, DisplayName().hashCode(), static_cast<int>(bodyArea.getWidth() * bodyArea.getHeight() / 40.0f), 0.10f);
			// A soft sheen across the top, like a lacquered enclosure under a light.
			g.setGradientFill(ColourGradient(Colours::white.withAlpha(0.16f), bodyArea.getX(), bodyArea.getY(),
				Colours::white.withAlpha(0.0f), bodyArea.getX() + bodyArea.getWidth() * 0.6f, bodyArea.getY() + 120.0f, false));
			g.fillRect(bodyArea);
		}
		if (shellName.isEmpty())   // the shell has its own edges
		{
			g.setColour(Colours::white.withAlpha(0.32f));
			g.drawRoundedRectangle(bodyArea.reduced(1.0f), corner - 1.0f, 1.2f);
			g.setColour(Colours::black.withAlpha(0.65f));
			g.drawRoundedRectangle(bodyArea, corner, 1.5f);
		}

		if (!panelArea.isEmpty())
		{
			g.setColour(Colours::black.withAlpha(0.28f));
			g.fillRoundedRectangle(panelArea, 8.0f);
			g.setColour(Colours::white.withAlpha(0.12f));
			g.drawRoundedRectangle(panelArea.translated(0.0f, 1.0f), 8.0f, 1.0f);
		}

		// The pedal's name, big, in its own colour, with its description under it.
		auto name = nameArea;
		g.setColour(Colours::black.withAlpha(0.25f));
		const auto displayName = DisplayName();
		g.setFont(Font(FontOptions(jmin(34.0f, nameArea.getWidth() / jmax(4, displayName.length()) * 1.9f), Font::bold | Font::italic)));
		g.drawFittedText(displayName.toUpperCase(), name.removeFromTop(40.0f).translated(0.0f, 2.0f).toNearestInt(), Justification::centred, 1);
		name = nameArea;
		g.setColour(fg);
		g.drawFittedText(displayName.toUpperCase(), name.removeFromTop(40.0f).toNearestInt(), Justification::centred, 1);
		g.setColour(fg.withAlpha(0.7f));
		g.setFont(LabelFont(11.0f, false));
		g.drawFittedText(DisplayDescription(), name.toNearestInt(), Justification::centredTop, 2);

		// The footswitch well: a recessed band at the bottom.
		const float well = static_cast<float>(footSwitch->getY()) - 12.0f;
		g.setColour(Colours::black.withAlpha(0.35f));
		g.drawHorizontalLine(static_cast<int>(well), bodyArea.getX() + 14.0f, bodyArea.getRight() - 14.0f);
		g.setColour(Colours::white.withAlpha(0.18f));
		g.drawHorizontalLine(static_cast<int>(well) + 1, bodyArea.getX() + 14.0f, bodyArea.getRight() - 14.0f);

		PaintLed(g, ledArea, Colour(0xffff2a1a));
	}

	void PedalView::PaintAmp(Graphics& g)
	{
		if (!shellArea.isEmpty())
		{
			const auto* shell = Skin::GetShell(shellName.toRawUTF8());
			g.setOpacity(1.0f);
			g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
			g.drawImage(shell->image, shellArea, RectanglePlacement::stretchToFit);

			// The name plate on the grille.
			const auto plate = nameArea;
			g.setColour(Colours::black.withAlpha(0.55f));
			g.fillRoundedRectangle(plate.translated(0.0f, 3.0f), 5.0f);
			if (!SkinSurface(g, Skin::Get("plate"), plate, 5.0f)) PedalLookAndFeel::DrawBrushed(g, plate, Colour(0xffb9bdc4), 11);
			g.setColour(Colours::black.withAlpha(0.6f));
			g.drawRoundedRectangle(plate, 5.0f, 1.2f);
			for (auto corner : { plate.getTopLeft().translated(9.0f, 9.0f), plate.getTopRight().translated(-9.0f, 9.0f),
				plate.getBottomLeft().translated(9.0f, -9.0f), plate.getBottomRight().translated(-9.0f, -9.0f) })
				PedalLookAndFeel::DrawScrew(g, corner, 3.0f, 0.7f);
			// The name as large as the plate takes it: long capture names ("Twin Reverb Normal Bright") go smaller
			// rather than being cut.
			const auto name = DisplayName();
			const float fit = (plate.getWidth() - 36.0f) / jmax(1.0f, GlyphArrangement::getStringWidth(Font(FontOptions(30.0f, Font::bold | Font::italic)), name)) * 30.0f;
			g.setFont(Font(FontOptions(jlimit(14.0f, jmin(34.0f, plate.getHeight() * 0.55f), fit), Font::bold | Font::italic)));
			g.setColour(Colours::white.withAlpha(0.35f));
			g.drawFittedText(name, plate.reduced(18.0f, 4.0f).translated(0.0f, 1.0f).toNearestInt(), Justification::centred, 1, 0.8f);
			g.setColour(Colour(0xff121214));
			g.drawFittedText(name, plate.reduced(18.0f, 4.0f).toNearestInt(), Justification::centred, 1, 0.8f);

			// Labels sit on the panel: dark on the light panels.
			if (!drawerArea.isEmpty())
			{
				g.setColour(Colours::black);
				g.fillRoundedRectangle(drawerArea.expanded(2.0f, 0.0f).withTrimmedTop(-4.0f), 6.0f);
				PedalLookAndFeel::DrawBrushed(g, drawerArea.reduced(3.0f), Colour(0xff2a2c30), 17);
			}
			PaintLed(g, ledArea, Colour(0xffff3b1f));
			return;
		}
		constexpr float top = 16.0f;
		const auto box = getLocalBounds().toFloat().withTrimmedTop(top).reduced(4.0f, 0.0f).withTrimmedBottom(4.0f);
		const float width = static_cast<float>(getWidth());

		// Tolex: black, grained, a faint light from above.
		if (SkinSurface(g, Skin::Get("tolex"), box, 10.0f))
		{
			// The photo's grain is loud at editor size: darken it, more towards the bottom (light from above).
			g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.35f), box.getX(), box.getY(),
				Colours::black.withAlpha(0.6f), box.getX(), box.getBottom(), false));
			g.fillRoundedRectangle(box, 10.0f);
		}
		else
		{
			g.setGradientFill(ColourGradient(Colour(0xff2a2a2d), box.getX(), box.getY(), Colour(0xff0b0b0c), box.getX(), box.getBottom(), false));
			g.fillRoundedRectangle(box, 10.0f);
			Speckle(g, box, 77, static_cast<int>(box.getWidth() * box.getHeight() / 12.0f), 0.12f);
		}
		g.setColour(Colours::black);
		g.drawRoundedRectangle(box, 10.0f, 2.0f);

		// The strap handle on top, with its two metal end caps.
		const auto handle = Rectangle<float>(170.0f, 14.0f).withCentre({ width * 0.5f, top - 2.0f });
		g.setColour(Colours::black.withAlpha(0.6f));
		g.fillRoundedRectangle(handle.translated(0.0f, 2.0f), 7.0f);
		g.setGradientFill(ColourGradient(Colour(0xff3a3a3e), 0.0f, handle.getY(), Colour(0xff111113), 0.0f, handle.getBottom(), false));
		g.fillRoundedRectangle(handle, 7.0f);
		for (auto x : { handle.getX() - 8.0f, handle.getRight() - 14.0f })
		{
			const auto cap = Rectangle<float>(x, handle.getY() + 1.0f, 22.0f, top - handle.getY() + 3.0f);
			g.setGradientFill(ColourGradient(Colour(0xffd5d8dd), cap.getX(), cap.getY(), Colour(0xff50545b), cap.getRight(), cap.getBottom(), false));
			g.fillRoundedRectangle(cap, 3.0f);
		}

		// Metal corners.
		for (auto corner : { box.getTopLeft(), box.getTopRight(), box.getBottomLeft(), box.getBottomRight() })
		{
			const auto c = Rectangle<float>(26.0f, 26.0f).withCentre(corner).constrainedWithin(box);
			g.setGradientFill(ColourGradient(Colour(0xffd5d8dd), c.getX(), c.getY(), Colour(0xff50545b), c.getRight(), c.getBottom(), false));
			g.fillRoundedRectangle(c, 5.0f);
			PedalLookAndFeel::DrawScrew(g, c.getCentre(), 3.0f, 0.6f);
		}

		// The logo plate.
		const auto plate = nameArea;
		g.setColour(Colours::black.withAlpha(0.5f));
		g.fillRoundedRectangle(plate.translated(0.0f, 2.0f), 5.0f);
		if (!SkinSurface(g, Skin::Get("plate"), plate, 5.0f))
			PedalLookAndFeel::DrawBrushed(g, plate, plugin.background.interpolatedWith(Colour(0xff9a9ea6), 0.6f), 11);
		g.setColour(Colours::black.withAlpha(0.55f));
		g.drawRoundedRectangle(plate, 5.0f, 1.2f);
		g.setFont(Font(FontOptions(30.0f, Font::bold | Font::italic)));
		g.setColour(Colours::white.withAlpha(0.35f));
		g.drawFittedText(DisplayName(), plate.reduced(10.0f, 4.0f).translated(0.0f, 1.0f).toNearestInt(), Justification::centred, 1);
		g.setColour(Colour(0xff121214));
		g.drawFittedText(DisplayName(), plate.reduced(10.0f, 4.0f).toNearestInt(), Justification::centred, 1);

		// The faceplate, piped in cream.
		g.setColour(Colour(0xffe8dcc0));
		g.drawRoundedRectangle(panelArea.expanded(4.0f), 6.0f, 1.5f);
		if (!SkinSurface(g, Skin::Get("faceplate"), panelArea, 0.0f))
			PedalLookAndFeel::DrawBrushed(g, panelArea, Colour(0xffc6a964), 23);
		g.setColour(Colours::black.withAlpha(0.6f));
		g.drawRect(panelArea, 1.0f);

		PaintLed(g, ledArea, Colour(0xffff3b1f));
	}

	void PedalView::PaintRack(Graphics& g)
	{
		constexpr float ear = 22.0f;
		const auto bounds = getLocalBounds().toFloat();
		const auto face = bounds.reduced(ear - 6.0f, 4.0f);

		// Rack ears with their slots and screws.
		for (auto x : { bounds.getX() + 2.0f, bounds.getRight() - ear - 2.0f })
		{
			const auto earArea = Rectangle<float>(x, bounds.getY() + 10.0f, ear, bounds.getHeight() - 20.0f);
			g.setGradientFill(ColourGradient(Colour(0xff4a4c51), earArea.getX(), 0.0f, Colour(0xff1f2023), earArea.getRight(), 0.0f, false));
			g.fillRoundedRectangle(earArea, 3.0f);
			for (float sy : { earArea.getY() + 22.0f, earArea.getBottom() - 22.0f })
			{
				g.setColour(Colours::black);
				g.fillRoundedRectangle(Rectangle<float>(10.0f, 16.0f).withCentre({ earArea.getCentreX(), sy }), 5.0f);
				PedalLookAndFeel::DrawScrew(g, { earArea.getCentreX(), sy }, 4.0f, 0.4f);
			}
		}

		if (!shellArea.isEmpty())
		{
			// The rack unit's photo: ears and edges at their own proportions, the faceplate stretched between them.
			const auto* shell = Skin::GetShell(shellName.toRawUTF8());
			Skin::DrawSliced(g, shell->image, shellArea, shell->inner);
			g.setColour(Colour(0xffe9ebef));
			g.setFont(LabelFont(10.0f));
			g.drawText("ON", Rectangle<float>(static_cast<float>(footSwitch->getX()) - 12.0f, static_cast<float>(footSwitch->getBottom() + 2), 50.0f, 12.0f), Justification::centred);
			g.setFont(Font(FontOptions(15.0f, Font::bold | Font::italic)));
			g.drawFittedText(plugin.name, nameArea.toNearestInt(), Justification::centred, 2);
			PaintLed(g, ledArea, Colour(0xffff2a1a));
			return;
		}

		// The face: dark worn steel.
		g.setColour(Colours::black);
		g.fillRoundedRectangle(face.expanded(1.5f), 8.0f);
		{
			Graphics::ScopedSaveState save(g);
			Path clip;
			clip.addRoundedRectangle(face, 8.0f);
			g.reduceClipRegion(clip);
			if (!SkinSurface(g, Skin::Get("rack-face"), face, 8.0f))
			{
				PedalLookAndFeel::DrawBrushed(g, face, Colour(0xff3a3c40), 5);
				Speckle(g, face, 91, static_cast<int>(face.getWidth() * face.getHeight() / 18.0f), 0.18f);
			}
		}
		g.setColour(Colours::white.withAlpha(0.14f));
		g.drawRoundedRectangle(face.reduced(1.0f), 7.0f, 1.0f);

		g.setColour(Colour(0xffe9ebef));
		g.setFont(LabelFont(10.0f));
		g.drawText("ON", Rectangle<float>(ear + 22.0f, 74.0f, 50.0f, 12.0f).withY(static_cast<float>(footSwitch->getBottom() + 2)), Justification::centred);
		g.setFont(Font(FontOptions(15.0f, Font::bold | Font::italic)));
		g.drawFittedText(plugin.name, nameArea.toNearestInt(), Justification::centred, 2);

		PaintLed(g, ledArea, Colour(0xffff2a1a));
	}

	void PedalView::PaintCabinet(Graphics& g)
	{
		if (!shellArea.isEmpty())
		{
			// The control strip as a dark box sitting on the cab, then the cab's photo and its name plate.
			const auto top = getLocalBounds().toFloat().withBottom(shellArea.getY() + 12.0f).reduced(10.0f, 4.0f);
			g.setColour(Colours::black);
			g.fillRoundedRectangle(top, 8.0f);
			PedalLookAndFeel::DrawBrushed(g, panelArea, Colour(0xff2b2d31), 9);
			g.setColour(Colours::white.withAlpha(0.12f));
			g.drawRoundedRectangle(panelArea, 5.0f, 1.0f);
			const auto* shell = Skin::GetShell(shellName.toRawUTF8());
			g.setOpacity(1.0f);
			g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
			g.drawImage(shell->image, shellArea, RectanglePlacement::stretchToFit);
			g.setColour(Colours::black.withAlpha(0.5f));
			g.fillRoundedRectangle(nameArea.translated(0.0f, 2.0f), 4.0f);
			if (!SkinSurface(g, Skin::Get("plate"), nameArea, 4.0f)) PedalLookAndFeel::DrawBrushed(g, nameArea, Colour(0xffb9bdc4), 13);
			g.setColour(Colour(0xff151517));
			g.setFont(Font(FontOptions(17.0f, Font::bold | Font::italic)));
			g.drawFittedText(DisplayName(), nameArea.reduced(6.0f, 2.0f).toNearestInt(), Justification::centred, 1);
			return;
		}
		const auto box = getLocalBounds().toFloat().reduced(4.0f);

		if (SkinSurface(g, Skin::Get("cab-tolex"), box, 10.0f))
		{
			g.setGradientFill(ColourGradient(Colours::black.withAlpha(0.35f), box.getX(), box.getY(),
				Colours::black.withAlpha(0.6f), box.getX(), box.getBottom(), false));
			g.fillRoundedRectangle(box, 10.0f);
		}
		else
		{
			g.setGradientFill(ColourGradient(Colour(0xff26262a), box.getX(), box.getY(), Colour(0xff09090a), box.getX(), box.getBottom(), false));
			g.fillRoundedRectangle(box, 10.0f);
			Speckle(g, box, 55, static_cast<int>(box.getWidth() * box.getHeight() / 12.0f), 0.12f);
		}
		g.setColour(Colours::black);
		g.drawRoundedRectangle(box, 10.0f, 2.0f);

		// The control strip.
		PedalLookAndFeel::DrawBrushed(g, panelArea, Colour(0xff2b2d31), 9);
		g.setColour(Colours::white.withAlpha(0.12f));
		g.drawRoundedRectangle(panelArea, 5.0f, 1.0f);

		// The grille: a woven cloth (two sets of fine diagonals) with a soft shadow at its edges.
		g.setColour(Colour(0xffe8dcc0));
		g.drawRoundedRectangle(grilleArea.expanded(3.0f), 6.0f, 1.5f);
		{
			Graphics::ScopedSaveState save(g);
			g.reduceClipRegion(grilleArea.toNearestInt());
			g.setColour(Colour(0xff24201c));
			g.fillRect(grilleArea);

			// The speakers behind the cloth, as many as the chosen IR's name says ("4x12 V30 SM57": four).
			int speakers = 1;
			for (auto& control : controls)
				if (control->desc.type == ParamType::File && control->GetCombo())
				{
					const auto ir = control->GetCombo()->getText();
					const int x = ir.indexOfChar('x');
					if (x > 0 && CharacterFunctions::isDigit(ir[x - 1]) && CharacterFunctions::isDigit(ir[x + 1]))
						speakers = jlimit(1, 8, ir.substring(jmax(0, x - 1), x).getIntValue());
				}
			const int cols = speakers >= 4 ? (speakers >= 8 ? 4 : 2) : speakers, rows = (speakers + cols - 1) / cols;
			const float cell = jmin(grilleArea.getWidth() / cols, (grilleArea.getHeight() - 30.0f) / rows);
			const auto grid = Rectangle<float>(cell * cols, cell * rows).withCentre(grilleArea.getCentre().translated(0.0f, -12.0f));
			for (int i = 0; i < speakers; ++i)
			{
				const auto cone = Rectangle<float>(grid.getX() + (i % cols) * cell, grid.getY() + (i / cols) * cell, cell, cell).reduced(cell * 0.07f);
				g.setGradientFill(ColourGradient(Colour(0xff0c0b0a), cone.getCentreX(), cone.getCentreY(),
					Colour(0xff3a342c), cone.getRight(), cone.getCentreY(), true));
				g.fillEllipse(cone);
				g.setColour(Colours::white.withAlpha(0.06f));
				g.drawEllipse(cone.reduced(cone.getWidth() * 0.06f), 2.0f);
				g.setColour(Colour(0xff4b443a).withAlpha(0.6f));
				g.fillEllipse(cone.withSizeKeepingCentre(cone.getWidth() * 0.26f, cone.getHeight() * 0.26f));
			}

			// The cloth over them: the skin's grille cloth, the speakers showing faintly through it; else a weave of
			// two sets of fine diagonals.
			if (!SkinSurface(g, Skin::Get("grille"), grilleArea, 0.0f, 0.82f))
			{
				const float span = grilleArea.getWidth() + grilleArea.getHeight();
				for (float d = -grilleArea.getHeight(); d < span; d += 3.0f)
				{
					g.setColour(Colour(0xffb7a57f).withAlpha(0.16f));
					g.drawLine(grilleArea.getX() + d, grilleArea.getY(), grilleArea.getX() + d + grilleArea.getHeight(), grilleArea.getBottom(), 1.0f);
					g.setColour(Colours::black.withAlpha(0.25f));
					g.drawLine(grilleArea.getX() + d, grilleArea.getBottom(), grilleArea.getX() + d + grilleArea.getHeight(), grilleArea.getY(), 1.0f);
				}
			}
			g.setGradientFill(ColourGradient(Colours::transparentBlack, grilleArea.getCentreX(), grilleArea.getCentreY(),
				Colours::black.withAlpha(0.65f), grilleArea.getX(), grilleArea.getY(), true));
			g.fillRect(grilleArea);
		}

		// The logo plate on the cloth.
		g.setColour(Colours::black.withAlpha(0.5f));
		g.fillRoundedRectangle(nameArea.translated(0.0f, 2.0f), 4.0f);
		if (!SkinSurface(g, Skin::Get("plate"), nameArea, 4.0f))
			PedalLookAndFeel::DrawBrushed(g, nameArea, Colour(0xffb9bdc4), 13);
		g.setColour(Colour(0xff151517));
		g.setFont(Font(FontOptions(17.0f, Font::bold | Font::italic)));
		g.drawFittedText(DisplayName(), nameArea.reduced(6.0f, 2.0f).toNearestInt(), Justification::centred, 1);
	}
}
