#include "ShellWindow.hpp"
#include "ShellState.hpp"

#include "OverlayUi.hpp"
#include "ShellPages.hpp"
#include "../Shared/ProductVersion.hpp"

#include "Log.hpp"

#include <algorithm>
#include <cmath>

using namespace Overlay;

namespace AudioBridge::Shell
{
	namespace
	{
		constexpr float HEADER_HEIGHT = 58.0f;
		constexpr float SIDEBAR_WIDTH = 208.0f;
		constexpr float SIDEBAR_MINIMUM = 520.0f;

		// Set by FillWindowHeight while the page draws, read once it has (render thread).
		bool pageFillsHeight = false;
		float pageNaturalHeight = 0.0f;
		constexpr wchar_t SETTINGS_KEY[] = L"Software\\RSMods\\Overlay";

		// The page last shown, by id, remembered across launches (HKCU, as 4.x kept it).
		std::string LoadLastPage()
		{
			wchar_t value[64] = {};
			DWORD size = sizeof(value);
			if (RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"LastPage", RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return {};
			std::string id;
			for (const wchar_t* c = value; *c; ++c) id += static_cast<char>(*c & 0x7F);
			return id;
		}

		void SaveLastPage(const std::string& id)
		{
			const std::wstring name(id.begin(), id.end());
			const LSTATUS result = RegSetKeyValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"LastPage", REG_SZ, name.c_str(),
				static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
			if (result != ERROR_SUCCESS) LOG_WARNING("(OVERLAY) Could not save the last overlay page, error " << result << std::endl);
		}

		std::string& SelectedPage()
		{
			static std::string selected = LoadLastPage();
			return selected;
		}

		std::vector<Page> AvailablePages()
		{
			std::vector<Page> pages = Pages();
			pages.erase(std::remove_if(pages.begin(), pages.end(), [](const Page& page) {
				return page.available && !page.available();
			}), pages.end());
			return pages;
		}

		void Header(bool* open, const std::string& keyName)
		{
			const ImVec2 origin = ImGui::GetWindowPos();
			const float width = ImGui::GetWindowWidth();
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + HEADER_HEIGHT), Color::Header, ImGui::GetStyle().WindowRounding, ImDrawFlags_RoundCornersTop);
			list->AddLine(ImVec2(origin.x, origin.y + HEADER_HEIGHT - 1.0f), ImVec2(origin.x + width, origin.y + HEADER_HEIGHT - 1.0f), Color::CardEdge);

