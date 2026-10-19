/* Lockstep netplay core (transport-agnostic). Every peer runs the same deterministic simulation
 * (PORT_DETERMINISTIC=1, sched.c) from boot and feeds it the same pad inputs on the same frames, so only
 * inputs travel. The page (web/port.js) or the Node harness (web/node_env.js) moves messages between peers.
 *
 * Frames are Present calls. At Present f the local pad state becomes this player's input for frame f + D
 * (D = input delay) and goes into the outbox; then the game thread waits until every player's input for
 * frame f + 1 is known and applies all of them at once, before the game reads input again. Frames 1..D
 * are neutral for everyone. Physical input never reaches the game directly in netplay: input_gamepad()
 * for the local pad only updates the sample taken at the next Present.
 *
 * Frame time is an input too: in deterministic mode the game's clock only moves by what Present gives it,
 * so a fixed 1/60 s per frame would make the game run in slow motion whenever the players manage fewer
 * than 60 fps. The host (slot 0) measures the real time between its frames and sends it with its input;
 * every peer advances the clock by exactly that much for that frame (vclock_frame), so the game keeps real
 * speed (choppier, not slower, when someone is slow) and every peer still sees the same times.
 *
 * Every NP_HASH_EVERY frames each peer also sends a hash of the game's global data; a mismatch means the
 * simulations diverged (desync) and is reported.
 *
 * Inputs from the transport arrive on another host thread (the page's main thread), so each buffer entry
 * carries the frame it holds, published with release/acquire ordering. */
#include "win32.h"

#include <pthread.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#define EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXPORT
#endif

#define NP_SLOTS 4
#define NP_RING 512          /* frames of input kept per player */
#define NP_HASH_EVERY 60

typedef struct { uint32_t frame; uint32_t buttons; int32_t axis[4]; uint32_t dt_us; } NpInput;   /* frame 0 = empty */
typedef struct { uint32_t kind, slot, frame, buttons; int32_t axis[4]; uint32_t hash_lo, hash_hi; } NpMsg;
enum { MSG_INPUT = 1, MSG_HASH = 2 };

static int g_np = -1;                       /* netplay on */
static int g_np_players, g_np_slot, g_np_delay;
static NpInput g_in[NP_SLOTS][NP_RING];
static int g_left[NP_SLOTS];                /* peer gone: its pad reads neutral from then on */
static NpInput g_local;                     /* latest physical/scripted state of the local pad */
static uint32_t g_local_pressed;            /* buttons pressed since the last Present (see netplay_frame) */
static pthread_mutex_t g_local_lock = PTHREAD_MUTEX_INITIALIZER;

#define OUTBOX 1024
static NpMsg g_out[OUTBOX];
static uint32_t g_out_w, g_out_r;           /* single producer (game thread), single consumer (transport) */

/* Desync detection: our hashes and theirs, by frame / NP_HASH_EVERY */
#define NP_HASHES 64
static uint64_t g_my_hash[NP_HASHES]; static uint32_t g_my_hash_frame[NP_HASHES];
static uint32_t g_desync_frame;             /* first frame found to differ (0 = none) */
static uint32_t g_frame;                    /* last frame processed */
static uint32_t g_wait_ms;                  /* time spent waiting for remote input, for the page's status line */
static uint32_t g_frame_dt_us = 16667;      /* game time for the next frame (see top) */
int netplay_on(void);
uint32_t netplay_frame_dt_us(void) { return netplay_on() ? g_frame_dt_us : 0; }

int netplay_on(void) {
    if (g_np < 0) {
        g_np = getenv("PORT_NETPLAY") != NULL && deterministic();
        if (getenv("PORT_NETPLAY") && !deterministic())
            fprintf(stderr, "[netplay] PORT_NETPLAY needs PORT_DETERMINISTIC=1; netplay off\n");
        if (g_np) {
            g_np_players = getenv("PORT_NP_PLAYERS") ? atoi(getenv("PORT_NP_PLAYERS")) : 2;
            g_np_slot = getenv("PORT_NP_SLOT") ? atoi(getenv("PORT_NP_SLOT")) : 0;
            g_np_delay = getenv("PORT_NP_DELAY") ? atoi(getenv("PORT_NP_DELAY")) : 4;
            if (g_np_players < 1 || g_np_players > NP_SLOTS || g_np_slot < 0 || g_np_slot >= g_np_players ||
                g_np_delay < 1 || g_np_delay > 60) {
                fprintf(stderr, "[netplay] bad configuration (players %d, slot %d, delay %d)\n", g_np_players,
                        g_np_slot, g_np_delay);
                exit(1);
            }
            fprintf(stderr, "[netplay] %d players, local slot %d, input delay %d frames\n", g_np_players, g_np_slot,
                    g_np_delay);
        }
    }
    return g_np;
}

