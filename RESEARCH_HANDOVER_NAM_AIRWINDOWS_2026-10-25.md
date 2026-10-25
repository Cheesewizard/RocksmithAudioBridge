# Research handover: portable Tone Designer tones, NAM, Airwindows and CDLC switching

Date: 25 October 2026  
Workspace: `C:\Programming\RSModsPlus`  
Branch observed: `feature/4.3-features`  
Status: research and proposed design, awaiting independent technical review.

## Purpose and scope

This document consolidates the conversation's research so another person can check the conclusions before implementation. It covers both the original automatic whole-rig capture idea and the more immediately practical NAM/Airwindows preset approach.

No product source was changed, no builds were run, no software was installed, and no live game/audio acceptance testing was performed during this research. Creating this handover is the only requested file change. The checkout contains substantial existing work in progress, including audio bridge, Tone Designer and Note-by-Note files; preserve it.

Evidence categories used below:

- **Source observation:** read in the current repository or upstream source/documentation.
- **Measurement:** actually measured during this conversation, with scope stated.
- **Proposal:** a recommended design, not existing functionality.
- **Unproven:** requires experiments or independent verification.

The user wants confirmation of this research, not implementation yet. Read the repository instructions before making any later changes. Do not commit, deploy, alter the game, foreground applications, or overwrite work in progress as part of reviewing this document.

## 1. User's intended outcome

People should be able to create complete guitar tones inside Tone Designer, associate them with CDLC arrangements and tone changes, and share small song packages. Recipients should not need the author's third-party amp software, original plugin installations, original download folders, accounts, or online tone listings.

The mod may have a substantial installed engine and effects collection. Small per-song data is the priority; the installed playback engine does not need to fit into each song.

Two approaches were explored:

1. **Automatic full-rig capture:** at song export, send test audio through the current VST3 chain or external amp application, with every effect left enabled. Learn a compact input/output model and export it. No manual pedal configuration, bypassing effects, or separate stage captures should be required by the user workflow.
2. **Shared built-in components:** integrate existing open-source amp playback and effects into Tone Designer. Creators build tones using those components; export their settings and all required sound assets. NAM plus Airwindows is the current recommended direction to investigate first.

These solve the dependency problem differently. NAM plus Airwindows does not automatically convert arbitrary third-party VST rigs into portable tones. Do not conflate the two approaches.

## 2. Main conclusions for the reviewer

1. **Self-contained NAM sharing is technically practical.** A `.nam` file contains model configuration and learned weights. Embed the actual capture bytes in the song's tone bank, rather than a URL. If the online listing disappears later, that embedded copy remains available for compatible local playback.
2. **Measured official NAM examples were small enough to embed.** Two examples were 407,452 and 665,758 bytes before compression, and 132,948 and 143,271 bytes inside individual lossless ZIP archives. These are example measurements, not universal limits or verified commercial capture sizes.
3. **Airwindows has a direct library integration route.** `airwin2rack` provides a static library and common effect API. It need not be installed as an external VST3 plugin.
4. **The current Tone Designer persistence is plugin-dependent.** `RABTONE2` stores plugin paths and opaque plugin settings. It is not a portable semantic effects format.
5. **Current song assignment is song-start routing.** No arrangement tone-event scheduler driving Tone Designer was found in the inspected code. The native TONE SWITCH popup is presentation, not proof that chart events are routed to the host.
6. **Automatic arbitrary whole-rig capture remains research.** Published results support parts of it, including multi-effect chain estimation, but the investigation did not establish a ready-made compact, low-latency solution for arbitrary amps plus long delay/reverb, modulation and unusual routing.
7. **A large shared engine does not eliminate all per-tone information.** It can share algorithms, but custom model weights and measured impulse responses still need storage unless represented adequately by compact parameters.

## 3. Current repository evidence

Line numbers were checked while preparing this handover. They may move as existing work continues. Paths below are relative to the workspace.

