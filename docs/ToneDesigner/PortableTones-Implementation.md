# Portable tones: implementation guide

Audience: the developer implementing the design in `PortableTones-Design.md`. Read that first, especially the
**Decisions** list and §7. This guide says **what code to add, where, and in what order**, and how each part
connects to what already exists. It also lists the traps found while reviewing the code.

Nothing here has been compiled. Upstream API names were read at the commits in the design's Sources list.
Where a signature must be checked against the pinned source, the guide says **(check)**.

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
- A namespace that matches the folder (`VstHost::Builtin` for `VstHost/Source/Builtin/`).
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
- Anything that calls NAM `Reset`, `SetSlimmableSize`, `get_dsp` or Airwindows `generator()` runs on the main
  thread or a worker thread. Never on the audio thread.

### 0.3 Milestones (each one a reviewable PR)

| # | Milestone | Depends on | Done when |
|---|---|---|---|
| M0a | Game tone-slot and arrangement spike (DLL only, logging) | — | §1 log criteria met |
| M0b | NAM core and Airwindows building in the host; one hard-coded block; timing log | — | Builds; 99.9th percentile block time logged |
| M1 | Built-in blocks as JUCE processors in `Chain`; `.rabtone` files | M0b | A built-in tone saves, loads and plays |
| M2 | Asset library: import, hash, validate | M1 | Import refuses bad models with specific messages |
| M3 | Overlay tab split; built-in pedals with on-board knobs | M1 | Edit a built-in tone without any plugin window |
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

## 2. M0b and M1: built-in blocks in the host

### 2.1 Third-party code

```
VstHost/Lib/NeuralAmpModelerCore/        pinned 0b3d3c97…; with its Eigen and nlohmann submodule contents copied in
VstHost/Lib/NeuralAmpModelerCore/UPSTREAM.md
VstHost/Lib/airwin2rack/                 pinned dcf98a3e…; only the files for the chosen effects
VstHost/Lib/airwin2rack/UPSTREAM.md
```

- **Copy pattern.** Follow `DLL/Audio/ThirdParty/SignalsmithStretch/UPSTREAM.md`: source URL, commit, what was
  copied, local changes (ideally none).
- **Project file.** Add the NAM `.cpp` files and the chosen Airwindows `.cpp` files to
  `RocksmithAudioBridgeVstHost.vcxproj`. Keep `stdcpp17`, then **check** that MSVC accepts the NAM sources with
  it. If not, move the host to `stdcpp20`; JUCE 9 supports it.
- **Sample type.** Define NAM's float sample type so buffers are `float`. The macro name is in `NAM/dsp.h`
  **(check)**.
- **Airwindows subset.** Compile only the chosen effects' `*.cpp`/`*Proc.cpp`, plus `AirwinRegistry.cpp` and
  `airwin_consolidated_base.cpp`. Replace the generated `ModuleAdd.h` with one that registers only those effects.
  **Check** whether `AirwinRegistry.cpp` needs the `awdoc_resources` (CMakeRC) documentation. If it does, stub
  the documentation lookup instead of compiling the docs.
- **Notices.** Add the MIT notices for NAM core, Eigen (MPL2: note its file-level terms), nlohmann/json and
  Airwindows/airwin2rack to `VstHost/THIRD-PARTY-NOTICES.txt`.

Effects for the first release (keep the list short and extend it later):

| Role | Effects |
|---|---|
| Gate | Gatelope |
| Compressor | Pressure5 |
| Drive | Drive, Density2 |
| EQ | an Airwindows EQ suited to guitar, e.g. Baxandall2 or EQ (check what the subset offers) |
| Modulation | Chorus |
| Delay | TapeDelay2 |
| Reverb | Chamber2, Verbity2 |

### 2.2 New files

```
VstHost/Source/Builtin/BuiltinId.hpp           ids, "builtin:" prefix parsing
VstHost/Source/Builtin/BuiltinRegistry.hpp/.cpp   id -> factory, display names, kinds
VstHost/Source/Builtin/BuiltinProcessor.hpp/.cpp  common juce::AudioPluginInstance base
VstHost/Source/Builtin/NamAmpProcessor.hpp/.cpp
VstHost/Source/Builtin/CabIrProcessor.hpp/.cpp
VstHost/Source/Builtin/AirwindowsProcessor.hpp/.cpp
VstHost/Source/Builtin/AssetLibrary.hpp/.cpp      (M2)
VstHost/Source/Builtin/BuiltinTone.hpp/.cpp       .rabtone read/write (JSON)
```

