# The engine

`src/` holds the standalone engine the plugin starts. It is the original Euclidier sequencer, reworked for the Force.
This page covers what changed from upstream and how the plugin talks to it.

## Features (unchanged from upstream)

8 lanes. Per lane: enable, note (0-127), time division (11 values), steps (2-64), fill (1-64, Bjorklund), shift
(0-64), loop point (0 = off; less than steps shortens the pattern; more than steps gives a polyrhythm), gate (10-95%),
MIDI channel (1-15; 16 is reserved for feedback), velocity, humanise (0-50), and a mode (1 note, 2 drum, 3 and 4 CC).
Globals: 128 preset slots, a randomiser, receive-notes, velocity-sense octave shifts, internal or external clock.
The full CC map is in [MIDI-CC.md](MIDI-CC.md). The engine opens two virtual MIDI ports named "Euclidier" (an input for
clock and CC, an output for the notes).

The plugin's screen uses steps 2-32 only, and no preset slots; the engine still allows 64 steps and all of the above.

## What the Force rework changed

- **No resync after an edit.** Upstream kept a step counter per lane, so changing steps, fill, shift or division needed
  a transport restart to bring the lanes back into line. Here the position is a pure function of the clock:
  ```
  ticksPerStep = 96 / getDiv(div)     // every getDiv() value divides 96 evenly
  n            = tickCount / ticksPerStep
  k            = n % (loop ? loop : steps)
  idx          = k % steps            // loop > steps gives the polyrhythm
  fire if SEQ[idx]
  ```
  Changing a parameter only rebuilds the pattern (and the modulus), so lanes stay phase-locked. Start (0xFA) sets the
  tick to 0, continue (0xFB) resumes, stop (0xFC) halts, and song position (0xF2) sets the tick to SPP x 6.
  A division change keeps the tick grid but the step phase jumps, as the step index changes.
- **Edits apply at once.** Master-sync quantise (CC 50) is an option now, off by default, because edits are safe.
- **Lower CPU.** An event-driven main loop with a sorted note-off list replaced a 100 microsecond polling loop: about
  5.3% of a core down to about 0.4% while playing. A mutex now guards the lane state shared by the MIDI callback, the
  control socket and the main loop.
- **Drum mode note default.** Switching a lane into drum mode sets its note to that lane's own General MIDI drum slot
  (lane 1 is 36, lane 2 is 37, up to lane 8 at 43), unless it was already in drum mode.
- **Control socket**, below.

Dormant: CC track modes 3 and 4 and the internal clock (CC 39, 40, 59, 60, 80) are still in the engine but not reachable
from the plugin or the control socket; use MIDI CC. Known quirks inherited from upstream: `getVel()` mode 4 does
`rand() % diff` with `diff == 0` possible, and upstream's README lists 1/64 as division 7 when the engine maps 7 to
1/24.

## Command line

| Option | Meaning |
|---|---|
| `-v` | run quietly (no console output) |
| `--ctrl-sock <path>` | the control socket's path (below) |
| `--client-name <name>` | the ALSA client name of the engine's two MIDI ports (default `Euclidier`); the plugin gives each instance its own, so the port shows up as "Euclidier", "Euclidier 2" and so on |
| `--clock-from <client name>` | follow only the MIDI clock, start, stop, continue and song-position messages sent by that ALSA client, and drop the rest. The plugin uses it because MPC also sends its own clock to every MIDI port (a device's "sync" setting), and two clocks would run the engine too fast |

## Control socket

`euclidier -v --ctrl-sock <path>` listens on a Unix socket (default `/tmp/euclidier_ctrl.sock`). One request per
connection: `GET <key>` returns the value and a newline; `SET <key> <value>` returns `OK` or `ERR`. Each SET runs
through the same code as the equivalent MIDI CC, so the two interfaces cannot disagree.

| Key | Meaning |
|---|---|
| `l<1-8>_enable`, `_note`, `_div`, `_steps`, `_fill`, `_shift`, `_gate`, `_ch`, `_vel`, `_velh`, `_loop` | lane parameters |
| `l<1-8>_mode` | 0 note, 1 drum |
| `l<1-8>_toggle` | SET only: flip a step (not used by the plugin) |
| `sel_<param>` | the same, for the selected lane; `sel` itself is the lane (0-7) |
| `GET l<N>_pattern` | `steps\|bits\|play\|loop\|enabled\|selected`, for example `16\|1,0,0,0,1,...\|3\|0\|1\|1` |
| `GET l<N>_info` | `steps/fill SHshift`, plus ` LP<loop>` when a loop is set |
| `GET l<N>_note_txt`, `_div_txt` | the note name and the division label |
| `preset`, `preset_load`, `preset_save` | preset slot select, load and save |
| `rand_l<1-8>`, `rand_go`, `rand_lane` | randomise: lane flags (none set means all), go, and the CC 89 lane picker |
| `GET sel_name`, `preset_txt`, `rand_lane_txt`, `transport` | readouts (`transport` is `RUN <bpm>` or `STOP`) |

`euclidier_vst.cpp` polls `l<N>_pattern` for every lane about 33 times a second and sends parameter changes from a
worker thread. The preset bank is saved as `euclidierBANK.bin` next to the engine binary.
