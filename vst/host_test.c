/* Offline x86 host test for euclidier_vst's VST2 wrapper. Spawns the real
 * standalone euclidier binary (EUCLIDIER_BIN env var, set by build.sh to an x86
 * build of the same source), talks to it over its real control socket, and
 * drives a synthesized transport, same shape as force-acid/host_test.c. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "params.h"

typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*d)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void *proc;
    void (*setP)(AEffect *, int32_t, float);
    float (*getP)(AEffect *, int32_t);
    int32_t np, npar, ni, no, flags;
    intptr_t r1, r2;
    int32_t a, b, c;
    float io;
    void *obj, *user;
    int32_t uid, ver;
    void (*pr)(AEffect *, float **, float **, int32_t);
    void *pdr;
    char f[56];
};
typedef struct { double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
                 int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags; } TI;
enum { kPlaying = 1 << 1, kPpq = 1 << 9, kTempo = 1 << 10 };

extern AEffect *VSTPluginMain(cb);

static TI g_ti;
static int automated[NPARAMS];
static intptr_t host(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    (void)e; (void)v; (void)p; (void)o;
    if (op == 0 && idx >= 0 && idx < NPARAMS) automated[idx]++;   /* audioMasterAutomate */
    if (op == 7) return (intptr_t)&g_ti;   /* audioMasterGetTime */
    return 0;
}

int main(void) {
    AEffect *a = VSTPluginMain(host), *b = VSTPluginMain(host);
    printf("magic=%x params=%d flags=%x uid=%x twoInstances=%d\n", a->magic, a->npar, a->flags, a->uid, a != b);

    char name[64], disp[64];
    for (int i = 0; i < a->npar && i < 6; i++) {
        a->d(a, 8, i, 0, name, 0);
        a->d(a, 7, i, 0, disp, 0);
        printf("  p%-2d %-10s = %-8s (norm %.3f)\n", i, name, disp, a->getP(a, i));
    }

    /* l1_mode (index 11): options NOTE/DRUM -- set to DRUM */
    a->setP(a, 11, 1.0f);
    a->d(a, 7, 11, 0, disp, 0);
    printf("l1_mode -> 1.0 => %s (expect DRUM)\n", disp);

    /* l1_steps (index 3): numeric round trip */
    a->setP(a, 3, 0.5f);
    a->d(a, 7, 3, 0, disp, 0);
    printf("l1_steps -> 0.5 => %s\n", disp);

    /* momentary: preset_load must not crash and should spring back */
    for (int i = 0; i < a->npar; i++) {
        a->d(a, 8, i, 0, name, 0);
        if (!strcmp(name, "Load")) { a->setP(a, i, 1.0f); printf("fired %s (idx %d)\n", name, i); break; }
    }

    /* drive transport a bit: playing at 120 BPM for a couple seconds of ppq */
    g_ti.flags = kPlaying | kPpq | kTempo;
    g_ti.tempo = 120.0;
    float L[128], R[128], *out[2] = { L, R };
    double sr = 44100.0, ppq_per_block = (g_ti.tempo / 60.0) * (128.0 / sr);
    for (int k = 0; k < 400; k++) {
        a->pr(a, 0, out, 128);
        g_ti.ppqPos += ppq_per_block;
    }
    printf("400 blocks of transport fed with no crash\n");

    /* popup (wrapper/popup.h): a tap opens it, a Q-Link nudge leaves it open, a pick closes it and
     * tells the host once, and the flag stays out of the chunk */
    int fails = 0;
    for (int i = 0; i < NPARAMS; i++) {
        if (PARAMS[i].popup_of < 0) continue;
        int t = PARAMS[i].popup_of, no = PARAMS[t].nopts;
        a->setP(a, i, 1.0f);
        int opened = a->getP(a, i) > 0.5f;
        a->setP(a, t, 0.5f / (no - 1)); a->pr(a, 0, out, 128);
        int kept = a->getP(a, i) > 0.5f && !automated[i];
        a->setP(a, t, 1.0f); a->pr(a, 0, out, 128);
        int closed = a->getP(a, i) < 0.5f && automated[i] == 1;
        printf("popup %s: opens %d, nudge keeps it open %d, pick closes it %d\n", PARAMS[i].key, opened, kept, closed);
        fails += !(opened && kept && closed);
    }

    /* chunk round-trip */
    void *chunk = 0;
    intptr_t n = a->d(a, 23, 0, 0, &chunk, 0);
    printf("chunk %ld bytes: %.160s...\n", (long)n, (char *)chunk);
    if (n > 0) b->d(b, 24, 0, n, chunk, 0);
    printf("chunk applied to instance b with no crash\n");
    if (n > 0 && strstr((char *)chunk, "__open")) { printf("FAIL popup flag saved in the chunk\n"); fails++; }

    a->d(a, 1, 0, 0, 0, 0);
    b->d(b, 1, 0, 0, 0, 0);
    printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
