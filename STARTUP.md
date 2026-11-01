# Native startup implementation and validation

The change is opt-in and only affects startup. No polling loop, extra thread,
external launcher, profile file edit, or system-wide network rule is added.

## Supported executable

- Windows Learn & Play, December 2024.
- SHA-256: `bb056959c0c6371d4ecf78f84c5d27e2fae93a9d0258830c2a36f33e6a2778eb`.
- PE timestamp `0x67497d00`, image size `0x330c000`.
- Runtime version and instruction checks are required before patching.
- Older Remastered builds are not yet supported or tested by this module.

## Native flow

`ModManager::ApplyStartupMods` calls `Startup::Install` after the existing audio
hooks, when the executable is unpacked. The normal settings reader loads two
keys from `[Startup]`. `OfflineStartup` implies `SkipOptionalUbisoftLogin`.

At RVA `0x4ca79a`, before the saved-credentials/automatic-login branch and
optional sign-in UI construction, the stub preserves
registers and flags and validates `RSProfileService_Win32` (vtable RVA `0xddd894`,
startup state at `+0xcc` equals 0). It calls the game's completion callback at
RVA `0x4cabe0` on the original game thread. The callback preserves the actual
account state and sets local startup state 4. The stub resumes existing string
cleanup at RVA `0x4ca900`. A controller mismatch replays the displaced
comparison and conditional branch,
resuming at RVA `0x4ca7a5` or the original prompt path at RVA `0x4ca85c`.

Offline startup hooks the existing `RendezVous.SkipLogin` property branch at
RVA `0x4bf4ec` and takes its original skip target at RVA `0x4bf518`. The optional
profile auto-connect request is never made and its requested flag stays unset.
The later wait/failure UI is therefore not entered for that request. This does
not fabricate successful authentication or a server connection. Both hook sites
and continuation instructions are guarded. Sign-in skip must install before the
offline hook is attempted. Unknown versions and failed checks are logged.

## Settings UI

The Game startup card maps three readable choices to the existing two INI keys.
It writes only those keys with the same Windows INI API used by the surrounding
setup page. The existing full INI writer already preserves unknown sections,
including `[Startup]`. Profile choices reuse the original controls/save handlers;
the dropdown reads each installation's existing profile list. Missing configured
profiles are shown explicitly and not silently replaced. Profile changes are
disabled while the game runs; startup mode remains editable for the next launch.

## Validation checklist

- [ ] Build the DLL in Master/Win32 and settings app in Release/x64.
- [ ] Absent keys: original startup; profile defaults to Choose in game.
- [ ] Normal: both startup keys off; original hook-site bytes remain unchanged.
- [ ] Skip sign-in: local completion; original connection behavior retained.
- [ ] Offline: local completion and native SkipLogin path; local menu reached.
- [ ] Repeat an ordinary Steam launch without a diagnostic injector.
- [ ] Fixed profile, empty profile preference, manual selection, missing profile.
- [ ] Unicode profile names survive a settings edit.
- [ ] Unrelated INI sections survive edits and the normal GUI save path.
- [ ] Unsupported executable: no startup hooks installed; clear log message.
- [ ] Saved profiles, song access, input capture and existing audio buffers work.

Attach results to the PR, distinguishing prototype runtime checks, integrated
build checks, UI harness checks, and combinations still requiring community
validation. Share source and short instruction signatures only. Game snapshots,
DLL dumps, saved profiles, personal INIs and machine-specific logs are excluded.
