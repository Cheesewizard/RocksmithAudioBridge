# Bundled audio resampler

The `soxr` directory contains libsoxr 0.1.3 sources from the python-soxr
0.5.0.post1 source distribution, downloaded from PyPI on 2026-09-07.
The Python wrapper is not compiled or shipped. The upstream CMake build produces
a separate x64 `soxr.dll`, with OpenMP, examples, tests, and LSR bindings disabled.

Source distribution: https://pypi.org/project/soxr/0.5.0.post1/#files
Upstream: https://sourceforge.net/projects/soxr/
License text and component notices: `soxr/LICENCE`, `soxr/COPYING.LGPL`.

The resampler uses the same HQ settings as the trained Python audio frontend.
Keep this DLL replaceable alongside RSModsPlus.dll in the release package.