Namespace `VstHost::Builtin`.

### 2.3 How built-ins enter the existing chain

`Chain::Slot::path` stays a `std::wstring`. A built-in slot's path is an id string:

```
builtin:nam                       a NAM amp; its model is in its state (asset hash)
builtin:ir                        a cabinet IR; asset hash in its state
builtin:airwindows/Chamber2       an Airwindows effect, by registry name
```

`PluginHost::Load(path)` checks the prefix first:

```cpp
// PluginHost.cpp, at the top of Load (edit in the file's own style)
if (Builtin::IsBuiltinId(path))
{
	m_plugin = Builtin::BuiltinRegistry::Create(path, error);
	if (!m_plugin) return false;
	m_name = m_plugin->getName().toStdString();
	m_identifier = juce::String(path.c_str()).toStdString();
	m_plugin->setPlayHead(m_playHead.get());
	m_plugin->addListener(this);
	ChooseBuses();
	m_quietUntil = GetTickCount64() + kSettleMs;
	return true;
}
```

Everything else in `PluginHost` and `Chain` then works unchanged: `Prepare`, `Process`, `GetState`/`SetState`,
dirty tracking, spares, `Duplicate`, `Move` and `OpenEditor`, which falls back to the generic editor. Three
places in `Chain.cpp` assume a file on disk; teach each to skip the check for built-in ids:

- `LoadTone`: `GetFileAttributesW(entry.path…)`.
- `TonePlugins`/`NotLoaded`: fine as they are; they compare strings.
- `Main.cpp` `PreloadStep`: `GetFileAttributesW`.

**Separate chains per source.** Add a second `Chain` instance in `Main.cpp`: `g_builtinChain` next to
`g_chain`. Add a small sink that forwards to whichever source plays:

```cpp
namespace VstHost
{
	// The one AudioSink BridgeAudio calls. Forwards each block to the source the player chose (VST rig chain,
	// built-in chain, or a song's scene set). Main thread sets; stream thread reads once per block.
	class SourceRouter final : public AudioSink
	{
	public:
		void SetSource(AudioSink* sink) { activeSink.store(sink, std::memory_order_release); }
		void Process(const float* guitar, float* left, float* right, int frames) override;

	private:
		std::atomic<AudioSink*> activeSink{ nullptr };
	};
}
```

Switching sources is a hard cut, which is acceptable because it is a deliberate user action. If it ever needs a
fade, do the fade in the router.

**The retire pattern.** Use it wherever the audio thread reads a pointer the main thread replaces:

```cpp
// main thread
auto* old = live.exchange(fresh.release(), std::memory_order_acq_rel);
retired.push_back({ std::unique_ptr<T>(old), blockCounter.load() + 2 });   // freed once two blocks have run
// main-thread timer: erase entries whose stamp <= blockCounter
```

`blockCounter` is an `std::atomic<uint64_t>` incremented at the end of every `SourceRouter::Process`.

### 2.4 `BuiltinProcessor` base

It implements the `juce::AudioPluginInstance` boilerplate once:

- `fillInPluginDescription` (name, `pluginFormatName = "Builtin"`, `fileOrIdentifier = id`);
- no MIDI; one program;
- `isBusesLayoutSupported`: mono in to stereo out, or stereo to stereo.

Subclasses implement:

```cpp
virtual void PrepareBlock(double sampleRate, int maxFrames) = 0;   // from prepareToPlay; main thread
virtual void ProcessBlock(juce::AudioBuffer<float>& buffer) = 0;     // from processBlock; audio thread
virtual juce::var SaveParameters() const = 0;                        // JSON object; the tone file's view
virtual bool LoadParameters(const juce::var& state, std::string& error) = 0;
```

