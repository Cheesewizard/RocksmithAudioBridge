# Built-in Tone designer: handover

26 October 2026. Mod branch `dev_portable-tone-designer` at `c09b3ba`; tones repository
`Cheesewizard/RocksmithAudioBridge-Tones` at pre-release `v1.0.0-beta.1`.

This is the single place to start. The design ([PortableTones-Design.md](PortableTones-Design.md)), the plan
([PortableTones-MoSCoW.md](PortableTones-MoSCoW.md)) and the implementation status
([VstHost/Documentation/Implementation-status.md](../../VstHost/Documentation/Implementation-status.md)) go deeper.

## 1. The goal and where it stands

**Goal for this timebox:** NAM amps that sound good, are configurable, can be saved, and can be exported and
embedded with a CDLC so another player with only the mod hears the same tone.

| Part | State |
|---|---|
| Engine (Stompbox in the VST host) | Built and running in the game |
| Tone designer page (board, browser, editor, saved tones) | Running in the game; later fixes not yet built (section 9) |
| Starter content (58 captures, 10 cabinets, 26 tones) | Released (`v1.0.0-beta.1`), installed by the installer, seen working in the game |
| Installer ships the engine, its licences and the starter tones | Built once; the starter tones installed |
| Input calibration | Written, not built |
| Song tones (switch with the chart) | Written, not verified live |
| Export / import a song's tones with a CDLC (`.rabtones`) | **Not started**: the main remaining Must |

## 2. The feature model (agreed)

The **Guitar Tone** page (sidebar and page title, renamed from "Tone designer") has one choice: where the sound
comes from.

| Mode | What it is | Shareable |
|---|---|---|
| Game amp | Rocksmith's own amps and tones | They are the game's |
| **Tone designer** | NAM captures, cabinet IRs, Stompbox effects | Yes: save, export, embed in CDLC |
| VST rig | The player's VST3 plugins | No, this PC only |
| External app | An amp sim app on Rocksmith Audio Bridge ASIO | No |

Everything else is a **source of Tone designer tones**, not a mode: the starter tones, tones the player saves,
tones a CDLC carries (they play unless the player overrides them), and song tone assignments.

Today Game amp is reached through the power button and the A/B switch; making it the first tab is agreed but not
done. Converting Rocksmith's own song tones onto the Tone designer was discussed and moved to Won't (section 11).

## 3. Repositories and branches

| Where | What |
|---|---|
| `Cheesewizard/RocksmithAudioBridge`, branch `dev_portable-tone-designer` | All mod work. Develop and push here. |
| The owner's worktree `C:\Users\Philip\.codex\worktrees\portable-tone-designer\RSModsPlus` | Where that branch is checked out locally. Build from here. |
| `C:\Programming\RSModsPlus`, branch `feature/4.3-features` | The owner's 4.3 work, with uncommitted changes. **Do not pull the Tone designer branch into it**; merging the two is a separate, deliberate step. |
| `Cheesewizard/RocksmithAudioBridge-Tones` (private), clone at `C:\Programming\RocksmithAudioBridge-Tones` | Captures, IRs, starter tones, their licences and tools. |
| Remote branch `feature/4.3-features` on the mod repo | Pushed by mistake earlier; the owner should delete it. |

Notes for an agent working in a cloud session: the Claude GitHub App is not installed on the tones repository, so
pushes to it fail; the owner pushes (or installs the app). `/docs` is git-ignored in the mod repo: force-add
(`git add -f docs/ToneDesigner/...`). Commits end with the Co-Authored-By and Claude-Session trailers; no model names
in commits.

## 4. Architecture

