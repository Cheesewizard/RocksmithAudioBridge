# Portable Tone Designer tones: NAM, Airwindows and CDLC tone switching

Revision 2, 25 October 2026. Reviews and supersedes the research handover of the same date ("Research handover:
portable Tone Designer tones, NAM, Airwindows and CDLC switching").

Status: design agreed (see Decisions). Nothing here is implemented. No builds, listening tests or in-game tests were run.

Evidence labels, as in the handover:

- **Source:** read in this repository or in upstream source at the commit given.
- **Measured:** measured during this review; the scope is stated.
- **Proposal:** a recommended design, not existing functionality.
- **Unproven:** needs an experiment before anyone relies on it.

---

## Decisions (agreed 25 October 2026)

0. **Revision 4 (supersedes Revision 3 below):** the Tone designer runs the open-source **Stompbox engine**
   (mikeoliphant/stompbox, GPL-3) inside the host. The engine provides NAM amps, circuit-modelled tone stacks,
   cabinet IRs and effects in one fixed rig. Its window is drawn natively in the overlay, and saved tones are
   Stompbox's plain-text presets.
   - **No plugin windows, no .NET for players.** Stompbox's own C# remote is used only to evaluate the engine
     during development.
   - **Status:** `VstHost/Documentation/Implementation-status.md`.

1. **Built-in tones only.** Portable tones use only the **bundled plugins**: pinned builds of the open-source
   NAM VST3 and Airwindows Consolidated VST3, which the mod ships in its own folder. No conversion of VST rigs
   (§10).
   - **Revision 3 (supersedes §4.1, §4.4 and §7.2):** there are no custom built-in processors, no on-board
     knobs and no new editor UI.
   - **Their own windows:** double-click opens the plugin's own window, and several can be open at once, as for
     any VST3. The NAM plugin includes the gate, Bass/Mid/Treble, the IR loader, input calibration and the A2
     quality setting.
   - **Export:** export reads their states, embeds the NAM model and IR files those states point to, and
     rewrites the paths on import. See the implementation guide §2–§3.
2. **Three tabs.** **Tone designer** is new, built-in and exportable. **VST rig** is today's page, renamed: local
   only, with backup and restore. **External app** is unchanged (§7.1).
3. **No mixing.** VST and built-in blocks are never mixed in one tone.
4. **Song switching.** VST rig tones stay song-start only. Mid-song slot switching is for built-in tones.
5. **One song tone set file per CDLC.** It covers every arrangement and instrument, and every tone slot (base,
   A–D), with each tone and asset stored once (§7.3).
6. **Version 1 ships the file next to the CDLC** (`<psarc name>.rabtones`). Embedding it in the PSARC follows once
   the game is confirmed to accept the extra entry (§6.3).
7. **Switching follows the game's own tone slot.** There is no custom chart scheduler, unless the phase 0a spike
   fails (§5.2).
8. **What plays when a song starts:** the player's own choice for the song, then the bundled tone set (a setting,
   on by default), then the player's default (§7.5).

---

## 0. What changed from the handover

The handover's direction holds up. Ship NAM and Airwindows as built-in blocks and embed the actual model bytes in a
per-song bank. The review changes six things, and drops the rig-capture track.

| # | Handover position | Revised position | Why |
|---|---|---|---|
| 1 | Extract the chart's tone events and build our own sample-timed scheduler (§8, items 2, 5 and 7) | **Follow the game's own tone slot.** The game already switches between Tone_Base and Tone_A–D at chart times, and through seeks, Riff Repeater, speed changes and the 1–4 hotkeys. We only map each slot to a prepared scene. | The mod already detours `GameComponentTones::Update` (Source: `DLL/ToneDesigner/ToneSwitchPopup.cpp`). SNG tone events are just `{time, id 0..3}` (Source: Rocksmith2014.NET). Rebuilding the game's timeline would duplicate the hardest work and add a new way to drift. |
| 2 | C++17 vs C++20 is an open build blocker | **Probably not a blocker.** The NAM core sources pass a C++17 syntax check. The single C++20 use has a guarded C++11 fallback. | Source: NeuralAmpModelerCore `0b3d3c9`. Checked with GCC 13 only; MSVC v143 is unproven. |
| 3 | Consider `nam-binary-loader` for size | **Drop it.** Store the original `.nam` JSON and let the container compress it. | The loader has no A2, Slimmable or Sequential support and does not build against current core. zstd -19 already shrinks the A2 example to 119 KB. |
| 4 | Cabinet convolution "where needed", unspecified | **Use NAM core's own `Linear` model for cabinet IRs.** It is MIT, does partitioned FFT convolution with no added latency, resamples the IR in `Reset` and loads WAV through `get_dsp`. | No new dependency. JUCE `dsp::Convolution` is AGPL like the rest of JUCE, and `juce_dsp` is not in the vendored JUCE. |
| 5 | Scenes are an open-ended set | **Bounded: at most 5 scenes per arrangement** (base plus A–D). Prepare them all at song start. | RS2014 structure (Source: Rocksmith2014.NET `ToneInfo`, manifest `Tone_Base`/`Tone_A..D`). This removes lazy loading and cache-eviction design during play. |
| 6 | Automatic whole-rig capture is a parallel R&D track | **Out of scope.** Built-in blocks only (§10). | Exactness and simplicity. A converted delay or reverb would only be an approximation. |

Corrections to individual facts are in §11.

---

## 1. Goal and non-goals

