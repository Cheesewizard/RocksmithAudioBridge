#pragma once
// Recording takes started from inside the game (overlay Record button and the record hotkey), without the
// desktop Audio Bridge window.
//
// Audio: the engine records the wet + dry WAVs in process (control ops 2 start / 3 stop), exactly as the
// desktop bridge asks it to. In multiplayer Player 2 gets its own dry WAV (-dry-p2) beside Player 1's (-dry);
// the wet stays one file because the game mixes both players before the output tap. All files of a take start
// at the same moment (padded to the wet file's first block), so they line up when dropped into a DAW.
// Video: RSMods.exe is started hidden with --video-take (GUI/Features/VideoRecorder/VideoTakeHost.cs). It runs the same Windows
// Graphics Capture recorder the desktop bridge uses, and on stop muxes the capture with the finished WAV into an
// MP4, deletes the now-redundant wet WAV (a video take is always dry WAV + MP4), then exits. Out of process on purpose: an encoder crash or hang loses one take, not the game, and the
// encoder's memory stays out of Rocksmith's 32-bit address space. RSMods.exe is reused rather than a new helper
// executable because antivirus tends to quarantine new unsigned capture tools.

#include <string>

namespace Audio::Takes
{
	enum class VideoPhase { Off, Starting, Capturing, Finishing, Saved, Failed };

	struct Status
	{
		VideoPhase video = VideoPhase::Off;
		std::string message;        // last outcome or problem, UTF-8, empty when nothing to say
		bool messageIsError = false;
	};

	// Start a take (video adds an MP4 of the game window) or stop the open one. Returns false when nothing
	// happened (for example the engine refused to start), with the reason in GetStatus().message.
	bool Toggle(bool video);
	// True while this controller owns the open take (as opposed to one started by the desktop bridge).
	bool OwnsTake();
	// Cheap; also advances the video helper's state. Call from the overlay each frame or from a timer.
	Status GetStatus();

	// Persisted choices, shared with the desktop bridge through RSMods.ini [Audio Bridge].
	bool PreferVideo();                    // CaptureMode = Video (default) / Audio
	void SetPreferVideo(bool video);
	int VideoQuality();                    // VideoQuality = High (0, default) / Standard (1) / Small (2, 1080p)
	void SetVideoQuality(int quality);
	std::wstring Folder();                 // RecordingDirectory, or <game>\Recordings
	std::wstring SessionFolder();          // Folder()\<game launch time>: where this launch's takes go
}
