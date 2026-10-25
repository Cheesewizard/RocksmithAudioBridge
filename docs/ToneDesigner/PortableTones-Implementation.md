# Portable tones: implementation guide

Audience: the developer implementing the design in `PortableTones-Design.md`. Read that first, especially the
**Decisions** list and §7. This guide says **what code to add, where, and in what order**, and how each part
connects to what already exists. It also lists the traps found while reviewing the code.

Nothing here has been compiled. Upstream API names were read at the commits in the design's Sources list.
Where a signature must be checked against the pinned source, the guide says **(check)**.

**Revision 3.** The Tone designer uses **bundled, pinned copies of the NAM and Airwindows Consolidated VST3
plugins**, with their own windows. There are no custom processors or UI. See §2.

---

## 0. Before writing code

### 0.1 Read these files first

| File | Why |
|---|---|
| `VstHost/Source/Chain.{hpp,cpp}` | Slots, spares, `Pause()`/`Resume()`, `RABTONE2` save/load. Built-in blocks plug in here. |
| `VstHost/Source/PluginHost.{hpp,cpp}` | Wraps one `juce::AudioPluginInstance`: buses, prepare, process, state, editor. Built-ins reuse it. |
| `VstHost/Source/BridgeAudio.{hpp,cpp}` | The audio callback: int32 to float, one `AudioSink`, the hard clamp. |
| `VstHost/Source/Main.cpp` (`RunCommand`, ~line 785) | How overlay commands are handled on the main thread. |
| `DLL/Shared/VstHostControl.hpp` | The shared-memory contract between the overlay (x86) and the host (x64). |
| `DLL/ToneDesigner/VstHostLink.{hpp,cpp}` | Overlay side: command queue (`g_commands`), snapshot, `FollowSong`. |
| `DLL/ToneDesigner/ToneSwitchPopup.cpp` | The existing `GameComponentTones::Update` detour and the song-object checks. Phase 0a starts here. |
| `DLL/Overlay/OverlayShell.cpp` (namespace `ToneDesigner`, ~line 783; `AmpTabs`, ~line 3214) | The page, the pedalboard drawing and the tab strip. |
| `GUI/Features/ToneDesigner/ToneDesignerTools.cs` | The hidden `RSMods.exe --song-index` / backup helpers that read PSARCs. |
| `DLL/AsioProxy/AmpLink.hpp` | Why the host has a hard one-block deadline. |

### 0.2 Code rules for this work

**New files** follow the project owner's C# and C++ style rules:

- Tabs for indentation, and braces on their own lines.
- A namespace that matches the folder (`VstHost::Song` for `VstHost/Source/Song/`).
- PascalCase types and methods, camelCase fields, UPPER_SNAKE_CASE constants.
- No nested classes; avoid LINQ on hot paths.
- Exceptions or logs for anything unexpected. No silent fallbacks.

**Edits to existing files** keep that file's conventions (`m_` members, its comment style), so a diff reads like
the surrounding code.

**Real-time rules.** These apply to every `Process`/`processBlock`/`Render` path:

- No allocation, no locks that can block, no file or registry I/O, no logging. A log call inside the audio
  callback is a bug.
- Swap anything the audio thread reads with an atomic pointer. Destroy the old object **on the main thread**,
  after the audio thread has moved on (see the retire pattern in §2.3).
- Loading a plugin, setting its state and `Prepare` run on the main thread, as `Chain` does today. Never on the
  audio thread.

### 0.3 Milestones (each one a reviewable PR)

