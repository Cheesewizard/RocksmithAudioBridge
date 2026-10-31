# Pedal editor art

Photographic pieces for the generated pedal editors (`Source/PedalUi/PedalSkin`). The host loads them from
`PedalSkin\` next to its exe; the build copies them there and into the game folder. Every piece is optional: a
missing one falls back to the editors' own drawing (a missing shell to the textured look, a missing texture to
vectors).

## Shells: whole bodies

Each shell is the whole unit with its controls area left blank; the code puts the knobs, switches, name plate and
displays on it. Their areas are measured in `skin.json` as fractions of the image (x, y, width, height).

| File | What | Areas |
|---|---|---|
| `amp-modern.webp` | black head, metal mesh grille, silver panel: high-gain amps | `panel`, `grille` |
| `amp-british.webp` | black head, basketweave grille, gold panel: crunch amps (and amps not in the manifest) | `panel`, `grille` |
| `amp-vintage.webp` | blonde head, oxblood grille, chrome panel: clean amps | `panel`, `grille` |
| `cab-front.webp` | a 4x12 front | `grille` (for the name plate) |
| `rack-unit.webp` | a rack faceplate with ears | `inner`: drawn 9-slice, the ears keep their shape |
| `pedal-shell.webp` | a die-cast enclosure in neutral grey | `inner`: drawn 9-slice and tinted to each pedal's colour |

The amp head follows the capture's `character` in the tones manifest (`clean`, `crunch`, `high-gain`).

## Pieces

| File | What | How it is used |
|---|---|---|
| `knob-amp.webp` | chicken-head amp knob, pointer up | turned as a whole with the value |
| `knob-stomp.webp` | black pedal knob, no pointer | stays still; a pointer line is drawn over it |
| `knob-chrome.webp` | chrome pedal knob, no pointer | likewise (dark pedals, cabinets) |
| `footswitch.webp` | chrome stomp switch in its hex nut | slightly smaller while pressed |
| `plate.webp` | brushed nickel | name plates |
| `enclosure.webp`, `tolex.webp`, `cab-tolex.webp`, `faceplate.webp`, `grille.webp`, `rack-face.webp` | surfaces | the textured look, used when a shell is missing |

Text (names, labels, values) is always drawn by the code, never part of the art.

## Where it came from

Generated on 2026-10-26 with ChatGPT's image generation through the Codex CLI (`codex exec -m gpt-6-sol`), one
image per call. Objects and shells were asked for as photorealistic straight-on product shots on a transparent
background, with their controls area blank and no text or logos; surfaces as full-bleed, evenly lit textures. The
shells were cropped to their opaque area and measured for `skin.json`. Everything is stored as lossy WebP (with
transparency), about 3.4 MB in all. The images are the project's own (OpenAI's terms assign generated output to the
user) and ship under the project's licence.

To regenerate one, run a single `codex exec` at a time: parallel sessions can copy each other's latest image. A new
shell needs its areas measured again.
