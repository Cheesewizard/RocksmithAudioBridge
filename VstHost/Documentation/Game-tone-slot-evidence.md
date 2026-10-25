# M0a static evidence, 25 October 2026

Source: an existing September 2022 unpacked .text capture (base 0x401000).
SHA-256: b7f0fd88e9c0c893128768287f96448daab902cb86b52ab4005b35a37ab42cdf.
No new game capture or live validation was performed.

`GameComponentTones::Update`, VA 0x4649E0:

- 0x464A02: reads the owner from component+0x0C.
- 0x464AAE: reads the song clock from owner+0x3B4.
- 0x464ADD: calls 0x559DB0 with the clock and an output slot.
- 0x559DB0: walks eight-byte {time, toneId} records backwards in PlayerSong+0xDC..0xE0,
  returning the latest record at or before that time. This is the game's own seek-aware lookup.
- 0x464B10..0x464B7E: when no chart event applies, compares owner+0x198 against
  owner+0x1B0, +0x1C8, +0x1E0 and +0x1F8, assigning raw indices 0..3.
- 0x464B8A: compares that result with component+0x10.
- 0x464BCD: dispatches raw 0..3 to the four tone cases.
- 0x464F39: commits the result to component+0x10.
- 0x4649C4 (constructor) and 0x464A5F (no PlayerSong): initialise/store raw 5.

Interpretation: raw 0..3 identify A..D; raw 5 is the base/default sentinel.
Hotkeys, initial base selection and arrangement identity still require runtime confirmation.
In particular, diagnostics log both raw index and current key to detect any hotkey change
that changes the key without changing the candidate index.

The diagnostic reader checks the existing entry/owner signatures and the compare/store
instructions, then guards every component, owner, clock and key read. The existing popup
keeps its one-second clock gate; the diagnostic path can observe clock zero.
The reader logs raw slot, song clock and current key on a change or clock rewind.
It logs component/owner addresses on a new owner and explicitly reports that PersistentID
is unresolved. It does not publish a playback slot or modify any game field.

Acceptance still required: chart events, seeks, Riff Repeater, 50% speed, keys 1-4,
lead/rhythm/bass, and a resolved arrangement identity. Learn & Play is unvalidated;
its diagnostic reader disables itself if these body signatures do not match.