| Area | Evidence | Implication |
|---|---|---|
| Separate 64-bit host | `VstHost/RocksmithAudioBridgeVstHost.vcxproj:14`, `README.md`, Tone designer section | Suitable existing process boundary for built-in NAM/Airwindows playback outside the 32-bit game |
| Host compiler setting | `VstHost/RocksmithAudioBridgeVstHost.vcxproj:31` | Currently C++17; current upstream NAM CMake requests C++20, so check the selected pinned version/build requirements |
| Audio processing | `VstHost/Source/Chain.cpp:390`, `BridgeAudio.cpp:166` | Current chain receives mono guitar and produces stereo; slots are currently VST-specific |
| Structural edits | `VstHost/Source/Chain.cpp:55`, `Chain.hpp` thread contract | Chain changes pause processing; unsuitable as the final mechanism for precise, seamless mid-song scene changes |
| Existing tone format | `VstHost/Source/Chain.cpp:443`, `SaveTone` and `LoadTone` | `RABTONE2` contains paths, bypass values and plugin-owned state blobs |
| Plugin-owned state | `VstHost/Source/PluginHost.cpp`, `GetState`/`SetState` | JUCE state is not interpreted into portable amp/effect parameters |
| Global trims | `VstHost/Source/Chain.cpp`, `SaveTone` | Saved trim fields are neutral; player trims are deliberately global |
| Gate persistence | `DLL/ToneDesigner/ToneGates.cpp:45` | Gate settings live separately in `ToneGates.ini`; export must explicitly decide which authored gate behaviour belongs in a portable tone |
| Song routing | `DLL/ToneDesigner/VstHostLink.cpp:798` | `FollowSong` selects the assigned/default tone at song start, with preparation before play |
| Popup | `DLL/ToneDesigner/VstHostLink.cpp:892`, `ToneSwitchPopup.cpp` | Names the loaded tone and requests a notification; not a chart-to-renderer switch scheduler |
| Song indexing | `GUI/Features/ToneDesigner/ToneDesignerTools.cs:88`, `:116` | Reads PSARC manifests; groups by SongKey and aggregates arrangement names rather than retaining a full arrangement timeline |
| Host commands | `DLL/Shared/VstHostControl.hpp` | Shared-memory commands are acknowledged control operations, not a sample-timed event queue |
| Plugin transport | `VstHost/Source/PluginHost.cpp:46`, `:52` | Reports 120 BPM, 4/4 and always playing; song-aware effect timing needs additional work |
| External link ownership | `DLL/AsioProxy/AmpLinkGuest.cpp:289` | One guest owns output slots; a capture recorder should not attach as a competing guest |
| External processing boundary | `DLL/AsioProxy/AmpLinkGuest.cpp:349`, `AmpLinkHost.cpp`, `AmpLink.hpp` | Known input blocks and stereo return blocks are available before final game output mixing |
| Cable clock handling | `DLL/ExternalAmp/CableAmpLink.cpp` | Cable return uses buffering/rate matching; capture source processing before output clock correction |

The song key is derived from game preview event names in `DLL/GameState.cpp`. It is not, by itself, a unique arrangement identity. Lead, rhythm, bass, alternate arrangements and duplicate packages need deliberate identification.

The current host is declared AGPLv3 in `VstHost/LICENSE.txt`; the rest of the project is separately licensed. Integrating permissive DSP components does not change the existing JUCE licensing situation.

## 4. NAM: what would actually be shared

NAM is an inference/training technology, not a mandatory online catalogue. TONE3000 is one source of models. A downloaded `.nam` contains the configuration and weights needed by a compatible renderer; it is not merely a pointer to a server-side tone.

Primary sources:

