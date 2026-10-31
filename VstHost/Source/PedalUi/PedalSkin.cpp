#include "PedalSkin.hpp"

#include <map>

namespace PedalUi::Skin
{
	using namespace juce;

	namespace
	{
		std::map<String, Image>& Pieces()
		{
			static std::map<String, Image> pieces;
			return pieces;
		}

		std::map<uint32, Image>& TintCache()
		{
			static std::map<uint32, Image> cache;
			return cache;
		}

		std::map<String, Shell>& Shells()
		{
			static std::map<String, Shell> shells;
			return shells;
		}

		std::map<String, Image>& PieceTints()
		{
			static std::map<String, Image> tints;
			return tints;
		}

		// A piece's file: WebP (what ships, small even with transparency), else PNG or JPEG (art being worked on).
		File PieceFile(const File& folder, const char* name)
		{
			for (const char* extension : { ".webp", ".png", ".jpg" })
				if (auto file = folder.getChildFile(String(name) + extension); file.existsAsFile()) return file;
			return {};
		}

		Rectangle<float> ReadRect(const var& value)
		{
			if (auto* list = value.getArray(); list && list->size() == 4)
				return { static_cast<float>((*list)[0]), static_cast<float>((*list)[1]), static_cast<float>((*list)[2]), static_cast<float>((*list)[3]) };
			return {};
		}

		// The generated art is 1000+ pixels; drawn every repaint in software that costs, so each piece is scaled
		// down once to what the editors draw at most (knobs ~150 px, surfaces up to a large window's width).
		int MaxSize(const String& name) { return name.startsWith("knob") || name == "footswitch" ? 320 : (name.startsWith("amp-") || name == "rack-unit" ? 1800 : 1400); }

		Image ScaledDown(const Image& image, int maxSize)
		{
			const int longest = jmax(image.getWidth(), image.getHeight());
			if (longest <= maxSize) return image;
			const float factor = static_cast<float>(maxSize) / longest;
			return image.rescaled(roundToInt(image.getWidth() * factor), roundToInt(image.getHeight() * factor), Graphics::highResamplingQuality);
		}
	}

	void Load(const File& folder)
	{
		Pieces().clear();
		TintCache().clear();
		PieceTints().clear();
		Shells().clear();
		for (const char* name : { "knob-amp", "knob-stomp", "knob-chrome", "footswitch", "enclosure", "tolex", "cab-tolex",
			"faceplate", "grille", "rack-face", "plate" })
		{
			const auto file = PieceFile(folder, name);
			if (file == File()) continue;
			auto image = ImageFileFormat::loadFrom(file);
			if (image.isValid()) Pieces()[name] = ScaledDown(image.convertedToFormat(Image::ARGB), MaxSize(name));
		}

		// Shells: the image, and its areas from skin.json.
		const auto areas = JSON::parse(folder.getChildFile("skin.json").loadFileAsString());
		for (const char* name : { "amp-modern", "amp-british", "amp-vintage", "cab-front", "rack-unit", "pedal-shell" })
		{
			const auto file = PieceFile(folder, name);
			const auto& entry = areas[name];
			if (file == File() || !entry.isObject()) continue;
			auto image = ImageFileFormat::loadFrom(file);
			if (!image.isValid()) continue;
			Shell shell;
			shell.image = ScaledDown(image.convertedToFormat(Image::ARGB), MaxSize(name));
			shell.panel = ReadRect(entry["panel"]);
			shell.grille = ReadRect(entry["grille"]);
			shell.inner = ReadRect(entry["inner"]);
			Shells()[name] = shell;
			Pieces()[name] = shell.image;
		}
	}

	const Shell* GetShell(const char* name)
	{
		const auto found = Shells().find(name);
		return found != Shells().end() ? &found->second : nullptr;
	}

	void DrawSliced(Graphics& g, const Image& image, Rectangle<float> dest, Rectangle<float> inner)
	{
		if (!image.isValid() || dest.isEmpty()) return;
		const float iw = static_cast<float>(image.getWidth()), ih = static_cast<float>(image.getHeight());
		// Source edges, in pixels, and destination edges: the margins at the scale the height sets, never more than
		// half the destination.
		const float sx[4] = { 0.0f, inner.getX() * iw, inner.getRight() * iw, iw };
		const float sy[4] = { 0.0f, inner.getY() * ih, inner.getBottom() * ih, ih };
		const float scale = dest.getHeight() / ih;
		const float left = jmin(sx[1] * scale, dest.getWidth() * 0.5f), right = jmin((iw - sx[2]) * scale, dest.getWidth() * 0.5f);
		const float top = jmin(sy[1] * scale, dest.getHeight() * 0.5f), bottom = jmin((ih - sy[2]) * scale, dest.getHeight() * 0.5f);
		const float dx[4] = { dest.getX(), dest.getX() + left, dest.getRight() - right, dest.getRight() };
		const float dy[4] = { dest.getY(), dest.getY() + top, dest.getBottom() - bottom, dest.getBottom() };
		Graphics::ScopedSaveState save(g);
		g.setOpacity(1.0f);
		g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
		for (int row = 0; row < 3; ++row)
			for (int col = 0; col < 3; ++col)
			{
				const float dw = dx[col + 1] - dx[col], dh = dy[row + 1] - dy[row];
				const float sw = sx[col + 1] - sx[col], sh = sy[row + 1] - sy[row];
				if (dw <= 0.0f || dh <= 0.0f || sw <= 0.0f || sh <= 0.0f) continue;
				// A hair of overlap so the slices' edges never show a seam.
				g.drawImageTransformed(image.getClippedImage(Rectangle<float>(sx[col], sy[row], sw, sh).getSmallestIntegerContainer()),
					AffineTransform::scale((dw + 0.6f) / sw, (dh + 0.6f) / sh).translated(dx[col] - 0.3f, dy[row] - 0.3f));
			}
	}