**Goal.** A creator builds a tone in Tone Designer from built-in blocks (NAM amps, cabinet IRs, Airwindows
effects) and assigns one scene to each of an arrangement's tone slots. The creator exports a small bank that
travels with the CDLC. The recipient needs the mod and nothing else: no third-party plugins, no accounts, no
network, none of the author's files.

**Non-goals for the first release:**

- Converting third-party VST chains or external amp apps into portable tones (§10).
- Adding tone changes the chart does not contain. The game's five slots are the switching vocabulary.
  Creator-authored extra events are a possible later extension (§6.6).
- Tempo-synced effects. Airwindows effects take a sample rate, not a transport.

---

## 1a. Accuracy

Built-in tones are reproduced exactly, not approximated. The recipient runs the same pinned DSP code, with the
same model bytes and the same parameter values, so the same DI input produces the same output.

- **Check:** the fixture renders in §6.5.
- **Input level:** what really varies is the player's input level. Input calibration (§4.3) is therefore
  required.

---

## 2. Facts the design rests on

### 2.1 Repository (Source, this branch)

- **The host process.** `VstHost` is a separate x64 process, JUCE 9.0.3, MSVC v143, `stdcpp17`. It reaches the
  game as the AmpLink guest of the 64-bit bridge driver.
- **Sample rate.** The host always runs at **48 kHz** (`AmpLink::kSampleRate`, `PluginHost.cpp` `kSampleRate`).
- **Block deadline.** `AmpLink.hpp` says a guest that runs late misses that block, and the host plays it as
  silence. The deadline is **one game block** (2.67 ms at 128 frames), so DSP cost must be budgeted for the
  **worst-case block**, not the average.
- **Chain processing.** `Chain::Process` feeds mono guitar to both channels and runs the slots ping-pong. A block
  whose frame count differs from the prepared size plays silence.
- **Output clamp.** `BridgeAudio::Render` hard-clamps the output to ±1.0. Nothing upstream of the clamp manages
  headroom today.
- **Structural changes pause the stream.** `Chain::LoadTone` loads new plugins while the old chain plays, but the
  swap itself goes through `Pause()`: at least one block of silence, then a hard cut. That is fine for a
  song-start load and unacceptable for a mid-song scene change.
- **Tone files.** `RABTONE2` holds plugin paths, bypass flags and opaque JUCE plugin states. The trims are
  written level-neutral on purpose, because player trims are global.
- **Overlay-to-host link.** `VstHostControl.hpp` (shared memory, v9 on the current branch) is a one-at-a-time acknowledged command
  channel. It is right for edits and wrong for timing.
- **Song-start routing only.** `VstHostLink.cpp` `FollowSong` picks a tone per **song key** at song start. The
  song key comes from the preview event name (`GameState::GetSongKey`), so it identifies a song, not an
  arrangement.
- **Existing game hooks.** `ToneSwitchPopup.cpp` already:
  - detours `GameComponentTones::Update`, which captures the tone component and, through `+0x0C`, the
    `GamePlaysongLAS` song object;
  - reads the song clock at song object `+0x3B4`;
  - detours `ShowNotification`, where type 7 is the game's own tone-change readout. Its comment says the game
    raises it "only when its own tone slot changes, e.g. the 1-4 hotkeys".
- **Host transport.** `PluginHost` gives VST plugins a fixed 120 BPM, 4/4, always-playing transport.

### 2.2 NAM core (Source: NeuralAmpModelerCore `0b3d3c97`, DSP 0.6.0, 2026-09-18)

- **Loading.** `get_dsp` takes a path, a `dspData` or an `nlohmann::json`; there is no raw-buffer overload. To
  load from memory, call `nlohmann::json::parse(bytes)` and then `get_dsp(json)`.
- **Architectures.** Linear (IR), ConvNet, LSTM, WaveNet, SlimmableContainer (A2) and Sequential (chained
  sub-models, such as amp plus IR).
- **Reset.** `Reset(sampleRate, maxBufferSize)` allocates and prewarms by default, so call it off the audio
  thread. Generic WaveNet only *asserts* that a block fits `maxBufferSize`; A2-fast and Sequential *allocate* if
  a block exceeds it. Always pass the true maximum block size.
- **No resampling of neural models.** `GetExpectedSampleRate()` returns the file's top-level `sample_rate`, or
  -1 when the file has none.
- **Channels.** `NumInputChannels`/`NumOutputChannels` are exposed. The official plugin accepts only 1-in/1-out
  models.
- **A2 quality.** `SetSlimmableSize(0..1)` switches between the 3-channel and 8-channel sub-models. It is
  thread-safe but not real-time safe.
- **Weights are not shared.** Each loaded DSP owns its weights; there is no shared-weights API.
- **Metadata read by core.** `metadata.loudness`, `metadata.input_level_dbu`, `metadata.output_level_dbu`.

### 2.3 Measured sizes (NAM core `0b3d3c97`, in-memory, raw `.nam` bytes)

| File | Weights | Raw | zlib -9 | zstd -19 | xz -9 |
|---|---:|---:|---:|---:|---:|
| `wavenet_a1_standard.nam` | 13,802 | 407,452 | 132,810 | 114,295 | 116,616 |
| `A2.nam` (3ch 1,871 + 8ch 12,146) | ~14,017 | 665,758 | 143,167 | 119,309 | 121,300 |

The handover's ZIP figures (132,948 and 143,271) agree; they include ZIP overhead. WaveNet presets in trainer
v0.9.0 have these parameter counts: standard 13,802, lite 6,554, feather 3,026, nano 842. The current trainer's
default is a packed A2 config.

