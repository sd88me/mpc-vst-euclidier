/* =============================================================================
 * euclidier_vst.cpp - Euclidier as a VST2 MIDI generator for the MPC OS plugin
 * host (see https://github.com/sd88me/mpc-vst-plugins). Unlike Force Acid/Crate
 * Digger, euclidier.cpp is a monolithic standalone app (no plugin_api_v2/v1
 * library split), so this wrapper does NOT link the engine in-process. Instead
 * it drives the existing standalone binary exactly the way force-shadow's GUI
 * already does on the Force:
 *
 *   force-shadow (Force)                    euclidier_vst.cpp (MPC plugin)
 *   ------------------------------------    ---------------------------------
 *   engine started by NSMODULE.json/        posix_spawn's the same binary per
 *   run_euclidier.sh, its own process        instance (--ctrl-sock <unique path>)
 *   real MIDI clock -> engine's RtMidi       audioMasterGetTime (ppqPos/tempo)
 *   virtual input port "Euclidier"           synthesises 24-PPQN 0xF8/0xFA/0xFC,
 *                                             sent via our own ALSA seq client
 *                                             into the engine's virtual input port
 *   GET/SET over ctrl_sock (Unix socket)     same protocol, same socket path
 *   engine's own virtual MIDI output port    unchanged -- MPC hot-detects it like
 *   "Euclidier", user routes it in Force     any other MIDI-generator ALSA port
 *                                             (mpc-vst-plugins docs/NOTES.md); no
 *                                             VST MIDI-out plumbing needed here
 *
 * So processReplacing()'s only job is feeding a synthesized MIDI clock into the
 * child's ALSA port. Parameter get/set do NOT talk to the socket synchronously:
 * setParameter()/getParameter()/effGetParamDisplay() only touch an in-memory
 * per-instance cache (nothing else ever changes these values but us, so the
 * cache is authoritative), and every actual "SET key val" round-trip happens on
 * a dedicated worker thread (coalesced -- rapid Q-Link nudges collapse to the
 * latest value per key). This mirrors the repo's own rule for app-style plugins
 * ("never block the audio thread... network/disk go on a worker thread", see
 * force-cratedigger and mpc-vst-plugins docs/NOTES.md "Beyond synths"), just
 * applied to setParameter/getParameter instead of processReplacing: a first cut
 * that did the socket round-trip inline measured WARN (worst p99 15.7%) on a
 * Q-Link sweep purely from blocking connect/send/recv syscalls, none of it real
 * work -- moving it off is the fix (docs/PORTING.md "Bench" + this file's own
 * bench run, 2026-09-24).
 * ========================================================================== */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NO_ALSA
#include <alsa/asoundlib.h>
#endif

#include "params.h"
#include "popup.h"    /* mpc-vst-plugins wrapper/popup.h, copied into build/ by build.sh */
#include "plugin_dir.h"   /* mpc-vst-plugins wrapper/plugin_dir.h: the folder this .so was loaded from */

extern char **environ;

/* ---- VST2 ABI (hand-written; no Steinberg SDK) ---------------------------- */
struct AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
struct AEffect {
    int32_t magic;
    intptr_t (*dispatcher)(AEffect *, int32_t, int32_t, intptr_t, void *, float);
    void (*process)(AEffect *, float **, float **, int32_t);
    void (*setParameter)(AEffect *, int32_t, float);
    float (*getParameter)(AEffect *, int32_t);
    int32_t numPrograms, numParams, numInputs, numOutputs, flags;
    intptr_t resvd1, resvd2;
    int32_t initialDelay, realQualities, offQualities;
    float ioRatio;
    void *object, *user;
    int32_t uniqueID, version;
    void (*processReplacing)(AEffect *, float **, float **, int32_t);
    void (*processDoubleReplacing)(AEffect *, double **, double **, int32_t);
    char future[56];
};
typedef struct { int32_t type, byteSize, deltaFrames, flags; char data[16]; } VstEvent;
typedef struct {
    int32_t type, byteSize, deltaFrames, flags, noteLength, noteOffset;
    unsigned char midiData[4];
    char detune, noteOffVelocity, reserved1, reserved2;
} VstMidiEvent;
typedef struct { int32_t numEvents; intptr_t reserved; VstEvent *events[64]; } VstEvents;
typedef struct {
    double samplePos, sampleRate, nanoSeconds, ppqPos, tempo, barStartPos, cycleStartPos, cycleEndPos;
    int32_t timeSigNumerator, timeSigDenominator, smpteOffset, smpteFrameRate, samplesToNextClock, flags;
} VstTimeInfo;

enum {
    effOpen = 0, effClose = 1, effGetParamLabel = 6, effGetParamDisplay = 7, effGetParamName = 8,
    effSetSampleRate = 10, effSetBlockSize = 11, effMainsChanged = 12, effGetChunk = 23,
    effSetChunk = 24, effProcessEvents = 25, effCanBeAutomated = 26, effGetPlugCategory = 35,
    effGetEffectName = 45, effGetVendorString = 47, effGetProductString = 48,
    effGetVendorVersion = 49, effCanDo = 51, effGetVstVersion = 58,
};
enum { audioMasterAutomate = 0, audioMasterGetTime = 7, audioMasterUpdateDisplay = 42 };
enum { kVstTransportPlaying = 1 << 1, kVstPpqPosValid = 1 << 9, kVstTempoValid = 1 << 10 };
enum { effFlagsCanReplacing = 1 << 4, effFlagsProgramChunks = 1 << 5, effFlagsIsSynth = 1 << 8 };

