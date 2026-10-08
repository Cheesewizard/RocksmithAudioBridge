# Rocksmith Audio Bridge 4.1 release notes

A bug-fix release on top of 4.0. To update, run the installer over your existing install; your `RSMods.ini` settings and key bindings are kept.

## RS_ASIO: use 0.7.2 or newer
ASIO users need [RS_ASIO](https://github.com/mdias/rs_asio/releases) 0.7.2 or newer; the latest release is recommended (0.7.4 and 0.7.5 are tested).
- Before 0.6.0, RS_ASIO cannot patch the current Rocksmith game patch: the sound is distorted and there is no guitar input, or the game closes.
- 0.6.0 crashes on the title screen when used with RSMods.
- With 0.6.1 to 0.7.1, 4.0 crashes right after the profile screen. 4.1 fixes that (see below), but 0.7.2 or newer is still recommended, and 0.6.2 has known compatibility issues.

To check your version, open `RS_ASIO-log.txt` in the Rocksmith folder: the first line reads `Wrapper DLL loaded (vX.Y.Z)`. To update, download the release from the RS_ASIO releases page, replace both `RS_ASIO.dll` and `avrt.dll`, and keep your own `RS_ASIO.ini`.

## Fixes
- **No more crash right after the profile screen with RS_ASIO 0.6.1 to 0.7.1.** Those RS_ASIO versions and the inherited "two Real Tone Cables" message bypass patch the same game code. With the bypass off, it mistook RS_ASIO's patch for its own and wrote over it, leaving a broken instruction the game ran into at the profile screen. The bypass now only ever undoes its own patch.
- **A rare crash on the main menu or while the game changes screens is fixed.** The top-right audio readout asked the game every frame whether multiplayer was on, through a chain of game pointers that is briefly invalid during a screen change. That read, and the other game reads that run every frame, now check every step and skip the read instead of crashing.
- **Windows no longer logs a Rocksmith error about 40 seconds into every launch.** An inherited hook on the Steam DLC scan left the game's error-handling chain broken, so the game's own internal check tripped over it on every boot. The game kept running, but Windows wrote an error report each time; on PCs set to save full crash dumps, the freeze while writing it could stop the guitar input or the game sound for the rest of the session. Updating the song list from the overlay still works.
- The log (`RSMods_debug.txt`) now records the RS_ASIO version, or `RS_ASIO: not loaded` in Cable mode, and says so when that version is too old. When Note by Note cannot wire its Riff Repeater switch or its MISSED label, the log shows what it found in the game code, and names the program that hooked it if another mod or overlay got there first.

## Known issues
- Speaker Mode: on a heavily loaded PC, a short high-pitched tone can play while a song loads. Starting at a moderate volume is a good habit.
- Two-player Drop Pedal works when both players use the same kind of input, both ASIO or both Real Tone Cable.
- With auto-tuning on, the Drop Pedal / Speaker Mode shift is sometimes applied when the song loads rather than while you browse the song list.
- Note by Note is in beta. Some visuals are still being polished: note markers during holds, bends and loop restarts, and the section colour on the Riff Repeater timeline.

Report problems on [this repository](https://github.com/Cheesewizard/RocksmithAudioBridge/issues). Attach `RSMods_debug.txt` from the Rocksmith folder (logging is on when that file exists, so create an empty one if it is missing), `RS_ASIO-log.txt` and `RS_ASIO.ini` if you use ASIO, and the logs in `%LOCALAPPDATA%\Rocksmith Audio Bridge\Logs`.

---

# Rocksmith Audio Bridge 4.0.0 release notes

RSModsPlus is now **Rocksmith Audio Bridge**. The old name was easily mistaken for Rocksmith+, and the mod has grown into a full suite: play any song without retuning, run your own amp sim, record your playing and practise note by note, all controlled from an in-game overlay.

https://github.com/user-attachments/assets/1b51d057-285e-4ded-a113-5c25af1f3346

## Install

1. Close Rocksmith and RSMods.
2. Download **`RocksmithAudioBridge-Installer.exe`** below and run it.
3. Press **Install** and accept the Windows admin prompt (it registers the Audio Bridge ASIO driver).
4. With Rocksmith closed, open RSMods (`RSMods\RSMods.exe` in your Rocksmith folder), go to the **Rocksmith Audio Bridge** tab and set **Guitar input** to **Real Tone Cable** or **ASIO interface**.

The installer includes the full RSMods suite, so you don't need to install RSMods first. Run it again to **Reinstall / Repair** (for example after a Steam file check) or **Uninstall**. Uninstall asks before removing your settings and never touches your recordings.

**Upgrading from RSModsPlus 3.x:** run the installer over your existing install. Your `RSMods.ini` settings and key bindings are kept. Cable users no longer need the MultiPitch tone for the Drop Pedal.

Requirements: the Steam version of Rocksmith 2014 Remastered on Windows, and the MS Visual C++ 2015-2019 redistributable. **The Learn & Play edition is not supported yet.** Works with a Real Tone Cable, or an ASIO interface through [RS_ASIO](https://github.com/mdias/rs_asio).

Full guide for every feature: [README](https://github.com/Cheesewizard/RocksmithAudioBridge#readme).

## What's new

### In-game overlay
Press `\` in game. Pages for the mixer, guitar input, output, recording, External amp, Note by Note, the Drop Pedal and general settings. Every control has a tooltip, and your settings are saved.

### Stable audio on any speakers
- No more "No audio output device" at start-up. The Audio Bridge gives Rocksmith its own input and output devices, so one badly behaved Windows device, or no speakers at all, can't stop the game from starting.
- Rocksmith always sees the 48 kHz stereo device it expects, and the Audio Bridge converts to your real speakers or headphones, so 44.1 kHz and 5.1 devices play without errors or crackling.
- Move the game's sound to another playback device while you play, with no restart, plus an optional volume limiter.
- The Audio Bridge starts with the mod every time and does not need ASIO: with a Real Tone Cable all of this works with no setup.
- With an ASIO interface, set **Rocksmith Audio Bridge ASIO** as the driver in `RS_ASIO.ini` to get these too. The mod never edits `RS_ASIO.ini` for you.

### Drop Pedal and Speaker Mode
- **Drop Pedal** for Real Tone Cable users now works natively: no MultiPitch tone, lower latency, and any tone works.
- In multiplayer the Drop Pedal keeps separate settings for each player. **Speaker Mode** is one global setting that Player 1 controls, and both players tune to Player 1's tuning.
- Keys: `F7` cycles Drop Pedal / Speaker Mode / Off, `,` and `.` shift down and up, `F9` sets your guitar's tuning (Player 2 adds `Ctrl`). Readout colours are configurable.

### External amp (new)
Play through your own amp sim, such as AmpliTube 5, instead of Rocksmith's amp, while Rocksmith keeps your clean signal for note detection. In the amp sim pick **Rocksmith Audio Bridge ASIO**, with **Rocksmith guitar** as the input and **Rocksmith out L / R** as the outputs. The Drop Pedal shift reaches the amp sim too. External amp needs an ASIO interface with the Audio Bridge driver set in `RS_ASIO.ini`; Real Tone Cable support for External amp is planned. Everything else in this release works with the Real Tone Cable.

### Recording (new)
Press `F6` or the overlay's record button. An audio take saves the full game mix and your clean guitar as a pair; a video take saves an MP4 with the game sound. Recordings capture exactly what you hear, amp sim included, and each game launch gets its own dated folder in `Videos\Rocksmith Audio Bridge`.

### Guitar input cleanup (new)
Make-up gain, a noise suppressor that keeps your sustain, a compressor, a hum filter that removes only the mains hum, and an override for Rocksmith's own noise gate. Works with a cable or an ASIO interface.

### Mixer (new)
Seven faders: Master, Player 1, Player 2, Song, Microphone, Voice-over and Effects. Player levels change only what you hear, never note detection.

### Note by Note practice mode (beta)
A Riff Repeater mode that checks every note you play. Turn it on with the **NOTE BY NOTE** switch in Riff Repeater Advanced Settings.
- **Flow mode** (on by default, **FLOW MODE** switch just below): the song keeps playing while you hit the notes and stops at the first note you miss. Turn it off to stop at every note.
- Detection combines the game's own matcher, a pitch verifier and a bundled machine-learning detector that starts on its own; no Python or downloads.
- Chords need a fresh strum, and sliding into a note doesn't count; hammer-ons, pull-offs and taps pass without a pick.
- A bend meter shows how far to bend.
- `Right Arrow` is an emergency skip, in case you ever get stuck on a note.
- Bass arrangements are supported (experimental).
- Readout, target style and layout are set on the overlay's Note by Note page.

## Fixes
- More ASIO interfaces work with the Drop Pedal: the input hook now keeps retrying until RS_ASIO's capture is ready, instead of trying once at start-up.
- Speaker Mode works with custom DLC built on newer Wwise versions (DLC Builder, Wwise 2022/2023).
- Speaker Mode keeps working through long sessions; before, it could stop until the game was restarted.
- Custom string colours match note colours.
- The in-game tuner holds one target pitch with a Drop Pedal or Speaker Mode shift active, instead of flickering between two tunings.
- The settings window no longer offers an "Update RSMods" button that replaced the mod with the original RSMods.

## Known issues
- Speaker Mode: on a heavily loaded PC, a short high-pitched tone can play while a song loads. Starting at a moderate volume is a good habit.
- Two-player Drop Pedal works when both players use the same kind of input, both ASIO or both Real Tone Cable.
- With auto-tuning on, the Drop Pedal / Speaker Mode shift is sometimes applied when the song loads rather than while you browse the song list.
- Note by Note is in beta. Some visuals are still being polished: note markers during holds, bends and loop restarts, and the section colour on the Riff Repeater timeline.

Report problems on [this repository](https://github.com/Cheesewizard/RocksmithAudioBridge/issues), not to the RSMods authors. Attach `RSMods_debug.txt` from the Rocksmith folder and the logs in `%LOCALAPPDATA%\Rocksmith Audio Bridge\Logs`.

## Checksum
`RocksmithAudioBridge-Installer.exe` SHA-256: `cfb024672e58534fc1c5ba215dd8060e17355c4b158653f9ceaf18f142c51cf4`

Rocksmith Audio Bridge is built on [RSMods](https://github.com/Lovrom8/RSMods) by Lovrom8 and ffio1. If it has helped you, you can [buy me a beer](https://buymeacoffee.com/cheesewizard). Thank you!



