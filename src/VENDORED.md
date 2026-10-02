# Vendored: Euclidier standalone engine

Source: `sd88me/force-euclidier` (fork of `intelliriffer/EUCLIDIER-CONSOLE`), repo root:
`euclidier.cpp`, `eqseq.cpp`/`.h`, `bjlund.cpp`/`.h`, `commontypes.h`, `RtMidi.cpp`/`.h`, `RtError.h`,
`testalgo.cpp`.

Vendored at commit: `1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25).
License: MIT (same author, same license as this repo — see `LICENSE`).

The VST port (`vst/`) doesn't link this engine in-process -- `euclidier_vst.cpp` spawns the
already-deployed standalone `euclidier` binary on the device and drives it over its Unix control
socket + its own ALSA seq virtual MIDI ports (it's a monolithic standalone app, not a library).
These sources are kept here as the reference for the engine's behaviour (its control-socket keys, the
`pattern` reply format that the plugin polls). `vst/build.sh` no longer builds them: the offline host test runs against
`vst/fake_engine.py`, because the engine needs an ALSA sequencer that the test container does not have. Not modified
from the source commit above; re-vendor by diffing against force-euclidier's repo root at a newer commit and copying
the same files over.
