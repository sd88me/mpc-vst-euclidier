/* transport_grid.h -- host transport (audioMasterGetTime) -> the MIDI real-time stream the engine follows, anchored to
 * the SONG POSITION instead of to a pulse count.
 *
 * The engine's step is a pure function of its own `tick` (tick % ticks_per_step), and `tick` counts pulses from Start. If the
 * wrapper only fed it pulses, any lost pulse, a start in the middle of a 16th, or a loop wrap would shift every step for good.
 * So the wrapper also tells the engine WHERE it is: a Song Position Pointer (0xF2, in 16ths = 6 clocks) before the pulse at a
 * 16th boundary -- at Start, after every loop wrap / locate, and once per bar to heal a dropped pulse. The engine already
 * handles 0xF2 (tick = pos*6) while it follows an external clock. Pulses are still 24 per quarter, but they now always
 * arrive with tick == round(ppq*24).
 *
 * Header-only and engine-independent so grid_test.cpp can drive it with a model of the engine. Emitter E must provide:
 *   void start();            // 0xFA
 *   void stop();             // 0xFC
 *   void songpos(long s16);  // 0xF2, position in 16th notes (the emitter masks it to 14 bits)
 *   void pulse();            // 0xF8
 * See docs/MIDI_TIMING.md in mpc-vst-plugins for why. */
#pragma once
#include <cmath>

struct TransportGrid {
    double last_ppq = 0.0;
    bool was_playing = false;
    bool need_sync = false;   /* next 16th boundary must be announced with a Song Position Pointer */
    bool need_start = false;  /* ... and preceded by Start (a fresh play, not a loop/locate) */

    /* Once per audio block. ppq = ppqPos at the start of this block; the interval since the previous block is what gets sent
     * (a block late, a constant ~3 ms). */
    template <class E>
    void block(E &e, bool playing, double ppq, double tempo, double sample_rate, int frames) {
        if (playing && !was_playing) { last_ppq = ppq; need_sync = need_start = true; }
        else if (!playing && was_playing) { e.stop(); need_sync = need_start = false; }
        was_playing = playing;
        if (!playing) return;

        double blk = frames * (tempo > 0 ? tempo : 120.0) / (60.0 * (sample_rate > 0 ? sample_rate : 44100.0));
        double start = last_ppq, end = ppq;
        if (end < start) { start = end - blk; need_sync = true; }        /* loop wrap: re-cover the block holding the loop start */
        else if (end - start > 1.0) { start = end - blk; need_sync = true; }   /* locate forward: re-announce (and re-cover the landing block), no flood */
        last_ppq = ppq;

        for (long m = (long)std::ceil(start * 24.0 - 1e-9); m / 24.0 < end - 1e-9; m++) {
            if (m < 0) continue;                       /* count-in / pre-roll */
            if (need_sync) {
                if (m % 6 != 0) continue;              /* wait for a 16th boundary: SPP can only name those */
                if (need_start) { e.start(); need_start = false; }
                e.songpos(m / 6);
                need_sync = false;
            } else if (m % 96 == 0) {
                e.songpos(m / 6);                      /* once per bar: heals a dropped or doubled pulse */
            }
            e.pulse();
        }
    }
};