- **State.** `getStateInformation`/`setStateInformation` serialise `SaveParameters()` as UTF-8 JSON. That keeps
  `PluginHost::GetState`/`SetState` and the spares working unchanged.
- **Parameters.** Expose them as `juce::AudioParameterFloat`, so `audioProcessorParameterChanged` marks the
  tone dirty and the generic editor works.
- **Denormals.** Put `juce::ScopedNoDenormals` at the top of `processBlock`.

### 2.5 `NamAmpProcessor`

- **Parameters:** `Input` (dB, -24..+24), `Output` (dB, -40..+12). The asset hash and the A2 quality value are
  state, not automatable parameters.
- **`LoadParameters`:**
  1. Resolve the hash through `AssetLibrary` to a file.
  2. Parse it with `nlohmann::json::parse`, then call `nam::get_dsp(json)`.
  3. Validate: `GetExpectedSampleRate() == 48000`, 1-in/1-out channels.
  4. Apply the A2 quality with `SetSlimmableSize` when the model is slimmable.
  5. Call `Reset(48000, maxFrames)`.
  6. Swap the model in with the retire pattern.
  7. Fail with a specific `error` and log it; never load a substitute.
- **`ProcessBlock`:** apply the input gain to channel 0, run the model mono in place **(check the `process`
  signature at the pin)**, apply the output gain, copy to channel 1.
- **Input calibration:** if the model has `input_level_dbu`, add the calibration offset (design §4.3). Keep the
  device reference constant in one place, `Builtin/Calibration.hpp`, documented and measured. Do not scatter
  magic numbers.
- **Block size:** pass the real maximum block size. The A2-fast and Sequential paths **allocate** if a block is
  larger than the size given to `Reset`.

### 2.6 `CabIrProcessor`

- Load a WAV through `nam::get_dsp(path)`, which produces a `Linear` model and resamples it in `Reset`.
- At import (M2), truncate IRs to 8,192 samples and store them as float WAV at 48 kHz, so playback never
  resamples.
- Parameter: `Mix` (0..1). Mono input, stereo output when the IR is stereo.

### 2.7 `AirwindowsProcessor`

- **Create:** `AirwinRegistry::nameToIndex` gives the registry entry, then call its `generator()` on the main
  thread, then `setSampleRate(48000)`. Name, category and parameter count come from the registry entry.
- **Parameters:** one `AudioParameterFloat` per Airwindows parameter, using `getParameterName(i)` and 0..1
  values. Show the display text through `getParameterDisplay` and `getParameterLabel` **(check names)**, which
  the overlay knobs show (§4).
- **Process:** `processReplacing(in, out, frames)`. Use separate input and output buffers: allocate a scratch
  stereo buffer in `PrepareBlock`. **Check** whether in-place processing is safe before removing the copy.
- **Saved form:** `{ "name": "Chamber2", "version": 1, "params": [ … ] }`. `version` is our own
  `algorithmVersion` (design §6.5). It starts at 1 and is bumped only if a pinned-source update changes that
  effect's sound.

### 2.8 `.rabtone` (local built-in tone)

```json
{ "format": "rab-tone", "formatVersion": 1,
  "blocks": [ { "id": "builtin:nam", "bypass": false, "state": { "asset": "9f2c…", "inputDb": 0, "outputDb": -3, "slimmable": 1.0 } },
              { "id": "builtin:airwindows/Chamber2", "bypass": false, "state": { "name": "Chamber2", "version": 1, "params": [0.3,0.4,0.5,0.2] } } ],
  "outputDb": 0.0 }
```

- **Read and write** in `BuiltinTone.cpp` with `juce::JSON` (in `juce_core`, so no new dependency). Write to a
  temp file then rename, the same as `Chain::SaveTone`.
