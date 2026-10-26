# Built-in Tone designer: MoSCoW for the first release

26 October 2026, branch `dev_portable-tone-designer` at `fb451b3`.

## The method, briefly

MoSCoW (Dai Clegg, Oracle, 1994; adopted by DSDM) sorts requirements **for one timebox**: Must, Should, Could,
Won't have this time.

- **Must:** without it the release has no point, or is illegal or unsafe. The test is "what happens if this is
  missing?" If the answer is "then don't ship", it is a Must.
- **Effort share:** Musts should take **no more than about 60% of the effort**. Coulds, about 20%, are the
  contingency dropped first when time runs short, then Shoulds. The share is measured by effort, not by count.
- **Won't:** a deferral for this timebox, not a rejection. It is left out of the effort sums.

Source: [Agile Business Consortium, MoSCoW prioritisation](https://www.agilebusiness.org/?p=1907);
[St Andrews, "MoSCoW prioritisation is on effort"](https://digitalcommunications.wp.st-andrews.ac.uk/2016/08/05/moscow-prioritisation-is-on-effort/).

## The timebox

**"Tone designer 1.0":** a player can build great-sounding tones from built-in NAM amps, cabinets and effects
in the overlay, and have songs switch between them with the chart. A creator can share a song's tones so that
another player with only the mod hears the same thing.

The second sentence is the reason the feature exists (portable tones for CDLC), so sharing is treated as a Must.
If you would rather ship a local-only 1.0 first, move **M6** and **M7** to Should and the release still makes
sense. The effort check below shows that both choices stay under the 60% line.

## What is on the branch now

Everything below is written but **has not been compiled or run**. The new C++ files only passed a syntax check
against stand-in Windows headers.

| Area | State |
|---|---|
| Stompbox engine in the host (`stompbox-capi.dll`, build script with 4 checked patches) | Written |
| Engine service: loads at host start, pedal catalogue, commands, presets, routing | Written |
| Tabs: Tone designer \| VST rig \| External app | Written |
| Pedalboard page: tones, pedals, browser (Effects/Amps/Cabinets), drag and drop, generic in-overlay editor | Written |
| Live load meter (typical load against real time, slowest block, late blocks) | Written |
| Song tones: every tone of the song from its manifest, assignment in the overlay, tones prepared before play, switch on the game's tone key, 10 ms dip, 2 s tails | Written |
| Game's current tone key (`song+0x198`) | Found statically; not confirmed live |
| Sharing: export/import of a song's tones with model and IR files | **Not started** |
| Installer shipping the engine (`stompbox-capi.dll`) with its GPL-3.0 licence, source pointer and the notices of every library in it (M4, M5) | Written, not built |
| Starter content (S1): 58 NAM captures, 10 cabinet IRs and 26 genre tones, every file with an author and a licence that may travel with CDLC | Built in its own repository, `RocksmithAudioBridge-Tones` (each tone checked in the engine offline); not yet published or installed by the mod |

## MoSCoW

Effort is relative: S = 1, M = 3, L = 5, XL = 8. These are planning sizes, not estimates of anything measured.

### Must have

| # | Item | Why it is a Must | Effort |
|---|---|---|---|
| M1 | **Build it.** Compile the host, the DLL and the GUI, and fix what breaks. | Nothing on the branch has been compiled. | M (3) |
| M2 | **Verify sound and timing live.** Stompbox plays clean through the bridge at 128 and 64 frames, the load meter reads sensibly, and switching away from the Tone designer gives the VST rig back intact. | A tone designer that crackles or breaks the VST rig has no point. | M (3) |
| M3 | **Verify song switching live.** The tone key changes exactly at chart tone events, seeks, Riff Repeater and 50% speed, for lead, rhythm and bass. The dip and tails sound right. | Song tones are the reason to use this over the VST rig. Wrong-time switches make it unusable. | M (3) |
| M4 | **Ship the engine.** The installer carries `stompbox-capi.dll` next to the host and the uninstaller removes it. The pinned version is recorded. | Players do not run a build script. | S (1) |
| M5 | **Licence compliance.** Ship Stompbox's GPL-3 text and a written source offer or link for the pinned commit with our four patches. Include the NeuralAudio (MIT) notice and the other bundled dependencies' notices. | Shipping it without these is not legal. | S (1) |
| M6 | **Export a song's tones.** One `.rabtones` file beside the CDLC. It holds every assigned saved tone (Stompbox preset text) and every NAM and IR file they name, each stored once and hashed. Export refuses files without a recorded permission. | The release goal: tones that travel with the song. | L (5) |
| M7 | **Import and play them.** Find a `.rabtones` beside a CDLC, validate it, extract it to the library, map its tones to the song, and play it with no account and no network. Do the offline "files deleted" test. | Without it, M6 is a file nobody can use. | M (3) |
| M8 | **Safe failure.** A missing DLL, a bad model file or a broken bank leaves the game and the VST rig working, with a message that says what failed. | A failure that silences the guitar is unsafe in the DSDM sense. | S (1) |

**Must total: 20.**

### Should have

| # | Item | Why not a Must | Effort |
|---|---|---|---|
| S1 | **Starter pack.** 20–40 NAM captures and some cabinet IRs whose provenance and redistribution rights are checked (the GPL-3 NAM_models collection, CC-licensed captures, CC0 IRs, our own captures). | Players can add their own files. Without content, though, first use is empty. | M (3) |
| S2 | **Rescan the library** while the host runs, so new models and IRs appear without a restart. | A restart works around it. | S (1) |
| S3 | **Consistent levels.** Calibrate the input against each NAM model's recorded input level, so a capture drives the same on every machine, and give scenes matching loudness. | Without it, shared tones are louder or quieter than intended, but still play. | M (3) |
| S4 | **Backups include the Tone designer** (Stompbox presets, song tones and the library) in `--tone-backup` and `--tone-restore`. | Data loss is recoverable by hand, but it is the existing promise of the Tone designer page. | S (1) |
| S5 | **Rights capture in the UI.** On first export of a file, ask for attribution and permission, and remember them. | Export can refuse instead (M6). This makes it usable. | S (1) |

**Should total: 9.**

### Could have

| # | Item | Effort |
|---|---|---|
| C1 | Assign song tones from the Songs pane, without choosing the song first (the command already takes any song). | S (1) |
| C2 | A "standard settings" benchmark: render the tone offline at 128 frames and report its measured cost, for creators. | M (3) |
| C3 | Release removed pedals safely, after the audio thread has left the old chain. | S (1) |
| C4 | Skip the VST rig's song-start tone loading while the Tone designer plays. | S (1) |
| C5 | Overlapping tails across rapid switches, instead of cutting the older one. | S (1) |
| C6 | Pedal art and knob styling beyond the generic editor. | M (3) |

**Could total: 10.**

### Won't have this time

- **Embedding the bank inside the PSARC.** The sidecar file comes first; embedding needs proof that the game accepts
  the extra entry.
- **Converting VST rigs or external apps into portable tones,** whether by automatic capture or system
  identification.
- **TONE3000 browsing:** it needs an account, and its terms forbid redistributing catalogue captures.
- **Mid-song switching for VST rig tones.**
- **Creator-authored extra switch points** beyond the chart's own tone changes.
- **Arrangement `PersistentID` detection:** song tones are keyed by tone key, so it is not needed.

## Effort check

Total for Must, Should and Could: 20 + 9 + 10 = 39.

| Split | Must | Should | Could |
|---|---|---|---|
| As above (sharing is a Must) | 20 / 39 = **51%** | 23% | 26% |
| Local-only 1.0 (M6, M7 moved to Should) | 12 / 39 = **31%** | 44% | 26% |

Both are under the 60% guideline, with the Coulds as a real contingency of about a quarter of the work.

The biggest risk to the Musts is **M2 and M3**: they depend on things only a live game can show, namely the tone key
read and Stompbox's real cost in this host. They should go first. If either fails, the design has a fallback for
the tone read (a scheduler driven by the chart), but that changes the plan before any sharing work is worth doing.

## Order of work

1. M1, then M2 and M3: build, then prove the sound, the timing and the switching live.
2. M8 and M4/M5: fail safely, then ship legally.
3. M6 and M7, with S5: sharing.
4. Shoulds, in the order S1 → S3 → S2 → S4.
5. Coulds, while time remains.
