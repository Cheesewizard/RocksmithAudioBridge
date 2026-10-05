# Audio Bridge app icon

The Audio Bridge window/taskbar icon (`AudioBridge256.ico`, wired through
`Properties/Resources.resx` as `AudioBridgeIcon`) and its master raster
`AudioBridge256.png`.

## Concept

"Jack Bridge": two chrome 1/4" TS guitar plugs (elongated barrel, twin
diamond-knurled grips, black insulator ring, gold conical tip) plug in from the
left and right, bridged by a glowing rainbow spectrum soundwave. The plugs =
audio in/out being routed ("bridged"); the rainbow spectrum = the audio itself.
Replaces the earlier unreadable waveform blob.

## Sources (edit these, then regenerate)

- `AudioBridge256.svg`       - detailed art (plugs + full rainbow), used 48-256 px.
- `AudioBridge256-small.svg` - bold simplified rainbow only (no plugs, thicker
  petals) so the taskbar frames (16/24/32 px) stay legible.
- `AudioBridge256.gen.py`    - Python generator for the detailed art. Tune the
  `LOBES` rainbow envelope, `X0/X1` span, and the `plug()` proportions, then
  re-run to emit the SVG.

Palette: deep-navy tile `#132038 -> #0A1120`; chrome barrels (grey metallic
gradient) with `#141821` insulator ring and gold tip `#fef3c7 -> #b45309`;
rainbow petals purple `#7C3AED` -> magenta -> red -> orange `#FB7115` (peak) ->
yellow -> green -> teal -> cyan `#08BCD4` (peak) -> blue -> indigo `#5B6EE6`.

## Regenerating the PNG + ICO

There is no SVG rasterizer on the box; assets were produced by rendering the
SVGs through a browser canvas at each target size (crisper small sizes than
downscaling one master) and assembling a PNG-compressed multi-resolution ICO
(16/24/32/48/64/128/256) by hand:

1. Edit `AudioBridge256.svg` (and/or the generator) and `AudioBridge256-small.svg`.
2. Render the small SVG at 16/24/32 and the detailed SVG at 48/64/128/256.
3. Pack the seven PNG frames into `AudioBridge256.ico` (ICONDIR + per-size
   ICONDIRENTRY pointing at each embedded PNG; width/height byte 0 == 256).
4. Copy the 256 px frame to `AudioBridge256.png`.
