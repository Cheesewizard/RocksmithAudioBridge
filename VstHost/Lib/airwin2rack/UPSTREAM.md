# airwin2rack

- Upstream: https://github.com/baconpaul/airwin2rack
- Pin: dcf98a3ec6d22932392c9127f5640d548a33c479.
- License: MIT, see LICENSE.md.
- Copied: consolidated base, registry, and generated headers/processors for: Gatelope, Pressure5, Drive, Density2, Baxandall2, Chorus, TapeDelay2, Chamber2, Verbity2, GrindAmp, LeadAmp, Cabs.
- Local changes: ModuleAdd.h registers only this subset. Documentation lookup returns
  the registry description instead of linking CMakeRC resources. DSP is unchanged.
