#include "RecordPage.hpp"

#include "PageCommon.hpp"
#include "../ShellPages.hpp"
#include "../../Recorder/TakeRecorder.hpp"

#include <cmath>

using namespace Overlay;

namespace AudioBridge::Pages
{
	namespace
	{
		// Round record button: red dot when idle, rounded stop square with a pulsing ring while recording.
		bool RecordButton(bool recording)
		{
			const float size = 72.0f;
			const ImVec2 start = ImGui::GetCursorScreenPos();
			const bool pressed = ImGui::InvisibleButton("##record", ImVec2(size, size));
			const bool hovered = ImGui::IsItemHovered();
			ImDrawList* list = ImGui::GetWindowDrawList();
			const ImVec2 c(start.x + size * 0.5f, start.y + size * 0.5f);
			list->AddCircleFilled(c, size * 0.5f, hovered ? Color::Hover : Color::Track, 40);
			if (recording)
			{
				const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
				list->AddCircle(c, size * 0.5f - 1.0f, Mix(IM_COL32(239, 68, 68, 60), Color::Record, pulse), 40, 3.0f);
				const float h = size * 0.18f;
				list->AddRectFilled(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), Color::Record, 4.0f);
			}
			else
			{
				list->AddCircleFilled(c, size * (hovered ? 0.26f : 0.24f), Color::Record, 32);
			}
			return pressed;
		}

		void Draw()
		{
			const LiveState& live = Poll();
			PageHeader("Record", "Capture a take without leaving the game.");
			const bool recording = live.status.recording != 0;
			const Audio::Takes::Status take = Audio::Takes::GetStatus();
			// PreferVideo reads the ini: once a second here, not every frame, and again after this page changes it.
			static int cachedFormat = -1;
			static ULONGLONG formatReadAt = 0;
			if (cachedFormat < 0 || GetTickCount64() - formatReadAt >= 1000)
			{
				cachedFormat = Audio::Takes::PreferVideo() ? 1 : 0;
				formatReadAt = GetTickCount64();
			}
			int format = cachedFormat;

			BeginCard();
			// The same toggle as the record key (AudioRecorderMod).
			if (RecordButton(recording)) Audio::Takes::Toggle(format == 1);
			Hint("Starts or stops a take. Same as the record hotkey.");
			ImGui::SameLine(0.0f, 20.0f);
			ImGui::BeginGroup();
			ImGui::PushFont(GetFonts().big);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(recording ? Color::Text : Color::Faint));
			ImGui::TextUnformatted(FormatTime(live.status.recordedFrames / 48000.0).c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();
			using Audio::Takes::VideoPhase;
			if (take.video == VideoPhase::Starting) StatusPill("Starting video", Color::Warn, true);
			else if (take.video == VideoPhase::Capturing && recording) StatusPill("Recording video", Color::Record, true);
			else if (take.video == VideoPhase::Finishing) StatusPill("Saving video", Color::Warn, true);
			else if (recording) StatusPill("Recording audio", Color::Record, true);
			else StatusPill("Ready", Color::Muted);
			ImGui::EndGroup();

			ImGui::Dummy(ImVec2(0, 4));
			static const char* const formats[] = { "Audio", "Video" };
			ImGui::BeginDisabled(recording);
			if (SegmentedRow("Format", &format, formats, 2, "Audio saves WAVs. Video also records the Rocksmith window into an MP4 with the game sound."))
			{
				Audio::Takes::SetPreferVideo(format == 1);
				cachedFormat = format;
			}
			if (format == 1)
			{
				static int cachedQuality = -1;
				static ULONGLONG qualityReadAt = 0;
				if (cachedQuality < 0 || GetTickCount64() - qualityReadAt >= 1000)
				{
					cachedQuality = Audio::Takes::VideoQuality();
					qualityReadAt = GetTickCount64();
				}
				int quality = cachedQuality;
				static const char* const qualities[] = { "High", "Standard", "Small" };
				if (SegmentedRow("Quality", &quality, qualities, 3, "High is the sharpest (about 250 MB a minute at 1440p). Standard is about half that. Small records at 1080p, about 80 MB a minute, for sharing."))
				{
					Audio::Takes::SetVideoQuality(quality);
					cachedQuality = quality;
				}
			}
			ImGui::EndDisabled();
			Caption("The full game mix and Player 1's dry guitar are always saved together as a pair.");
			if (!take.message.empty()) ColorText(take.message.c_str(), take.messageIsError ? Color::Bad : Color::Good);
			if (FAILED(live.status.recordingError))
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Color::Bad));
				ImGui::Text("Recording error 0x%08X", static_cast<unsigned>(live.status.recordingError));
				ImGui::PopStyleColor();
			}
			EndCard();

			BeginCard("Saving");
			// Folder() reads the ini and the known-folder API; once a second is plenty for a label.
			static std::wstring cachedFolder;
			static ULONGLONG folderReadAt = 0;
			if (folderReadAt == 0 || GetTickCount64() - folderReadAt >= 1000)
			{
				cachedFolder = Audio::Takes::Folder();
				folderReadAt = GetTickCount64();
			}
			const std::wstring folder = cachedFolder;
			ImGui::TextUnformatted(Utf8(folder).c_str());
			if (Button("Open folder", false, "Opens behind the game, so alt-tab to see it."))
			{
				// This launch's session folder once it has a take in it, otherwise the folder of all sessions.
				const std::wstring session = Audio::Takes::SessionFolder();
				std::error_code ec;
				const bool hasSession = std::filesystem::is_directory(session, ec);
				if (!hasSession) std::filesystem::create_directories(folder, ec);
				OpenAsync(hasSession ? session : folder);   // off the render thread (a 103 ms frame here)
			}
			KeyBindPicker("RecordingHotkey", "Record hotkey");
			Caption("Each game launch saves into its own dated folder. The hotkey uses the format chosen above. Change the folder in RSMods, Rocksmith Audio Bridge page.");
			EndCard();
		}
	}

	void RegisterRecordPage()
	{
		SnapshotKeyBinds();
		Shell::RegisterPage({ "record", "Audio", Glyph::Record, "Record", 60, Draw, {} });
	}
}