| # | Milestone | Depends on | Done when |
|---|---|---|---|
| M0a | Game tone-slot and arrangement spike (DLL only, logging) | — | §1 log criteria met |
| M0b | Bundled NAM and Airwindows Consolidated VST3 builds load in the host; timing log | — | Both play and open their windows; 99.9th percentile block time logged |
| M1 | `bundled:` ids, the Tone designer chain and source router | M0b | A Tone designer tone saves, loads and plays on another machine path |
| M2 | NAM and Airwindows state parsers and writer (C#), with round-trip tests | M1 | Paths and parameters read back byte-exact |
| M3 | Overlay tab split (same page, parameterised) | M1 | The Tone designer tab lists only bundled plugins |
| M4 | Song tone sets: local editing; export and import of the `.rabtones` sidecar | M2, M3 | Offline-disappearance test passes |
| M5 | `SceneSet` with mid-song switching driven by the game's slot | M0a, M4 | Switches on chart events, seeks, Riff Repeater and 1–4 |
| M6 | Fixture renders and acceptance | all | Design §12 checklist |

M0a and M0b are independent and can run in parallel.

---

## 1. M0a: read the game's tone slot and arrangement (DLL)

The whole switching design depends on this, so do it first and keep it to logging only.

### 1.1 Move the tone-component capture into its own unit

`ToneSwitchPopup.cpp` currently owns the `GameComponentTones::Update` detour and `PlayingSongObject()`. Split
them out so two features can use them:

```
DLL/ToneDesigner/GameTones.hpp
DLL/ToneDesigner/GameTones.cpp
```

```cpp
#pragma once

#include <cstdint>
#include <string>

// The game's tone state during a song: which of the arrangement's tones is active (base, A-D) and which
// arrangement is playing. Read from GameComponentTones, captured by a detour on its per-frame Update
// (moved here from ToneSwitchPopup). Render/game thread reads only; every read is guarded.
namespace GameTones
{
	enum class ToneSlot : int8_t { Unknown = -1, Base = 0, A = 1, B = 2, C = 3, D = 4 };

	void Initialize();                       // installs the Update detour (was in ToneSwitchPopup::Initialize)
	uintptr_t PlayingSongObject();           // 0 when no song plays; same checks as today
	ToneSlot CurrentSlot();                  // M0a: find the field; Unknown until found
	bool TryGetArrangementId(std::string& persistentId);   // 32 upper-case hex; M0a: find it
}
```

`ToneSwitchPopup` then calls `GameTones::PlayingSongObject()` instead of keeping its own copy. That is a pure
move with no behaviour change: land it as its own commit.

### 1.2 Find the current-slot field

1. Log `g_tonesComponent` and the song object once per song.
2. Inspect the component in a memory viewer (x32dbg or Cheat Engine attached to `Rocksmith2014.exe`).
   - Press 1–4 in a song. Find the dword or byte that cycles through four or five values.
   - Confirm it also changes when the chart's own tone change fires: use a CDLC with known tone changes and
     compare against the SNG times in Rocksmith2014.NET's tone list.
3. A second way in: `ShowNotification` type 7 is raised for each change. Break on it and walk up the call stack
   to the code that wrote the new tone index. That code is the source of truth.
4. Watch out: the stored value may be a **tone key string pointer** rather than an index. If so, map it to
   Base/A–D by comparing it with the arrangement's `Tone_Base`/`Tone_A..D` keys (§1.3).
5. Add the offset as a named constant with a byte-signature check, like the existing `TONES_UPDATE_*` constants.
   Log once if the signature does not match.

### 1.3 Find the arrangement identity

1. Search memory for the playing arrangement's `PersistentID`, taken from its manifest JSON (32 hex characters,
   ASCII or UTF-16) while the song plays.
2. Pointer-scan back to the song object or the tone component. Prefer a path that starts from objects you
   already validate by vtable.
3. **If no stable path exists:** read the arrangement's tone **key strings** instead. Match them against the
   index from §5.1, which lists each arrangement's `Tone_Base..D` keys. That is enough to pick the arrangement,
   because arrangements with identical keys get identical tones.
4. **If that also fails:** use the design's player-pick fallback (§5.1, option 3). Do not guess silently.

### 1.4 Done when

The log shows `slot -> X at songTime T` exactly at chart tone events:

- for lead, rhythm and bass arrangements;
- after a seek, and inside a Riff Repeater loop;
- at 50% speed;
- for the 1–4 keys.

The arrangement id must also be logged once per song.

---

## 2. M0b and M1: bundled plugins (host)