```
Rocksmith (32-bit)                          RocksmithAudioBridgeVstHost.exe (64-bit, JUCE)
  overlay DLL                                  StompboxService ── StompboxEngine ── stompbox-capi.dll
    StompboxPage.cpp  (the page)                  │  (one per tone: the board's, and one per prepared song tone)
    StompboxLink.cpp  (commands, snapshot) ◄─────►│  SceneSet (song tones: switch, 10 ms dip, 2 s tails)
    ToneCatalog.cpp   (starter tones' manifest)   │  SourceRouter (routes the guitar, safety limiter, block timing)
                        shared memory: StompboxControl v4 (DLL/Shared/StompboxControl.hpp)
```

- **Commands:** the page sends Stompbox's own text commands one at a time (`SetChain Main NoiseGate NAM Cabinet`,
  `SetParam NAM Model "5150 Block Letter"`, `LoadPreset "Grunge"`), plus the page's own `SongTone` command. The host
  polls every 15 ms, runs one, and republishes the program (`DumpProgram`), the presets and errors.
- **Shared block** `Local\RocksmithAudioBridge.Stompbox.v4`: wanted flag, command, catalogue (`DumpConfig`),
  program, presets, load figures, song and tone keys, song assignments, playing tone, and (v4) the input trim.
  **The host and the DLL must be built from the same commit**: a version mismatch means they do not connect.