- **Where:** `%APPDATA%\RocksmithAudioBridge\Builtin\Tones\`. That is separate from the VST rig's
  `VstHost\Tones\`.
- **Load path:** `Chain` gets `SaveBuiltinTone`/`LoadBuiltinTone` next to the `RABTONE2` functions. Its loading
  logic (reuse matching slots, build the new ones while the old play, then swap) is reused as it is.

### 2.9 M0b timing log

- Add a block-time histogram around `SourceRouter::Process`. Use `QueryPerformanceCounter`, write into a fixed
  array, and log it every 30 s from the **main** thread.
- Log the 50th, 99th and 99.9th percentiles and the maximum, against the block period.
- Measure at 64 and 128 frames with one NAM amp, one IR, Chamber2 and TapeDelay2. Then measure the same chain
  twice over, which is the crossfade case.

---

## 3. M2: asset library (host)

```cpp
namespace VstHost::Builtin
{
	enum class AssetKind { NamModel, ImpulseResponse };

	struct AssetInfo
	{
		std::string sha256;          // 64 lower-case hex of the stored bytes
		AssetKind kind;
		std::string displayName;
		std::string attribution;     // required before export
		std::string permission;      // "author-original", "cc-by-4.0", ...; empty blocks export
		bool includesCabinet = false;
		float loudnessDb = 0.0f;     // metadata.loudness, else measured at import
	};