**Revision 3.** The Tone designer does **not** build its own processors or UI. The mod ships two pinned
open-source VST3 plugins. The host loads them like any other VST3, double-click opens their own windows, and
several can be open at once, exactly as today.

| Bundled plugin | What it gives | Its own UI | Licence of the binary |
|---|---|---|---|
| **Neural Amp Modeler** (NeuralAmpModelerPlugin, iPlug2) | NAM model loader, noise gate, Bass/Mid/Treble tone stack, input/output, IR (cab) loader, input calibration, A2 quality (`kSlim`), resampling for non-48 kHz models | Yes, the official NAM UI | MIT (iPlug2: zlib-style) |
| **Airwindows Consolidated** (airwin2rack `src-juce`) | Any of the 528 Airwindows effects per instance: gate, compressor, drive, EQ, chorus, delay, reverb, simple amps | Yes, its JUCE editor with an effect picker and knobs | Code MIT; the built binary is GPL3 because of JUCE. Ship the source link and notice. |

Source: NeuralAmpModelerPlugin `16be8697` (`NeuralAmpModeler.h` `EParams`, `SerializeState`); airwin2rack
`dcf98a3e` (`src-juce/AWConsolidatedProcessor.cpp` `getStateInformation`).

### 2.1 Ship and pin them

- **Build from the pinned commits.** Record each build's version, commit and SHA-256 in
  `Installer/BundledPlugins/UPSTREAM.md`. Never pick up whatever NAM or Airwindows the player has installed.
- **Install location.** Install them into the mod's own folder, `<game>\RocksmithAudioBridge\Plugins\`, not
  `Common Files\VST3`. The player's own copies of NAM or Airwindows are then unaffected, and the Tone designer
  always uses the exact bundled build. The player's own copies still appear in the VST rig tab as ordinary
  plugins.
- **Bundled ids.** In tone files and slot paths, a bundled plugin is `bundled:nam` or `bundled:airwindows`, not
  an absolute path. `PluginHost::Load` resolves the id to the bundled file, so a tone opens on any machine
  whatever the install folder.
- **Version check.** The tone and the bank record the bundled plugin version (§5.5). An older mod refuses a bank
  that needs a newer bundled plugin.

### 2.2 Host changes (small)

`PluginHost.cpp`:

```cpp
// At the top of Load (in the file's own style): bundled ids resolve to the mod's own plugin files.
const std::wstring file = BundledPlugins::Resolve(path);   // a path passes through unchanged
```

New unit `VstHost/Source/BundledPlugins.hpp/.cpp`, namespace `VstHost`:

```cpp
namespace VstHost
{
	enum class BundledPlugin { None, Nam, Airwindows };

	// The VST3 plugins the mod ships for the Tone designer (Installer/BundledPlugins), by id: "bundled:nam",
	// "bundled:airwindows". Their exact builds are pinned, so a tone made with them plays the same everywhere.
	namespace BundledPlugins
	{
		BundledPlugin Identify(const std::wstring& idOrPath);
		std::wstring Resolve(const std::wstring& idOrPath);   // the file for an id; anything else unchanged
		std::wstring IdOf(BundledPlugin plugin);
		const char* VersionOf(BundledPlugin plugin);           // the pinned build's version string
	}
}
```

Other changes:

- **`Chain`.** `LoadTone`, `TonePlugins` and `PreloadStep` call `GetFileAttributesW` on slot paths. Resolve the
  bundled id first.
- **Plugin scan.** The scan (`VstHostLink::Plugins`) adds the bundled folder, and marks those entries as
  bundled for the Tone designer tab's browser.
- **No new UI.** No new processor classes and no new editor code. `OpenEditor` opens their own UIs.

### 2.3 Tone designer tones are ordinary chains with one rule

