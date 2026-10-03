# mpc-vst-euclidier

💬 Questions or feedback? Join the [Open MPC Discord](https://discord.gg/sRRysZSgu3).

Euclidier as a native plugin for Akai MPC OS standalone devices (Force, MPC Live/One/X/Key): an 8-lane Euclidean
MIDI sequencer with its own MPC screen skin and Q-Links. It is a port of
[force-euclidier](https://github.com/sd88me/force-euclidier) (a fork of
[intelliriffer/EUCLIDIER-CONSOLE](https://github.com/intelliriffer/EUCLIDIER-CONSOLE) by Amit Talwar).

Status: working and tried on a Force. There is no release zip or catalog entry yet.

## What it does

Eight independent lanes each spread a number of hits (**fill**) as evenly as possible over a number of **steps**, with a
**shift** (rotation), an optional **loop** point, a note (or a drum note), MIDI channel, gate, velocity and humanise, and
a time division. The plugin makes no sound: it plays MIDI into other tracks, and it follows the project transport and
tempo, so edits stay in phase with the beat.

## The screen

Two tabs, each with its own Q-Link page. The screen shows live state: patterns and play-heads follow the engine.
Both tabs have **DRUM MODE ALL** (every lane to drum mode) and **RANDOMISE ALL** in the top bar.

| Tab | What is on it |
|---|---|
| **LANES** | The lane you are editing. A lane stepper (◀ LANE 1 ▶) chooses it. Its pattern is drawn as a circle with the play-head, with a RANDOMISE button for that lane in the corner of the panel. Knobs for STEPS, FILL, SHIFT, LOOP, GATE, VELOCITY, HUMANIZE, NOTE and MID CH., a division stepper, a NOTE / DRUM switch and the lane's enable. Q-Links 1-8 are steps, fill, shift, loop, gate, velocity, note and MID CH. (HUMANIZE is touch-only). |
| **ALL** | All 8 lanes as rows: an enable lamp, the pattern as a row of cells with the play-head, and a RANDOMISE button for that lane. Q-Links 1-8 are the lane enables. |

A new instance starts with every lane in drum mode, lanes 1-4 on and 5-8 off, each of the four with its own Euclidean pattern: 16 steps with 4 hits (four on
the floor), 8 with 3 (tresillo), 12 with 5, and 16 with 5. A saved project restores its own settings instead.

Switching a lane to DRUM makes the engine set its note to that lane's own General MIDI drum slot (lane 1 is 36, lane 2
is 37, and so on). The NOTE knob follows.

Pattern displays change size with the lane's step count: the row cells and the circle come in size classes (rows: 8,
16, 32 cells; circle: 8, 12, 16, 24, 32 dots). Counts between classes use the next class up, so a 12-step row has 12
cells at the 16-step size and the circle spreads its steps evenly. The 32-step row doesn't show the play-head.

## Limits

- **32 steps.** STEPS, FILL, SHIFT and LOOP stop at 32 (the engine allows 64). The step displays are built from many small
  image components, and 64 steps made pages too slow to open.
- **Slow first draw.** A page can take a moment to fill in when the plugin is first inserted. MPC is decoding the skin's
  images, and the cells fill in once it is done.
- **Display only.** Tapping a step does not edit it. Patterns come from steps, fill, shift and loop.
- **No preset page.** The engine's 128 preset slots are not on the screen. The plugin's settings are saved with the MPC
  project like any other plugin.
- **Randomise** hits one lane or all of them; there is no lane picker.

## Setting it up

1. The plugin starts the standalone `euclidier` engine, one per plugin instance, and talks to it over a Unix control
   socket. The engine must be on the device at `/media/662522/AddOns/Euclidier/euclidier` (it comes with
   force-euclidier; set `EUCLIDIER_BIN` to use another path). It is not bundled here.
2. Install the plugin: the `euclidier.so` and the skin folder `sd88me - VST - Euclidier` (both from `vst/build/`), and
   register the `<PLUGIN>` line in `vst/build/pluginlist-entry.xml` in MPC's settings. The workflow is in
   [mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins) (`docs/PORTING.md`). Registering needs an MPC restart;
   replacing the `.so` or skin later does not (remove the instance and insert it again).
3. Insert Euclidier on a track. The engine appears as a MIDI port named "Euclidier": MPC detects it on its own, and
   you enable it for the tracks that should receive it in Preferences, MIDI. MPC ignores a plugin's own MIDI output,
   which is why the engine has its own port.
4. The plugin turns the host's transport and tempo into the MIDI clock the engine follows, so it plays and stops with
   the project.

## How it works

- `vst/euclidier_vst.cpp` is a hand-written VST2 wrapper. Parameter changes only touch an in-memory cache; a worker
  thread sends them to the engine and polls it about 33 times a second for each lane's `l<N>_pattern`
  (`steps|bits|play|loop|enabled|selected`). Randomise buttons send the engine an ordered script (lane flag, `rand_go`, clear
  the flags), because the engine's randomise acts on flagged lanes. Changes are reported to the host from `processReplacing` with
  `audioMasterAutomate`, because MPC does not redraw engine-driven values on its own.
- The step displays are display-only parameters, one per cell per size class (`g<lane>_<cap>_<slot>` for the lane rows,
  `c<cap>_<slot>` for the circle). A cell is hidden, off, on, or (for the play-head) off or on with the play-head on it.
  The skin shows one image per state. The play-head is a cell state because MPC crops each picture to an opaque
  rectangle, so a ring drawn over the cells would hide them.
- `vst/make_skin.py` generates `params.json`, `layout.conf` and `images/` (the engine's parameter list comes from
  `vst/module.json`, the theme from `vst/layout.theme`). `tools/gen_vst.py` from mpc-vst-plugins then builds `params.h`
  and the skin with the browser renderer (`"art": "html"` in `vst/vst.json`).

## Building

```
vst/build.sh          # needs Docker and a checkout of mpc-vst-plugins at ../../mpc-vst (or set MPC_VST)
```

It runs `make_skin.py` and `gen_vst.py`, builds the plugin and its offline host test, runs the test, then cross-builds
`vst/build/euclidier.so` for armhf. The skin is in `vst/build/skin/`. The host test (`vst/host_test.c`) checks, among
other things, two instances, popups, the pattern cells and play-head against a stand-in engine
(`vst/fake_engine.py`), the lane stepper, fresh-instance defaults, randomise (one lane, the selected lane, all) and DRUM MODE ALL. The test container has no ALSA sequencer, so the real
engine can't run in it, which is why the stand-in speaks the engine's control-socket protocol.

## Files

| Path | What |
|---|---|
| `vst/euclidier_vst.cpp` | the plugin |
| `vst/make_skin.py`, `vst/layout.theme` | skin generator and its theme |
| `vst/params.json`, `vst/layout.conf`, `vst/images/` | generated by `make_skin.py` (committed) |
| `vst/module.json` | the engine's control-socket key set |
| `vst/fake_engine.py`, `vst/host_test.c` | offline test |
| `src/` | the standalone engine's sources (see `src/VENDORED.md`) |

MIT licence (see `LICENSE`).