- [NAM C++ core](https://github.com/sdatkinson/NeuralAmpModelerCore)
- [.nam file specification](https://neural-amp-modeler.readthedocs.io/en/latest/model-file.html)
- [Loading API](https://github.com/sdatkinson/NeuralAmpModelerCore/blob/main/NAM/get_dsp.h)
- [DSP interface](https://github.com/sdatkinson/NeuralAmpModelerCore/blob/main/NAM/dsp.h)
- [Current upstream CMake requirements](https://github.com/sdatkinson/NeuralAmpModelerCore/blob/main/CMakeLists.txt)
- [A2 release announcement](https://www.neuralampmodeler.com/post/a2-is-released)

The loading API includes parsed-model/configuration overloads, so a container can supply embedded data without requiring users to install model files in the original author's directory. Loading, validation, preparation and model warm-up belong outside the real-time processing callback.

Support should be defined against a pinned core revision and tested model architectures. Do not claim every file with a `.nam` extension is supported. Validate channels, sample rate, architecture and resource requirements. The current API supports more layouts than older mono-only wrappers; inspect the selected core rather than assuming all NAM is internally mono.

A NAM capture represents the captured device at particular settings. Input drive and added EQ do not automatically reproduce the original amp's entire gain/tone-control behaviour. A useful amp library can supply clean/crunch/high-gain snapshots, but the interface must describe its controls honestly.

An amp-head capture needs a cabinet stage. An amp-plus-cab capture already includes that response; avoid unintentionally adding another cabinet. See [NAM versus IRs](https://www.tone3000.com/guides/impulse-responses).

### 4.1 Actual file-size measurements

The following files were fetched from the official core repository's moving `main` branch during this conversation. No immutable commit SHA was recorded for the measurement. Recheck and pin a revision before using the numbers as a release budget.

| File | Architecture observed | Original bytes | ZIP bytes |
|---|---|---:|---:|
| `wavenet_a1_standard.nam` | WaveNet | 407,452 | 132,948 |
| `A2.nam` | SlimmableContainer | 665,758 | 143,271 |

Sources:

- [A1 example](https://github.com/sdatkinson/NeuralAmpModelerCore/blob/main/example_models/wavenet_a1_standard.nam)
- [A2 example](https://github.com/sdatkinson/NeuralAmpModelerCore/blob/main/example_models/A2.nam)

Method: fetch original bytes; create one in-memory ZIP per file using Python `zipfile.ZIP_DEFLATED`, compression level 9; report the raw length and total ZIP length. Nothing was written to disk for this measurement. The compressed figures include ZIP overhead and are not measurements of PSARC compression.

These are official examples, not a sound-quality benchmark. Very small synthetic/test models in that directory must not be presented as evidence that a realistic high-quality amp needs only a few kilobytes.

Four distinct captures of approximately 140 KB compressed would add approximately 560 KB before cabinet assets, settings and container overhead. This is an illustrative extrapolation, not a measured four-tone CDLC.

### 4.2 Binary representation and compression

[TONE3000's nam-binary-loader](https://github.com/tone-3000/nam-binary-loader) is an MIT-licensed compact binary format/loader. Its README reports roughly 80% size reduction for a typical WaveNet model by storing weights as binary float32 instead of textual JSON.

Do not apply that reported ratio to every architecture or assume complete coverage of A2 containers/custom models. Its documented supported architectures and converter behaviour need review against the pinned version. A float32 conversion is also not the same as preserving every original JSON byte.

**Initial proposal:** embed the original `.nam` bytes with ordinary lossless container compression. Investigate binary conversion only if measured size/load-time gains justify it and supported-model/audio-equivalence checks pass. Do not quantize or retrain models merely to meet an assumed size target.

## 5. Airwindows: integration findings

Primary sources:

- [Original Airwindows source](https://github.com/airwindows/airwindows)
- [Airwindows Consolidated / airwin2rack](https://github.com/baconpaul/airwin2rack)
- [Static-library documentation](https://github.com/baconpaul/airwin2rack#the-library-version)
- [Registry API](https://github.com/baconpaul/airwin2rack/blob/main/src/AirwinRegistry.h)
- [Common processing API](https://github.com/baconpaul/airwin2rack/blob/main/src/airwin_consolidated_base.h)
- [Build configuration](https://github.com/baconpaul/airwin2rack/blob/main/CMakeLists.txt)
- [Licensing](https://github.com/baconpaul/airwin2rack#a-note-on-licensing)

Recommended route: pin the source and integrate selected processors as a native/static DSP library in the existing x64 host. Do not require recipients to install the Airwindows VST3 plugin.

The library provides effect names/categories, parameter counts and factories. Its common API exposes sample rate, parameter names/display/values and stereo float/double processing. This maps naturally onto a built-in Tone Designer effect adapter.

Candidate effects to audition, not an approved final selection:

| Role | Candidates | Review needs |
|---|---|---|
| Drive | `Drive` | Guitar usefulness, aliasing, level behaviour |
| Dynamics | `Dynamics` | Compression/gating behaviour, release and noise handling |
| Modulation | `Chorus`, `ChorusEnsemble` | Stereo behaviour, modulation, state/CPU cost |
| Delay | `TapeDelay2` | Delay range, flutter, feedback, parameter-to-time mapping |
| Reverb | `Chamber2`, `Galactic2` | Sound suitability, CPU, tail duration, memory |

The `airwin-registry` target builds the broad suite by default. A curated subset requires adjusting the source/registry build consistently, not simply hiding entries in the UI. Inspect its support-file dependencies before implementation.

Do not persist registry array positions as effect IDs: generated order can change. Use project-owned stable processor IDs, explicit algorithm revision, parameter schema and validated values.

The common base has no explicit reset or tail-length interface. Constructors initialize internal buffers/filter state and may initialize random state. A prepared fresh instance can establish a fresh lifecycle, but recreation must happen off the audio thread and does not automatically guarantee bit-identical random behaviour. Parameter changes alone do not reset delay/reverb history.

Effects generally receive sample rate and audio, not a full host tempo/transport API. Song-tempo mapping must be implemented per selected effect where its parameter semantics support it. Do not assume every Airwindows delay can immediately become a musically synced delay.

The original algorithms and the Consolidated library's documented reusable source are MIT-licensed. Plugin/Rack wrappers introduce separate framework dependencies. Use the direct library and preserve notices. The existing Tone Designer host remains subject to its own JUCE-related licence.

## 6. Proposed Tone Designer architecture

This is a design proposal, not code already present.

Introduce a common processor contract for preparation, audio processing, parameters/state, latency, channel layout and lifecycle. Implement explicit processor kinds for:

- NAM model playback.
- Selected Airwindows effects.
- Cabinet convolution and utility processing where needed.
- Existing external VST processing for local rigs.

The portable export contract should accept only components/assets the recipient's mod can reproduce. A local third-party VST chain does not become portable because its preset blob is packaged. Explain any unsupported block and refuse to label such an export dependency-free.

Keep built-in playback in the current 64-bit host, using the established audio bridge. No new hardware driver is justified for NAM/Airwindows integration. Current host C++17 versus upstream NAM's C++20 setting is a build decision to resolve explicitly: select a supported release and compile settings, rather than assuming a drop-in build.

Player/device calibration and listening trims remain separate from authored tone gain staging. Portable tones need relative levels and capture reference metadata without moving the player's global volume whenever a scene changes.

## 7. Portable container and asset lifetime

Proposed container contents:

| Item | Purpose |
|---|---|
| Manifest/schema version | Interpret and validate the package |
| Engine and processor requirements | Define supported playback behaviour |
| Tone/scenes graph | Block types, order, routing, bypass, wet/dry and authored levels |
| NAM assets | Actual model bytes, stored once per unique model |
| Cabinet/custom response assets | Actual IR bytes, stored once per unique asset |
| Arrangement mappings | Arrangement identity, base tone and authored switch assignments |
| Transition policy | Crossfades, retained effects and outgoing tails |
| Attribution/distribution metadata | Track asset origin and permission basis |
| Content hashes | Integrity, identity and deduplication |

URLs, filenames and display names must not be runtime dependencies. Import should copy assets into a managed library. Export should traverse all song scenes and embed every required asset automatically. Sharing standalone tones should use the same self-contained asset rules.

Preserve the original data and store each unique asset once within a song bank. Scenes sharing one amp model reference it by content hash. Cross-song caching can reduce installed duplication, but each distributed song should remain independently complete.

This directly addresses the user's scenario: an online listing may disappear without breaking a previously exported song. It cannot rescue a model that was never downloaded or embedded. Future engine support is a separate preservation obligation: pin dependencies, define stable behaviour and test old exported fixtures against releases. Do not silently replace missing assets or algorithms with another sound.

Runtime delay/reverb buffers contain the recipient's live audio and are allocated during playback; they do not need to be exported. Custom measured IRs are different: their response samples are tone-specific data and do require storage.

No final file extension, schema or archive method has been selected.

## 8. CDLC tone-switch design

Current song-start assignments and notification hooks are useful entry points but do not complete this feature.

Required work:

1. Retain a stable selected-arrangement identity; handle lead/rhythm/bass, alternate arrangements and duplicate packages.
2. Extract/observe the arrangement's base tone and actual authored tone-change events. A tone change is not necessarily a section or phrase boundary.
3. Associate those tone identifiers with portable scenes. Allow creator-authored additional events only through an explicitly defined format.
4. Preload/decompress/validate all required assets and prepare model/effect instances before playback.
5. Deliver scene switches against a defined song/audio timeline through a dedicated scheduling path; do not depend on the ordinary command acknowledgement loop for precise event timing.
6. Crossfade prepared paths and retain or cut outgoing tails according to an explicit policy, with a bounded processing budget.
7. Recompute the active scene after seeks, restarts and Riff Repeater loops. Define what happens to history/tails on discontinuities.
8. Define pause, tempo and practice-speed behaviour. Repeating the complete neural rendering history after a seek is not automatically affordable or necessary; the chosen reset/prewarm policy needs listening tests.

Immutable model weights can be shared while mutable model/effect states remain independent where needed. Avoid running every scene continuously merely to keep it warm unless measurements justify that cost.

The present PSARC index reader can help discover tone-bank resources, but new entry writing, arrangement event extraction, package enumeration and game acceptance remain unverified integration tasks. Do not claim existing export support. The final goal is an embedded song resource; a development test fixture can be external without defining the shipped distribution format.

## 9. Original automatic whole-rig capture research

### 9.1 Terminology and feasibility

The general method is **audio system identification**, specifically black-box neural modelling/profiling. A hybrid with known processing blocks is often called grey-box modelling; differentiable DSP is one way to optimize those blocks.

Suggested description: "An automatic rig profiler that learns the complete guitar input/output response and exports a compact, plugin-independent model."

A frequency-response graph or ordinary IR cannot represent all nonlinear/dynamic behaviour. A model of the whole rig needs internal state/history for echoes, reverb and dynamics, and internal modulation/conditioning for moving effects. It can conceptually be one learned algorithm; separate user-visible pedals are not mandatory.

One automatic capture session can contain multiple repeated probes, several drive levels, guitar material and long silent tails, with all original effects left on. It need not mean one short audio snippet. Known dry input plus aligned wet output is the useful supervision; a wet snippet alone is much more ambiguous.

### 9.2 Primary research and its limits

| Source | Evidence | Important limit |
|---|---|---|
| [DAFx 2024: Audio Effect Chain Estimation and Dry Signal Recovery](https://www.dafx.de/paper-archive/2024/papers/DAFx24_paper_53.pdf) | Guitar chains containing distortion, delay, chorus and reverb; order/parameters estimated; 64 configurations | Four known Pedalboard implementations, each used at most once; not arbitrary commercial rig capture |
| [ICASSP 2026: Prediction and Search](https://arxiv.org/abs/2604.22276) | Neural prediction followed by search improves reconstruction; type prediction plus order/parameter search is promising | Restricted chorus/distortion/reverb dataset with controlled ranges and normalization; not a complete stereo amp-rig benchmark |
| [DiffVox](https://arxiv.org/html/2504.14735v2) | Joint fitting of EQ, compression/expansion, delay, FDN reverb and sends; 152 effect parameters | Vocal-oriented fixed model; compactness trades off expressiveness; not a universal guitar solution |
| [Searching for Music Mixing Graphs](https://sh-lee97.github.io/grafx-prune/) | Optimize a larger processing graph and prune unnecessary processors | Fixed processor ordering and mixing-oriented domain; arbitrary ordering is identified as further work |
| [Modulation Extraction for LFO-driven Audio Effects](https://dafx.de/paper-archive/details/esXCZ49ZPaHxYCl_i1nMWg) | Models moving effects using extracted modulation and dry/wet data; real-time plugin demonstration | Specialized technique, not proof of a complete arbitrary rig |
| [Differentiable black-box and gray-box modelling comparison](https://www.frontiersin.org/journals/signal-processing/articles/10.3389/frsip.2025.1580395/full) | Compares LSTM/TCN/GCN/S4 models on nonlinear effects; S4 shows strong results | Does not establish compact modelling of arbitrary multi-second delay/reverb chains |
| [NablAFx framework](https://github.com/mcomunita/nablafx) | Research framework with neural and differentiable DSP architectures, including S4 | Training/research framework, not a finished Windows rig profiler/player |
| [Real-time RNN modelling](https://research.spa.aalto.fi/publications/papers/dafx19-rnn/) | Demonstrated real-time small recurrent models of an amp and distortion pedal | Single-device results, not full ambience chains |
| [Hybrid Neural Audio Effects](https://riccardovib.github.io/Hybrid-Neural-Audio-Effects_pages/) | Combined preamp/compressor/tape modelling | Does not prove conventional delay/reverb whole-rig capture |
| [Fitting compact feedback delay networks](https://dafx.de/paper-archive/details/SMUqo0VexPo41yfKNKqJ4g) | Fits an algorithmic reverberator to measured responses | Approximation of target responses; not automatically a nonlinear/modulated full-rig model |

The separate reference implementations [S4](https://github.com/state-spaces/s4) and [Mamba](https://github.com/state-spaces/mamba) are architecture resources, not ready-made guitar capture engines. Do not present a generic sequence benchmark as real-time effects evidence. S4 does have effects-specific evidence through the comparison/NablAFx work above; it would be incorrect to say it is untested for audio effects in general.

ST-ITO and DeepAFx are also relevant optimisation research, but controlling a chain of original plugins is not the same as replacing that chain with a dependency-free renderer:

- [ST-ITO](https://arxiv.org/abs/2410.21233)
- [Differentiable Signal Processing With Black-Box Audio Effects](https://arxiv.org/abs/2105.04752)

An unverified SIREN-FX model-card search result was encountered. Its broad claims were not accepted as evidence for an implementation recommendation.

### 9.3 Recommended experiment if capture research resumes

Compare a compact causal recurrent/S4 model with a jointly fitted neural-plus-DSP model. Fit the full dry/wet response with all original effects enabled. The internal optimizer may infer structure without asking the user to isolate stages.

Use explicit delay/reverb memory where that gives better quality per byte/CPU. Do not train an ordinary NAM model on an entire wet chain and assume adding guessed delay/reverb afterward makes it accurate. Parameters and learned nonlinear behaviour must be optimized coherently against the target.

No credible primary evidence was established here for a single compact model that reproduces arbitrary full VST/external rigs, including long ambience, modulation and unusual feedback routing, with published small-block CPU/latency results suitable for this mod.

Random LFO/noise behaviour may be matchable perceptually or statistically rather than sample-for-sample. External MIDI, expression, sidechains and tempo changes are additional inputs; a static snapshot does not reveal their complete response.

The earlier hundreds-of-kilobytes-to-one-megabyte full-rig target was a proposed engineering budget, not a demonstrated result. The concrete NAM compression measurements apply to the built-in component approach, not arbitrary full-chain learning.

## 10. Licensing and asset redistribution questions

- NAM Core and Airwindows reusable library code have MIT licences; preserve notices and audit dependencies of the exact pinned revisions.
- A downloaded capture or IR has separate content rights. Engine licensing is not blanket permission to redistribute every community or commercial asset.
- [TONE3000 terms](https://www.tone3000.com/terms) say uploaders retain ownership. Do not infer a universal sublicence to repackage downloads.
- [TONE3000 tone-sharing policy](https://www.tone3000.com/guides/tone-sharing-guidelines) prohibits sharing captures of current plugins/modelers on that platform, with stated exceptions. This is its platform policy, not a legal determination about all capture workflows or another distribution service.
- A factory library should use original or explicitly redistributable captures and IRs. User exports should retain attribution and the documented permission basis for embedded assets.
- This research did not audit particular third-party capture licences or provide a legal opinion.

## 11. Suggested implementation phases, after review

1. **Dependency proof:** pin NAM/Airwindows sources, resolve compiler/dependency requirements, play representative NAM and Airwindows effects through the existing host.
2. **Built-in processor model:** add first-class native blocks, UI parameters, lifecycle and serialization, without disguising native data as VST paths.
3. **Self-contained bank:** import assets into managed storage, export original bytes plus settings, deduplicate, validate and reopen offline.
4. **Scenes and scheduling:** prepare all song tones, implement timed switches, crossfades, tails, seeking and tempo behaviour.
5. **CDLC packaging:** verify arrangement identity, event extraction, archive integration and discovery.
6. **Acceptance:** verify sound, portability, resource cost and song behaviour on real playback hardware.

The automatic arbitrary rig-capture feature is a separate R&D track. Do not make the practical NAM/Airwindows system depend on proving that research first.

## 12. Independent review / acceptance checklist

Please return confirmed findings, corrections and unresolved questions separately.

- Reproduce the NAM byte/compression measurements at a pinned commit and measure several real redistributable amp captures.
- Verify NAM architecture support, block-size preparation, sample-rate handling and A2 quality-mode semantics in the selected core.
- Verify the host compiler/toolset/dependency integration; current host is C++17 and current upstream core requests C++20.
- Verify Airwindows library linkage, subset registration, effect IDs, parameter mapping, state lifecycle, notices and any dependencies.
- Audition candidates before claiming they are the best-sounding choices; no listening tests were performed here.
- Measure CPU deadlines, memory and underruns at the actual game block sizes. Average render throughput alone is insufficient for small real-time callbacks.
- Check noise, clipping, gain staging and input calibration using new performances, not just the same audio used to configure a tone.
- Verify shared effects and independent scene states, transitions, long tails, bypass, pause and discontinuities.
- Confirm selected arrangement identity and authored tone-event extraction, including base tone, alternate arrangements, seeking, loops and practice-speed changes.
- Confirm PSARC packaging/discovery, rather than assuming an arbitrary extra file will be found or accepted.
- Confirm the export gathers every model/IR used by every scene and preserves attribution/distribution permission.
- Test disappearance explicitly: export, remove the original model files, disable network access, and play the imported package.
- Final portability test: transfer a multi-tone song to a clean machine with the mod but no NAM/Airwindows/source VST installations or author assets. Verify playback, switches, tails and seek/loop behaviour.
- Verify release-to-release reference fixtures so processor/model updates do not silently change existing exported tones.

## 13. Research process and remaining uncertainty

Repository observations were static source inspection. Upstream claims were checked against primary documentation, code and papers where practical. The only numerical measurements performed for this integration were the NAM example file sizes and in-memory ZIP sizes. No CPU, latency, sound quality, switching or archive acceptance measurements were performed.

Several read-only subagent assignments were requested using GPT-5.6 Luna at medium effort for capture constraints, tone storage/switching, long-memory models and Airwindows integration. Runtime model identity was not independently confirmed. Their findings were reviewed and combined by the primary agent; this document explicitly corrects overbroad interpretations such as generic S4 evidence proving full-rig capture or an external app needing preset introspection merely to record its input/output.

The actionable conclusion is to validate a self-contained NAM/Airwindows preset system first. Its asset-lifetime solution is straightforward: ship the actual captures and settings with each song. Seamless CDLC switching and native processor integration remain implementation and live-verification work, not completed features.