- **Data folder** `%APPDATA%\RocksmithAudioBridge\Stompbox\`: `NAM\` (models), `Cabinets\` (IRs), `Presets\` (saved
  tones), `SongTones\<song>.txt` (assignments), `StarterTones\` (manifest, attribution, licences). Stompbox reads no
  subfolders and lists files **without their extension**.
- **Settings** in `HKCU\Software\RSMods\VstHost`: `ToneDesignerEngine` (the Tone designer plays),
  `ToneDesignerInputTrim` (hundredths of a dB), `StompboxDev` (development: Stompbox's own remote server).

### Key files

| File | Role |
|---|---|
| `DLL/Overlay/StompboxPage.cpp` | The Tone designer page |
| `DLL/Overlay/ToneDesignerUi.hpp` | Drawing parts shared with the VST rig page (panel, rack, pedal tiles, meters, pills, chips) |
| `DLL/Overlay/OverlayShell.cpp` | Shell, tabs, the VST rig page; sets the page to fill the window from `DrawStompboxPage()`'s natural height |
| `DLL/ToneDesigner/StompboxLink.{hpp,cpp}` | Overlay side of the shared block: snapshot, command queue, input trim |
| `DLL/ToneDesigner/ToneCatalog.{hpp,cpp}` | Reads the installed starter tones' `manifest.json` (sound, instrument, description per file) |
| `DLL/ToneDesigner/GameTones.cpp`, `SongLibrary.cpp` | The game's current tone key (`song+0x198`); each song's tones from the song index |
| `DLL/Shared/StompboxControl.hpp` | The shared block layout |
| `VstHost/Source/StompboxService.{hpp,cpp}` | Host side: runs commands, publishes, song tones, input trim |
| `VstHost/Source/StompboxEngine.{hpp,cpp}` | Wrapper around `stompbox-capi.dll`; applies the input trim |
| `VstHost/Source/SceneSet.{hpp,cpp}` | Prepared song tones and switching |
| `VstHost/Stompbox/Build-Stompbox.ps1`, `UPSTREAM.md` | Builds the engine DLL |
| `VstHost/Stompbox/LICENSE.txt`, `THIRD-PARTY-NOTICES.txt` | Shipped with the DLL |
| `Installer/StarterTones.{cs,json}`, `AcquireStarterTones.ps1` | The starter tones in the installer |

## 5. The engine

**Stompbox** (github.com/mikeoliphant/stompbox, GPL-3.0) at commit `17bc65264ff89a6b8a19843264292ca7762fdad3`, built
as `stompbox-capi.dll` by `VstHost\Stompbox\Build-Stompbox.ps1` with four checked patches (the script stops if
upstream changed a patched line):

1. its remote server listens on 127.0.0.1 only;
2. the `Tonestack` pedal is registered (upstream defines it but leaves it out);
3. `Tonestack` gets a description;
4. `DumpConfig` is exported from the C API (the pedal catalogue the page draws from).

Build: `powershell -ExecutionPolicy Bypass -File VstHost/Stompbox/Build-Stompbox.ps1` (forward slashes in Git Bash).
It finds Visual Studio's own CMake through vswhere; it needs Visual Studio 2022 with "C++ CMake tools for Windows",
or CMake on the PATH. The host project copies the DLL next to the host.

**Behaviour worth knowing:**

- Stompbox loads a preset on a thread of its own and only while `Process` runs (a fade out, the load, a fade in).
  Engines prepared ahead of play are pumped on silence (`StompboxEngine::Pump`).
- Choosing a model loads it synchronously on the calling thread. Measured: 7 to 11 ms per capture.
- The Cabinet pedal uses the first 2048 samples of an IR, drops a tail quieter than -66 dB, and applies a fixed
  -18 dB. IRs must be peak-normalised.
- Its default effect settings suit a demo, not a guitar: ConvoReverb starts with no dry signal, Reverb 50% wet,
  Delay at 50% mix and feedback. The page overrides them for pedals added from the browser (section 6).

## 6. The Tone designer page

Laid out like the VST rig page, with the same drawing parts: a bar (File; the tone between arrows; Save; **Input**;
the load pill), the saved tones on the left, the board (IN and OUT meters, pedals, Add tile), a draggable divider,
and a pane with **Pedals | Song tones**.

- **Adding pedals:** drag from the browser onto the board (between pedals to insert, onto a pedal to replace,
  anywhere else for its usual place), or right-click a row. Double-click does **not** add, because a few
  double-clicks stacked amps at the end of the board. Double-click still loads a saved tone and opens a pedal's
  editor.
- **Usual places:** a gate first; a pedal capture (drive, boost) in front of the first amp; an amp before the first
  cabinet; a cabinet after the last amp; effects at the end.
- **An amp with no cabinet** gets one after it ("4x12 V30 SM57", or "Synthetic 8x10 Bass" for a bass capture), with
  a notice. An amp alone is its line out: thin and fizzy.
- **Starting settings:** effects added from the browser start at moderate values (`STARTING_SETTINGS` in
  `StompboxPage.cpp`). Presets keep their own.
- **Browser:** Effects, Amps, Cabinets. Amps filter by All, Clean, Crunch, High gain, Pedals and Bass; Cabinets by
  All, Guitar and Bass. Each row has a sound pill and the capture's description from `ToneCatalog`; the player's
  own files show last as "Your file". Search matches descriptions too.
- **Editor:** a floating window per pedal, built from Stompbox's parameter definitions, put in the middle of the
  panel each time it opens (`shouldCenterEditor`), not saved in imgui.ini.
- **Input calibration:** the bar's Input button opens the level panel: the guitar as it arrives and as the amps
  get it, against a -12 dBFS target. Measure listens to the player's hardest playing for 5 s and sets the trim;
  a slider sets it by hand (-12 to +30 dB). The host applies it before every chain.
- **Load meter:** the typical block's share of real time (judged against the base game's 480-frame blocks) and
  this PC's slowest block; late blocks from the amp link.

## 7. Song tones

While a song is chosen, the Song tones pane lists every tone of every arrangement (from the song index, which
RSMods.exe builds), each with a saved-tone picker. Assignments are stored per song. While the Tone designer plays,
the host loads each assigned tone into an engine of its own, pumps it until ready, and switches between them when the
game's tone key changes: a 10 ms fade out and in, so two amps never run at once, with the old tone's tail ringing
for 2 s with its NAM pedals bypassed.

The tone key read (`song+0x198`) was found statically. **None of this has been verified live** (MoSCoW M3).

## 8. Starter content: RocksmithAudioBridge-Tones

| Folder | Contents | Licence |
|---|---|---|
| `NAM/` | 45 amp and 13 pedal captures: 55 from the NAM community collection (pelennor2170/NAM_models, byte for byte, renamed), 3 NAM project example models (Steven Atkinson, repacked into single files) | GPL-3.0-only / MIT |
| `Cabinets/` | 6 measured 4x12s from Jester Dyne Productions' Brutal and Emerald packs; 4 synthetic cabinets made here (1x12 open back, 2x12, 4x10 bass, 8x10 bass) | CC0-1.0 |
| `Presets/` | 26 genre tones, written for this pack, levels set by measurement, **not tuned by ear** | CC0-1.0 |

- `manifest.json` is the contract: each file's kind, instrument, sound, description, author, licence, source and
  SHA-256. `ATTRIBUTION.md` is generated from it.
- **Names are permanent.** Tones and CDLC refer to files by name, so a released file is never renamed, changed or
  removed; a better version gets a new name.
- **Accepted licences:** CC0-1.0, CC-BY-4.0, MIT, GPL-3.0. No non-commercial or no-derivatives licences. No
  captures of plugins, modellers or profilers, and nobody else's files. Contributors sign off (DCO);
  `CONTRIBUTING.md`, the pull request template and the "Capture submission" issue form explain it.
- **Left out on purpose:** NeuralAudio's `BossWN-feather` (CC BY-NC-ND, though DotCharter lists it as MIT); three
  captures of commercial modellers (two Axe-Fx, one Line 6). TONE3000 captures cannot be redistributed.
- **Jester IRs are not in git:** `tools/fetch_jester_irs.py` downloads them, checks every zip, the CC0 handbook and
  each WAV against pinned hashes. CI, calibration and releases run it first, so release zips contain them.

**Tools** (`tools/`): `validate.py` (files, licences, hashes, formats; `--write` regenerates ATTRIBUTION.md);
`check/build_check.py` builds `check_tone`, the Stompbox engine at the mod's pinned commit, offline;
`check_tones.py` plays every tone through it (loads with every setting kept, no NaNs or clipping, level at
-20 dBFS RMS or peaks at -3 dBFS; `--calibrate`, `--wav`); `make_tones.py` writes the presets; `add_file.py` adds a
contribution; `import_nam.py` and `make_synthetic_cabs.py` rebuild the starter files.

**CI** (GitHub Actions): Check on every push; **Calibrate tone levels** by hand after a tone, model or cabinet changes
(commits the result); Release on a `v*` tag; a tag with a suffix (`v1.0.0-beta.2`) is a pre-release. A release zip is
about 8 MB.

**To ship a new tones release:** tag it in the tones repo, then set its tag, asset name and SHA-256 (from the
release's `.sha256` file) in the mod's `Installer/StarterTones.json`.

## 9. Installer

- Writes `stompbox-capi.dll` next to the VST host, and `RSMods\Stompbox-LICENSE.txt` and
  `RSMods\Stompbox-ThirdParty-Notices.txt` (the pinned commit, where the source and patches are, and the licence of
  every library compiled in). A Release build refuses to package without the DLL and checks it is x64.
- Embeds the pinned tones release: `AcquireStarterTones.ps1` downloads it with `gh` at build time (needs the GitHub
  CLI, signed in, while the tones repo is private) and checks its SHA-256 (`59518942...87B92`); a mismatch stops the
  build. `StarterTones.cs` unpacks it into the data folder, **never overwriting a file that exists**, so a player's
  own tones and edited starter tones survive. Each file written is recorded with its hash; uninstall and a
  rolled-back install remove only files still as installed.

## 10. Build, install, check (Windows)

1. Pull `dev_portable-tone-designer` in the worktree.
2. `powershell -ExecutionPolicy Bypass -File VstHost/Stompbox/Build-Stompbox.ps1` (once, or after a patch changes).
3. Build `RSMods.sln` in Release (Visual Studio, or `msbuild RSMods.sln /p:Configuration=Release /m` in a Developer
   PowerShell). Look for `Starter tones v1.0.0-beta.1 -> ...` and the `PASS:` lines.
4. Run the installer. Check `%APPDATA%\RocksmithAudioBridge\Stompbox\`: 58 files in `NAM`, 10 in `Cabinets`, 26 in
   `Presets`, plus `StarterTones\`.
5. In the game: Guitar Tone > Tone designer.

## 11. What has been verified, and what has not

**Seen working in the game** (owner's screenshots, build up to `2d0b899`): the engine loads; the starter captures,
cabinets and tones are listed; starter tones load; the board, editor and load meter work; the page fills the window.

**Written but not built yet** (syntax-checked only, with the real ImGui headers and stand-in Windows headers; no C#
compiler was available): `8f3a564` browser filters and editor centring; `c3c7832` Guitar Tone rename and no
double-click add; `9cb472a` automatic cabinet, pedal capture placement, starting settings; `c09b3ba` input
calibration (host and DLL, shared block v4).

**Never verified:** song tone switching live (M3); the tone key read; behaviour at 128 and 64 frames over a session
(M2); uninstall of the starter tones.

## 12. Open issues

| Issue | What is known | Next step |
|---|---|---|
| Tones sound thin, fizzy or distant | Likely causes: an amp without a cabinet (fixed), heavy effect defaults (fixed), a quiet cable under-driving the captures (input calibration, to test), tones not tuned by ear | Build, run Input > Measure, report the trim; tune starter tones by ear in the tones repo |
| "Slow" amp loading | A model loads in 7 to 11 ms in the engine; the host polls every 15 ms. Not reproduced | Ask which action is slow and how slow; time the path from the page to the host |
| Late buffers (25 to 33 seen at 64 frames, slowest block 34 to 63%) | Unclear whether they rise when a model loads | Watch the count while loading an amp |
| Double pedal on drag | Almost certainly the old double-click-to-add plus a drag; ImGui gives a drop to one target only | Confirm gone after rebuilding |
| Editor opened off-centre | Fixed by placing it explicitly on open | Confirm |
| Tones repo CI build fix | A commit limiting the checker build to one job per core may not have reached `main` | Check `git log origin/main` in the tones clone |
| Remembered last page | The rename means a stored "Tone designer" page opens the default page once | None |

## 13. Next work (agreed order)

1. **Verify** the unbuilt commits and the input calibration in the game.
2. **`.rabtones` export and import** (MoSCoW M6, M7, with S5): one file beside a CDLC holding the song's assigned
   tones (Stompbox preset text) and every model and IR they name, each once, hashed, with each file's credit and
   licence from the manifest. Import validates it, extracts to the data folder, maps its tones to the song, and plays
   offline with no account. Starter files need not be embedded when the player has them (same name and hash).
3. **Game amp as the first tab** of the Guitar Tone page (replacing the power button and A/B as the way back).
4. **Per-model input levels** (rest of S3), then the other Shoulds: rescan the library without a restart (S2),
   Tone designer data in tone backups (S4), rights capture in the UI (S5).
5. Safe failure (M8) and live verification (M2, M3) remain Musts.

**Won't, this time:** converting Rocksmith's own song tones onto the Tone designer (DotCharter, GPL-3, does this; if
ever done, write our own mapping and convert on the player's PC only, never ship converted official tones);
embedding tones inside the PSARC; TONE3000 browsing; preloading every model (measured: nothing to gain).

## 14. Gotchas

- Git Bash eats backslashes: use `VstHost/Stompbox/...` there.
- Pasting long blocks into Windows PowerShell runs the lines after a `throw`; ship scripts as `.ps1` files.
- `gh auth setup-git` makes git use the GitHub CLI login; for a private repo GitHub answers "not found" when git
  has no credential.
- Stompbox file values are names without extension; never strip "extensions" from them ("JCM800 v1.2").
- `StompboxLink` replaces the snapshot's chain at once on `SetChain`: work on copies of pedals while drawing.
- Licences: the mod is MIT, the VST host AGPL-3.0, Stompbox GPL-3.0, the tones repo mixed per file.
