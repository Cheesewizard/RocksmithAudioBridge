# Portable tones: implementation status

Branch `dev_portable-tone-designer`. This is a partial implementation, not the finished portable Tone Designer
feature.

**Design:** [PortableTones-Design.md](../../docs/ToneDesigner/PortableTones-Design.md). **Revision 4 (current
direction):**

- **Engine:** the Tone designer runs the **Stompbox engine** (NAM amps, tone stacks, cabinets and effects) inside
  the host.
- **Window:** the shipped window is drawn natively in the overlay.
- **Development stand-in:** Stompbox's own desktop remote is used only during development.

Revision 3's bundled-plugin approach is superseded.

**Implementation guide:** [PortableTones-Implementation.md](../../docs/ToneDesigner/PortableTones-Implementation.md).

## Removed after Revision 3

Revision 3 replaced the earlier prototype of custom built-in processors. The prototype is removed from the
branch; it remains in the history at commit `128b93f`.

- **Processors:** `VstHost/Source/Builtin/` (NAM amp, cab IR and Airwindows processors, the asset library and
  the JSON tone format).
- **Vendored libraries:** NeuralAmpModelerCore and airwin2rack under `VstHost/Lib/`, `VstHost/Builtin.props`, and
  their notices in `VstHost/THIRD-PARTY-NOTICES.txt`. The bundled plugins are built from their own repositories
  and ship with their own notices (guide §2.1).
- **Harness:** `VstHost/Tools/BuiltinCheck` (`--portable-self-test`), and the built-in branches in `Chain`,
  `PluginHost` and `Main.cpp`.

## Kept

- **`VstHost/Source/SourceRouter.{hpp,cpp}`:** the router, safety limiter, block counter and allocation-free
  timing histogram (guide §2.3 and §2.5). Not yet connected to the player's audio chain.
- **`Chain::LatencySamples` and `PluginHost::LatencySamples`:** part of the 4.3 latency work.
- **M0a diagnostics in the DLL:** see `Game-tone-slot-evidence.md`.
- **`Plugin-browser-responsiveness.md`:** the asynchronous plugin scan.

## Milestones

| Milestone | State | Remaining |
| --- | --- | --- |
| M0a | Static evidence: `component+0x10`, raw 0..3 = A..D, 5 = base. Signature-guarded diagnostic reader logs slot, clock and key. | Live chart, seek, Riff Repeater, speed and hotkey checks. Arrangement PersistentID. |
| M0b | Offline cost of NAM core measured (below) | Load the bundled NAM and Airwindows Consolidated VST3s in the host. Timing on the live callback. |
| M1–M6 | Not started under Revision 3 | Everything in the guide |

## Offline timing (prototype harness, still valid for NAM's cost)

These numbers were measured on an AMD Ryzen 7 7840HS, as a normal-priority offline process, 20,000 blocks per
case.

- **One graph:** A2 amp, stereo 8,192-frame IR, Chamber2 and TapeDelay2.
- **Two graphs:** two complete chains.
- **Values:** 99.9th-percentile block time in microseconds.

| A2 quality | Graphs | 64 frames | 128 frames |
| --- | --- | ---: | ---: |
| Full (1.0) | 1 | 956 | 1578 |
| Full (1.0) | 2 | 1602 | 2380 |
| Reduced (0.0) | 1 | 576 | 934 |
| Reduced (0.0) | 2 | 954 | 1584 |
| Half-block budget | | 666.7 | 1333.3 |

The full reports are `Offline-A2-full.json` and `Offline-A2-lite.json`.

- **Why it still applies:** the bundled NAM plugin runs the same core, so these costs carry over.
- **Consequence:** two NAM instances must never run at the same time. Scene switches use a short dip instead of an
  overlapping crossfade, and only non-NAM plugins ring out as tails (guide §6.2).
- **Limit:** these offline measurements do not prove live ASIO deadline behaviour.

## Stompbox engine (Revision 4)

### Engine (host)

- **`VstHost/Stompbox/`:** `UPSTREAM.md` and `Build-Stompbox.ps1`. The script builds `stompbox-capi.dll` from
  commit `17bc652` with three checked patches:
  - the remote server listens on 127.0.0.1 only;
  - the circuit-modelled `Tonestack` pedal is registered;
  - `DumpConfig` is exported from the C API.
