# Portable tones: implementation status

Branch `dev_portable-tone-designer`. This is a partial implementation, not the finished portable Tone Designer
feature.

**Design:** [PortableTones-Design.md](../../docs/ToneDesigner/PortableTones-Design.md), Revision 3. The Tone
designer uses bundled, pinned builds of the NAM and Airwindows Consolidated VST3 plugins, with their own windows.

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

## Next work

1. Validate the M0a diagnostic build in a live game session, and resolve the arrangement identity.
2. Build the pinned NAM and Airwindows Consolidated VST3s. Load them through the existing host, and measure
   block times on the real callback (M0b).
3. M1: `bundled:` ids, the Tone designer chain and `SourceRouter` wired into the host.
