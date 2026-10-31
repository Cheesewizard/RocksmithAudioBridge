# TONE3000 engine (vendored)

- **Source:** https://github.com/tone-3000/tone3000-plugin, MIT (`VstHost/Lib/Tone3000/LICENSE.txt`).
- **Pinned commit:** `12d0072d15c06b2ce6ef44f698cf2ae7d19029c1` (26 October 2026, VERSION 0.0.12), with its submodules
  NeuralAmpModelerCore (MIT, v0.5.4 in tree) and AudioDSPTools (MIT; only its header-only `ResamplingContainer`).
  Eigen (MPL-2.0) and nlohmann/json (MIT) come with NeuralAmpModelerCore.
- **What is taken:** the audio processor only (`plugin/include`, 19 files of `plugin/src`, the NAM core). Not the
  interface (`plugin/ui`), the plugin wrappers, the standalone app or anything iOS.
- **How:** `python VstHost/Tone3000/Vendor-Tone3000.py <checkout at the pinned commit, submodules initialised>`
  replaces `VstHost/Lib/Tone3000` and applies the patches below, each anchored on the upstream text (the script stops
  if upstream moved a line).
- **Built into** `RocksmithAudioBridgeVstHost.exe` against the host's bundled JUCE 9.0.3 (the version upstream uses),
  with `HEADLESS=1` and `NAM_ENABLE_A2_FAST`, as upstream's own DSP test suite builds it. The host is C++20 for it.
  `juce_dsp` and `juce_audio_formats` were added to `VstHost/Lib/JUCE` from JUCE 9.0.3 for it.

## Patches

1. **A1 captures:** `namConfigIsA2` admits any WaveNet. Upstream refuses local A1 files, but NeuralAmpModelerCore plays
   them, and the Tone designer's starter captures are A1.
2. **Data folder:** presets, the local model stash, settings and the log go under
   `%APPDATA%\RocksmithAudioBridge\ToneDesigner\` instead of `%APPDATA%\TONE3000\`.
3. **No network:** `fetchModelFromUrl` returns nothing for anything but a local file. No sign-in, no TONE3000 API.

## Updating

Check out the new commit with submodules, change `PIN` in the script, run it, rebuild the host, and render the
starter captures again offline before anyone plays it.