- **`VstHost/Source/StompboxEngine.{hpp,cpp}`:** loads the DLL, creates the engine on
  `%APPDATA%\RocksmithAudioBridge\Stompbox` (`NAM\`, `Cabinets\`, `Reverb\`, `Presets\`), and prepares it with
  the stream stopped. It processes mono into stereo, and exposes commands, `DumpConfig`, `DumpProgram`, presets and
  preset loading.
- **`VstHost/Source/StompboxService.{hpp,cpp}`:** loads the engine **when the host starts**, so the pedal
  catalogue is ready before the page opens. It runs the page's commands, republishes the program and presets, and
  routes the stream through `SourceRouter` when the page asks. The engine processes only while it plays; otherwise
  the VST chain plays exactly as before.
- **`StompboxDev` = 1** (development): Stompbox keeps its own layout and starts its local server for
  StompboxRemote.

### Overlay (DLL)

- **`DLL/Shared/StompboxControl.hpp`:** a shared block of its own (`...Stompbox.v1`), so `VstHostControl` is
  untouched. It carries:
  - `wanted`, set by the overlay;
  - one Stompbox command line at a time;
  - text blocks under a sequence lock: the catalogue (`DumpConfig`), the program (`DumpProgram`) and the presets.
- **`DLL/ToneDesigner/StompboxLink.{hpp,cpp}`:**
  - parses the catalogue and the program;
  - queues commands, keeping only the latest value of a knob being dragged;
  - holds back a program arriving mid-edit;
  - remembers the player's choice (`HKCU\...\VstHost ToneDesignerEngine`).

  `VstHostLink::OnFrame` drives it, so the choice applies while the page is closed.
- **`DLL/Overlay/StompboxPage.{hpp,cpp}`:** the agreed pedalboard (design §7.0).
  - **Library and board:** saved tones on the left; one pedal per effect, with its colour, file name, LED and
    footswitch; drag to reorder or drop from the browser; a right-click menu.
  - **Browser:** Effects, Amps (NAM files) and Cabinets (IRs), each with search and a Folder button.
  - **Editor:** one generic in-overlay panel built from the parameter definitions. VSliders are drawn as a bank;
    advanced parameters are folded away; output meters are left out.
  - **Rules:** default insertion places (gate, amp, cabinet); spinners while a file loads. There is no pedal-count
    limit.
  - **Load meter:** measured live, once a second, by the host's block timing (`SourceRouter`, published through
    `StompboxControl` v2). Two figures:
    - **Load** is the typical block's share of real time. It barely changes with block size, so it is judged
      against the base game (480-frame blocks). A warning shows at 50%, and "heavy" at 70%.
    - **Slowest** is the worst block of the last second at this PC's block size. A warning shows at 85% or after
      late blocks. ASIO's 128 or 64 frames are a bonus, not the target.
- **`OverlayShell.cpp`:** the tabs are now **Tone designer | VST rig | External app**. Choosing Tone designer or
  VST rig also chooses what plays. VST rig is today's page, unchanged.

### Song tones (prepared tones, following the game)

- **How tones are keyed.** By the game's own **tone key** (`GameTones::CurrentToneKey`, `song+0x198`: the
  manifest's `Tone_Base`/`Tone_A..D` key), not the slot number or the arrangement. Arrangements that share a key
  share a tone, so the unresolved arrangement `PersistentID` is not needed.
- **Assigning, from the overlay only.** While a song is chosen, the page lists **every tone of every arrangement**
  (Base, A–D, with the names players see) and gives each one a saved-tone picker. Nothing has to be reached in the
  game first, and there is no hotkey limit. The game's current tone is marked "(now)".
  - **Where the list comes from:** the song list (`RSMods.exe --song-index`, `ToneDesignerTools.cs`) now also
    writes `song-index.tones.tsv` from each arrangement manifest's `Tone_Base`/`Tone_A..D` keys and `Tones` names.
    `SongLibrary` reads it into `Song::tones`. The cache key is now v3, so every PSARC is read once more.
  - **The command:** the page's own `SongTone "<song key>" "<tone key>" "<saved tone>"`. It works for any song,
    not just the one playing.
  - **Storage:** per song, in `%APPDATA%\RocksmithAudioBridge\Stompbox\SongTones\<song key>.txt`.
- **Preparing (host, `StompboxService`).** When the song is chosen, and only while the Tone designer plays, every
  assigned saved tone is loaded into an engine of its own at the stream's block size. Each is run on silence on the
  main thread until Stompbox has loaded it and it has settled for 0.5 s. Stompbox loads presets only inside its own
  processing. The prepared tones then become the stream's sink (`SceneSet`).
- **Playing (`SceneSet`).**
  - Only the active tone is processed. Scene 0 is the board's own engine, which plays for a game tone without an
    assignment until an assigned one comes.
  - A switch fades out over 10 ms, then in over 10 ms, so two amps never run on the guitar at once.
  - The outgoing tone rings out on silence for 2 s, with its NAM pedals bypassed meanwhile, so tails cost only the
    effects after the amp. The pedals are switched back on before that tone is used again.
  - A buffer-size change loads the song's tones again at the new size.
- **Shared block.** `StompboxControl` v3 carries the song and tone keys from the overlay, and the song's
  assignments, its state (preparing, ready or failed) and the tone playing from the host.

### Verification

Nothing has been built or run. The new C++ files passed `clang++ -fsyntax-only` with the real ImGui headers and
stand-in Windows headers. `Main.cpp` and `OverlayShell.cpp` were not compiled.

### Known gaps

- **New files:** models and IRs added while the host runs appear after it restarts. Stompbox lists its folders
  when the engine loads.
- **Removed pedals:** they stay loaded in Stompbox's factory, and are not processed. Releasing one straight after
  `SetChain` could free it while the audio thread still runs the previous chain.
- **Song-start switching:** VST rig song tones still load at song start while the Tone designer plays (harmless,
  but wasted work).
- **Rapid switches:** a switch while a tail rings out cuts the older tail.
- **Editing during a song:** the board edits its own engine (scene 0). A saved tone the song uses is heard as
  edited once it is saved, because saving reloads the song's tones.
- **Overlay tab work:** the three-tab layout in your local build may differ from this one; merge them.

## Next work

1. Build `stompbox-capi.dll` with the script, then build the host and the DLL. Check the Tone designer tab
   end to end: add pedals, load a NAM model and an IR, edit, save and load tones, and watch the `stompbox:`
   block-timing lines at 64 and 128 frames.
2. **Song tones, live** (implemented, not run):
   - check that the tone key changes exactly at chart tone events, seeks, Riff Repeater, 50% speed and the 1–4 keys
     (the `(GAME TONES)` log lines);
   - listen to the 10 ms dip and the tails;
   - check that the load meter stays sane while a tail runs.
4. Song tone sets and export (design §6–§7), storing Stompbox preset text plus the files it names.