static FILE *g_log;
#define LOG(...) do { if (g_log) { std::fprintf(g_log, __VA_ARGS__); std::fflush(g_log); } } while (0)
/* One slot per live instance (lowest free), so names stay stable when an instance is removed and inserted again: the first
 * instance is "Euclidier" / "Euclidier Clock", the next "Euclidier 2" / "Euclidier Clock 2", and MPC keeps a device's settings. */
static std::mutex g_slot_mu;
static unsigned g_slot_mask = 0;
static int slot_acquire() {
    std::lock_guard<std::mutex> lk(g_slot_mu);
    for (int i = 0; i < 31; i++) if (!(g_slot_mask & (1u << i))) { g_slot_mask |= 1u << i; return i; }
    return 31;
}
static void slot_release(int i) { std::lock_guard<std::mutex> lk(g_slot_mu); if (i >= 0) g_slot_mask &= ~(1u << i); }
static std::atomic<int> g_sock_seq{0};         /* control-socket paths: counted even when ALSA is unavailable */

/* ---- default location of the standalone engine binary, override with
 * EUCLIDIER_BIN for host testing. ../DESIGN.md: already deployed at this path
 * on the Force. -------------------------------------------------------------- */
static const char *engine_path() {
    static std::string path;
    const char *p = getenv("EUCLIDIER_BIN");
    if (p && *p) return p;
    char dir[512];
    if (mpc_plugin_dir(dir, sizeof dir)) {   /* the engine shipped in the plugin folder, next to this .so */
        path = std::string(dir) + "/euclidier";
        if (access(path.c_str(), X_OK) == 0) return path.c_str();
    }
    return "/media/662522/AddOns/Euclidier/euclidier";   /* force-euclidier's own install */
}

/* ---------------------------------------------------------------------------
 * Unix control-socket client -- same "GET key\n"/"SET key val\n" protocol as
 * euclidier.cpp's ctrlThread(). One connect/send/recv/close per request,
 * short-timeout so a wedged child can't hang the caller.
 * ------------------------------------------------------------------------- */
static bool ctrl_request(const std::string &sockpath, const std::string &req, std::string &reply) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct timeval tv = {0, 200 * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sockpath.c_str(), sizeof a.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return false; }
    std::string line = req + "\n";
    if (send(fd, line.c_str(), line.size(), MSG_NOSIGNAL) < 0) { close(fd); return false; }
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    reply.assign(buf);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    return true;
}

/* ---- per-instance state ----------------------------------------------------- */
struct Plugin {
    AEffect fx;
    audioMasterCallback master;
    std::string sockpath;
    pid_t child = -1;
    volatile int release[NPARAMS] = {0};
    float open[NPARAMS] = {0};     /* popup "open" flags (popup.h): wrapper-only, never sent or saved */
    double last_ppq = 0.0;
    bool was_playing = false;
#ifndef NO_ALSA
    snd_seq_t *seq = nullptr;
    int seq_port = -1;
    int dest_client = -1, dest_port = -1;
#endif
    char chunk[4096] = {0};
    std::string clock_name;   /* our ALSA clock client: the only clock the engine follows (--clock-from) */
    std::string engine_client;   /* the engine's ALSA client name (--client-name): where the clock is sent */
    int slot = -1;

    /* Step displays (make_skin.py): per-lane pattern views polled by the worker, turned into the values of the
     * display-only cell/ring params, and pushed to the host from processReplacing when they change. */
    struct LaneView { int steps = 0, play = -1, enabled = 0, selected = 0; int bits[64] = {0}; };
    LaneView lv[8];
    std::mutex lvmu;                      /* guards lv[] and text[] */
    std::string text[NPARAMS];            /* string_display params (lane info) */
    std::atomic<float> told[NPARAMS];     /* what the host last knew for each param (-1: nothing yet); a cache change that differs is pushed */
    std::atomic<bool> playing{false};

    /* Cache + async worker: setParameter/getParameter/effGetParamDisplay only
     * ever touch `cache` (see file header comment). */
    std::atomic<float> cache[NPARAMS];
    std::thread io_thread;
    std::mutex qmu;
    std::condition_variable qcv;
    std::unordered_map<std::string, std::string> pending; /* key -> value string, coalesced */
    std::vector<std::pair<std::string, std::string>> script; /* ordered SETs (randomise: mask, go, clear), run after `pending` */
    bool want_refresh = false;
    bool stop_io = false;
};

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static void copy_str(void *dst, const std::string &s, size_t max) {
    std::strncpy((char *)dst, s.c_str(), max - 1);
    ((char *)dst)[max - 1] = 0;
}
static void norm_to_str(const param_t *p, float n, char *buf, int len) {
    if (p->nopts) std::snprintf(buf, len, "%d", (int)std::lround(clamp01(n) * (p->nopts - 1)));
    else std::snprintf(buf, len, "%d", (int)std::lround(p->min + (p->max - p->min) * clamp01(n)));
}
static float str_to_norm(const param_t *p, const std::string &s) {
    if (p->nopts) {
        int idx = std::atoi(s.c_str());
        if (idx < 0) idx = 0;
        if (idx > p->nopts - 1) idx = p->nopts - 1;
        return p->nopts > 1 ? (float)idx / (p->nopts - 1) : 0.0f;
    }
    return p->max > p->min ? clamp01((float)((std::atof(s.c_str()) - p->min) / (p->max - p->min))) : 0.0f;
}