/* input_gamepad() lands here in netplay mode: only the local player's first pad counts. */
int netplay_local_pad(int idx, int connected, uint32_t buttons, const int32_t axis[4]) {
    if (!netplay_on()) return 0;
    if (idx != 0) return 1;
    pthread_mutex_lock(&g_local_lock);
    g_local.buttons = connected ? buttons : 0;
    g_local_pressed |= g_local.buttons;
    for (int i = 0; i < 4; i++) g_local.axis[i] = connected ? axis[i] : 0;
    pthread_mutex_unlock(&g_local_lock);
    return 1;
}

static void outbox_push(const NpMsg *m) {
    uint32_t w = g_out_w;
    if (w - __atomic_load_n(&g_out_r, __ATOMIC_ACQUIRE) >= OUTBOX) { fprintf(stderr, "[netplay] outbox full\n"); return; }
    g_out[w % OUTBOX] = *m;
    __atomic_store_n(&g_out_w, w + 1, __ATOMIC_RELEASE);
}

static void store_input(int slot, uint32_t frame, uint32_t buttons, const int32_t axis[4], uint32_t dt_us) {
    NpInput *e = &g_in[slot][frame % NP_RING];
    e->buttons = buttons;
    e->dt_us = dt_us;
    for (int i = 0; i < 4; i++) e->axis[i] = axis[i];
    __atomic_store_n(&e->frame, frame, __ATOMIC_RELEASE);
}

static int have_input(int slot, uint32_t frame) {
    if (frame <= (uint32_t)g_np_delay || __atomic_load_n(&g_left[slot], __ATOMIC_ACQUIRE)) return 1;
    return __atomic_load_n(&g_in[slot][frame % NP_RING].frame, __ATOMIC_ACQUIRE) == frame;
}

static void check_hash(uint32_t frame, uint64_t theirs, int slot) {
    int k = (int)((frame / NP_HASH_EVERY) % NP_HASHES);
    if (g_my_hash_frame[k] != frame) return;   /* ours isn't computed yet or already overwritten */
    if (g_my_hash[k] != theirs && !g_desync_frame) {
        g_desync_frame = frame;
        fprintf(stderr, "[netplay] DESYNC with player %d at frame %u (%016llx vs %016llx)\n", slot, frame,
                (unsigned long long)g_my_hash[k], (unsigned long long)theirs);
    }
}

/* Remote hashes that arrive before ours is computed wait here. */
static struct { uint32_t frame; int slot; uint64_t hash; } g_pending[NP_SLOTS][NP_HASHES];

/* Called at every Present on the game thread, with the frame just presented and a hash of global state. */
void netplay_frame(uint32_t frame, uint64_t (*state_hash)(void)) {
    if (!netplay_on()) return;
    g_frame = frame;
    /* our input for frame + D */
    NpMsg m = { MSG_INPUT, (uint32_t)g_np_slot, frame + (uint32_t)g_np_delay, 0, { 0 }, 0, 0 };
    /* A button pressed and released since the last Present still counts as held for this frame: the pad
       is only sampled here, once a frame, so a quick tap between two samples would otherwise be lost (and
       the slower the game runs, the more taps fall between samples). */
    pthread_mutex_lock(&g_local_lock);
    m.buttons = g_local.buttons | g_local_pressed;
    g_local_pressed = 0;
    for (int i = 0; i < 4; i++) m.axis[i] = g_local.axis[i];
    pthread_mutex_unlock(&g_local_lock);
    if (g_np_slot == 0) {   /* the host's real frame time, for the frame this input belongs to */
        static double last;
        double now = host_seconds();
        double dt = last ? now - last : 1.0 / 60;
        last = now;
        if (dt < 0.004) dt = 0.004;     /* high-refresh displays present more often: still real time */
        if (dt > 0.050) dt = 0.050;     /* below 20 fps the game slows down rather than taking huge steps */
        m.hash_lo = (uint32_t)(dt * 1e6);
    }
    store_input(g_np_slot, m.frame, m.buttons, m.axis, m.hash_lo);
    outbox_push(&m);
    static int log_input = -1;   /* PORT_NP_LOG_INPUT=1: log every non-neutral input this player sends */
    if (log_input < 0) log_input = getenv("PORT_NP_LOG_INPUT") != NULL;
    if (log_input && m.buttons) fprintf(stderr, "[netplay] sent buttons %x for frame %u\n", m.buttons, m.frame);
    /* test only: PORT_NP_PERTURB=<frame> flips a byte of the game's globals here, to check desync detection */
    static int perturb = -1;
    if (perturb < 0) perturb = getenv("PORT_NP_PERTURB") ? atoi(getenv("PORT_NP_PERTURB")) : 0;
    if (perturb && frame == (uint32_t)perturb) { { uint32_t a = g_data_lo + (g_data_hi - g_data_lo) / 2; W8(a, R8(a) ^ 1); } fprintf(stderr, "[netplay] test: perturbed state at frame %u\n", frame); }
    if (frame % NP_HASH_EVERY == 0) {
        uint64_t h = state_hash();
        int k = (int)((frame / NP_HASH_EVERY) % NP_HASHES);
        g_my_hash[k] = h; g_my_hash_frame[k] = frame;
        NpMsg hm = { MSG_HASH, (uint32_t)g_np_slot, frame, 0, { 0 }, (uint32_t)h, (uint32_t)(h >> 32) };
        outbox_push(&hm);
        for (int s = 0; s < g_np_players; s++)
            if (g_pending[s][k].frame == frame) check_hash(frame, g_pending[s][k].hash, s);
    }
    /* wait for everyone's input for the next frame (wall time only; the game's clock doesn't move) */
    uint32_t next = frame + 1;
    double t0 = host_seconds(), last_note = t0;
    for (;;) {
        int all = 1;
        for (int s = 0; s < g_np_players; s++) if (!have_input(s, next)) { all = 0; break; }
        if (all) break;
        usleep(500);
        double now = host_seconds();
        if (now - last_note > 5) { last_note = now; fprintf(stderr, "[netplay] waiting for players (frame %u, %.0f s)\n", next, now - t0); }
    }
    g_wait_ms += (uint32_t)((host_seconds() - t0) * 1000);
    /* apply every player's pad for the next frame */
    static const int32_t zero[4] = { 0, 0, 0, 0 };
    extern void dinput_pad(int idx, int connected, uint32_t std_buttons, const int32_t axis[4]);
    for (int s = 0; s < g_np_players; s++) {
        NpInput *e = &g_in[s][next % NP_RING];
        int live = next > (uint32_t)g_np_delay && !g_left[s] && e->frame == next;
        dinput_pad(s, 1, live ? e->buttons : 0, live ? e->axis : zero);
    }
    /* the frame time every peer uses for the next frame: the host's (1/60 s before inputs start or if the
       host has left) */
    NpInput *h = &g_in[0][next % NP_RING];
    g_frame_dt_us = next > (uint32_t)g_np_delay && !g_left[0] && h->frame == next && h->dt_us ? h->dt_us : 16667;
}