A **Tone designer tone** is a normal `Chain` whose slots are all `bundled:*`. It is saved as `RABTONE2`, the
existing format, in `%APPDATA%\RocksmithAudioBridge\VstHost\Tones\Tone designer\`.

- **Why the folder.** A separate folder keeps the two libraries apart. The format stays the same, so
  save/load, spares, duplicate, backups and song assignment all carry over.
- **The add rule.** The Tone designer tab only offers bundled plugins in its browser. The host refuses
  `AddPlugin`/`ReplaceSlot` of a non-bundled plugin when the active chain is the Tone designer one, with an
  explicit `loadError` message.
- **Two chains.** Keep two `Chain` instances in `Main.cpp`: `g_chain` for the VST rig and `g_designerChain` for
  the Tone designer. A `SourceRouter` (an `AudioSink` holding an atomic pointer to the active sink) decides which
  one plays. Source switches are user actions, so a hard cut is fine.

**The retire pattern.** Use it wherever the audio thread reads a pointer the main thread replaces (the router's
sink, a `SceneSet`):

```cpp
// main thread
auto* old = live.exchange(fresh.release(), std::memory_order_acq_rel);
retired.push_back({ std::unique_ptr<T>(old), blockCounter.load() + 2 });   // freed once two blocks have run
// main-thread timer: erase entries whose stamp <= blockCounter
```

`blockCounter` is an `std::atomic<uint64_t>` incremented at the end of every `SourceRouter::Process`.

### 2.4 What the exporter must read from their states

The tone file holds each plugin's state as JUCE stores it. For export (§5.4), two parsers are needed:

- **NAM** (iPlug2 chunk inside the VST3 component state):
  1. The string `###NeuralAmpModeler###`.
  2. The plugin version string.
  3. **The model file path.**
  4. **The IR file path.**
  5. The parameters in `EParams` order: `kInputLevel`, `kNoiseGateThreshold`, `kToneBass`, `kToneMid`,
     `kToneTreble`, `kOutputLevel`, `kNoiseGateActive`, `kEQActive`, `kIRToggle`, `kCalibrateInput`,
     `kInputCalibrationLevel`, `kOutputMode`, `kSlim`.

  The model and IR are referenced by **path**, not embedded. That is the one thing export has to fix (§5.4).
- **Airwindows Consolidated** (JUCE XML):
  - `currentProcessorName` (for example `Chamber2`);
  - `awp_0..` normalised parameters;
  - `inlev`, `outlev`, `monoBehaviour`;
  - `streamingVersion`.

  Nothing is external; the effect name is stable, the index is not.

Write both parsers in C# (`GUI/Features/ToneDesigner/BundledStates.cs`), where export lives. For NAM, also
write a small *writer* that puts new paths back into the chunk on import. Check the exact iPlug2 chunk encoding,
length-prefixed strings, at the pinned build **(check)**. Add round-trip unit tests for both: parse, write, then
compare the bytes.

### 2.5 M0b: check them in the host

- **Load and switch.** Load both bundled plugins through the existing host, play a NAM model, IR and gate plus
  two Airwindows instances, and open several windows at once. Load times matter for preparing a song's scenes;
  NAM and Airwindows start in milliseconds, unlike AmpliTube.
- **Timing log.** Add a block-time histogram around `SourceRouter::Process`. Use `QueryPerformanceCounter`, write
  into a fixed array, and log every 30 s from the **main** thread. Log the 50th, 99th and 99.9th percentiles and
  the maximum, at 64 and 128 frames. Measure that chain and the same chain twice over, which is the crossfade
  case.

---

## 3. M2: models and IRs travel with the tone

There is **no separate asset library UI**. Creators pick model and IR files in NAM's own file browsers, as they
would anyway.

- **At export (§5.4):** the exporter reads each NAM state's model and IR paths. It hashes the files (SHA-256) and
  copies them into the bank once each. It asks the creator **once per new file** for attribution and permission,
  remembered in `%APPDATA%\RocksmithAudioBridge\Library\rights.json` by hash, and refuses to export without them.
- **At import:** files go to `%APPDATA%\RocksmithAudioBridge\Library\<sha256>.nam|.wav`. The importer **rewrites
  the path strings** inside each NAM state to point there, using the §2.4 writer, before the host loads the
  scene.
