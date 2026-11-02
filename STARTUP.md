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

The in-game General / Game startup card and the desktop card map three readable
choices to the existing two INI keys. The in-game card caches settings on entry,
uses the existing overlay layout helpers, and writes only on explicit edits. It
never reloads the live settings map or changes the loaded profile mid-session.
It writes only those keys with the same Windows INI API used by the surrounding
setup page. The existing full INI writer already preserves unknown sections,
including `[Startup]`. The desktop profile choices reuse the original controls/save handlers;
the dropdown reads each installation's existing profile list. Missing configured
profiles are shown explicitly and not silently replaced. Profile changes are
disabled in the desktop app while the game runs; in-game startup preferences are
all editable for the next launch. The desktop app synchronizes the profile keys
on activation so a later desktop save preserves in-game edits.

## Recorded validation

Testing so far covers one Windows installation with the supported Learn & Play
executable. This contribution is based on `release/4.3`; compare against that
branch to keep unrelated release changes out of the startup review.

- Built the contribution DLL in Master/Win32 and settings app in Release/x64.
  The GUI build reused the released, unchanged RocksmithAudioBridge.dll ML
  dependency; private ML model sources were not rebuilt.
- An ordinary Steam launch of the contribution's native module reached local
  startup state 4, followed RendezVous.SkipLogin, and reached the main menu.
  RS_ASIO 0.7.4 bound to ASIO4ALL in Real mode at 128 frames. Audio INIs were
  unchanged during feature validation.
- The installed variant of the in-game General card also reached the main menu
  through an ordinary Steam launch. Its local native hook prototype differs
  from the contribution module, so later local tests do not count as repeated
  runtime validation of the exact PR binary.
- Passed 14 startup-preference persistence checks and an 11-check harness using
  the actual desktop UI partial. The actual ImGui startup panel was rendered
  offscreen for visual inspection; these checks do not replace full desktop UI
  end-to-end testing.

Other PCs, real cached Ubisoft sessions, all sign-in/profile combinations, and
full desktop UI end-to-end behavior still require validation. No general
startup-time or FPS improvement is claimed.

### Separate local audio observations

A subsequent intermittent startup crash on the test PC also occurred before
the new startup hooks were installed. A separate local RS_ASIO compatibility
build completed two consecutive Steam launches. The exact crash cause and a
permanent resolution remain unproven; that RS_ASIO build is not part of this
contribution.

A loss of instrument input was traced to an existing Realtek ASIO input
selection. Restoring the test setup's ASIO4ALL input restored signal. The
startup feature does not write that driver preference. These observations do
not establish compatibility for every audio setup.

## Remaining validation checklist

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
