# mpc-vst-euclidier

MPC/Force VST2 port of [force-euclidier](https://github.com/sd88me/force-euclidier)'s 8-lane
Euclidean MIDI sequencer, for the Akai MPC OS plugin host (MPC Live/One/X/Key, Force).

`vst/` is the plugin port (see `vst/build.sh`); `src/` vendors the standalone engine's own
sources (see `src/VENDORED.md`) so `vst/build.sh`'s offline host_test build is self-contained.

Build/deploy workflow: see `sd88me/mpc-vst-plugins`' `docs/PORTING.md` and
`.claude/skills/mpc-vst-plugin/SKILL.md`.