	// %APPDATA%\RocksmithAudioBridge\Builtin\Library: <sha256>.nam / <sha256>.wav plus library.json (AssetInfo
	// per hash). Main thread.
	class AssetLibrary
	{
	public:
		bool Import(const std::wstring& sourcePath, AssetInfo& imported, std::string& error);
		std::wstring PathOf(const std::string& sha256) const;   // empty when not in the library
		const AssetInfo* Find(const std::string& sha256) const;
		std::vector<AssetInfo> List() const;
		bool SetRights(const std::string& sha256, const std::string& attribution, const std::string& permission);
	};
}
```

**`Import` rules.** Each refusal gives the player a specific message.

- **NAM model:**
  1. Parse the file and call `get_dsp`.
  2. Refuse it if the sample rate is not 48,000 or is missing, if it is not 1-in/1-out, or if the architecture is
     not built in.
  3. Run `Reset(48000, 4096)` to prove it prepares.
  4. If `metadata.loudness` is missing, measure loudness on a fixed internal test signal.
  5. Store the **original bytes** unchanged.
- **IR (WAV):**
  1. Read it with `juce::AudioFormatManager` (WAV only).
  2. Resample to 48 kHz, offline, with a high-quality resampler.
  3. Truncate to 8,192 samples.
  4. Store the result as float32 WAV. The hash is of the stored bytes.
- **Hash:** SHA-256 via Windows CNG (`BCryptHash`, add `bcrypt.lib`). `juce_cryptography` is not vendored.
- **Copy:** the library keeps its own copy, so deleting the original file changes nothing.

**Overlay access.** New commands (§5.2): `ImportAsset` (the host opens a file dialog, as `AddScanFolder` does
today) and `SetAssetRights`. The host publishes a compact asset list in shared memory, or writes `library.json`
for the overlay to read; the file is simpler and avoids enlarging the mapping.

---

## 4. M3: overlay tab split and on-board knobs

### 4.1 Tabs

In `OverlayShell.cpp`, `AmpTab` becomes `{ ToneDesigner, VstRig, ExternalApp }` and `AmpTabs` draws three
segments: the label loop already handles `i < 2`, so generalise it to a count. The current Tone designer page
code is renamed to the **VST rig** page; only its title changes.

### 4.2 Reuse the board instead of copying it

The `ToneDesigner` namespace in `OverlayShell.cpp` holds about 2,000 lines of page code. Do not copy it.
Extract the parts both tabs share into:

```
DLL/Overlay/ToneBoard.hpp/.cpp          bar, library list, pedalboard, meters, songs pane
DLL/Overlay/IToneBoardSource.hpp        what differs per tab
DLL/Overlay/VstRigSource.cpp            IToneBoardSource over VstHostLink (today's behaviour)
DLL/Overlay/BuiltinToneSource.cpp       IToneBoardSource over the built-in chain
```

```cpp
namespace Overlay
{
	enum class PedalKind { Unknown, Amp, Cab, Dynamics, Drive, Modulation, Delay, Reverb, Effect, Instrument };

	struct PedalView
	{
		std::string name;
		PedalKind kind;
		bool isBypassed;
		bool isWindowOpen;           // VST rig only
	};

	// What the shared Tone board needs from a tab. The VST rig tab opens plugin windows; the Tone designer tab
	// edits knobs on the pedal.
	class IToneBoardSource
	{
	public:
		virtual ~IToneBoardSource() = default;
		virtual std::vector<PedalView> Pedals() const = 0;
		virtual const std::vector<std::string>& Tones() const = 0;
		virtual void LoadTone(const std::string& name) = 0;
		virtual void SaveTone(const std::string& name) = 0;
		virtual void AddPedal(const std::wstring& idOrPath, int position) = 0;
		virtual void RemovePedal(int slot) = 0;
		virtual void MovePedal(int slot, int delta) = 0;
		virtual void SetBypass(int slot, bool isBypassed) = 0;
		virtual void ActivatePedal(int slot) = 0;              // double-click: plugin window, or knob edit mode
		virtual bool CanExport() const = 0;                    // false for the VST rig
		virtual void DrawBrowser(float width, float height) = 0;   // VST plugin browser, or block list + library
	};
}
```

Do the extraction **first, as a pure refactor**: the VST rig page must behave identically, so review it by
running the page. Only then add `BuiltinToneSource`.

### 4.3 Knobs on the pedal

- **Host to overlay (VstHostControl v8):** the selected built-in slot's parameters:

```cpp
	constexpr int kMaxParams = 16;
	struct Param
	{
		char name[24];                 // UTF-8
		char text[16];                 // display, "320 ms"
		volatile LONG valueMillionths; // normalised 0..1, times 1e6
	};
	// in Shared (v8):
	volatile LONG paramSlot;           // whose parameters these are, -1 = none
	volatile LONG paramCount;
	Param params[kMaxParams];
```

- **Overlay to host:** `Command::SetParameter`, where `arg` is the slot, `arg2` the parameter index and the new
  `commandArg3` the value in millionths. In `VstHostLink.cpp` it coalesces the same way `SetLevels` already does
  (line ~205): while the last queued command is a `SetParameter` for the same slot and parameter, overwrite its
  value.
- **Drawing:** the pedal already draws knobs (`PedalLook::knobs`). For built-in pedals, draw one per parameter
  (amps up to 5, others up to 4; a pedal with more opens a small parameter strip under the board). Drag edits;
  double-click resets to the default; the hover tooltip shows `text`.

### 4.4 Source switch

The bar's `VST | Game amp` switch becomes `Built-in | VST rig | Game amp`, which sends
`Command::SetSource(arg)` to the host's `SourceRouter`. Editing a pedal on the Tone designer tab switches the
source to Built-in automatically, so you always hear what you edit. Say so in the bar.

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
	volatile LONG commandArg3;
```

Plus the `Param` block from §4.3. The new commands:

| Command | Args | Host action |
|---|---|---|
| `LoadBuiltinTone` / `SaveBuiltinTone` / `DeleteBuiltinTone` / `RenameBuiltinTone` | text | `.rabtone` files, as the VST equivalents |
| `SetParameter` | arg slot, arg2 index, arg3 value | built-in chain parameter |
| `SetSource` | arg 0 built-in, 1 VST rig, 2 game amp | `SourceRouter` |
| `ImportAsset` / `SetAssetRights` | text | §3 |
| `PrepareSceneSet` | text = bank folder or local set file, newline, arrangement id | §6 |
| `ClearSceneSet` | — | back to the player's source |

### 5.3 Local song tone sets

- **Where:** `%APPDATA%\RocksmithAudioBridge\Builtin\SongSets\<songKey>.json`.
- **Format:** the bank's `manifest.json` shape, but with tones referenced by local tone **name** instead of being
  embedded.
- **Who owns it:** a new DLL unit, `DLL/ToneDesigner/SongToneSets.hpp/.cpp`. `VstHostLink.cpp` is already about
  1,400 lines, so keep it out of there.
- **UI:** `Edit song tones` in the Songs pane opens set-editing mode on the Tone designer tab. Its pickers come
  from the arrangements index, and assigning a slot writes this file.

### 5.4 Export (C#)

Add `RSMods.exe --tone-export <songSet.json> <out.rabtones>` to `ToneDesignerTools`, which already writes ZIPs
for backups:

1. Read the set. Resolve each slot's tone name to its `.rabtone` file.
2. Collect asset hashes from every block, using a set so each is stored once. Read each `AssetInfo` from
   `library.json`.
3. **Refuse** with a specific message if any asset lacks `attribution`/`permission`, any file is missing, or any
   block id is not built-in. VST tones cannot be referenced, by construction.
4. Write `manifest.json` (design §6.2: tones embedded by value, arrangements, slot maps, `gameToneKeys`,
   `engine` pins, `minModVersion`) plus `assets/<sha>.*`. Use deflate for a sidecar file.
5. Write to a temp file, then rename.

The overlay starts it with the same background-process pattern `ToneBackup` uses.

### 5.5 Version checks on load

- If the bank's `minModVersion` is newer than the running mod, refuse it with "This song's tones need Rocksmith
  Audio Bridge ≥ X".
- If a block's id or `version` is unknown, refuse the bank with a message naming the block.
- Never substitute another block or skip one.

---

## 6. M5: SceneSet and mid-song switching (host)

### 6.1 Classes

```
VstHost/Source/Song/SceneSet.hpp/.cpp      up to 5 prepared scenes, the crossfade, the tails; an AudioSink
VstHost/Source/Song/Scene.hpp/.cpp         one tone's built-in processors, prepared, no PluginHost
VstHost/Source/Song/SceneSetBuilder.cpp    bank or local set -> SceneSet (main thread)
```

- **`Scene`.** A plain `std::vector<std::unique_ptr<Builtin::BuiltinProcessor>>`, its own stereo buffers and
  the tone's output gain. It does not use `Chain`: there are no spares, windows or commands here, and keeping
  them out keeps the audio path small. Each processor is marked time-based when its kind is Delay or Reverb.
- **Building.** `SceneSetBuilder` creates one `Scene` per **distinct** tone in the arrangement's slot map,
  `prepareToPlay`s every processor at the stream's block size, and returns the set. It never touches the audio
  thread.
- **Activation.** `SourceRouter::SetSource(sceneSet)` happens when `PrepareSceneSet` finishes. The old set is
  retired with the §2.3 pattern.

### 6.2 Audio-thread logic (`SceneSet::Process`)

```text
read toneSlotWord (one atomic load); slot -> scene index via the slot map (null slot = keep current)
if target != active and no fade running: start fade (outgoing = active, active = target, fadePos = 0)
if target changes during a fade: outgoing's non-time-based part stops now; its time-based blocks join the tail pool
process active scene on the guitar
if fading: process outgoing scene on the guitar; equal-power mix by fadePos/fadeLen; at the end, move outgoing's
  time-based blocks to the tail pool (fed silence)
tail pool (at most one scene's blocks): process with silent input, add to output; drop when RMS < -60 dBFS over
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

Add a console project, `VstHost/Tools/ToneRender/` (x64, sharing `Source/Builtin` and `Source/Song`):

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
2. **`BridgeAudio::Render` hard-clamps.** The new limiter goes before it, inside `SceneSet` and the built-in
   chain. Do not remove the clamp; it protects the int32 conversion.
3. **`PluginHost::kSampleRate` is 48,000 everywhere.** Keep built-ins on the same constant; do not add a second
   one.
4. **`Chain::LoadTone` uses `Pause()`.** It is fine for the editing chain, and never used for scene changes.
5. **The command channel is one at a time.** Knob drags must coalesce (§4.3), or the queue lags behind the hand.
6. **The game can relocate (ASLR).** Every new game offset is an RVA with a byte-signature check and a one-time
   log on mismatch, the same as `ToneSwitchPopup`.
7. **The overlay is x86; the host is x64.** Any new shared struct uses fixed-width 32-bit fields only (see the
   header comment in `VstHostControl.hpp`).
8. **Backups.** `--tone-backup` must include `Builtin\Tones`, `Builtin\Library` and `Builtin\SongSets`.
   `--tone-restore` must put them back.
9. **Airwindows registry indices change between builds.** Store names only.
10. **NAM `Reset` allocates and prewarms.** Never call it from `processBlock`, not even on the first block.