**Rule of thumb (Proposal):** budget about 120–145 KB compressed per distinct standard or A2 amp model, and
about 5–30 KB per cabinet IR (§4.4). Five distinct amps plus two IRs come to under 1 MB. Most songs reuse one
or two amps across their scenes.

### 2.4 Airwindows via airwin2rack (Source: `dcf98a3e`)

- **License and build.** MIT. Static target `airwin-registry` (528 effects, C++17, needs `/bigobj`).
- **No subset option in CMake.** A subset needs our own source list plus a trimmed `ModuleAdd.h`.
- **API.** `setSampleRate(float)`, `setParameter(int, 0..1)`, `processReplacing(float** in, float** out, int32)`.
- **Channels.** **Every effect is 2-in/2-out.**
- **Real-time behaviour.** No allocation in the processing code. Instances can be large, so create them off the
  audio thread. Each effect injects dither below 1e-23 itself; the library does not set FTZ/DAZ.
- **Stable key.** The effect **name** (for example `"Cabs"`). Indices and ordering change between builds.
- **Guitar-relevant effects that exist:**
  - Gate: Gatelope, SoftGate
  - Dynamics: Pressure5, PurestSquish, Dynamics3
  - Drive: Drive, Density2/3, Mojo, Tube2
  - Modulation: Chorus, ChorusEnsemble
  - Delay: TapeDelay2, PitchDelay
  - Reverb: Chamber2, Galactic3, kCathedral3, Verbity2
  - Amp sims: GrindAmp, LeadAmp, BigAmp, MidAmp, Cabs, Cabs2 and others
- **No effect called "NoiseGate", "Gate" or plain "Console".**

### 2.5 Rocksmith 2014 tone data (Source: Rocksmith2014.NET `4cf3325`)

- **Slots.** Each arrangement has `Tone_Base` plus up to four switchable slots, `Tone_A`–`Tone_D`. In the
  manifest these are tone **Keys**.
- **SNG tone events.** `{Time: float seconds, ToneId: int32 0..3}`. The id indexes A–D; there are no names.
- **Manifest `Tones`.** One entry per used key: Key, Name, GearList, Volume and so on.
- **Arrangement identity.** Every arrangement has a `PersistentID`, a 32-hex GUID. This is the arrangement
  identity that §1 of the handover found missing.

---

## 3. Architecture overview

```
 GUI (C#)                       Game DLL (x86)                     VstHost (x64, AmpLink guest)
 ─────────                      ──────────────                     ─────────────────────────────
 PSARC indexer ──bank cache──▶  arrangement PersistentID ──┐
 Tone bank export/import        current tone slot (0..4) ──┼─ shared memory v10 ▶ SceneSet (≤5 prepared graphs)
                                song start / end          ─┘   (atomic slot word)   └ audio thread: crossfade,
                                                                                       spillover tails
```

The design has three boundaries, each with one job:

1. **The GUI owns files.** It reads banks from PSARCs or sidecar files, validates them and extracts them into a
   content-addressed cache. The DLL and the host never parse PSARCs.
2. **The DLL owns game state.** It reports which arrangement is playing and which tone slot the game is in. It
   makes no timing decisions of its own.
3. **The host owns audio.** It builds every scene of the arrangement before the song starts, then reacts to slot
   changes in the audio thread.

---

## 4. Host: built-in processors and the scene graph

### 4.1 Built-in blocks as JUCE processors (superseded by Revision 3: bundled plugins)

Each built-in block is a `juce::AudioPluginInstance` subclass created by a small factory inside the host. They
are not separate plugin files. `PluginHost` already wraps a `juce::AudioPluginInstance`, so a built-in block
gets the existing Tone Designer behaviour for free:

- slots, bypass, move, duplicate and spares;
- `GetState`/`SetState` for tones;
- dirty tracking;
- the plugin window. `OpenEditor` falls back to `juce::GenericAudioProcessorEditor`, which already gives sliders.

The changes needed are small:

- **Slot identity.** A slot's identity becomes either a file path (VST3) or a built-in id. The id is
  `builtin:nam`, `builtin:ir` or `builtin:airwindows/<registry name>`, with an `algorithmVersion`. Registry
  indices are never stored.
- **State.** A built-in block's state is its parameter values plus, for NAM and IR blocks, the SHA-256 of the
  asset. It is not an opaque blob.
- **Overlay command.** `AddPlugin`/`ReplaceSlot` accept a built-in id in `text`. The shared-memory layout does
  not change.
- **Custom editors.** A built-in editor (a model picker, an IR picker, named knobs) can replace the generic one
  later without touching anything else.
- **Mono input.** NAM is mono. A block before the stereo split takes L, which equals R in `Chain::Process`.
  Airwindows blocks are stereo.
- **Local-only VST blocks.** `PluginHost` blocks that load a VST3 stay local only and are never exportable.

### 4.2 Scene set and real-time switching (Proposal)

The fixed slot list in `Chain` becomes a `SceneSet` for song playback:

- **Build every scene before the song starts.** At song selection, the host builds and `Prepare`s up to five
  graphs (base, A–D) on the main thread. That covers model parsing, `Reset` and prewarm.
- **Duplicate shared models.** Scenes that use the same model get **separate DSP instances**, because core has no
  shared weights. A standard model is about 55 KB of float weights, so duplicating is cheaper than adding sharing.
- **Publish once.** The finished `SceneSet` is published to the audio thread with one atomic pointer exchange.
  The old set is destroyed on the main thread after the audio thread acknowledges, using a generation counter.
  `Pause()` is not used.