- **Missing files on the creator's machine:** if a path in a state points to a missing file, export fails and
  names the tone and the file.
- **Validation:** the bundled NAM plugin itself refuses models it cannot load. The exporter additionally checks
  that the file parses as JSON with a known `architecture`, and that an IR is a readable WAV.

---

## 4. M3: overlay tabs

### 4.1 Tabs

- **Three tabs.** In `OverlayShell.cpp`, `AmpTab` becomes `{ ToneDesigner, VstRig, ExternalApp }`, and
  `AmpTabs` draws three segments: generalise the `i < 2` loop to a count.
- **One page.** **Both the Tone designer and VST rig tabs are today's page.** It takes a parameter saying which
  chain it drives. Only these differ:

| | Tone designer tab | VST rig tab |
|---|---|---|
| Plugin browser lists | the bundled NAM and Airwindows only | every scanned VST3, as today |
| Tones listed | `Tones\Tone designer\` | the other tone folders, as today |
| Double-click a pedal | opens its own window (NAM UI, Airwindows UI) | same, as today |
| Song menu | **Edit song tones**, **Export song tones** | the song tone assignment, as today |

- **Pedal names.** An Airwindows pedal is labelled with its `currentProcessorName`, not "Airwindows
  Consolidated", so the board reads "Gatelope → NAM → Chamber2". To get the name, the host publishes each slot's
  plugin-reported name. JUCE's `getProgramName` or the state's `currentProcessorName` **(check which updates
  live)**. Use the existing `Slot::name` field.
- **No ImGui knobs, no new editors, no board refactor.** The page code is parameterised, not split or copied.

### 4.2 Source switch

The bar's `VST | Game amp` switch becomes `Tone designer | VST rig | Game amp`, which sends
`Command::SetSource(arg)` to the `SourceRouter`. Opening a pedal or loading a tone on a tab switches the source
to that tab, so you always hear what you edit. Say so in the bar.

---

## 5. M4: song tone sets, export and import

### 5.1 Song index: arrangements and tone keys (C#)

`ToneDesignerTools.SongIndex` already parses every manifest. Write a second index next to the existing TSV
(do not change the existing line format, which `SongLibrary.cpp` parses):

```
<out>.arrangements.tsv   songKey \t persistentId \t arrangementName \t toneBase \t toneA \t toneB \t toneC \t toneD \t toneNames(A|B|C|D) \t psarcFile
<out>.banks.tsv          psarcFile \t bankSha256 \t extractedFolder
```

- **Bump the cache key** (`.cache.v3.json`). The cached `Song` gains its arrangements list.
- **Banks:** for each `foo_p.psarc`, look for `foo_p.rabtones` next to it.
  1. Validate it: ZIP, `manifest.json` schema, every asset's SHA-256 recomputed.
  2. Extract it to `%LOCALAPPDATA%\Rocksmith Audio Bridge\Banks\<sha256>\`, skipping it when that folder
     already exists.
  3. Log rejections with the reason.
- **DLL side:** `SongLibrary` gains `struct Arrangement` and reads both files.

### 5.2 Shared-memory v8 and new commands

Bump `kMappingName`/`kVersion` to v8 (the file's rule: a new name per version). Add the fields:

```cpp
	// Written by the overlay (v8): the game's tone slot during a song (GameTones), read by the host's audio
	// thread once per block. Packed so one 32-bit read is consistent: bits 0-7 slot (0 base, 1-4 A-D,
	// 255 none), bits 8-31 sequence.
	volatile LONG toneSlotWord;
