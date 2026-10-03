# MIDI CC map

The engine's original MIDI CC interface is unchanged. Under the plugin it is not needed (the screen and Q-Links drive the
engine over its control socket, see [ENGINE.md](ENGINE.md)), but it still works: the engine's virtual MIDI input port
"Euclidier" accepts these messages. The plugin screens do not use them, and they have not been exercised through the plugin.

| CC(s) | Lanes 1–8 | Meaning |
|---|---|---|
| 1,11,21,…,71 | ✓ | Enable lane (value > 63 = on) |
| 2,12,22,…,72 | ✓ | Note/pitch (0–127) |
| 3,13,23,…,73 | ✓ | Time division — see table below |
| 4,14,24,…,69,74 | ✓ | Steps (2–64) |
| 5,15,25,…,75 | ✓ | Fill/pulses (1–64); fill ≥ steps fills every step |
| 6,16,26,…,76 | ✓ | Shift/rotation (0–64), wraps around |
| 9,17,27,…,77 | ✓ | Gate length, 10–95% (default 65%) |
| 8,18,28,…,78 | ✓ | Output MIDI channel (1–15; 16 is reserved for feedback) |
| 81–88 | ✓ | Base value (velocity, or CC base value), default 96 |
| 91–98 | ✓ | Value-alt: note mode = humanize range (0–50) added to base velocity; CC mode 1 = second CC value sent after gate; CC mode 2 = random value between base and alt |
| 101–108 | ✓ | Loop point (0 = off; < steps shortens the pattern; > steps creates a polyrhythm) |
| 111–118 | ✓ | Track mode: 1 = note, 2 = drum, 3/4 = CC (not reachable from the plugin screen). Switching a lane into drum mode defaults its note to that lane's own GM drum slot (lane 1 → 36, lane 2 → 37, … lane 8 → 43) if it wasn't already in drum mode — you can still set a different note afterward. |

Time divisions (CC 3,13,…,73 and the the plugin's division stepper), value → label:

| Value | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Label | 1/16 | 1/16 | 1/8 | 1/4 | 1/2 | 1 | 1/32 | 1/24 | 1/12 | 1/6 | 1/3 |

(Values 7–10 are triplet-style divisions, not the "dotted" values the original CC-value ordering might
suggest — this matches the engine's actual `getDiv()` table.)

**Global CCs:**

| CC | Meaning |
|---|---|
| 20 | Select preset slot (0–127) |
| 29 | Load from selected preset slot |
| 30 | Save to selected preset slot |
| Program Change | Load from preset slot 0–127 |
| 50 | Master-sync quantize (0–8 bars) — quantizes parameter edits to the clock; edits apply immediately when this is 0 (the default here) |
| 70 | Sync all tracks (resets internal step position to 0) |
| 89 | Randomize lane select: 0 = all, 1–8 = one lane, 9 = all but lane 1, 10 = all but lane 5 |
| 90 | Randomize go (fires on any even value > 0) |
| 99 | Receive notes (0/1) |
| 100 | Reset all transpose/octave offsets (any positive value) |
| 119 | Velocity-sense octave switching (0/1) |
| 59 | Use external clock (0/1); internal clock is the fallback when off |
| 60 | Start/stop internal clock (0/1) |
| 80 | Alternate internal-clock start/stop (0/1) |
| 39, 40 | Internal clock BPM1 + BPM2 (actual BPM = BPM1 + BPM2, e.g. 100 + 40 = 140) |

**Realtime note input** (when receive-notes is on):

| Notes | Meaning |
|---|---|
| 0–11, 12–23, …, 84–95 | Transpose lane 1, 2, …, 8 by +0–11 semitones |
| 96–103 | Reset octave shift on lanes 1–8 respectively |
| 108–115 | +1 octave shift on lanes 1–8 respectively |
| 120–127 | −1 octave shift on lanes 1–8 respectively |
