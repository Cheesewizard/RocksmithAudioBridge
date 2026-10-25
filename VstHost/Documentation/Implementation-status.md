# Portable tones implementation checkpoint

25 October 2026. Branch `dev_portable-tone-designer` (renamed from `codex/portable-tone-designer`).
This is a partial implementation, not the completed portable Tone Designer feature.

The worktree was created from current HEAD and overlaid with all 36 locally modified
tracked files and the untracked research handover. Every overlay copy was hash-checked.
Local documentation, tools, tests and research source were copied as well, excluding
build outputs. The original checkout remains on `feature/4.3-features`.
At this initial checkpoint, no commit, push, installation, game launch or deployment had been performed.
The diagnostic DLL and matching host were subsequently deployed for user testing.
The later plugin-scan responsiveness fix has been built but not deployed.

## Milestones

| Milestone | Implemented | Acceptance remaining |
| --- | --- | --- |
| M0a | Shared GameTones capture/validated owner reader; signature-guarded raw-slot/current-key/clock diagnostics; static evidence identifies component+0x10 | Live chart, seek, Riff Repeater, speed and hotkey checks; arrangement PersistentID |
| M0b | Pinned NAM core and 12-effect Airwindows subset build with MSVC v143/C++17; isolated offline functional and CPU harness | CPU transition budget not met; actual AmpLink late blocks and 30-minute acceptance untested |
| M1 | JUCE built-in processor adapters, strict versions, source separation in Chain, JSON .rabtone read/write, state-before-prepare loading and duplication | Player-facing host command/UI integration and live playback |
| M2 | Managed SHA-256 library, original NAM byte storage, strict rate/channel/model validation, IR conversion/truncation, rights persistence | Import/rights dialogs, cabinet flags, measured input calibration and authored loudness workflow |
| M3-M6 | Not implemented | Shared board extraction and tab split, on-board parameters, song index/set/bank/export/import, SceneSet switching/tails, fixtures and clean-machine acceptance |

The source router, limiter meter, block counter and allocation-free timing histogram
are implemented and exercised by the offline harness. They are not connected to the
player's current audio chain. No shared-memory layout change was made. The checkout
already uses v9; the next contract extension must use a new version (not the guide's v8).

## Verification

- Release x64 host and Release Win32 DLL build successfully with deployment disabled.
  Final build logs: `build-portable-host-final.log`, `build-portable-final.log` at the
  worktree root (ignored local build artifacts). Existing/dependency warnings remain.
- Isolated functional checks pass: known SHA-256 vector, deduplication, persisted rights,
  source-file deletion, bad/missing model rates, multichannel models, unknown architectures,
  unknown effects/algorithm versions, 48 kHz float32 IR conversion and 8192-frame truncation,
  all twelve effects producing finite output, analytic stereo NAM/IR rendering,
  local tone save/load, NAM duplication, source separation, router counter and limiter meter.
- A separate corruption check altered a managed asset and confirmed an explicit
  SHA-256 mismatch refusal before loading.
- `Game-tone-slot-evidence.md` records the unpacked capture identity and checked instructions.
  No proprietary game bytes were copied into the source tree.

## Offline timing

AMD Ryzen 7 7840HS, normal-priority offline process, 20,000 blocks per case.
One graph: A2 amp, stereo 8192-frame IR, Chamber2, TapeDelay2.
Two graphs: two independent complete chains, without a spillover tail pool.
Values below are p99.9 histogram upper bounds in microseconds (2 us resolution).

| A2 quality | Graphs | 64 frames | 128 frames |
| --- | --- | ---: | ---: |
| Full (1.0) | 1 | 956 | 1578 |
| Full (1.0) | 2 | 1602 | 2380 |
| Reduced (0.0) | 1 | 576 | 934 |
| Reduced (0.0) | 2 | 954 | 1584 |
| Design half-block budget | | 666.7 | 1333.3 |

Only the reduced-quality single-graph cases meet this offline half-block budget.
The complete reports, including medians, p99 and maxima, are adjacent JSON files.
A functional `status: passed` is separate from `cpuBudgetPassed: false`.
These normal-priority offline measurements do not prove live ASIO deadline behaviour.
The earlier standard WaveNet run overlapped compilation and is not used for acceptance;
its upstream example also omitted sample_rate and was correctly refused until a test-only
copy explicitly declared 48000. A2 was measured with its unmodified upstream bytes.

## Next work

1. Validate the M0a diagnostic build in a live game session and resolve arrangement identity.
   The diagnostic DLL is built but has not been installed into the game.
2. Profile/measure the intended transition workload on the real audio callback before enabling
   crossfades and tails; do not hide this failed offline margin behind average block times.
3. Continue the guide's M1/M2 integration, then the shared-board refactor, without changing
   the existing VST rig's local-only file format and song-start semantics.
4. Keep sidecar v1 delivery separate from the later, unproven PSARC embedding experiment.

## Updated design

The authoritative updated design is [PortableTones-Design.md](../../docs/ToneDesigner/PortableTones-Design.md).
Revision 3 chooses bundled NAM and Airwindows VST3s with their own editors, superseding
custom built-in processors and on-board controls. The native processors in this checkpoint
are earlier prototype work, not completion of that revised design. The documents copied
beside this status file are the earlier inputs, retained as historical context.
