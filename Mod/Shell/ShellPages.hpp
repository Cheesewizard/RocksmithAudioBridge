#pragma once

#include <functional>
#include <string>
#include <vector>

// The Audio Bridge overlay's pages. Each mod registers its own page (Drop Pedal, Input, Output, ...) and the shell lays
// them out in its sidebar, grouped and ordered, so the shell knows no feature and a feature brings its own page. A
// page's draw function runs on the render thread inside the shell's page area, inside an ImGui frame.
//
// Shaped like upstream's MenuRegistry on purpose: the same idea (a host window other mods add pages to) would serve any
// mod with more than a few settings, so it can be offered to the framework.
namespace AudioBridge::Shell
{
	struct Page
	{
		std::string id;          // unique; registering the same id again replaces the page
		std::string group;       // sidebar heading the page sits under ("Practice", "Audio", "System")
		unsigned glyph = 0;      // Overlay::Glyph code point
		std::string label;
		int order = 0;           // lower first, across all groups
		std::function<void()> draw;
		// Whether the page shows right now (its feature is on). Render thread; keep it cheap. Empty = always.
		std::function<bool()> available;
		// A window the page keeps over the game while the overlay is open, whatever page shows (the Input page's Noise
		// gate pedal). Drawn after the shell's window, so it floats over it; it opens its own ImGui window. Optional.
		std::function<void()> floating;
	};

	// MainThread, from a mod's OnInitialize.
	void RegisterPage(Page page);
	// Render thread: the registered pages, by order.
	std::vector<Page> Pages();

	// Render thread, from a page's draw: this frame the page sizes itself to fill the window (the Guitar Tone page's
	// Tone designer panels) rather than to its content. The window's height normally follows the page's content; a
	// page that also follows the window's height would feed back and grow every frame, so the shell lets such a page's
	// height be dragged instead, opening at `naturalHeight` (the page's preferred height, page padding included)
	// until it is. 4.x's g_pageFillsHeight / g_pageNaturalHeight.
	void FillWindowHeight(float naturalHeight);
}
