# Rocksmith Audio Bridge 4.2 beta release notes

Two big additions: the **Tone designer**, which plays your guitar through your own VST3 plugins (amp sims, pedals, NAM captures) inside Rocksmith, and **live audio routing**, which lets you pick any interface and channel for each player while the game runs. Real Tone Cable players get both, plus much lower latency. To update, run the installer over your existing install; your `RSMods.ini` settings and key bindings are kept.

## RS_ASIO
ASIO users need [RS_ASIO](https://github.com/mdias/rs_asio/releases) 0.6.1 or newer; 0.7.2 or newer is recommended. The settings app's new Audit tab checks this for you.

## New

### Tone designer (VST3 amp sims and pedals)
Open it from the overlay's **Amp sim** page (`\` in game). Your plugins run in a separate 64-bit program, so modern 64-bit plugins work in 32-bit Rocksmith, and a plugin that crashes does not take the game down with it.
- **Pedalboard:** up to 8 plugins in a chain. Drag plugins from the browser onto the board, drag pedals to reorder or replace, click a footswitch to bypass, double-click to open the plugin's window. Each plugin has its own window and remembers where you put it. Scroll wheel or middle-drag moves along a long chain.
- **Tones:** save the whole chain with every plugin's settings as a tone. Tones can sit in folders, be favourited, and be renamed, moved or deleted several at a time. Previous / next arrows step through them.
- **Song tones:** give a song its own tone and it loads when the song starts. Pick a default tone for songs without one. A **Songs** pane lists your whole library with each song's tone and how long it took to load; drag a tone onto songs to assign it.
- **The game names your tone:** Rocksmith's own TONE SWITCH readout appears when a Tone designer tone switches in, with your tone's name.
- **Fast tone changes:** plugins that leave the chain stay loaded for a while, the plugins a highlighted song needs are loaded in the background before you start it, and any plugin can be set to **Always keep loaded** from its right-click menu. Tones that share plugins switch without reloading them.
- **Reload a plugin:** right-click it in the plugin browser and pick **Reload**, or right-click a pedal and pick **Reload plugin**. Every copy of that plugin is unloaded and loaded again from scratch with its settings kept, for plugins that only see new content, such as tones you imported into them, when they start. The rest of the chain keeps playing meanwhile.
- **Standby:** the power button switches the Tone designer off (the game's amp comes back) but keeps the plugins loaded, so switching it on again is instant.
- **Compare with the game's amp** for quick A/B, input and output trims with meters that stay the same for every tone, and **Start with the game**.
- **Backups:** File > Backups zips your tones, song tones and plugin settings. One is made each day on its own (the newest 7 are kept) and any of them can be restored.
- **Crash recovery:** if a plugin crashes, the Tone designer names it, restarts, and loads your tone and the song's tone again. It restarts up to three times in five minutes; after that a crash leaves it off and it says so.
- **Works with the Real Tone Cable** as well as with an ASIO interface. With the cable, the Amp sim page has an **Amp sim block** setting (Automatic, 64, 128, 256).

The VST host is built on JUCE and licensed AGPLv3; the rest of the mod stays MIT.

### Amp sim page
The External amp and Tone designer pages are now one **Amp sim** page: an Amp link card with the shared settings (latency, late buffers, extra safety buffer, compare), then a **Tone designer** tab and an **External app** tab (AmpliTube and other apps on the bridge's ASIO driver, as before).

### Live audio routing (ASIO)
Pointing `RS_ASIO.ini` at the Audio Bridge used to cost you RS_ASIO's choice of device and channel for each player. The bridge now does that routing itself, and every change applies while you play:
- **Output page, Routing card:** pick the output pair, and for Player 1, Player 2 and the mic the interface, input channel and clock. Each input can come from a different ASIO interface; the bridge keeps it in sync with the output's clock.
- **Switch output** lists every ASIO interface and Windows device, grouped. ASIO4ALL and other wrapper drivers no longer block switching.
- The settings app's Rocksmith Audio Bridge tab has the same **ASIO interface** and **Input interface** pickers, live while the game runs.

### Real Tone Cable: lower latency
- **Smaller game engine blocks:** the game's own amps now run at a small block size in Cable mode instead of 10 ms blocks. Pick it on the Output page's **Game engine** card: Automatic (follows your output), 64, 128, 256 or 512. **Apply now** switches without a restart; otherwise it applies at the next game start.
- **Exclusive mode for lower latency:** turn on **Exclusive mode** on the Output page and the game plays straight to your Windows device at its smallest buffer, skipping the Windows mixer, which is most of the output delay in Cable mode. It is off by default: not every device supports it (one that refuses stays shared and the switch turns itself off), and on a few it adds latency, so compare the buffer the page shows.
- **Play through an ASIO interface:** in Cable mode the Output page can also play the game through an ASIO interface's own driver while the guitar stays on the cable.
- **Other apps still play:** with Exclusive mode or an ASIO output, other programs cannot open that device while the game runs, so **Hear other apps here** (on by default) mixes YouTube, Discord and the rest into it for you.
- **Direct USB read:** the bridge reads the cable straight over USB every millisecond instead of in Windows' 10 ms packets, so your guitar reaches the Tone designer about 10 ms sooner. It uses UsbDk (Apache 2.0, by Red Hat and Daynix), which the bridge driver install adds to Windows. **Restart your PC after installing:** UsbDk only starts after a restart, and the installer asks you to. Until then the cable works through Windows as before, with its usual delay. It is tried first; when UsbDk is missing or does not see the cable, the bridge uses the Windows cable input as before. Outside the game the cable keeps its normal Windows driver. Uninstall asks whether to remove UsbDk too (Yes by default when the installer put it there). If UsbDk cannot be installed, the install still finishes with a warning, and only the direct USB read is unavailable.
- **Guitar to ear readout:** the overlay header estimates the delay from the cable to your ears: for the Tone designer from its audio timing, and for the game's own tones from your picks (the rougher of the two). Treat both as a guide, not a calibrated measurement. General > Display > **Latency readout** shows a compact one on the game screen (it needs the Audio diagnostics HUD).
- **Device volume** on the Output page, and a warning when Windows has the device muted or at 0.

### Settings app
- **Audit tab:** checks the basics for your input mode. Both modes: Rocksmith's own audio settings against the recommended values, with **Use recommended**. ASIO: RS_ASIO installed and set up, the bridge's interface, and why it failed last launch. Cable: the cable is visible to Windows, and microphone privacy allows the game.
- **Set up for me** (on the Audit tab, ASIO): points `RS_ASIO.ini` at the Audio Bridge, carries your interfaces and channels over, and backs up your original first. **Restore** puts your original back, and uninstalling restores it too.
- **Make bug report** on the Rocksmith Audio Bridge tab: zips the logs and settings that explain a bad launch, removes your Windows user name from them, and opens an email to the developer.

### Safer audio setup
- **Never boots twice into a setup that crashed the game.** Some ASIO drivers crash the whole game (Realtek ASIO used as an input is one). The bridge remembers the last setup that worked; after a crash it goes back to it, or opens no interface until you pick one, and the Output page says what happened.
- **Says why the guitar is silent:** when your interface will not open (most often because another app holds it: its control app, a DAW, OBS), the Output page shows "Not open" with the reason and what to close, and the settings app shows it after the game closes.
- **Silence guitar while tuning** (General page, off by default) mutes the game's amp, the Tone designer and external amp apps in the tuner screens.
- **One log:** the bridge driver now writes into `RSMods_debug.txt`, so one log holds the whole audio story.

## Fixes
- **No silence after changing an interface's setup.** Switching a device on or off in ASIO4ALL, or changing the buffer size in an interface's control panel, left the game playing into a stale channel list. The bridge now reopens the interface by itself.
- **Real Tone Cable: less delay after every note.** Windows' default 10 ms playback period was most of it; the bridge now asks for about 3 ms. A cold start could also queue a backlog that made every note 15 to 20 ms late for about two minutes; it is now dropped at once.
- **The overlay no longer opens and shuts in one press**, or reopens right after closing.
- **The settings app no longer reverts changes made in the in-game overlay** (Note by Note, HUD layout, Drop Pedal and audio settings) when you change something with it open.
- **The settings app finds the game folder by itself again** instead of sometimes asking where Rocksmith is.
- **The RS_ASIO Settings tab shows again.** 4.0 and 4.1 hid it by mistake. It appears whenever `RS_ASIO.ini` exists, now with a small Audit card that shows the result and opens the audit.
- Slider hints open above the slider, not over the value you are setting.

## Known issues
- Rocksmith can crash on PCs where Windows forces **Mandatory ASLR** on every program (it moves the game in memory, so no mod can patch it safely). Turn it off for `Rocksmith2014.exe` in Windows Security > App & browser control > Exploit protection > Program settings.
- Speaker Mode: on a heavily loaded PC, a short high-pitched tone can play while a song loads. Starting at a moderate volume is a good habit.
- Two-player Drop Pedal works when both players use the same kind of input, both ASIO or both Real Tone Cable.
- The Tone designer plays one guitar (Player 1). For two-player songs, use the game's amp.
- With auto-tuning on, the Drop Pedal / Speaker Mode shift is sometimes applied when the song loads rather than while you browse the song list.
- Note by Note is in beta. Some visuals are still being polished: note markers during holds, bends and loop restarts, and the section colour on the Riff Repeater timeline.

## Troubleshooting
- **Real Tone Cable feels as slow as before, or the game shows "Restart Windows to finish installing the Real Tone Cable's low-latency driver (UsbDk)".** The direct USB read needs UsbDk, which only starts after Windows restarts. Restart your PC and start the game again. Until then the cable still works, through Windows, with about 10 ms more delay. `RSMods_debug.txt` says which case you are in: a line starting `(PERSISTENT INPUT) Direct USB is not active` gives the reason (restart needed, or UsbDk not installed: run the installer again, then restart), and `Real Tone Cable opened directly over USB` means it is working.

## Coming next
The next update brings stability fixes from this beta's reports and support for Rocksmith's **Learn to Play** update.

Report problems with the **Make bug report** button on the settings app's Rocksmith Audio Bridge tab, or on [this repository](https://github.com/Cheesewizard/RocksmithAudioBridge/issues).

---

# Rocksmith Audio Bridge 4.1 release notes

A bug-fix release on top of 4.0. To update, run the installer over your existing install; your `RSMods.ini` settings and key bindings are kept.

## RS_ASIO
ASIO users need [RS_ASIO](https://github.com/mdias/rs_asio/releases) 0.6.1 or newer; the latest release is recommended. Your version is on the first line of `RS_ASIO-log.txt`.

## Improved
- Video recordings are now 60 fps and sharper: the GPU converts and encodes each frame directly, and the bitrate follows the window size. Frames land on a steady clock, so the scrolling highway no longer judders. PCs where the GPU path can't be set up fall back to the previous CPU path at the same frame rate.
- Video quality setting on the overlay's Record page: **High** (the default, about 250 MB a minute at 1440p), **Standard** (about half that) or **Small** (1080p, about 80 MB a minute, for sharing). It is remembered between launches. Bitrates are capped, so takes play smoothly in Windows Media Player. On PCs that use the CPU recording path, Small keeps the full resolution and only lowers the bitrate.
- Recordings are now saved in a `Recordings` folder inside the Rocksmith folder by default, next to the game (4.0 used `Videos\Rocksmith Audio Bridge`). A folder you picked yourself is kept. Uninstalling never touches it.

## Fixes
- **No more crash right after the profile screen with RS_ASIO 0.6.1 to 0.7.1.** Those RS_ASIO versions and the inherited "two Real Tone Cables" message bypass patch the same game code. With the bypass off, it mistook RS_ASIO's patch for its own and wrote over it, leaving a broken instruction the game ran into at the profile screen. The bypass now only ever undoes its own patch.
- **A rare crash on the main menu or while the game changes screens is fixed.** The top-right audio readout asked the game every frame whether multiplayer was on, through a chain of game pointers that is briefly invalid during a screen change. That read, and the other game reads that run every frame, now check every step and skip the read instead of crashing.
- **Windows no longer logs a Rocksmith error about 40 seconds into every launch.** An inherited hook on the Steam DLC scan left the game's error-handling chain broken, so the game's own internal check tripped over it on every boot. The game kept running, but Windows wrote an error report each time; on PCs set to save full crash dumps, the freeze while writing it could stop the guitar input or the game sound for the rest of the session. Updating the song list from the overlay still works.
- **No more total silence when RS_ASIO is loaded but its settings are not read.** If `RS_ASIO.ini` is misnamed (for example `RS_ASIO.ini.txt`), saved as UTF-16, or missing its `[Config]` line, RS_ASIO runs with everything switched off and the mod fell back to Cable mode, which cannot install over RS_ASIO and then blocked the game's own sound. The game sound is now left alone in that case, and `RSMods_debug.txt` says exactly what is wrong with `RS_ASIO.ini`.
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