```

The new commands:

| Command | Args | Host action |
|---|---|---|
| `SetSource` | arg 0 Tone designer, 1 VST rig, 2 game amp | `SourceRouter`; the existing tone and slot commands then act on that chain |
| `PrepareSceneSet` | text = bank folder or local set file, newline, arrangement id | §6 |
| `ClearSceneSet` | — | back to the player's source |

### 5.3 Local song tone sets

- **Where:** `%APPDATA%\RocksmithAudioBridge\VstHost\SongSets\<songKey>.json`.
- **Format:** the bank's `manifest.json` shape, but with tones referenced by local tone **name** instead of being
  embedded.
- **Who owns it:** a new DLL unit, `DLL/ToneDesigner/SongToneSets.hpp/.cpp`. `VstHostLink.cpp` is already about
  1,400 lines, so keep it out of there.
- **UI:** `Edit song tones` in the Songs pane opens set-editing mode on the Tone designer tab. Its pickers come
  from the arrangements index, and assigning a slot writes this file.

### 5.4 Export (C#)

Add `RSMods.exe --tone-export <songSet.json> <out.rabtones>` to `ToneDesignerTools`, which already writes ZIPs
for backups:

1. Read the set. Resolve each slot's tone name to its `RABTONE2` file in `Tones\Tone designer\`.
2. **Refuse** any slot that is not `bundled:*`. This cannot normally happen, because the host refuses them
   (§2.3).
3. Parse each NAM state (§2.4). Hash the model and IR files and collect them, using a set so each is stored
   once. Look up their rights in `rights.json`.
4. **Refuse** with a specific message if a file is missing or lacks attribution/permission.
5. Write `manifest.json` plus `assets/<sha>.*`. Use deflate for a sidecar file. The manifest holds:
   - `tones`: each tone's slots as `{ id, bypass, state }`. The state is base64 of the plugin state, with the NAM
     model and IR paths replaced by `asset:<sha256>` placeholders.
   - The arrangements and slot maps, and `gameToneKeys`.
   - `bundled` versions and `minModVersion`.
5. Write to a temp file, then rename.

The overlay starts it with the same background-process pattern `ToneBackup` uses.

### 5.5 Version checks on load

- If the bank's `minModVersion` is newer than the running mod, refuse it with "This song's tones need Rocksmith
  Audio Bridge ≥ X".
- If a bank needs a newer bundled NAM or Airwindows build than the mod ships, refuse it with a message naming
  the plugin.
- Never substitute another block or skip one.

---

## 6. M5: SceneSet and mid-song switching (host)

### 6.1 Classes

```
VstHost/Source/Song/SceneSet.hpp/.cpp      up to 5 prepared scenes, the crossfade, the tails; an AudioSink
VstHost/Source/Song/Scene.hpp/.cpp         one tone's bundled plugin instances, prepared and settled
VstHost/Source/Song/SceneSetBuilder.cpp    bank or local set -> SceneSet (main thread)
```

- **`Scene`.** A `std::vector<std::unique_ptr<PluginHost>>` of bundled plugins, with their states set, plus its
  own stereo buffers. It does not use `Chain`: there are no spares, windows or commands here, and keeping them
  out keeps the audio path small.
- **Instances per scene.** Each scene has its own plugin instances; NAM and Airwindows load in milliseconds.
- **Settling before use.** NAM stages a newly set model and applies it on its next `processBlock`
  **(check `mStagedNAM`)**. The builder therefore runs a few blocks of silence through each scene on the main
  thread before publishing the set, so the first audible block already has the model.
- **Building.** `SceneSetBuilder` creates one `Scene` per **distinct** tone in the arrangement's slot map,
  `Prepare`s every plugin at the stream's block size, and returns the set. It never touches the audio
  thread.
- **Activation.** `SourceRouter::SetSource(sceneSet)` happens when `PrepareSceneSet` finishes. The old set is
  retired with the §2.3 pattern.

### 6.2 Audio-thread logic (`SceneSet::Process`)

```text
read toneSlotWord (one atomic load); slot -> scene index via the slot map (null slot = keep current)
if target != active and no fade running: start fade (outgoing = active, active = target, fadePos = 0)
if target changes during a fade: the outgoing scene joins the tail pool now
process active scene on the guitar
if fading: process outgoing scene on the guitar; equal-power mix by fadePos/fadeLen; at the end, the outgoing
  scene joins the tail pool (fed silence, so its delay and reverb ring out)
tail pool (at most one scene): process with silent input, add to output; drop when RMS < -60 dBFS over
  100 ms or after tailCapMs
