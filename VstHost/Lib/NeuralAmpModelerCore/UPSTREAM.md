# NeuralAmpModelerCore

- Upstream: https://github.com/sdatkinson/NeuralAmpModelerCore
- Commit: 0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842 (DSP 0.6.0).
- License: MIT, see LICENSE.
- NAM sources and the upstream nlohmann/json single header are unchanged.
- Eigen submodule: bc3b39870ecb690a623a3f49149a358b95c5781d, MPL2,
  copied from https://gitlab.com/libeigen/eigen at the recorded gitlink.
- AudioDSPTools is not required by these NAM runtime sources and is not vendored.
- Host defines NAM_SAMPLE_FLOAT and NAM_ENABLE_A2_FAST; C++17, MSVC v143.