/* ---- display-only step-display params (names from make_skin.py) -------------- */
enum VKind : unsigned char { V_NONE = 0, V_GCELL, V_CCELL, V_INFO };
struct VInfo { VKind kind = V_NONE; int lane = 0, cap = 0, slot = 0; };
static VInfo g_v[NPARAMS];
static std::vector<int> g_vlist;                       /* display-only params, lane info first */
static std::vector<int> g_gcaps, g_ccaps;              /* size-class capacities, ascending */
static std::unordered_map<std::string, int> g_kidx;    /* key -> param index */
static void add_cap(std::vector<int> &v, int c) { if (std::find(v.begin(), v.end(), c) == v.end()) v.push_back(c); }
static void build_vtable() {
    std::vector<int> infos, cells;
    for (int i = 0; i < NPARAMS; i++) {
        const char *k = PARAMS[i].key;
        int a, b, c;
        g_kidx[k] = i;
        VInfo &v = g_v[i];
        if (std::sscanf(k, "g%d_%d_%d", &a, &b, &c) == 3 && k[0] == 'g') { v = {V_GCELL, a - 1, b, c}; add_cap(g_gcaps, b); cells.push_back(i); }
        else if (std::sscanf(k, "c%d_%d", &a, &b) == 2 && k[0] == 'c') { v = {V_CCELL, 0, a, b}; add_cap(g_ccaps, a); cells.push_back(i); }
        else if (std::sscanf(k, "l%d_info", &a) == 1 && std::strstr(k, "_info")) { v = {V_INFO, a - 1, 0, 0}; infos.push_back(i); }
    }
    g_vlist = infos;
    g_vlist.insert(g_vlist.end(), cells.begin(), cells.end());
    std::sort(g_gcaps.begin(), g_gcaps.end());
    std::sort(g_ccaps.begin(), g_ccaps.end());
}
static bool is_virtual(int i) { return g_v[i].kind != V_NONE; }
static int class_for(const std::vector<int> &caps, int n) {
    for (int c : caps) if (c >= n) return c;
    return caps.empty() ? 0 : caps.back();
}
/* slot[0..cap): 0 hidden / 1 off / 2 on for `n` steps (bits[]) shown in `cap` slots. `contiguous`: a lane row, one
 * cell per step from the left while n <= cap; otherwise the steps are spread round the circle (several steps
 * sharing a slot when n > cap light it if any is on). */
static void fill_slots(const int *bits, int n, int cap, bool contiguous, int *slot) {
    for (int s = 0; s < cap; s++) slot[s] = 0;
    if (n <= 0 || cap <= 0) return;
    if (contiguous && n <= cap) { for (int s = 0; s < n; s++) slot[s] = 1 + (bits[s] ? 1 : 0); return; }
    for (int s = 0; s < cap; s++) slot[s] = 1;
    for (int i = 0; i < n && i < 64; i++) if (bits[i]) slot[(i * cap) / n] = 2;
    if (n < cap) {   /* only the slots a step lands on exist */
        int used[64] = {0};
        for (int i = 0; i < n; i++) used[(i * cap) / n] = 1;
        for (int s = 0; s < cap; s++) if (!used[s]) slot[s] = 0;
    }
}
static int play_slot(int play, int n, int cap, bool contiguous) {
    if (play < 0 || play >= n) return -1;
    return contiguous && n <= cap ? play : (play * cap) / n;
}

/* Blocking GET, used only off the hot path: initial cache fill and the worker
 * thread's post-trigger refresh (see io_worker). Never called from
 * setParameter/getParameter/effGetParamDisplay. */
static float get_norm_blocking(const std::string &sockpath, int i) {
    std::string reply;
    if (!ctrl_request(sockpath, "GET " + std::string(PARAMS[i].key), reply) || reply == "ERR")
        return PARAMS[i].def;
    return str_to_norm(&PARAMS[i], reply);
}

/* ---------------------------------------------------------------------------
 * Step displays: poll every lane's pattern, derive the display-only params' values.
 * "l<N>_pattern" -> "steps|bits|play|loop|enabled|selected" (euclidier.cpp ctrlGet).
 * ------------------------------------------------------------------------- */
