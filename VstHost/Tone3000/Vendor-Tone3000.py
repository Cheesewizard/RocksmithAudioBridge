# Copies the TONE3000 engine (github.com/tone-3000/tone3000-plugin, MIT) at the pinned commit into
# VstHost/Lib/Tone3000 and applies Rocksmith Audio Bridge's patches. Only the audio engine is taken: no interface,
# no plugin wrapper, no network. See UPSTREAM.md.
#
#   python VstHost/Tone3000/Vendor-Tone3000.py <checkout of tone3000-plugin with submodules>
#
# Every patch is anchored on the exact upstream text: if upstream changed the line, the script stops instead of
# guessing, so an update is always a deliberate review.
import os
import shutil
import subprocess
import sys

PIN = "12d0072d15c06b2ce6ef44f698cf2ae7d19029c1"
HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.normpath(os.path.join(HERE, "..", "Lib", "Tone3000"))

# The processor and its DSP; the interface (plugin/ui), the plugin wrappers and the iOS/standalone-only files stay out.
ENGINE_SOURCES = [
    "AutoOffset.cpp", "BlockEq.cpp", "BlockSpectrum.cpp", "LegacyParamIds.cpp", "MidiMapper.cpp", "NamEngine.cpp",
    "PitchShift.cpp", "PresetFile.cpp", "PresetManager.cpp", "Processor.cpp", "ProcessorChain.cpp",
    "ProcessorHistory.cpp", "ProcessorModelLoader.cpp", "ProcessorPresets.cpp", "ProcessorState.cpp", "Spread.cpp",
    "StandaloneStateAutosave.cpp", "StereoOffset.cpp", "TunerDetector.cpp",
]
UI_ONLY_HEADERS = {"DarkLookAndFeel.h", "StandaloneAudioSettings.h", "AudioPermissions.h", "IosAudioRoute.h",
                   "WindowKeyEvents.h"}

# Where the engine keeps its files: under the Tone designer's own data folder instead of %APPDATA%\TONE3000.
DATA_FOLDER = 'getChildFile("RocksmithAudioBridge").getChildFile("ToneDesigner")'

PATCHES = [
    # 1. A1 support. Upstream admits only A2 WaveNets for local files; NeuralAmpModelerCore plays A1 WaveNets too, and
    #    the Tone designer's starter captures are A1. Any WaveNet is phase-safe, so oversampling still works.
    ("plugin/src/ProcessorModelLoader.cpp",
     '    return modelJson.contains("config") &&\n'
     '           nam::wavenet::a2_fast::is_a2_shape(modelJson["config"], &channels);',
     '    (void)channels;  // Rocksmith Audio Bridge: A1 WaveNets too (the starter captures are A1)\n'
     '    return modelJson.contains("config");'),
    # 2. Data folder: presets, the local model stash, settings and the log.
    ("plugin/src/PresetManager.cpp",
     '  return base.getChildFile("TONE3000").getChildFile("Presets");',
     '  return base.' + DATA_FOLDER + '.getChildFile("Presets");'),
    ("plugin/src/ProcessorModelLoader.cpp",
     '  return base.getChildFile("TONE3000").getChildFile("LocalModels");',
     '  return base.' + DATA_FOLDER + '.getChildFile("LocalModels");'),
    ("plugin/src/ProcessorState.cpp",
     '  options.folderName = "TONE3000";',
     '  options.folderName = "RocksmithAudioBridge\\\\ToneDesigner";'),
    ("plugin/src/Processor.cpp",
     '  return juce::FileLogger::getSystemLogFileFolder()\n'
     '      .getChildFile("TONE3000")\n'
     '      .getChildFile("TONE3000.log");',
     '  return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)\n'
     '      .' + DATA_FOLDER + '\n'
     '      .getChildFile("tone-engine.log");'),
    # 3. No network: a model is only ever read from a local file. No sign-in, no catalogue, no TONE3000 API.
    ("plugin/src/ProcessorModelLoader.cpp",
     '  // The TONE3000 API requires a Bearer token on `model_url` requests;',
     '  // Rocksmith Audio Bridge: local files only, never the network.\n'
     '  juce::Logger::writeToLog("[ModelLoader] Network loading is off: " + modelUrl);\n'
     '  if (modelUrl.isNotEmpty()) return {};\n'
     '\n'
     '  // The TONE3000 API requires a Bearer token on `model_url` requests;'),
]


