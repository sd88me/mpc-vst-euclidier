# Where the engine comes from

`src/` is Euclidier's standalone sequencer engine, and this repo is now its home: edit it here.

- It started as [intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar.
- It was reworked for the Akai Force in `sd88me/force-euclidier` (now archived), then copied here from that repo's
  commit `1268b0e1f0f55e963c754891d94cc0055bee4cb1` (2026-09-25). The sources are unchanged from that commit, which
  was the last one to touch them.
- Files: `euclidier.cpp`, `eqseq.cpp`/`.h`, `bjlund.cpp`/`.h`, `commontypes.h`, `RtMidi.cpp`/`.h`, `RtError.h`,
  `testalgo.cpp`.

`vst/build.sh` builds them into the `euclidier` engine that the plugin starts (one per plugin instance, over its
control socket, see `docs/ENGINE.md`). The offline host test runs against `vst/fake_engine.py` instead, because the
engine needs an ALSA sequencer that the test container does not have.

Licence: see the note in the top-level `README.md`.
