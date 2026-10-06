# Rocksmith Audio Bridge Release Notes

## Release
- **Version:** `v4.0.0`
- **Build date:** `2026-10-06`
- **Scope:** `Release`

> Note by Note is a **beta** feature toggled from the menu, and USB cable compatibility is **experimental**.

## Feature list

### New name: Rocksmith Audio Bridge
- Rocksmith Audio Bridge is an independent extension of RSMods, with its own maintainer. Please report problems at https://github.com/Cheesewizard/RocksmithAudioBridge/issues, not to the RSMods authors.
- RSModsPlus is now called **Rocksmith Audio Bridge**, because the old name was easily mistaken for Rocksmith+. The Audio Bridge icon is the product logo. The settings window title reads "Rocksmith Audio Bridge 4.0 (based on RSMods 1.2.8.3)", its tab and footer use the new name, and the in-game watermark and overlay show it too. The in-game overlay shows the version (4.0) in its bottom-right corner, and the log header reads "Rocksmith Audio Bridge 4.0".
- The release is one installer, `RocksmithAudioBridge-Installer.exe`. Close Rocksmith and RSMods, run it and press **Install**. It copies the mod into the Rocksmith folder and adds the **NOTE BY NOTE** switch to Riff Repeater Advanced Settings (the switch lives in the game's `cache.psarc`; only that one entry is added, and anything other mods changed in the file is kept).
- Run the installer again for **Reinstall / Repair** (also puts the NOTE BY NOTE switch back after a Steam file check or another tool reset `cache.psarc`) or **Uninstall**. Uninstall removes the ASIO bridge driver (one Windows admin prompt), removes only the NOTE BY NOTE entry from `cache.psarc`, and deletes the files it installed. It asks before deleting your settings, and it never touches your recordings.
- `RSMods.ini` settings and the registry entries are unchanged. Logs are in `%LOCALAPPDATA%\Rocksmith Audio Bridge\Logs`, and recordings are saved to `Videos\Rocksmith Audio Bridge` by default.

### Audio Bridge (new)
The Audio Bridge has two places to control it: the **in-game overlay** (press `\`) for everything you use while playing, and the **Rocksmith Audio Bridge page in RSMods** for setup with the game closed. There is no separate desktop program.

**RSMods page (game closed)**
- Power switch: turns the bridge on or off. The bridge taps the game output rather than replacing it, so turning it off never forces an ASIO downgrade.
- Input switch: Cable or ASIO. Cable mode only renames the RS_ASIO files and never rewrites `RS_ASIO.ini`.
- ASIO bridge driver: status only. The installer installs it; if it is missing or broken, run the installer again.
- Recordings folder.

**ASIO bridge driver**
- A virtual ASIO device that sits between RS_ASIO and your real interface, passing audio through while tapping it. It enables ASIO game-mix recording and switching the game output from the overlay. The installer always registers it (one Windows admin prompt; Reinstall / Repair refreshes it). It is required: if the prompt is declined, the install is rolled back; run the installer again and approve it.
- RS_ASIO setup remains user-owned. Nothing ever rewrites `RS_ASIO.ini`. To put the driver in the signal chain, set the relevant output and paired input `Driver` entries to `Rocksmith Audio Bridge ASIO`; removing the driver also leaves the file untouched.
- Interface recovery (ASIO mode with the driver configured in RS_ASIO): booting without the interface keeps Rocksmith available through a shared-mode Windows output, with Real Tone Cable fallback when one is present. Reconnecting the interface does not switch automatically; pick it on the overlay's Output page to move back to ASIO without restarting the game.

**In-game overlay (press `\`)**
- The Rocksmith Audio Bridge control centre, branded with the Audio Bridge logo: a sidebar of pages (Note by Note, Mixer, Input, External amp, Output, Record, General, Drop Pedal), switch toggles, sliders with their value always readable, LED level meters, channel-strip faders and a record button with a live timer. Every control has a hover tooltip. The window sizes itself to the open page (up to 92% of the screen height, width adjustable) and the mouse does not reach the game while the cursor is over it. A REC indicator in its header shows on every page while recording.
- Mixer: seven channels. Master first, then Player 1 guitar and Player 2 guitar, then Song (backing track), Microphone, Voice-over and Effects. Player 1 and Player 2 levels are playback only and never affect note detection. Faders are session-only playback levels by design.
- Input: live guitar level meter on a dB scale (separate "input 1" and "input 2" bars in multiplayer) and guitar input conditioning for any input path (ASIO interface, Real Tone Cable, or a third-party cable), running on the Player 1 input before the game's amp and note gate: make-up gain for a quiet input; a noise suppressor that reduces between-note hiss without cutting sustain; an input compressor that evens out string-level variation; an adaptive hum filter that measures your mains hum between notes and removes only its lines (50 or 60 Hz, up to 8 kHz) without dulling the tone; and a Rocksmith gate override that sets the game's own noise floor (P1_NoiseFloor). The noise suppressor and the Rocksmith gate can be used separately or together.
- Output: names the device the game is playing through (for example "Playing to: M-Track (direct)") and lists your playback devices to move the game's sound to, live, without restarting. It works with the bridge driver in the ASIO chain (and switches back to the ASIO interface from the same list) and in Cable mode. The switch lasts until you close Rocksmith. The Windows device of the interface RS_ASIO is using is shown but locked, because playing to it would silence the interface. If the new device does not start playing within a few seconds, the overlay switches back on its own. Output buffer size (Cable mode) is also here.
- Output protection: an optional look-ahead limiter caps the digital output at the selected ceiling. It is off by default and does not replace setting a safe interface or headphone volume.
- Record: the Record button or the record hotkey (F6 by default; change it on the overlay's Record page, which refuses keys the game, Windows, Steam or another RSMods shortcut already use) captures paired game-mix and dry guitar takes, or video. Video is captured by a hidden recorder (RSMods.exe with no window) that saves an MP4 with the game sound when you stop; the picture starts about three seconds after the sound while the recorder starts up. Cable/shared-output game-mix recording uses the native render tap; ASIO game-mix recording requires the bridge driver to be installed and configured. Dry recording follows the active Player 1 input. A red REC indicator appears in the top right of the game while recording.
- Note by Note: shows or hides the detection readout, sets the readout and target sizes, moves the target Left, Center or Custom, sets line spacing, and edits the three detection colours (text, green for a correct read, amber when a detector helped pass a note but read a slightly different pitch) with a live preview. With the page open, each HUD block can be dragged, resized with the mouse wheel and reset with a double-click.
- Drop Pedal: shows the current tuning, mode and shift with your pedal shortcuts, turns the pedal on or off (applies at the next launch) and edits its three readout colours live with a preview.
- General: force update song list re-scans the dlc folder live, so Rocksmith picks up a psarc you just added without a restart.
- Overlay changes to Note by Note, Drop Pedal, input conditioning and output protection are saved to `RSMods.ini` and survive a relaunch.

### External amp (new; ASIO with the bridge driver)
- Play Rocksmith through your own amp sim, such as AmpliTube 5, instead of the game's amp, while Rocksmith keeps your clean guitar signal for note detection. Pick **Rocksmith Audio Bridge ASIO** as the audio device in the amp sim. It shares your interface with the game instead of opening it a second time, so there is no two-programs-on-one-driver crackle.
- The amp sim gets the raw guitar from your interface, before the overlay's input processing, and its sound is mixed into the game's output one buffer later, at the interface's buffer size. Sample rate and buffer size follow the game; an amp sim set to another buffer size is asked to switch.
- While an amp sim is connected, the game's own guitar is muted, and your Player 1 level comes back when it disconnects or you turn External amp off. Note detection is not affected.
- Overlay **External amp** page: on/off switch, the connected app, format, added latency, a late-buffer count, clean-signal and return meters, the amp return level, an optional extra safety buffer for busy PCs, and falling back to the game's amp when the app disconnects. Settings are saved. With the switch and the fallback on (the default) it is automatic: the game's amp plays until an amp sim connects.
- A 64-bit copy of the bridge driver ships for 64-bit amp sims. The installer registers both copies; on an existing install run Reinstall / Repair once (one admin prompt).
- ASIO interfaces with the bridge driver in RS_ASIO only. The Real Tone Cable is not covered yet (#71).

### USB cable compatibility (experimental)
- Converts the cable's sample rate to the 48 kHz Rocksmith expects, so a cable that runs at a different rate is still compatible.
- Reads the cable as soon as Windows has new audio, instead of on a fixed timer, so the input delay stays consistent rather than changing with whatever else is running on the PC.
- No more "No audio output device" in Cable mode. Rocksmith gets the Audio Bridge's own input and output, so a playback device Windows reports badly, or no playback device at all, no longer stops the game at start-up. Sound goes to the Windows default, or to the device picked on the overlay's Output page.

### Note by Note practice mode (beta; toggle in the menu)
- In-process, per-note detection built as a single tuned blend of three components that always work together for accuracy: the game's native pitch/chord matcher, the enhanced raw-pitch verifier, and the FretNet ML detector. None of the three is optional; the bundled FretNet service is auto-launched as part of detection, and all three feed one accept decision.
- Fretboard note cues during Riff Repeater (detector feedback colouring on the cues is not currently working, see Known bugs #90).
- Bend detection and visualization groundwork.
- Chords are confirmed from the guitar audio itself: each chord note has to ring out fresh at the strum, and the game has to agree in some way. Clean strums the game used to miss (common on two-note chords) now count, and a single note, a note still ringing from the last chord, or a partial strum no longer passes a chord.
- Sliding into a note or a chord no longer counts. A picked note needs the click of the pick and a chord needs the lift of a strum; hammer-ons, pull-offs and taps still pass without a pick.
- A note picked just as it arrives at the strike line now counts; before, it was sometimes ignored and had to be played again.
- Flow mode (on by default): the song keeps playing while you hit the notes and stops at the first note you miss. Switch it with **FLOW MODE**, just below NOTE BY NOTE in Riff Repeater Advanced Settings. With flow on, the Riff Repeater speed is capped to what detection can keep up with.
- Right Arrow skips the note Note by Note is waiting on, so you are never stuck.
- Bass arrangements (experimental). Note by Note now recognises a bass arrangement from the game's own detector and waits for each bass note too. On bass the game's own note detection decides with a bass-trained machine-learning model alongside it, and the guitar-only audio checks (pick click, strum checks) are skipped. Tested with a pitch-shifted guitar, not a real bass yet.

### Drop Pedal
- Cable users now get native Drop Pedal support: no more multipitch pitch-shifting, lower latency, and any tone can be used.
- Configurable overlay colours.
- Speaker Mode is one global setting that only Player 1 changes. In multiplayer, Player 2's note detection now follows the shifted song, so both players tune to Player 1's tuning; Player 2's shift keys do nothing in Speaker Mode.
- Reduced down-shift latency (measured).

### HUD and general
- HUD pitch labels spelled the way guitarists read them; corner status removed.

## Known bugs
Severity / impact should be set per entry. Issue numbers reference github.com/Cheesewizard/RocksmithAudioBridge.

- **[High]**
	- No audio when playing notes in dense passages.
	- #63 Speaker Mode emits a piercing high-pitched tone on song load under CPU load (buffer underrun; headphone safety risk). Less frequent since b6283cfe but still heard occasionally.

- **[Medium]**
	- Fretboard visual glitches: incorrect note fingers and frozen animations. Currently offset by a custom-UI readout of the target notes shown separately from the fingerboard.
	- #82 Drop Pedal does not account for mixed ASIO and Real Tone Cable multiplayer inputs.
	- #65 Riff Repeater timeline does not show the orange section colour (only purple). Not yet compared against v3.2.1, so it may not come from this mod.
	- #42 Note by Note: a bend can take a while to be confirmed (Right Arrow skips it).
	- #57 Auto-tuning sets the Drop Pedal / Speaker Mode offset too late (must apply at song-list scroll, before load).

- **[Low]** (mostly Note by Note visual polish; only affect the beta feature when enabled)
	- #90 Fretboard note cues do not show detector feedback colours (no visual detector feedback on the cues).
	- #60 Top strings flicker / wrong string animation during a held plain note.
	- #47 After a loop turnover the restarted pass renders all notes gray.
	- #44 Bend visuals wrong during the hold (floating marker, plain-note repaint).
	- #38 Bend on high E fret 15 triggers the open-E success animation.
	- #37 One-frame flash on note markers still occurs.

## Bug fixes
- #94 More ASIO audio devices are now supported. The input hook now waits for the RS_ASIO capture path to become ready instead of relying on a narrow startup timing window, fixing interfaces whose audio path initialized too late to be hooked.
- Speaker Mode now works with custom DLC built on newer Wwise versions (DLC Builder / Wwise 2022/2023). The shipped WEM decoder rejected unrecognized RIFF chunks (e.g. `akd`), so Speaker Mode failed on those songs even though Rocksmith played them fine; the WEM is now rewritten keeping only the chunks the decoder recognizes. This was never fixed before this release.
- #23 Custom string colours now match note colours (user-reported).
- #56 The in-game tuner holds one target pitch again. Before, it flickered between two neighbouring tunings (for example E and Eb) and could not be used to tune.
- Stopping a recording no longer freezes the game while the dry take is finished, and closing the game mid-take now saves both files properly.
- A recording that hits an error (for example a full disk) can now be stopped from the overlay or the record hotkey; before, each press tried to start a new take. Short disk stalls no longer end a take at small ASIO buffer sizes.
- Output limiter now also works in speaker mode (it was off there even when switched on).
- Moving the game sound to a 44.1 kHz or 5.1 device now works without crackling.
- Picking the ASIO interface in the overlay no longer freezes the game while the driver loads.
- Interfaces whose driver uses a 24-bit or 32-bit float sample format are now converted correctly instead of producing noise; a format that cannot be converted keeps speaker mode.
- A normal stop from RS_ASIO is no longer mistaken for a stalled interface (which switched a working interface to speaker mode).
- With two Real Tone Cables, Player 1 and Player 2 each get one (before, neither got any input).
- Speaker Mode keeps working through long sessions and through a long pause.
- Removing the ASIO bridge driver works even when its file is missing.
- The settings window no longer undoes Note by Note, HUD layout or Drop Pedal overlay changes made in game.
- The settings window no longer offers to "update" to the original RSMods, which replaced the Audio Bridge.
- RSMods.exe is back in the `RSMods` folder, as in the original RSMods. 4.0 pre-releases put it in the game root, which left Custom Mods, sound pack and Twitch files loose in the game folder and broke the toolkit and 7-Zip tools; the installer removes the old root copies.
- Note by Note installs its hooks only on the September 2022 game build it is made for, and a hold that runs into a fault no longer leaves the song stopped.