			const float markSize = 34.0f;
			const ImVec2 mark(origin.x + 16.0f, origin.y + (HEADER_HEIGHT - markSize) * 0.5f);
			if (ImTextureID logo = LogoTexture())
				list->AddImage(logo, mark, ImVec2(mark.x + markSize, mark.y + markSize));
			else
			{
				list->AddRectFilled(mark, ImVec2(mark.x + markSize, mark.y + markSize), Color::Accent, 7.0f);
				DrawIcon(list, ImVec2(mark.x + markSize * 0.5f, mark.y + markSize * 0.5f), Glyph::MusicNote, Color::Text);
			}
			ImGui::SetCursorScreenPos(ImVec2(mark.x + markSize + 12.0f, origin.y + 10.0f));
			ImGui::BeginGroup();
			ImGui::TextUnformatted("Rocksmith Audio Bridge");
			ImGui::PushFont(GetFonts().caption);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Muted));
			ImGui::TextUnformatted("In-game controls");
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::EndGroup();

			const float closeSize = 32.0f;
			ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 14.0f - closeSize, origin.y + (HEADER_HEIGHT - closeSize) * 0.5f));
			if (IconButton("##close", Glyph::Close, closeSize)) *open = false;
			Hint(("Close. Press " + keyName + " to open it again.").c_str());
		}

		// True when a page was picked (`selected` is then that page, and saved).
		bool Sidebar(const std::vector<Page>& pages, std::string& selected)
		{
			bool picked = false;
			const ImVec2 origin = ImGui::GetWindowPos();
			ImGui::GetWindowDrawList()->AddRectFilled(origin, ImVec2(origin.x + ImGui::GetWindowWidth(), origin.y + ImGui::GetWindowHeight()), Color::Sidebar);
			ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x + ImGui::GetWindowWidth() - 1.0f, origin.y),
				ImVec2(origin.x + ImGui::GetWindowWidth() - 1.0f, origin.y + ImGui::GetWindowHeight()), Color::CardEdge);

			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
			const std::string* shownGroup = nullptr;
			for (const Page& page : pages)
			{
				// A group's label shows above its first page.
				if (!page.group.empty() && (!shownGroup || *shownGroup != page.group))
				{
					ImGui::Dummy(ImVec2(0, 8));
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
					SectionLabel(page.group.c_str());
					ImGui::Dummy(ImVec2(0, 2));
				}
				shownGroup = &page.group;
				if (NavItem(page.glyph, page.label.c_str(), page.id == selected) && page.id != selected)
				{
					selected = page.id;
					SaveLastPage(selected);
					picked = true;
				}
			}
			ImGui::PopStyleVar();
			return picked;
		}

		// Sidebar and the selected page side by side; returns the page's content height, for the window to fit it, or
		// its natural height when it fills the window (pageFillsHeight).
		float SidebarAndPage()
		{
			const std::vector<Page> pages = AvailablePages();
			std::string& selected = SelectedPage();
			// A page asked for from outside (OpenOverlayAt: F9's Mod Settings row) counts as a pick, if it's offered.
			if (const std::string requested = TakeRequestedPage(); !requested.empty() && requested != selected &&
				std::any_of(pages.begin(), pages.end(), [&](const Page& page) { return page.id == requested; }))
			{
				selected = requested;
				SaveLastPage(selected);
			}
			// The remembered choice stays; while its page isn't offered (Guitar Tone before the driver loads, say) the
			// first page stands in, without replacing it, so the choice comes back once the page does.
			const auto current = std::find_if(pages.begin(), pages.end(), [&](const Page& page) { return page.id == selected; });
			std::string shown = current != pages.end() || pages.empty() ? selected : pages.front().id;

			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
			ImGui::BeginChild("##sidebar", ImVec2(SIDEBAR_WIDTH, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);
			if (Sidebar(pages, shown)) selected = shown;
			ImGui::EndChild();
			ImGui::PopStyleVar();

			ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26, 20));
			ImGui::BeginChild("##page", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
			const auto page = std::find_if(pages.begin(), pages.end(), [&](const Page& candidate) { return candidate.id == shown; });
			pageFillsHeight = false;
			if (page != pages.end() && page->draw) page->draw();
			else Caption("No pages yet.");
			const float contentHeight = pageFillsHeight ? pageNaturalHeight : ImGui::GetCursorPosY() + 20.0f;
			ImGui::EndChild();
			ImGui::PopStyleVar();
			return contentHeight;
		}
	}

	// The height follows the open page (last frame's measurement), eased, never shorter than the sidebar and at most
	// 92% of the screen; the width stays the user's (imgui.ini).
	// A page that fills the window (FillWindowHeight, the Guitar Tone page's Tone designer panels) instead lets the
	// height be dragged too, and fills whatever it is. Until it is dragged it opens at the page's natural height (the
	// board, or a long tone list, under the same 92% cap) rather than the cap itself, which left a tall empty library
	// column; once dragged, that height is remembered for the session and eased to when the page opens. As 4.x.
	void DrawWindow(bool* open, const std::string& keyName)
	{
		const ThemeScope theme;
		const Fonts& fonts = GetFonts();
		if (fonts.body) ImGui::PushFont(fonts.body);

		static float pageContentHeight = 0.0f;
		static float fillContentHeight = 0.0f;
		static float shownHeight = 0.0f;
		static float fillHeight = 0.0f;
		static bool fillDragged = false;   // the fill page's height was set by dragging the edge
		static bool fillPage = false;      // last frame's page filled the window
		const ImGuiIO& io = ImGui::GetIO();
		const float naturalFill = (std::min)(HEADER_HEIGHT + (std::max)(fillContentHeight, SIDEBAR_MINIMUM), io.DisplaySize.y * 0.92f);
		fillHeight = std::clamp(fillDragged ? fillHeight : naturalFill, HEADER_HEIGHT + SIDEBAR_MINIMUM,
			(std::max)(io.DisplaySize.y, HEADER_HEIGHT + SIDEBAR_MINIMUM));
		const float wanted = fillPage ? fillHeight : (std::min)(HEADER_HEIGHT + (std::max)(pageContentHeight, SIDEBAR_MINIMUM), io.DisplaySize.y * 0.92f);
		shownHeight = shownHeight <= 0.0f ? wanted : shownHeight + (wanted - shownHeight) * (std::min)(1.0f, io.DeltaTime * 14.0f);
		if (std::fabs(wanted - shownHeight) < 0.5f) shownHeight = wanted;
		const bool userHeight = fillPage && shownHeight == wanted;   // settled on a fill page: the height is the user's
		ImGui::SetNextWindowSize(ImVec2(900, shownHeight), ImGuiCond_FirstUseEver);
		if (userHeight) ImGui::SetNextWindowSizeConstraints(ImVec2(700, HEADER_HEIGHT + SIDEBAR_MINIMUM), ImVec2(4000, io.DisplaySize.y));
		else ImGui::SetNextWindowSizeConstraints(ImVec2(700, shownHeight), ImVec2(4000, shownHeight));

		if (ImGui::Begin("Rocksmith Audio Bridge##overlay", open,
			ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
		{
			if (userHeight && std::fabs(ImGui::GetWindowHeight() - shownHeight) >= 0.5f) {   // follows a drag of the edge
				fillDragged = true;
				fillHeight = shownHeight = ImGui::GetWindowHeight();
			}
			Header(open, keyName);
			ImGui::SetCursorPos(ImVec2(0, HEADER_HEIGHT));
			const float measured = SidebarAndPage();
			fillPage = pageFillsHeight;
			if (fillPage) fillContentHeight = measured;
			else pageContentHeight = measured;

			ImGui::PushFont(GetFonts().caption);
			static const std::string versionLabel = std::string("v") + ProductVersion::VERSION;
			const ImVec2 versionSize = ImGui::CalcTextSize(versionLabel.c_str());
			const ImVec2 windowPos = ImGui::GetWindowPos();
			const ImVec2 windowSize = ImGui::GetWindowSize();
			ImGui::GetWindowDrawList()->AddText(ImVec2(windowPos.x + windowSize.x - versionSize.x - 14.0f,
				windowPos.y + windowSize.y - versionSize.y - 10.0f), Color::Muted, versionLabel.c_str());
			ImGui::PopFont();
		}
		ImGui::End();
		// Pages' floating windows (the Noise gate pedal), over the shell and the game while the overlay is open.
		for (const Page& page : AvailablePages())
			if (page.floating) page.floating();
		if (fonts.body) ImGui::PopFont();
	}

	void FillWindowHeight(float naturalHeight)
	{
		pageFillsHeight = true;
		pageNaturalHeight = naturalHeight;
	}

}