	Image TintedPiece(const char* name, Colour colour)
	{
		const Image& piece = Get(name);
		if (!piece.isValid()) return {};
		const String key = String(name) + String::toHexString(static_cast<int>(colour.getARGB()));
		if (const auto found = PieceTints().find(key); found != PieceTints().end()) return found->second;

		double sum = 0.0;
		int count = 0;
		{
			const Image::BitmapData pixels(piece, Image::BitmapData::readOnly);
			for (int y = 0; y < piece.getHeight(); y += 4)
				for (int x = 0; x < piece.getWidth(); x += 4)
				{
					const auto c = pixels.getPixelColour(x, y);
					if (c.getAlpha() > 200) { sum += c.getPerceivedBrightness(); ++count; }
				}
		}
		const double mean = jmax(0.05, count > 0 ? sum / count : 0.5);
		Image tinted(Image::ARGB, piece.getWidth(), piece.getHeight(), true);
		{
			const Image::BitmapData in(piece, Image::BitmapData::readOnly);
			Image::BitmapData out(tinted, Image::BitmapData::writeOnly);
			for (int y = 0; y < piece.getHeight(); ++y)
				for (int x = 0; x < piece.getWidth(); ++x)
				{
					const auto c = in.getPixelColour(x, y);
					if (c.getAlpha() == 0) continue;
					const float k = static_cast<float>(c.getPerceivedBrightness() / mean);
					out.setPixelColour(x, y, Colour::fromFloatRGBA(jlimit(0.0f, 1.0f, colour.getFloatRed() * k),
						jlimit(0.0f, 1.0f, colour.getFloatGreen() * k), jlimit(0.0f, 1.0f, colour.getFloatBlue() * k), c.getFloatAlpha()));
				}
		}
		PieceTints()[key] = tinted;
		return tinted;
	}

	bool IsLoaded() { return !Pieces().empty(); }

	const Image& Get(const char* name)
	{
		static const Image missing;
		const auto found = Pieces().find(name);
		return found != Pieces().end() ? found->second : missing;
	}

	void Cover(Graphics& g, const Image& image, Rectangle<float> area, float opacity)
	{
		if (!image.isValid() || area.isEmpty()) return;
		const float scale = jmax(area.getWidth() / image.getWidth(), area.getHeight() / image.getHeight());
		const float w = image.getWidth() * scale, h = image.getHeight() * scale;
		Graphics::ScopedSaveState save(g);
		g.reduceClipRegion(area.getSmallestIntegerContainer());
		g.setOpacity(opacity);
		g.setImageResamplingQuality(Graphics::mediumResamplingQuality);
		g.drawImageTransformed(image, AffineTransform::scale(scale).translated(area.getCentreX() - w * 0.5f, area.getCentreY() - h * 0.5f));
	}

	// Multiplies the grey paint by the colour, normalised so the paint's mid grey gives the colour itself: the texture's
	// light and dark flecks stay, around the pedal's own colour.
	Image Tinted(Colour colour)
	{
		const Image& paint = Get("enclosure");
		if (!paint.isValid()) return {};
		auto& cache = TintCache();
		if (const auto found = cache.find(colour.getARGB()); found != cache.end()) return found->second;

		Image source = paint.getWidth() > 700 ? paint.rescaled(700, roundToInt(700.0f * paint.getHeight() / paint.getWidth()), Graphics::mediumResamplingQuality) : paint;
		double sum = 0.0;
		{
			const Image::BitmapData pixels(source, Image::BitmapData::readOnly);
			for (int y = 0; y < source.getHeight(); y += 4)
				for (int x = 0; x < source.getWidth(); x += 4) sum += pixels.getPixelColour(x, y).getPerceivedBrightness();
		}
		const double mean = jmax(0.05, sum / ((source.getWidth() / 4 + 1) * (source.getHeight() / 4 + 1)));

		Image tinted(Image::ARGB, source.getWidth(), source.getHeight(), false);
		{
			const Image::BitmapData in(source, Image::BitmapData::readOnly);
			Image::BitmapData out(tinted, Image::BitmapData::writeOnly);
			for (int y = 0; y < source.getHeight(); ++y)
				for (int x = 0; x < source.getWidth(); ++x)
				{
					const float k = static_cast<float>(in.getPixelColour(x, y).getPerceivedBrightness() / mean);
					out.setPixelColour(x, y, Colour::fromFloatRGBA(jlimit(0.0f, 1.0f, colour.getFloatRed() * k),
						jlimit(0.0f, 1.0f, colour.getFloatGreen() * k), jlimit(0.0f, 1.0f, colour.getFloatBlue() * k), 1.0f));
				}
		}
		cache[colour.getARGB()] = tinted;
		return tinted;
	}
}