- **Switch inside the audio thread.** The audio thread reads the target slot word (§5.2) at the start of each
  block. When it changes:
  - **Crossfade.** It runs the outgoing and incoming graphs together for a short equal-power crossfade. Start at
    30 ms and tune by listening.
  - **Spill over tails.** Time-based blocks in the outgoing scene keep running with their input muted until they
    fall below -60 dBFS or reach a cap (2 s to start with). This matches common hardware modeller behaviour.
    Non-time-based blocks stop at the end of the fade.
  - **Bound the cost.** At most one active graph, one fading graph and one tail set run at a time. A second switch
    during a fade jumps straight to the new target and drops the oldest tail.
- **Warm-up of idle scenes (Unproven).** A scene that has been idle has stale history. WaveNet receptive fields
  are short (of order 100 ms), so the crossfade should hide that. If it does not, feed the incoming graph live
  input for one receptive field before the fade starts, which costs one extra block of switch delay. Decide by
  listening.
- **Tone Designer editing.** The editing page keeps today's chain. A one-scene `SceneSet` can replace it later.

### 4.3 Levels, calibration and headroom (Proposal)

These matter more for NAM than for VSTs, because a capture's drive depends on the absolute input level.

- **Input reference.** Define one host constant: the dBu at the instrument that reads 0 dBFS in the host, per
  input path (Real Tone Cable, or an interface at the player's calibrated level). When a model has
  `input_level_dbu`, set the model's input gain so the same capture is driven the same way on every machine. The
  player's global input trim stays on top of that, as today.
- **Scene loudness.** Use `metadata.loudness`, or measure it at import with a fixed test signal when it is
  missing. Store a per-scene output gain so switching scenes does not jump in level, and let the creator offset
  it on purpose. This is authored data and travels with the bank. The player's output trim stays global, as
  `SaveTone` already decides.
- **Headroom.** Before the existing hard clamp in `BridgeAudio::Render`, add a transparent safety limiter at
  about -1 dBFS. Make it explicit and metered; it is not a silent fix. Stacked reverb tails and a hot model must
  not clip harshly. `LimiterDsp.h` in the proxy is only a static trim and does not do this.
- **Denormals.** Set FTZ/DAZ on the host's audio thread (`juce::ScopedNoDenormals` or `_mm_setcsr`). Airwindows'
  own dither does not cover NAM or the convolution code.

### 4.4 Cabinet IRs (Proposal)

- **Format and length.** Store IRs as mono or stereo float32 WAV at 48 kHz, truncated at import to a fixed
  maximum. 8,192 samples (170 ms) matches the NAM plugin's limit.
- **Size.** That is 32 KB per mono IR, smaller after compression. A typical 200–500 sample minimum-phase cab IR
  is a few KB.
- **Engine.** Run IRs through core `Linear`.
- **Double cabinets.** An amp-plus-cab NAM capture (often flagged in metadata, otherwise the creator says so)
  marks its scene so the editor warns before a second cabinet is added.

### 4.5 Block size and sample rate (Proposal)

- **Block size.** Call `Reset(48000, frames)` with the game's actual block size. Rebuild the `SceneSet` when the
  bridge requests a reset (`BridgeAudio::kResetMessage`). Process at the prepared size only, as `Chain` already
  does.
- **Sample rate.** At import, **refuse** a neural model whose `sample_rate` is not 48,000, or is missing, with a
  clear message. Do not wrap it in a resampler silently: a resampler adds latency and CPU, and nearly all NAM
  models are 48 kHz. IRs are resampled at import, because that is cheap and loses nothing audible.

### 4.6 CPU budget (Unproven; measure first)

- **Budget.** Worst case is one standard WaveNet, one IR and four Airwindows effects, doubled during a crossfade,
  plus one tail set. All of it must finish in under about 50% of the block period at 64 and 128 frames, on a
  mid-range CPU, measured as **99.9th-percentile block time**.
- **Fallback.** If A2 misses that budget, the creator can lower `SetSlimmableSize`. The setting is stored per
  scene and applied at prepare time; it is never changed in real time.

---

## 5. Game DLL: arrangement identity and tone slot

### 5.1 Arrangement identity (Unproven)

`GetSongKey` identifies the song only. The bank is keyed by arrangement `PersistentID`, so the DLL must find
which arrangement is playing. Candidates, in order:

1. **Song object.** Read the selected arrangement from the song object already captured by the
   tone-component detour. Reverse-engineering is needed.
2. **Manifest tone keys.** Read the current arrangement's tone keys from the game and match them against the
   manifest's `Tone_Base`..`Tone_D` for each arrangement of the song key. This is ambiguous when lead and rhythm
   share tones, but harmless then, because the scenes are the same.
3. **Player pick.** If neither works, the player picks the arrangement in the overlay once, and the choice is
   remembered per song key. This is shown as a state the player can see, not used silently.

### 5.2 Current tone slot (Unproven, small spike)

- **What it is.** Static analysis found it at `component+0x10`: raw 0..3 = A..D, 5 = base
  (`VstHost/Documentation/Game-tone-slot-evidence.md`). The game itself looks up the chart's tone events from
  the song clock, which makes it seek-aware. The live check is still outstanding. The tone-change notification
  (type 7) is only a confirmation signal, because the game delays the readout by about 0.5 s.
- **How to read it.** Read it in the existing `TonesUpdateDetour` with guarded reads and the same vtable
  identity checks `PlayingSongObject` uses. Publish `{arrangementId, slot 0..4, sequence}` to the host.
- **New shared field.** Add a single atomic word in a new shared-memory version (`v10`, per the file's
  one-name-per-version rule), next to the command channel rather than through it. The host's audio thread reads
  it lock-free.
- **Expected latency.** One game frame (about 16.7 ms at 60 fps) plus one audio block. Compared with the 30 ms
  crossfade and the chart's own coarse tone-change placement, that is inaudible as timing error. It is still
  Unproven until measured against the game's own amp switching.
- **What the game provides.** Seeks, Riff Repeater loops, restarts, practice speed and pause all come from the
  game choosing its slot. After a seek, the host just follows the new slot. On pause the stream stops; on resume
  the crossfade covers any discontinuity.

### 5.3 Song start and end

- **Selection.** `FollowSong` stays. When the arrangement is selected (pre-play), the overlay sends one
  `PrepareSceneSet` command with the bank hash and the arrangement id. That is a command, because it is not
  timing-critical.
- **Start and end.** The host builds the set while the old tone plays, and the first slot word switches to it at
  song start. At song end the host returns to the player's chain.

---

## 6. Bank format and lifetime

### 6.1 Container (Proposal)

The bank is a **ZIP** file (extension `.rabtones`) with this layout:

```
manifest.json
assets/<sha256>.nam
assets/<sha256>.wav
LICENSES/<sha256>.txt        (optional: attribution or permission text an asset requires)
```

- **Why ZIP.** Every tool can read it, and the GUI already has inflate code. Entries are stored *uncompressed*
  when the bank is embedded in a PSARC, because PSARC entries are compressed already. Entries are
  deflate-compressed in a standalone file.
- **Integrity.** Each asset's file name is the SHA-256 of its exact original bytes. The importer recomputes it;
  a mismatch rejects the bank.
- **Deduplication.** Each asset is stored once and referenced by hash from any number of scenes.

### 6.2 `manifest.json` sketch (Proposal)

```json
{
  "format": "rab-tonebank",
  "formatVersion": 1,
  "engine": { "namCore": "0b3d3c97", "airwindows": "dcf98a3e", "minModVersion": "4.3.0" },
  "assets": {
    "9f2c…": { "kind": "nam", "bytes": 665758, "sampleRate": 48000, "architecture": "SlimmableContainer",
              "includesCabinet": false, "attribution": "…", "permission": "author-original" },
    "41ab…": { "kind": "ir", "bytes": 4140, "sampleRate": 48000, "channels": 1, "attribution": "…", "permission": "cc-by-4.0" }
  },
  "scenes": {
    "clean": { "outputDb": -1.5, "blocks": [
      { "kind": "airwindows", "name": "Gatelope", "version": 1, "params": [0.2, 0.5, 0.1, 1.0] },
      { "kind": "nam", "asset": "9f2c…", "inputDb": 0.0, "slimmable": 1.0 },
      { "kind": "ir", "asset": "41ab…" },
      { "kind": "airwindows", "name": "Chamber2", "version": 1, "params": [0.3, 0.4, 0.5, 0.2], "mix": 0.25 } ] },
    "lead": { "outputDb": 0.0, "blocks": [ … ] }
  },
  "arrangements": {
    "<PersistentID>": { "label": "Lead",
      "slots": { "base": "clean", "A": "clean", "B": "lead", "C": null, "D": null },
      "gameToneKeys": { "base": "MySong_Clean", "A": "MySong_Clean", "B": "MySong_Lead" } }
  },
  "transitions": { "crossfadeMs": 30, "tailCapMs": 2000 }
}
```

- **Null slots.** A `null` slot keeps the current scene. The arrangement's chart never selects it anyway.
- **Stored tone keys.** `gameToneKeys` is kept so a bank can be checked against the PSARC's manifest at import,
  which catches a bank paired with the wrong chart version.
- **Exact parameter values.** Parameters are stored as Airwindows' normalised 0..1 floats exactly as set, plus
  the effect `version`, so a later Airwindows change can be detected.

### 6.3 Where banks live (Proposal)

- **Embedded:** a PSARC entry at a fixed path, for example `tones/rab/<dlckey>.rabtones`.
  - **Writing:** a GUI export step. Rocksmith2014.NET and RocksmithToolkitLib can both rewrite PSARCs.
  - **Unproven:** that the game ignores an unknown entry. It probably does, since many CDLCs carry extra files,
    but an in-game test must confirm it before this is the default.
- **Sidecar:** `<psarc name>.rabtones` next to the PSARC. It needs no repacking and is the development and
  fallback form. When both exist, the embedded bank wins and the overlay names the source.
- **Discovery:** the GUI's existing PSARC indexer (`ToneDesignerTools.cs`) also lists bank entries and sidecars.
  It records the arrangement `PersistentID`s it already reads past, and extracts each bank to
  `%LOCALAPPDATA%\Rocksmith Audio Bridge\Banks\<sha256>\`, keyed by content.

### 6.4 Export rules (Proposal)

- **Only exportable blocks.** Export refuses any scene that contains a `VstProcessor` and names the block. Such a
  bank is never labelled portable.
- **Walk every scene.** Export walks every scene of every arrangement, hashes each asset, and copies each one in
  once.
- **Permission is required.** Each imported asset carries `attribution` and `permission` fields, filled in when
  it entered the managed library. Export refuses an asset whose permission is unset; there is no default. See
  §9 for why.
- **Gate settings.** Today's `ToneGates.ini` behaviour (on the 4.3 branch) becomes an ordinary gate block in the
  scene, so it travels with the tone. Any player-level gate stays global, like the trims.

### 6.5 Asset lifetime and engine compatibility

- **Lifetime.** Once embedded, a model no longer depends on any website, file path or account.
- **Pinned engine.** Each release pins NAM core and airwin2rack commits.
- **Reference renders.** A `fixtures/` set of banks with reference renders (fixed DI input, rendered output) is
  checked on every engine bump. Any change beyond a numeric tolerance is either fixed or recorded as a new
  `algorithmVersion` that keeps the old code path.
- **No substitutes.** A bank that needs an unknown `algorithmVersion` fails to load with a message. It is never
  given a substitute sound.

### 6.6 Later: authored extra switches

If creators later want switches the chart lacks, add `"events": [{ "timeSec": 63.2, "scene": "lead" }]` per
arrangement, driven by the song clock the DLL already reads (`SONG_CLOCK`). Only then does the design need its
own timeline, with its seek and loop handling. Keep this out of version 1.

---

## 7. Product design: tabs, tones and song tone sets (Proposal)

## 7.0 Tone designer UI (Revision 4, agreed 26 October 2026)

The Tone designer keeps today's page layout:

- the tone list on the left;
- the pedalboard in the middle;
- the browser and the movable divider below.

In Tone designer mode every pedal is one **Stompbox effect**.

- **Board.** One horizontal chain from IN to OUT. Long chains scroll. Each pedal shows:
  - its name and colour (Stompbox's `BackgroundColor`/`ForegroundColor`);
  - a small icon;
  - the file it uses, if any ("G5 Crunch UK" on a NAM pedal, "British 4×12" on a Cabinet);
  - an on/off LED.

  Drag to reorder, and click the footswitch to bypass. A pedal that is loading a model shows a spinner, and the
  overlay stays responsive.
- **Browser.** Three groups:
  - **Effects:** Stompbox's user-selectable factory list.
  - **Amps:** NAM captures in the library.
  - **Cabinets:** IRs in the library.

  Drag an item between two pedals, or use *Add to chain* or *Replace selected*. An amp or cabinet item creates a
  NAM or Cabinet pedal with that file already loaded. Effects Stompbox adds later appear without UI work, because
  the list comes from the engine (`StompboxEngine::ListPlugins`).
- **Editing.** Double-click a pedal to open its editor **inside the overlay**, not as a separate window, so it
  works in exclusive fullscreen.
  - One generic panel is built from the parameters the engine reports: name, range, default, taper, display format
    and type (Knob, Bool, Int, VSlider, Enum, File, Power).
  - File parameters get a picker over the library.
  - VSlider parameters, such as EQ-7's bands, are drawn as sliders.
  - There is no per-effect UI code.
- **Order.** Free. Default insertion is a hint: a cabinet goes after the amp, and a gate near the input. There are
  no hard rules.
- **Limits.** At most two NAM pedals per tone, with a warning (see the CPU measurements in
  `VstHost/Documentation/Implementation-status.md`).
- **Modes.** The Tone designer and the VST rig use the same page, with their own libraries and saved tones. VST rig
  pedals keep their plugin windows.
- **Engine mapping.** The board is one Stompbox chain, `SetChain Main NoiseGate NAM EQ-7 Cabinet Delay Reverb`, and
  Stompbox runs it in board order. Several copies of one effect are separate instances (`Delay`, `Delay_2`). A saved
  tone is the engine's program text (`StompboxEngine::DumpProgram`: its `SetChain` and `SetParam` lines) plus the
  model and IR files it names.
- **Still to design: the overlay ↔ host link.** Today's command channel carries edits. The plugin list, the chain
  and parameter values are larger text, and need a new shared-memory block or a file the host writes. Decide this
  together with the three-tab overlay work that is not yet on the branch.

### 7.1 Three tabs, one host

The overlay already has a tab row, `Tone designer | External app` (`OverlayShell.cpp`). It becomes:

| Tab | What it plays | Pedal windows | Export with CDLC | Backup/restore |
|---|---|---|---|---|
| **Tone designer** (new) | Built-in blocks only: NAM amp, cab IR, Airwindows | None. Knobs are drawn on the board in the overlay. | **Yes** | Yes |
| **VST rig** (today's page, renamed) | VST3 plugins | Each plugin's own window, as today | **No, never** | Yes (`ToneBackup`, as today) |
| **External app** (unchanged) | AmpliTube, Neural DSP and so on, over the amp link | The app's own | No | No |

Rules:

- **One source plays at a time.** The choices are a built-in tone, a VST rig tone, an external app or the game
  amp. The existing `VST | Game amp` switch in the bar becomes the source switch.
- **No mixing in one chain.** A built-in tone never contains a VST block, so exportability is never a per-block
  question. A tone is portable by construction, and the export button needs no checks or warnings.
- **Shared page code.** Both tabs run in the same host process and use the same `Chain` (built-in blocks are
  JUCE processors, §4.1). The overlay reuses the page's parts with a source parameter:
  - the bar (tone name, previous/next arrows, the unsaved dot);
  - the tone library (favourites, folders, search);
  - the pedalboard (jacks, cables, footswitch bypass, drag to reorder);
  - the IN/OUT meters and the Songs pane.

  What differs per tab:
  - the bottom browser: VST plugins, or the built-in block list plus the model and IR library;
  - what a double-click does: open the plugin window, or edit the knobs in place.

### 7.2 Built-in blocks on the board (superseded by Revision 3: the plugins' own windows)

- **Pedal looks.** The board's pedal looks (`LookFor`, by `Kind`) gain built-in kinds: Amp (NAM), Cab (IR),
  Dynamics (gate, compressor), Drive, Modulation, Delay and Reverb.
- **Real knobs.** A built-in pedal's knobs are its real parameters, drawn and dragged in the overlay. That gives
  the native feel, with no window over the game.
- **Parameter feedback.** The host publishes the selected slot's parameter list in shared memory: name,
  normalised value and display text such as "320 ms", for up to 16 parameters.
- **Edits.** The overlay sends a `SetParameter(slot, index, value)` command, coalesced to one per frame while a
  knob is dragged. This is editing, not timing, so the ordinary command channel is fine.
- **Amp pedal.** The amp pedal shows the capture name plus Input and Output. A model picker opens from the pedal,
  listing the managed library with clean, crunch and lead captures grouped per amp.
- **Cab pedal.** It has an IR picker and a Mix knob.

### 7.3 What a "tone" and a "song tone set" are

- **Tone.** One chain of up to 8 built-in blocks. This is what the board edits.
  - Stored locally as `<name>.rabtone` (JSON: blocks, parameters, asset hashes) in the built-in tones folder,
    which is separate from the VST rig's `RABTONE2` files.
  - Assets live once in the managed library (§6.3), keyed by SHA-256.
- **Song tone set.** One per CDLC. It holds, for each arrangement, which tone plays on each of the game's tone
  slots:

```
Song tone set "Artist - Song"
  Lead   (PersistentID 3F2A…)   base: Clean   A: Clean   B: Lead solo   C: —   D: —
  Rhythm (PersistentID 9C01…)   base: Clean   A: Crunch
  Bass   (PersistentID 11B7…)   base: Bass DI+Amp
```

**Yes to one file per CDLC.** The song tone set exports as **one file** that holds:

- every arrangement's slot map;
- every tone it uses, each stored once;
- every model and IR those tones use, each stored once.

That file is the bank in §6 (`manifest.json` "scenes" are these tones). Guitar and bass arrangements are simply
separate entries in the same file.

Convenience actions, not new data:

- **Copy from Lead:** fills Rhythm with the same tone references.
- **Same tone for every slot:** for songs that never change tone.

### 7.4 Creating song tones

1. In the Songs pane, right-click a CDLC and choose **Edit song tones**. The bar then shows
   `Artist - Song · Lead · Slot B (Tone_B key name)`, with arrangement and slot pickers.
2. Pick a tone from the library for each slot, or build one on the board. Unassigned slots keep playing the
   previous tone, the same as the game.
3. Audition in game with the 1–4 hotkeys. They change the game's slot, so they test the real switching path (§5.2).
4. Choose **Export song tones** to write `<psarc name>.rabtones` next to the CDLC. Embedding it inside the PSARC
   can come later (§6.3).

### 7.5 What plays when a song starts

In order:

1. **The player's own choice for this song,** if they set one. It can be a built-in tone, a VST rig tone, the
   external app or the game amp. This is today's per-song assignment, extended to say which tab it comes from.
2. **The song's bundled tone set,** if the CDLC has one and the "Play tones bundled with songs" setting is on
   (default on). This uses the arrangement's slot map and mid-song switching.
3. **The player's default.** This is today's `DefaultTone`, extended the same way.

The overlay always says which of the three is playing, for example `Bundled: Lead · Slot B "Lead solo"`, so a
song never sounds different without the player being able to see why.

### 7.6 VST rig tab: what stays local

- **No change to how it works.** Plugin windows, spares, preloading, `RABTONE2`, song assignments and backups stay
  as they are today. Song tones remain song-start only.
- **Explaining the limit.** The VST rig tab has no Export action. Its right-click menu for a song shows "Song tones
  for sharing are made in the Tone designer tab" instead.
- **Restoring.** A backup restores both tabs' tones and song assignments, and the managed model and IR library.

---

## 8. Implementation phases

| Phase | Deliverable | Removes risk |
|---|---|---|
| 0a — spike | Read the current tone slot in `TonesUpdateDetour` and log it on every change, across chart events, seeks, Riff Repeater and the 1–4 keys. Also try to read the arrangement identity. | §5: the whole switching design depends on this. If it fails, fall back to the handover's own-scheduler plan, using SNG tone events and the song clock. |
| 0b — spike | Build NAM core and the airwin2rack subset into the host under MSVC v143 with `stdcpp17`. Play one NAM model, one IR and one Airwindows reverb through `Chain`. Measure 99.9th-percentile block time at 64 and 128 frames. | §2.2 build claim, §4.6 CPU budget. |
| 1 | Built-in blocks as JUCE processors in the existing chain (§4.1), saved as a new local tone format (`RABTONE3`, JSON plus asset hashes). | |
| 1b | Tab split: today's page becomes **VST rig**, and a new **Tone designer** tab gets on-board knobs (§7). | |
| 2 | Managed asset library, and bank export/import as a sidecar. Includes the offline-disappearance test (§12). | |
| 3 | `SceneSet`, shared memory v10 slot word, crossfades and spillover tails. | |
| 4 | PSARC-embedded banks, the indexer and `PersistentID` mapping. In-game test of unknown entries. | |
| 5 | Acceptance on a clean machine (§12). | |

---

## 9. Licensing and asset rights

- **Code.**
  - NAM core: MIT.
  - airwin2rack and Airwindows: MIT.
  - FFTConvolver: MIT, if it is ever needed.
  - NAM core's nlohmann/json and Eigen: MIT and MPL2 respectively. Audit the pinned Eigen, which is a master
    snapshot labelled 3.5.0 rather than a release.
  - Keep notices in `VstHost/THIRD-PARTY-NOTICES.txt`. The host stays AGPLv3 because of JUCE.
  - **Correction:** `juce_dsp` is AGPL/commercial like every JUCE module. It is not ISC.
- **Content.** Captures and IRs carry their own rights, and engine licences grant nothing about them.
  - TONE3000's terms leave ownership with the uploader.
  - Its sharing guidelines restrict captures of current plugins and modellers on that platform.
  - Hence the mandatory `permission` field.
  - A factory library of original captures and IRs, plus Airwindows-only tones (GrindAmp/LeadAmp plus Cabs, with
    no asset bytes at all), gives creators a zero-rights-risk starting point.

---

## 10. Out of scope: converting local VST rigs

This design does not convert third-party VST chains or external amp apps. Tones are built from the built-in
blocks, which is what makes them exact and portable.

A future converter could render each hosted plugin offline:

- cabinets and EQ become an IR;
- amps and drives become a NAM model;
- delays and reverbs are rebuilt with the built-in effects.

Delays and reverbs would only be approximations, so a converter like this is not planned. Nothing in this design
depends on it.

---

## 11. Fact corrections to the handover

- **Line numbers.** The handover's line numbers refer to the uncommitted `feature/4.3-features` work. On the
  pushed branches some files do not exist, for example `DLL/ToneDesigner/ToneGates.cpp`.
- **C++ standard.** NAM core CMake asks for C++20 but marks it not required (`CMAKE_CXX_STANDARD_REQUIRED OFF`).
  The sources are effectively C++17.
- **Loading API.** NAM core has no in-memory-buffer `get_dsp`; load from memory through parsed JSON.
  `ResetAndPrewarm` is deprecated, and `Reset` prewarms by default.
- **Architectures.** Add `Sequential` to the list.
- **Sample rate.** NAM core does not resample neural models; only `Linear` IRs are resampled.
- **Weights.** Weights are per instance; "share immutable weights" (handover §8) is not supported as is.
- **nam-binary-loader.** It does not support A2 and does not build against current core.
- **Airwindows subset.** There is no CMake subset switch. Every effect is stereo in and out. The library does not
  set FTZ/DAZ.
- **Airwindows candidates.** Dynamics3, Galactic3, Gatelope and the amp-sim category are worth adding.
- **RS2014 identity.** RS2014 provides an arrangement `PersistentID`. Tone events are id-based (0–3 → A–D); the
  base tone is separate.
- **`juce_dsp`.** It is AGPL/commercial, not ISC.

---

## 12. Acceptance checklist

The handover's §12 list stands. These items are added or sharpened:

- **Phase 0a log.** It shows the slot read changing exactly at chart tone events, after seeks, in Riff Repeater,
  at 50% speed and on keys 1–4, for lead, rhythm and bass.
- **Block-time distribution.** Measure the 99.9th percentile and the maximum, not the mean, with two graphs and a
  tail set running at 64 and 128 frames. There must be zero AmpLink late blocks over a 30-minute session.
- **Level matching.** Switching between scenes with a `loudness`-derived gain stays within ±1 LU on a fixed DI
  phrase. The safety limiter never engages on a normal performance.
- **Offline-disappearance test.** Export a bank, delete the library and the original files, disable the network,
  and play from the extracted bank only.
- **Clean machine.** Play a multi-tone CDLC with an embedded bank on a machine that has the mod only. The
  game must accept the PSARC with the extra entry.
- **Engine bumps.** Reference renders for each fixture bank are identical within tolerance across an engine
  update.
- **Bad imports.** A non-48 kHz model, a 2-channel model or an unknown architecture is refused at import with a
  specific message.
- **Leaks.** Rebuilding the `SceneSet` (block size change, song change) leaks no instances. Old sets are
  destroyed only after the audio thread releases them.

---

## Sources

- **This repository:**
  - `VstHost/Source/Chain.{hpp,cpp}`, `BridgeAudio.{hpp,cpp}`, `PluginHost.cpp`
  - `DLL/Shared/VstHostControl.hpp`
  - `DLL/ToneDesigner/VstHostLink.cpp`, `ToneSwitchPopup.cpp`
  - `DLL/AsioProxy/AmpLink.hpp`, `LimiterDsp.h`
  - `DLL/GameState.cpp`
  - `GUI/Features/ToneDesigner/ToneDesignerTools.cs`
- **Upstream source:**
  - [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) `0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842`
  - [neural-amp-modeler](https://github.com/sdatkinson/neural-amp-modeler) v0.9.0 `102333bf` and `0072676`
  - [NeuralAmpModelerPlugin](https://github.com/sdatkinson/NeuralAmpModelerPlugin) `16be8697`
  - [AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools) `e19ef4b5`
  - [nam-binary-loader](https://github.com/tone-3000/nam-binary-loader) `92c9e85b`
  - [airwin2rack](https://github.com/baconpaul/airwin2rack) `dcf98a3e`
  - [Rocksmith2014.NET](https://github.com/iminashi/Rocksmith2014.NET) `4cf33251`
  - [FFTConvolver](https://github.com/HiFi-LoFi/FFTConvolver) `f2cdeb04`
  - [JUCE licence](https://github.com/juce-framework/JUCE/blob/master/LICENSE.md)
- **Research papers:** as listed in the handover's §9.2. They were not re-reviewed here.