safety limiter (-1 dBFS, fixed lookahead 0, fast attack) -> output
```

Implementation notes:

- **Tail blocks** keep their state because the same objects are kept running, so nothing is copied.
- **A scene in two slots** (A and B both "Clean") has a single instance, so switching between those two slots
  does nothing, which is correct.
- **When a scene becomes active again** after being idle, it starts from whatever history it has. If that is
  audible (design §4.2, Unproven), call the processors' `reset` on the main thread while the scene is idle. Do
  it from a timer that checks which scenes are idle and not in the tail pool.
- **The limiter's state** belongs to the `SceneSet`, not to any one scene.

### 6.3 DLL side

`SongToneSets::OnFrame` runs on the render thread, from `VstHostLink::OnFrame`:

1. **Song start**, from `FollowSong`'s song-key change plus `GameTones::TryGetArrangementId`. Resolve the
   source in the design's §7.5 order:
   - the player's song choice;
   - the bundled bank (from `banks.tsv`) when the setting is on;
   - the default.

   For a bank or a local set, queue `PrepareSceneSet` before play, the same timing `FollowSong` uses for
   `PrepareTone` today.
2. **Every frame:** write `toneSlotWord` from `GameTones::CurrentSlot()`, bumping the sequence only on change.
3. **Song end:** `ClearSceneSet`.
4. **Show the source** in the bar, for example `Bundled: Lead · Slot B "Lead solo"`, and give `PublishPopupName`
   the active scene's tone name.

---

## 7. M6: fixtures and acceptance

### 7.1 Offline render tool

Add a console project, `VstHost/Tools/ToneRender/` (x64, sharing `Source/PluginHost`, `Source/BundledPlugins` and `Source/Song`):

```
ToneRender.exe --bank <folder> --arrangement <id> --slot-script <times.txt> --in di.wav --out wet.wav
```

- **Input:** a 48 kHz DI file, run offline at 128-frame blocks through `SceneSet`. The slot script feeds
  `toneSlotWord` changes at given sample times.
- **Fixtures:** a few banks and DI files under `VstHost/Tests/Fixtures/`, with reference renders.
- **Check:** CI or a pre-release script compares new renders with the references (RMS difference at or below
  -90 dB, design §1a).
- **Engine updates:** a NAM or Airwindows update that changes a render is either fixed or becomes a new block
  `version`.

The tool also gives a CPU benchmark with no game running.

### 7.2 Acceptance

Run the design's §12 checklist on a clean machine with the mod only.

---

## 8. Traps found while reviewing the code

1. **`Chain::Process` and `PluginHost::Process` play silence** when `frames != m_frames`. A `SceneSet` must be
   rebuilt on `BridgeAudio::kResetMessage` (buffer size change) just like `Chain::Prepare`.
2. **`BridgeAudio::Render` hard-clamps.** The new limiter goes before it, in the `SourceRouter`, so every
   source gets it. Do not remove the clamp; it protects the int32 conversion.
3. **`PluginHost::kSampleRate` is 48,000 everywhere.** The bundled NAM plugin resamples non-48 kHz models
   internally, which adds CPU. The exporter warns when a model is not 48 kHz.
4. **`Chain::LoadTone` uses `Pause()`.** It is fine for the editing chain, and never used for scene changes.
5. **Several plugin windows open.** The existing `openWindows` bitmask already supports this. Do not add a
   second window system.
6. **The game can relocate (ASLR).** Every new game offset is an RVA with a byte-signature check and a one-time
   log on mismatch, the same as `ToneSwitchPopup`.
7. **The overlay is x86; the host is x64.** Any new shared struct uses fixed-width 32-bit fields only (see the
   header comment in `VstHostControl.hpp`).
8. **Backups.** `--tone-backup` must include `Tones\Tone designer`, `Library` and `SongSets`.
   `--tone-restore` must put them back.
9. **Airwindows registry indices change between builds.** Read `currentProcessorName`, never an index.
10. **Absolute paths inside NAM states.** Every import rewrites them (§3); a tone must never depend on the
    author's folders.
