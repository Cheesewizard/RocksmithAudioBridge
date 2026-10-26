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
  - **Rules:** default insertion places (gate, amp, cabinet); at most 2 NAM pedals; spinners while a file loads.
- **`OverlayShell.cpp`:** the tabs are now **Tone designer | VST rig | External app**. Choosing Tone designer or
  VST rig also chooses what plays. VST rig is today's page, unchanged.

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
- **Overlay tab work:** the three-tab layout in your local build may differ from this one; merge them.

## Next work

1. Build `stompbox-capi.dll` with the script, then build the host and the DLL. Check the Tone designer tab
   end to end: add pedals, load a NAM model and an IR, edit, save and load tones, and watch the `stompbox:`
   block-timing lines at 64 and 128 frames.
2. **Prepared tones for songs** (agreed 26 October):
   - preload the active tone and every tone the current song uses before playback, each as its own prepared
     engine instance;
   - process only the active one, with controlled tails on a switch;
   - never keep every available capture running.
3. Validate the M0a tone-slot diagnostics in a live session, and resolve the arrangement identity.
4. Song tone sets and export (design §6–§7), storing Stompbox preset text plus the files it names.