static void parse_pattern(const std::string &r, Plugin::LaneView &L) {
    L = Plugin::LaneView();
    size_t p0 = r.find('|');
    if (p0 == std::string::npos) return;
    L.steps = std::atoi(r.c_str());
    if (L.steps > 64) L.steps = 64;
    size_t p1 = r.find('|', p0 + 1);
    std::string bits = r.substr(p0 + 1, p1 == std::string::npos ? std::string::npos : p1 - p0 - 1);
    int k = 0;
    for (size_t i = 0; i < bits.size() && k < 64; i++) if (bits[i] == '0' || bits[i] == '1') L.bits[k++] = bits[i] == '1';
    if (k < L.steps) L.steps = k;
    if (p1 == std::string::npos) return;
    int f[4] = {-1, 0, 0, 0}, nf = 0;   /* play, loop, enabled, selected */
    const char *q = r.c_str() + p1 + 1;
    while (nf < 4 && *q) { f[nf++] = std::atoi(q); const char *b = std::strchr(q, '|'); if (!b) break; q = b + 1; }
    L.play = f[0]; L.enabled = f[2]; L.selected = f[3];
}
static float idx_norm(int idx, int nopts) { return nopts > 1 ? (float)idx / (nopts - 1) : 0.0f; }
static void compute_views(Plugin *w) {
    /* a cell's value: 0 hidden, 1 off, 2 on, +2 while the play-head is on it (options "-", OFF, ON, OFF >, ON >) */
    Plugin::LaneView lv[8];
    bool playing = w->playing.load();
    { std::lock_guard<std::mutex> lk(w->lvmu); for (int i = 0; i < 8; i++) lv[i] = w->lv[i]; }
    int sel = 0;
    for (int i = 0; i < 8; i++) if (lv[i].selected) sel = i;
    int gcls[8], gslot[8][64], ccls = class_for(g_ccaps, lv[sel].steps), cslot[64];
    for (int n = 0; n < 8; n++) {
        gcls[n] = class_for(g_gcaps, lv[n].steps);
        fill_slots(lv[n].bits, lv[n].steps, gcls[n], true, gslot[n]);
        int ps = playing && lv[n].enabled ? play_slot(lv[n].play, lv[n].steps, gcls[n], true) : -1;
        if (ps >= 0 && gslot[n][ps] && gcls[n] <= 16) gslot[n][ps] += 2;   /* the 32-step row has no play-head state (make_skin.py GRID_CUR_MAX) */
    }
    fill_slots(lv[sel].bits, lv[sel].steps, ccls, false, cslot);
    int cps = playing && lv[sel].enabled ? play_slot(lv[sel].play, lv[sel].steps, ccls, false) : -1;
    if (cps >= 0 && cslot[cps]) cslot[cps] += 2;
    for (int i : g_vlist) {
        const VInfo &v = g_v[i];
        float val;
        if (v.kind == V_GCELL) val = v.cap == gcls[v.lane] ? idx_norm(gslot[v.lane][v.slot], PARAMS[i].nopts) : 0.0f;
        else if (v.kind == V_CCELL) val = v.cap == ccls ? idx_norm(cslot[v.slot], PARAMS[i].nopts) : 0.0f;
        else continue;
        w->cache[i].store(val);
    }
}
static void poll_views(Plugin *w, int tick) {
    for (int n = 0; n < 8; n++) {
        std::string r;
        char k[24];
        std::snprintf(k, sizeof k, "l%d_pattern", n + 1);
        if (ctrl_request(w->sockpath, std::string("GET ") + k, r)) {
            Plugin::LaneView L;
            parse_pattern(r, L);
            std::lock_guard<std::mutex> lk(w->lvmu);
            w->lv[n] = L;
        }
        char key[24];
        std::snprintf(key, sizeof key, "l%d_info", n + 1);
        auto it = g_kidx.find(key);
        if (tick % 8 == 0 && it != g_kidx.end()) {   /* the lane readout, if the skin has one */
            std::snprintf(k, sizeof k, "l%d_info", n + 1);
            if (ctrl_request(w->sockpath, std::string("GET ") + k, r)) { std::lock_guard<std::mutex> lk(w->lvmu); w->text[it->second] = r; }
        }
    }
    compute_views(w);
}

/* ---------------------------------------------------------------------------
 * Async worker: drains coalesced SET requests and, after a trigger (preset
 * load/save, randomize -- anything that changes engine state on its own, not
 * just the one key SET), re-GETs every param to catch what the engine changed
 * behind our back. Runs entirely off setParameter/getParameter's caller thread.
 * ------------------------------------------------------------------------- */
static void io_worker(Plugin *w) {
    std::unique_lock<std::mutex> lk(w->qmu);
    for (int tick = 0;; tick++) {
        w->qcv.wait_for(lk, std::chrono::milliseconds(30), [w] { return w->stop_io || !w->pending.empty() || !w->script.empty() || w->want_refresh; });
        if (w->stop_io) return;
        std::vector<std::pair<std::string, std::string>> todo(w->pending.begin(), w->pending.end());
        w->pending.clear();
        /* The engine's SETs are not independent: `sel` picks the lane the sel_* keys write to, and a lane's `mode` makes the
         * engine choose that mode's note, which would overwrite a note set before it. `pending` is unordered, so a preset or
         * project restore must be applied in this order: sel, then modes, then everything else. */
        auto prio = [](const std::string &k) { return k == "sel" ? 0 : (k.size() > 5 && k.compare(k.size() - 5, 5, "_mode") == 0) ? 1 : 2; };
        std::stable_sort(todo.begin(), todo.end(), [&](const auto &x, const auto &y) { return prio(x.first) < prio(y.first); });
        auto steps = std::move(w->script);
        w->script.clear();
        bool refresh = w->want_refresh;
        w->want_refresh = false;
        lk.unlock();

        for (auto &kv : todo) {
            std::string reply;
            ctrl_request(w->sockpath, "SET " + kv.first + " " + kv.second, reply);
        }
        for (auto &kv : steps) {
            std::string reply;
            ctrl_request(w->sockpath, "SET " + kv.first + " " + kv.second, reply);
        }
        if (refresh) {
            for (int i = 0; i < NPARAMS; i++) {
                if (PARAMS[i].momentary || popup_is(i) || is_virtual(i)) continue;
                w->cache[i].store(get_norm_blocking(w->sockpath, i));
            }
        }
        poll_views(w, tick);
        lk.lock();
    }
}
static void queue_script(Plugin *w, const std::vector<std::pair<std::string, std::string>> &steps, bool also_refresh) {
    std::lock_guard<std::mutex> lk(w->qmu);
    w->script.insert(w->script.end(), steps.begin(), steps.end());
    if (also_refresh) w->want_refresh = true;
    w->qcv.notify_one();
}
static void queue_set(Plugin *w, const std::string &key, const std::string &val, bool also_refresh) {
    std::lock_guard<std::mutex> lk(w->qmu);
    w->pending[key] = val;
    if (also_refresh) w->want_refresh = true;
    w->qcv.notify_one();
}

/* ---------------------------------------------------------------------------
 * Spawn the standalone engine. posix_spawn (never fork() -- mpc-vst-plugins
 * docs/NOTES.md "Beyond synths"), cleaned environment (drop LD_PRELOAD so a
 * MockbaMod-preloaded MPC doesn't hand it a C++ lib the child can't use).
 * ------------------------------------------------------------------------- */