def copy_tree(source, target, keep=lambda path: True):
    for root, _, files in os.walk(source):
        for name in files:
            path = os.path.join(root, name)
            if not keep(path):
                continue
            relative = os.path.relpath(path, source)
            os.makedirs(os.path.dirname(os.path.join(target, relative)), exist_ok=True)
            shutil.copyfile(path, os.path.join(target, relative))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__ or "usage: Vendor-Tone3000.py <tone3000-plugin checkout>")
    checkout = os.path.abspath(sys.argv[1])
    head = subprocess.run(["git", "-C", checkout, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    if head != PIN:
        sys.exit(f"The checkout is at {head}, not the pinned {PIN}.")
    plugin = os.path.join(checkout, "plugin")
    if os.path.isdir(DEST):
        shutil.rmtree(DEST)

    copy_tree(os.path.join(plugin, "include"), os.path.join(DEST, "plugin", "include"),
              lambda path: path.endswith(".h") and os.path.basename(path) not in UI_ONLY_HEADERS)
    os.makedirs(os.path.join(DEST, "plugin", "src"))
    for name in ENGINE_SOURCES:
        shutil.copyfile(os.path.join(plugin, "src", name), os.path.join(DEST, "plugin", "src", name))
    nam = os.path.join(plugin, "NeuralAmpModelerCore")
    copy_tree(os.path.join(nam, "NAM"), os.path.join(DEST, "NeuralAmpModelerCore", "NAM"))
    copy_tree(os.path.join(nam, "Dependencies", "eigen", "Eigen"), os.path.join(DEST, "NeuralAmpModelerCore", "Dependencies", "eigen", "Eigen"))
    copy_tree(os.path.join(nam, "Dependencies", "nlohmann"), os.path.join(DEST, "NeuralAmpModelerCore", "Dependencies", "nlohmann"))
    tools = os.path.join(plugin, "AudioDSPTools")
    copy_tree(os.path.join(tools, "dsp", "ResamplingContainer"), os.path.join(DEST, "AudioDSPTools", "dsp", "ResamplingContainer"))
    for source, target in [(os.path.join(checkout, "LICENSE"), "LICENSE.txt"),
                           (os.path.join(nam, "LICENSE"), "NeuralAmpModelerCore/LICENSE.txt"),
                           (os.path.join(tools, "LICENSE"), "AudioDSPTools/LICENSE.txt")]:
        shutil.copyfile(source, os.path.join(DEST, target))
    for name in os.listdir(os.path.join(nam, "Dependencies", "eigen")):
        if name.startswith("COPYING"):
            shutil.copyfile(os.path.join(nam, "Dependencies", "eigen", name), os.path.join(DEST, "NeuralAmpModelerCore", "Dependencies", "eigen", name))

    for relative, old, new in PATCHES:
        path = os.path.join(DEST, relative)
        with open(path, encoding="utf-8", newline="") as file:
            text = file.read()
        newline = "\r\n" if "\r\n" in text else "\n"
        anchor = old.replace("\n", newline)
        if text.count(anchor) != 1:
            sys.exit(f"Patch anchor not found exactly once in {relative}: upstream changed; review the patch.\n{old}")
        with open(path, "w", encoding="utf-8", newline="") as file:
            file.write(text.replace(anchor, new.replace("\n", newline)))
    print(f"TONE3000 engine {PIN[:8]} vendored into {DEST} with {len(PATCHES)} patches")


if __name__ == "__main__":
    main()
