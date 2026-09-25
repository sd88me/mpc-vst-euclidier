# Vendored: Euclidier standalone engine

Source: `sd88me/force-euclidier` (fork of `intelliriffer/EUCLIDIER-CONSOLE`), repo root:
`euclidier.cpp`, `eqseq.cpp`/`.h`, `bjlund.cpp`/`.h`, `commontypes.h`, `RtMidi.cpp`/`.h`, `RtError.h`,
`testalgo.cpp`.

Vendored at commit: `1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25).
License: MIT (same author, same license as this repo — see `LICENSE`).

The VST port (`vst/`) doesn't link this engine in-process -- `euclidier_vst.cpp` spawns the
already-deployed standalone `euclidier` binary on the device and drives it over its Unix control
socket + its own ALSA seq virtual MIDI ports (it's a monolithic standalone app, not a library).
These sources are vendored here only so `vst/build.sh` can build an x86 copy of the real engine for
`host_test`'s offline ASan run, without a docker mount of the force-euclidier repo. Not modified
from the source commit above; re-vendor by diffing against force-euclidier's repo root at a newer
commit and copying the same files over.