static bool spawn_engine(Plugin *w) {
    const char *bin = engine_path();
    if (access(bin, X_OK) != 0) { LOG("[euclidier_vst] engine binary not found/executable: %s\n", bin); return false; }

    std::vector<std::string> keep_env;
    for (char **e = environ; *e; e++) {
        if (std::strncmp(*e, "LD_PRELOAD=", 11) == 0) continue;
        keep_env.push_back(*e);
    }
    std::vector<char *> envp;
    for (auto &s : keep_env) envp.push_back(&s[0]);
    envp.push_back(nullptr);

    /* MPC sends its own MIDI clock to every MIDI port (a device's "sync" setting), on top of the clock this plugin makes from
     * the host transport: two clocks run the engine at double speed or worse, so it follows only ours. */
    std::vector<char *> args = {(char *)bin, (char *)"-v", (char *)"--ctrl-sock", (char *)w->sockpath.c_str()};
    if (!w->clock_name.empty()) {
        args.push_back((char *)"--clock-from"); args.push_back((char *)w->clock_name.c_str());
        args.push_back((char *)"--client-name"); args.push_back((char *)w->engine_client.c_str());
    }
    args.push_back(nullptr);
    char **argv = args.data();
    pid_t pid;
    int rc = posix_spawn(&pid, bin, nullptr, nullptr, argv, envp.data());
    if (rc != 0) { LOG("[euclidier_vst] posix_spawn failed: %s\n", strerror(rc)); return false; }
    w->child = pid;
    LOG("[euclidier_vst] spawned engine pid=%d sock=%s\n", (int)pid, w->sockpath.c_str());

    /* wait for the control socket to come up (engine binds it after ~1s of its
     * own startup delay, see euclidier.cpp main()'s sleep(1)) */
    for (int i = 0; i < 60; i++) {
        std::string reply;
        if (ctrl_request(w->sockpath, "GET transport", reply)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG("[euclidier_vst] engine control socket never came up\n");
    return true; /* keep going -- it may still come up a little late */
}

#ifndef NO_ALSA
/* ---------------------------------------------------------------------------
 * ALSA seq: open our own client/port and connect it to the child engine's
 * virtual "Euclidier" input port so we can feed it a synthesized clock. The
 * engine's own "Euclidier" *output* port needs no help from us -- MPC OS
 * hot-detects any ALSA seq port and the user routes it as a track's MIDI
 * input, same as every other MIDI-generator port (docs/NOTES.md).
 * ------------------------------------------------------------------------- */
static bool find_engine_input_port(const std::string &clientName, int &client, int &port) {
    snd_seq_t *probe;
    if (snd_seq_open(&probe, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0) return false;
    snd_seq_client_info_t *cinfo;
    snd_seq_port_info_t *pinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_port_info_alloca(&pinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    bool found = false;
    while (!found && snd_seq_query_next_client(probe, cinfo) >= 0) {
        int cl = snd_seq_client_info_get_client(cinfo);
        const char *cname = snd_seq_client_info_get_name(cinfo);
        if (!cname || clientName != cname) continue;   /* exactly this instance's engine: not MPC's own "Euclidier" ports */
        snd_seq_port_info_set_client(pinfo, cl);
        snd_seq_port_info_set_port(pinfo, -1);
        while (!found && snd_seq_query_next_port(probe, pinfo) >= 0) {
            const char *name = snd_seq_port_info_get_name(pinfo);
            unsigned int caps = snd_seq_port_info_get_capability(pinfo);
            if (name && (caps & SND_SEQ_PORT_CAP_WRITE)) {
                client = cl; port = snd_seq_port_info_get_port(pinfo);
                found = true;
            }
        }
    }
    snd_seq_close(probe);
    return found;
}
static void alsa_open(Plugin *w) {
    if (snd_seq_open(&w->seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { w->seq = nullptr; return; }
    w->slot = slot_acquire();
    char name[32], eng[32];
    if (w->slot == 0) { std::snprintf(name, sizeof name, "Euclidier Clock"); std::snprintf(eng, sizeof eng, "Euclidier"); }
    else { std::snprintf(name, sizeof name, "Euclidier Clock %d", w->slot + 1); std::snprintf(eng, sizeof eng, "Euclidier %d", w->slot + 1); }
    snd_seq_set_client_name(w->seq, name);
    w->clock_name = name;
    w->engine_client = eng;
    w->seq_port = snd_seq_create_simple_port(w->seq, "Clock Out",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
}
static void alsa_send_byte(Plugin *w, unsigned char b) {
    if (!w->seq || w->seq_port < 0) return;
    if (w->dest_client < 0 && !find_engine_input_port(w->engine_client, w->dest_client, w->dest_port)) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, w->seq_port);
    snd_seq_ev_set_dest(&ev, w->dest_client, w->dest_port);
    snd_seq_ev_set_direct(&ev);
    ev.type = SND_SEQ_EVENT_CLOCK;
    if (b == 0xFA) ev.type = SND_SEQ_EVENT_START;
    else if (b == 0xFB) ev.type = SND_SEQ_EVENT_CONTINUE;
    else if (b == 0xFC) ev.type = SND_SEQ_EVENT_STOP;
    snd_seq_event_output_direct(w->seq, &ev);
}
static void alsa_close(Plugin *w) {
    if (w->seq) snd_seq_close(w->seq);
    w->seq = nullptr;
    slot_release(w->slot);
    w->slot = -1;
}
#else
static void alsa_open(Plugin *) {}
static void alsa_send_byte(Plugin *, unsigned char) {}
static void alsa_close(Plugin *) {}
#endif

/* ---------------------------------------------------------------------------
 * Transport / clock synthesis: audioMasterGetTime -> synthetic 24-PPQN clock
 * sent to the child over ALSA seq, same math as acid_vst.cpp's feed_transport.
 * ------------------------------------------------------------------------- */
static void feed_transport(Plugin *w) {
    VstTimeInfo *ti = (VstTimeInfo *)w->master(&w->fx, audioMasterGetTime, 0,
                                                kVstTempoValid | kVstPpqPosValid, 0, 0);
    bool playing = ti && (ti->flags & kVstTransportPlaying);

    if (playing && !w->was_playing) { w->last_ppq = ti->ppqPos; alsa_send_byte(w, 0xFA); }
    else if (!playing && w->was_playing) alsa_send_byte(w, 0xFC);
    w->was_playing = playing;
    w->playing.store(playing);

    if (playing && ti) {
        const double step = 1.0 / 24.0;
        double start = w->last_ppq, end = ti->ppqPos;
        if (end < start) start = end;
        double next = std::ceil(start / step) * step;
        for (; next < end + 1e-9; next += step) alsa_send_byte(w, 0xF8);
        w->last_ppq = ti->ppqPos;
    }
}

/* ---------------------------------------------------------------------------
 * VST callbacks
 * ------------------------------------------------------------------------- */
static void processReplacing(AEffect *e, float **in, float **out, int32_t n) {
    (void)in;
    Plugin *w = (Plugin *)e->object;
    feed_transport(w);
    int budget = 96;   /* host calls per block: a lane resize touches ~100 cells, spread over a few blocks */
    for (int i : g_vlist) {   /* display-only params first, then any real param the engine changed behind the host's back */
        if (budget <= 0) break;
        float v = w->cache[i].load();
        if (v != w->told[i].load()) { w->told[i].store(v); w->master(&w->fx, audioMasterAutomate, i, 0, 0, v); budget--; }
    }
    for (int i = 0; i < NPARAMS && budget > 0; i++) {
        if (is_virtual(i) || PARAMS[i].momentary || popup_is(i)) continue;
        float v = w->cache[i].load();
        if (v != w->told[i].load()) { w->told[i].store(v); w->master(&w->fx, audioMasterAutomate, i, 0, 0, v); budget--; }
    }
    for (int i = 0; i < NPARAMS; i++)
        if (w->release[i]) { w->release[i] = 0; w->master(&w->fx, audioMasterAutomate, i, 0, 0, 0.0f); }
    for (int32_t i = 0; i < n; i++) out[0][i] = out[1][i] = 0.0f;   /* MIDI generator: no audio */
}

/* True for keys whose SET has side effects on other params inside the engine
 * (preset_load/preset_save mutate every lane; rand_go re-randomizes whichever
 * lanes are selected) -- see euclidier.cpp's ctrlSet(). Everything else's SET
 * only ever touches its own key. */
static bool triggers_refresh(const char *key) {
    return !std::strcmp(key, "preset_load") || !std::strcmp(key, "rand_go");
}

/* Keep the selected lane's "sel_<x>" and "l<N>_<x>" caches equal (the engine has one value for both). */
static void mirror_sel(Plugin *w, int i, float n) {
    const char *k = PARAMS[i].key;
    auto si = g_kidx.find("sel");
    if (si == g_kidx.end()) return;
    int sel = (int)std::lround(w->cache[si->second].load() * 7.0f);
    std::string other;
    if (!std::strncmp(k, "sel_", 4)) other = "l" + std::to_string(sel + 1) + "_" + (k + 4);
    else if (k[0] == 'l' && k[1] >= '1' && k[1] <= '8' && k[2] == '_' && k[1] - '1' == sel) other = std::string("sel_") + (k + 3);
    else return;
    auto it = g_kidx.find(other);
    if (it != g_kidx.end()) w->cache[it->second].store(n);
}
static int value_of(Plugin *w, int i) {   /* a param's current value in its own domain */
    const param_t *p = &PARAMS[i];
    float n = w->cache[i].load();
    return p->nopts ? (int)std::lround(n * (p->nopts - 1)) : (int)std::lround(p->min + (p->max - p->min) * n);
}
static void setParameter(AEffect *e, int32_t i, float n) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return;
    const param_t *p = &PARAMS[i];
    if (is_virtual(i)) return;   /* display only */
    if (popup_set(w->open, i, n)) return;
    if (p->momentary && p->step_target >= 0) {   /* a stepper arrow: nudge another param by step_delta */
        if (n > 0.5f) {
            const param_t *t = &PARAMS[p->step_target];
            int lo = t->nopts ? 0 : (int)t->min, hi = t->nopts ? t->nopts - 1 : (int)t->max;
            int nv = std::min(hi, std::max(lo, value_of(w, p->step_target) + (int)p->step_delta));
            float tn = t->nopts ? idx_norm(nv, t->nopts) : (float)(nv - t->min) / (t->max - t->min);
            char buf[32];
            norm_to_str(t, tn, buf, sizeof buf);
            w->cache[p->step_target].store(tn);   /* told[] stays: the host hears the new value from processReplacing */
            mirror_sel(w, p->step_target, tn);
            queue_set(w, t->key, buf, !std::strcmp(t->key, "sel"));   /* a new lane: re-read the sel_* values */
            w->release[i] = 1;
        }
        return;
    }
    if (p->momentary && !std::strncmp(p->key, "rand_", 5) && (!std::strcmp(p->key, "rand_all") || !std::strcmp(p->key, "rand_sel") || (p->key[5] >= '1' && p->key[5] <= '8' && !p->key[6]))) {
        /* randomise one lane (rand_1..rand_8) or all: the engine's rand_go acts on the rand_l<N> flags (none set = all), so
         * set the flags, fire, clear them: an ordered script */
        if (n > 0.5f) {
            int lane = p->key[5] == 'a' ? 0 : p->key[5] - '0';
            if (p->key[5] == 's') {   /* the lane being edited on MAIN */
                auto si = g_kidx.find("sel");
                lane = si == g_kidx.end() ? 1 : (int)std::lround(w->cache[si->second].load() * 7.0f) + 1;
            }
            std::vector<std::pair<std::string, std::string>> st;
            for (int l = 1; l <= 8; l++) st.push_back({"rand_l" + std::to_string(l), l == lane ? "1" : "0"});
            st.push_back({"rand_go", "1"});
            for (int l = 1; l <= 8; l++) st.push_back({"rand_l" + std::to_string(l), "0"});
            queue_script(w, st, true);
            w->release[i] = 1;
        }
        return;
    }
    if (p->momentary && !std::strcmp(p->key, "all_drum")) {   /* every lane to DRUM mode; the engine then moves each note to its drum note */
        if (n > 0.5f) {
            for (int l = 1; l <= 8; l++) {
                auto it = g_kidx.find("l" + std::to_string(l) + "_mode");
                if (it == g_kidx.end()) continue;
                w->cache[it->second].store(1.0f);
                mirror_sel(w, it->second, 1.0f);
                queue_set(w, PARAMS[it->second].key, "1", l == 8);   /* one re-read after the last */
            }
            w->release[i] = 1;
        }
        return;
    }
    if (p->momentary) {
        if (n > 0.5f) {
            queue_set(w, p->key, "1", triggers_refresh(p->key));
            w->release[i] = 1;
        }
        return;
    }
    char buf[32];
    const float raw = n;   /* what the host holds; if a nudge snaps it to an option, the snapped value is pushed back */
    bool nudge = false;
    if (p->nopts > 1) {
        float pos = clamp01(n) * (p->nopts - 1);
        if (std::fabs(pos - std::round(pos)) > 0.001f) {
            float cur = w->cache[i].load() * (p->nopts - 1);
            int idx = (int)std::lround(cur) + (pos > cur ? 1 : -1);
            if (idx < 0) idx = 0;
            if (idx > p->nopts - 1) idx = p->nopts - 1;
            n = (float)idx / (p->nopts - 1);
            nudge = true;
        }
    }
    w->cache[i].store(n);
    w->told[i].store(raw);
    mirror_sel(w, i, n);
    norm_to_str(p, n, buf, sizeof buf);
    size_t kl = std::strlen(p->key);
    /* a new lane re-reads the sel_* values; a note/drum switch makes the engine pick that mode's note, so re-read it too */
    bool refresh = !std::strcmp(p->key, "sel") || (kl >= 5 && !std::strcmp(p->key + kl - 5, "_mode"));
    queue_set(w, p->key, buf, refresh);
    if (!nudge) popup_picked(w->open, w->release, i);   /* a list pick closes it; a Q-Link nudge doesn't */
}

static float getParameter(AEffect *e, int32_t i) {
    Plugin *w = (Plugin *)e->object;
    if (i < 0 || i >= NPARAMS) return 0.0f;
    return popup_is(i) ? w->open[i] : w->cache[i].load();
}

static intptr_t dispatcher(AEffect *e, int32_t op, int32_t idx, intptr_t v, void *p, float o) {
    Plugin *w = (Plugin *)e->object;
    (void)o;
    switch (op) {
    case effOpen: return 1;
    case effClose:
        alsa_close(w);
        { std::lock_guard<std::mutex> lk(w->qmu); w->stop_io = true; }
        w->qcv.notify_one();
        if (w->io_thread.joinable()) w->io_thread.join();
        if (w->child > 0) { kill(w->child, SIGTERM); int st; waitpid(w->child, &st, 0); }
        delete w;
        return 1;
    case effGetPlugCategory: return 2;
    case effGetEffectName:
    case effGetProductString: copy_str(p, PLUG_NAME, 32); return 1;
    case effGetVendorString: copy_str(p, PLUG_VENDOR, 32); return 1;
    case effGetVendorVersion: return PLUG_VERSION;
    case effGetVstVersion: return 2400;
    case effCanBeAutomated: return idx >= 0 && idx < NPARAMS && !is_virtual(idx);
    case effGetParamName:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].name, 32);
        return 1;
    case effGetParamLabel:
        if (idx >= 0 && idx < NPARAMS) copy_str(p, PARAMS[idx].unit, 8);
        return 1;
    case effGetParamDisplay: {
        if (idx < 0 || idx >= NPARAMS) return 0;
        const param_t *pp = &PARAMS[idx];
        if (pp->momentary) { copy_str(p, "", 24); return 1; }
        if (pp->string_display) { std::lock_guard<std::mutex> lk(w->lvmu); copy_str(p, w->text[idx], 24); return 1; }
        if (pp->nopts) {
            int k = (int)std::lround((popup_is(idx) ? w->open[idx] : w->cache[idx].load()) * (pp->nopts - 1));
            copy_str(p, pp->opts[k], 24);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d",
                (int)std::lround(pp->min + (pp->max - pp->min) * w->cache[idx].load()));
            copy_str(p, buf, 24);
        }
        return 1;
    }
    case effSetSampleRate: case effSetBlockSize: case effMainsChanged: return 1;
    case effProcessEvents: return 1; /* MPC ignores plugin MIDI in for a generator; no-op */
    case effCanDo:
        return (!std::strcmp((char *)p, "receiveVstTimeInfo")) ? 1 : -1;
    case effGetChunk: {
        /* From cache -- no socket round-trip, consistent with get/setParameter. */
        std::string s;
        for (int i = 0; i < NPARAMS; i++) {
            if (PARAMS[i].momentary || popup_is(i) || is_virtual(i)) continue;
            char buf[32];
            norm_to_str(&PARAMS[i], w->cache[i].load(), buf, sizeof buf);
            s += PARAMS[i].key; s += '='; s += buf; s += ';';
        }
        copy_str(w->chunk, s, sizeof w->chunk);
        *(void **)p = w->chunk;
        return (intptr_t)std::strlen(w->chunk) + 1;
    }
    case effSetChunk: {
        if (v <= 0 || (size_t)v > sizeof w->chunk) return 0;
        std::memcpy(w->chunk, p, (size_t)v);
        w->chunk[v - 1] = 0;
        char *s = w->chunk, *save = nullptr;
        for (char *tok = strtok_r(s, ";", &save); tok; tok = strtok_r(nullptr, ";", &save)) {
            char *eq = std::strchr(tok, '=');
            if (!eq) continue;
            *eq = 0;
            if (!std::strncmp(tok, "sel_", 4)) continue;   /* mirrors of the selected lane's l<N>_ keys (older presets saved them): applying them writes to whatever lane is selected */
            for (int i = 0; i < NPARAMS; i++) {
                if (is_virtual(i) || std::strcmp(PARAMS[i].key, tok) != 0) continue;
                w->cache[i].store(str_to_norm(&PARAMS[i], eq + 1));
                queue_set(w, PARAMS[i].key, eq + 1, false);
                break;
            }
        }
        { std::lock_guard<std::mutex> lk(w->qmu); w->want_refresh = true; }   /* re-read what the engine really holds (it clamps, and fills the sel_* copies) */
        w->qcv.notify_one();
        return 1;
    }
    default: return 0;
    }
}

/* A fresh instance: every lane in DRUM mode (the engine then gives each lane its own General MIDI drum note), lanes 1-4 on with
 * four different standard Euclidean patterns, lanes 5-8 off. */
static void set_defaults(Plugin *w) {
    struct D { int steps, fill; };
    static const D pat[4] = {{16, 4}, {8, 3}, {12, 5}, {16, 5}};   /* four on the floor, tresillo, E(5,12), bossa-ish */
    auto put = [&](const std::string &key, int val, bool refresh) {
        auto it = g_kidx.find(key);
        if (it == g_kidx.end()) return;
        const param_t *p = &PARAMS[it->second];
        float n = p->nopts ? idx_norm(val, p->nopts) : (float)(val - p->min) / (p->max - p->min);
        w->cache[it->second].store(n);
        w->told[it->second].store(n);
        queue_set(w, key, std::to_string(val), refresh);
    };
    for (int l = 1; l <= 8; l++) {
        std::string b = "l" + std::to_string(l) + "_";
        put(b + "mode", 1, false);
        put(b + "enable", l <= 4 ? 1 : 0, l == 8);   /* one re-read after the last write: it also fills the sel_* copies */
        if (l <= 4) {
            put(b + "steps", pat[l - 1].steps, false);
            put(b + "fill", pat[l - 1].fill, false);
            put(b + "shift", 0, false);
        }
    }
}

extern "C" __attribute__((visibility("default"))) AEffect *VSTPluginMain(audioMasterCallback master) {
    if (!g_log) g_log = std::fopen("/tmp/euclidier_vst.log", "a");
    Plugin *w = new Plugin();
    w->master = master;
    int n = g_sock_seq.fetch_add(1);
    char sp[64];
    std::snprintf(sp, sizeof sp, "/tmp/euclidier_vst_%d_%d.sock", (int)getpid(), n);
    w->sockpath = sp;
    static std::once_flag once;
    std::call_once(once, build_vtable);
    for (int i = 0; i < NPARAMS; i++) { w->cache[i].store(PARAMS[i].def); w->told[i].store(-1.0f); }
    alsa_open(w);   /* first: the engine is told which ALSA client its clock comes from */
    spawn_engine(w);
    /* one-time blocking fill at load (counts against open time, not per-block
     * budget -- bench.sh measured "open 1105.6 ms" already, see this file's
     * header comment); every SET/GET after this is cache-only + async. */
    for (int i = 0; i < NPARAMS; i++)
        if (!PARAMS[i].momentary && !popup_is(i) && !is_virtual(i)) w->cache[i].store(get_norm_blocking(w->sockpath, i));
    for (int i = 0; i < NPARAMS; i++) if (!is_virtual(i)) w->told[i].store(w->cache[i].load());   /* the host reads these itself at load */
    w->io_thread = std::thread(io_worker, w);
    set_defaults(w);   /* a saved project's chunk (effSetChunk) comes after this and overrides it */

    AEffect *e = &w->fx;
    std::memset(e, 0, sizeof *e);
    e->magic = 0x56737450; /* 'VstP' */
    e->dispatcher = dispatcher;
    e->setParameter = setParameter;
    e->getParameter = getParameter;
    e->processReplacing = processReplacing;
    e->numParams = NPARAMS;
    e->numInputs = 0;
    e->numOutputs = 2;
    e->flags = effFlagsCanReplacing | effFlagsIsSynth | effFlagsProgramChunks;
    e->uniqueID = PLUG_UID;
    e->version = PLUG_VERSION;
    e->object = w;
    LOG("[euclidier_vst] up, %d params\n", NPARAMS);
    return e;
}
