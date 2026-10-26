# Stompbox engine

The Tone designer's amp engine: NAM amps (A1 and A2, through NeuralAudio), circuit-modelled tone stacks, cabinet
IRs and effects in one fixed rig, driven by text commands.

| | |
|---|---|
| Upstream | https://github.com/mikeoliphant/stompbox |
| Pinned commit | `17bc65264ff89a6b8a19843264292ca7762fdad3` (29 September 2026) |
| Licence | GPL-3.0 (`LICENSE.md` upstream). Combined with the AGPL-3.0 host under GPL-3.0 §13 / AGPL-3.0 §13. |
| Used | `stompbox-capi` (a DLL with a C API) and everything it links: `stompbox-core`, `stompbox-stomps`, NeuralAudio (MIT) with its NAM core and RTNeural forks, WDL, dr_libs, r8brain. Not used: the JACK and LV2 front ends, and the C# interface (`StompboxUI`). |

## Building

`Build-Stompbox.ps1` clones the pinned commit with its submodules, applies the local change below, builds
`stompbox-capi` (Release, x64, Visual Studio 2022), and copies `stompbox-capi.dll` to `VstHost\Stompbox\bin\`.
The host project copies it next to `RocksmithAudioBridgeVstHost.exe` and deploys it with it.

```powershell
powershell -ExecutionPolicy Bypass -File VstHost\Stompbox\Build-Stompbox.ps1
```

Requires Git, CMake 3.22+ and Visual Studio 2022 with the C++ workload.

## Local changes

The script fails if an upstream line it replaces has changed.

- **`stompbox-core/StompboxServer.cpp`:** the remote server listens on `127.0.0.1` instead of every interface, so
  only programs on this PC can control the engine.
- **`stompbox-stomps/DefaultPlugins.cpp`:** registers `Tonestack`, the circuit-modelled tone stacks (Bassman,
  Twin, Princeton, JCM800, JCM2000, JTM45, AC30, AC15, Mesa, Soldano and others) as a selectable pedal.
  Upstream defines it but leaves it out of the factory at the pinned commit. Its own UI fills that slot with EQ-7
  instead. Listen to it before shipping it.

## Development use (until the Tone designer page drives it)

1. Build the DLL, then build and deploy the host.
2. Set `HKCU\Software\RSMods\VstHost` `StompboxDev` (DWORD) to `1`, and restart the host.
3. Run StompboxRemote (from the upstream releases, needs .NET 10) and connect it to `127.0.0.1`.
4. Put NAM models and cabinet IRs in `%APPDATA%\RocksmithAudioBridge\Stompbox` (Stompbox lists that folder).
5. Block timing is logged every 30 s (`stompbox:` lines in the host log). Judge it by p99.9 and max against the
   block period.

Set `StompboxDev` back to `0` (or delete it) to return to the VST chain.
