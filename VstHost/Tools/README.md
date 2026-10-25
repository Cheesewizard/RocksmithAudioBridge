# Isolated built-in tone checks

Build the Release x64 host with `DeployToGame=false`. The diagnostic DLL build also
requires `DeployToGame=false` and `PostBuildEventUseInBuild=false` to keep it build-only.

Run:

```text
RocksmithAudioBridgeVstHost.exe --portable-self-test <absolute-output-folder> [absolute-model.nam] [quality-0-to-1]
```

This mode is handled before the host's single-instance mutex, registry restoration,
shared mapping, audio driver and visible windows. It creates its own library under
the output folder and writes `report.json`. It returns 0 for passing functional checks,
1 for a functional failure. Read `cpuBudgetPassed` separately for benchmark acceptance.
The usual host log is opened, but the player's tones, library and settings are not edited.

Without a supplied model, CPU results use a synthetic Linear fixture and are not a
neural amp benchmark. The supplied model must itself declare 48000 Hz. Quality defaults
to 1.0; a reduced value is accepted only by models implementing NAM's slimmable API.
No captures or IRs from an online library are distributed by these tests.

The benchmark measures single and doubled chains at 64 and 128 frames. It does not
include a SceneSet tail pool, ASIO scheduling, or the game's AmpLink deadline counters.
See `../Documentation/Implementation-status.md` for the current acceptance boundary.