/* ------------------------------------------------------------------ transport side (any thread) */

/* Next outgoing message: netplay_pop() loads it (0 if none), netplay_msg(i) reads its fields: kind, slot,
   frame, buttons, axes x4, hash lo, hash hi. (Fields by call, so the transport needs no view of wasm memory.) */
static uint32_t g_popped[10];
EXPORT int netplay_pop(void) {
    uint32_t r = g_out_r;
    if (r == __atomic_load_n(&g_out_w, __ATOMIC_ACQUIRE)) return 0;
    NpMsg *m = &g_out[r % OUTBOX];
    uint32_t v[10] = { m->kind, m->slot, m->frame, m->buttons, (uint32_t)m->axis[0], (uint32_t)m->axis[1],
                       (uint32_t)m->axis[2], (uint32_t)m->axis[3], m->hash_lo, m->hash_hi };
    memcpy(g_popped, v, sizeof v);
    __atomic_store_n(&g_out_r, r + 1, __ATOMIC_RELEASE);
    return 1;
}
EXPORT int32_t netplay_msg(int i) { return i >= 0 && i < 10 ? (int32_t)g_popped[i] : 0; }

EXPORT void netplay_remote_input(int slot, uint32_t frame, uint32_t buttons, int lx, int ly, int rx, int ry,
                                 uint32_t dt_us) {
    if (!netplay_on() || slot < 0 || slot >= g_np_players || slot == g_np_slot) return;
    const int32_t axis[4] = { lx, ly, rx, ry };
    store_input(slot, frame, buttons, axis, dt_us);
}

EXPORT void netplay_remote_hash(int slot, uint32_t frame, uint32_t lo, uint32_t hi) {
    if (!netplay_on() || slot < 0 || slot >= NP_SLOTS) return;
    uint64_t h = (uint64_t)hi << 32 | lo;
    int k = (int)((frame / NP_HASH_EVERY) % NP_HASHES);
    /* hashes are compared on the game thread when ours exists; this runs on the transport's thread, so
       just record it there and compare if ours is already done */
    g_pending[slot][k].frame = frame; g_pending[slot][k].slot = slot; g_pending[slot][k].hash = h;
    if (g_my_hash_frame[k] == frame) check_hash(frame, h, slot);
}

EXPORT void netplay_player_left(int slot) {
    if (slot < 0 || slot >= NP_SLOTS) return;
    __atomic_store_n(&g_left[slot], 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[netplay] player %d left; their pad is neutral from now on\n", slot);
}

/* For the page's status line: last frame, total ms spent waiting for others, first desync frame. */
EXPORT uint32_t netplay_status(int what) {
    return what == 0 ? g_frame : what == 1 ? g_wait_ms : g_desync_frame;
}
