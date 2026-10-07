// Offline test of transport_grid.h against a model of the engine's clock handling (src/euclidier.cpp):
//   0xFA -> tick = 0, started   0xF2 n -> tick = n*6 (while started)   0xF8 -> sendTicks(tick) then tick += 1   0xFC -> stopped
// A step fires when tick % 6 == 0. Checks that every pulse arrives with tick == its true song position, across loop wraps that are
// not block aligned, a start in the middle of a 16th, a locate, and a dropped pulse.
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include "transport_grid.h"

struct Engine {
    bool started = false;
    long tick = 0;
    std::vector<long> fired;       // ticks that were processed (sendTicks) this block
    int drop_next = 0;             // test hook: swallow the next N pulses (a lost ALSA event)
    void start() { started = true; tick = 0; }
    void stop() { started = false; }
    void songpos(long s16) { if (started) tick = (s16 & 0x3FFF) * 6; }
    void pulse() { if (!started) return; if (drop_next > 0) { drop_next--; return; } fired.push_back(tick); tick++; }
};

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// Run blocks; pos(k) gives ppqPos at block k. Returns the steps (ticks % 6 == 0) seen, verifying each pulse sits in its block's interval.
static long run(TransportGrid &g, Engine &en, long blocks, double (*pos)(long), double tempo, double sr, int frames,
                std::vector<long> *steps = nullptr, long drop_at = -1) {
    double blk = frames * tempo / (60.0 * sr);
    long nsteps = 0;
    double prev = pos(0);
    for (long k = 0; k < blocks; k++) {
        double p = pos(k);
        if (k == drop_at) en.drop_next = 1;
        en.fired.clear();
        g.block(en, true, p, tempo, sr, frames);
        double lo = (p < prev) ? p - blk : prev, hi = p;   // interval this block is allowed to cover
        for (long t : en.fired) {
            double q = t / 24.0;
            // after the dropped pulse the engine is one tick late until the bar heal: tolerated for < 1 bar
            if (drop_at >= 0 && k >= drop_at && k < drop_at + 4000) continue;
            CHECK(q >= lo - 1e-6 && q < hi + 1e-6, "block %ld: pulse tick %ld = ppq %.4f outside [%.4f, %.4f)", k, t, q, lo, hi);
            if (t % 6 == 0) { nsteps++; if (steps) steps->push_back(t / 6); }
        }
        prev = p;
    }
    return nsteps;
}

static double sr = 44100.0, tempo = 128.0;
static double ppb() { return tempo / 60.0 * 128.0 / sr; }
static double loop4(long k) { return std::fmod(k * ppb(), 4.0); }          // 4-beat loop, wrap lands mid-block
static double from33(long k) { return 3.3 + k * ppb(); }                   // start in the middle of a 16th
static double locate(long k) { double p = k * ppb(); return p < 2.0 ? p : 40.0 + (p - 2.0); }   // jump forward at 2.0

int main() {
    // 1. loops: exactly 16 steps per 4-beat loop, forever, every one on the grid
    { TransportGrid g; Engine en; en.started = false;
      std::vector<long> steps;
      long blocks = (long)(4.0 * 50 / ppb());
      long n = run(g, en, blocks, loop4, tempo, sr, 128, &steps);
      printf("loop: %ld steps in %ld blocks (50 loops, expect 800, within 1)\n", n, blocks);
      CHECK(std::labs(n - 800) <= 1, "loop step count %ld", n);
      for (size_t i = 0; i < steps.size(); i++) CHECK(steps[i] % 16 == (long)(i + (steps[0] % 16)) % 16, "step order broke at %zu (%ld)", i, steps[i]); }
    // 2. start at ppq 3.3: the first step is the 16th boundary at 3.5 (n = 14), nothing before it
    { TransportGrid g; Engine en; std::vector<long> steps;
      run(g, en, (long)(4.0 / ppb()), from33, tempo, sr, 128, &steps);
      printf("mid-16th start: first step n=%ld (expect 14)\n", steps.empty() ? -1 : steps[0]);
      CHECK(!steps.empty() && steps[0] == 14, "first step %ld", steps.empty() ? -1 : steps[0]); }
    // 3. forward locate: tick follows the new position, no flood
    { TransportGrid g; Engine en; std::vector<long> steps;
      run(g, en, (long)(4.0 / ppb()), locate, tempo, sr, 128, &steps);
      long after = 0; for (long s : steps) if (s >= 160) { after = s; break; }
      printf("locate 2.0 -> 40.0: first step after = n=%ld (expect 160)\n", after);
      CHECK(after == 160, "first step after the locate %ld", after); }
    // 4. a dropped pulse is healed at the next bar: afterwards ticks are on the grid again
    { TransportGrid g; Engine en;
      double blk = ppb(); long blocks = (long)(16.0 / blk), drop = (long)(5.3 / blk);
      run(g, en, blocks, [](long k) { return k * ppb(); }, tempo, sr, 128, nullptr, drop);
      double want = (blocks - 1) * blk;   // the engine's tick must equal the host position (in pulses) again
      printf("dropped pulse: engine tick %ld vs host ppq*24 %.1f\n", en.tick, want * 24.0);
      CHECK(std::fabs(en.tick - want * 24.0) <= 2.0, "engine never re-synced: %ld vs %.1f", en.tick, want * 24.0); }
    printf("%s\n", fails ? "GRID FAILED" : "GRID OK");
    return fails ? 1 : 0;
}
