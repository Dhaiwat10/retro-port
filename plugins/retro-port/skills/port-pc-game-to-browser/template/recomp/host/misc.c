/* GDI32, ADVAPI32 (registry), WINMM (timers) and the DirectX entry points. */
#include "win32.h"

#include <pthread.h>
#include <unistd.h>

#define W(name) static void w_##name(CPU *c)

/* ================================================================== GDI32
 * The game draws some text with GDI into a memory bitmap and reads pixels back with GetPixel.
 * Until there's a real rasterizer these produce blank glyphs.
 */
W(CreateCompatibleDC) { RET(1, 0x00050001u); }
W(DeleteDC) { RET(1, 1); }
W(CreateBitmap) { RET(5, 0x00050002u); }
W(CreateFontA) { LOG("[gdi32] CreateFontA(%s, h=%d)\n", ARG(13) ? GSTR(ARG(13)) : "", (int)ARG(0)); RET(14, 0x00050003u); }
W(SelectObject) { RET(2, 0x00050004u); }
W(DeleteObject) { RET(1, 1); }
W(GetStockObject) { RET(1, 0x00050005u); }
W(SetTextColor) { RET(2, 0); }
W(SetBkColor) { RET(2, 0); }
W(SetBkMode) { RET(2, 1); }
W(ExtTextOutA) { RET(8, 1); }
W(BitBlt) { RET(9, 1); }
W(GetPixel) { RET(3, 0); }

#define G(name) IMPORT("GDI32.DLL", name)
static const ImportHandler gdi_list[] = {
    G(CreateCompatibleDC), G(DeleteDC), G(CreateBitmap), G(CreateFontA), G(SelectObject), G(DeleteObject),
    G(GetStockObject), G(SetTextColor), G(SetBkColor), G(SetBkMode), G(ExtTextOutA), G(BitBlt), G(GetPixel),
};
const ImportTable gdi32_imports = { gdi_list, sizeof gdi_list / sizeof gdi_list[0] };

/* ================================================================== ADVAPI32: a small virtual registry */

typedef struct { const char *key, *value; uint32_t type; const char *data; uint32_t dword; } RegValue;
static const RegValue g_registry[] = {
    /* CPU speed: without it the game calibrates with a long QueryPerformanceCounter loop */
    { "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz", 4 /* REG_DWORD */, NULL, 2000 },
    /* My Documents (many games keep profiles and settings there; mapped to userdata/) */
    { "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", "Personal", 1,
      "C:\\Documents and Settings\\Player\\My Documents" },
    /* the game's own keys, from port.json */
    PORT_REGISTRY
};
static char g_open_keys[64][256];

W(RegOpenKeyA) {
    LOG("[advapi32] RegOpenKeyA(%08x, %s)\n", ARG(0), ARG(1) ? GSTR(ARG(1)) : "");
    for (int i = 1; i < 64; i++) if (!g_open_keys[i][0]) {
        snprintf(g_open_keys[i], 256, "%s", ARG(1) ? GSTR(ARG(1)) : "");
        W32(ARG(2), 0x00060000u + (uint32_t)i);
        RET(3, 0);
    }
    RET(3, 2);
}
W(RegOpenKeyExA) {
    LOG("[advapi32] RegOpenKeyExA(%08x, %s)\n", ARG(0), ARG(1) ? GSTR(ARG(1)) : "");
    for (int i = 1; i < 64; i++) if (!g_open_keys[i][0]) {
        snprintf(g_open_keys[i], 256, "%s", ARG(1) ? GSTR(ARG(1)) : "");
        W32(ARG(4), 0x00060000u + (uint32_t)i);
        RET(5, 0);
    }
    RET(5, 2);
}
W(RegCloseKey) {
    uint32_t i = ARG(0) - 0x00060000u;
    if (i < 64) g_open_keys[i][0] = 0;
    RET(1, 0);
}
W(RegQueryValueExA) {
    uint32_t i = ARG(0) - 0x00060000u;
    const char *key = i < 64 ? g_open_keys[i] : "";
    const char *name = ARG(1) ? GSTR(ARG(1)) : "";
    for (size_t k = 0; k < sizeof g_registry / sizeof g_registry[0]; k++) {
        if (strcasecmp(g_registry[k].key, key) || strcasecmp(g_registry[k].value, name)) continue;
        const RegValue *v = &g_registry[k];
        uint32_t len = v->type == 4 ? 4 : (uint32_t)strlen(v->data) + 1;
        if (ARG(3)) W32(ARG(3), v->type);
        if (ARG(4) && ARG(5) && R32(ARG(5)) >= len) {
            if (v->type == 4) W32(ARG(4), v->dword); else memcpy(HP(ARG(4)), v->data, len);
        }
        if (ARG(5)) { int small = ARG(4) && R32(ARG(5)) < len; W32(ARG(5), len); if (small) RET(6, 234); }
        LOG("[advapi32] RegQueryValueExA(%s\\%s) found\n", key, name);
        RET(6, 0);
    }
    LOG("[advapi32] RegQueryValueExA(%s\\%s) not found\n", key, name);
    RET(6, 2 /* ERROR_FILE_NOT_FOUND */);
}

#define A(name) IMPORT("ADVAPI32.DLL", name)
static const ImportHandler adv_list[] = { A(RegOpenKeyA), A(RegOpenKeyExA), A(RegCloseKey), A(RegQueryValueExA) };
const ImportTable advapi32_imports = { adv_list, sizeof adv_list / sizeof adv_list[0] };

/* ================================================================== WINMM */

W(timeGetTime) { RET(0, (uint32_t)(guest_seconds() * 1000.0)); }
W(timeBeginPeriod) { RET(1, 0); }
W(timeEndPeriod) { RET(1, 0); }
W(timeGetDevCaps) { W32(ARG(0), 1); W32(ARG(0) + 4, 1000000); RET(2, 0); }
W(waveOutGetNumDevs) { RET(0, 1); }

/* Multimedia timers. Like Windows, every callback runs on one timer thread (with its own guest
 * context); EA's TIMER code reschedules a one-shot timer every ~15 ms from inside the callback. */
typedef struct { uint32_t id, delay, callback, user, periodic; double due; } Timer;
static Timer g_timers[64];          /* id 0 = free */
static pthread_mutex_t g_timer_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_timer_cv = PTHREAD_COND_INITIALIZER;
static uint32_t g_next_timer_id = 1;
static int g_timer_thread_started;
static void *g_timer_sched;   /* deterministic mode: the timer thread's scheduler record */

/* Deterministic mode: when the earliest timer is due, in virtual microseconds (the scheduler wakes the
   timer thread then). Evaluated while every other thread is parked. */
static uint64_t timers_next_due_us(void) {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < 64; i++)
        if (g_timers[i].id) {
            double us = (g_timers[i].due - 60.0) * 1e6;
            uint64_t u = us <= 0 ? 0 : (uint64_t)us + 1;
            if (u < best) best = u;
        }
    return best;
}

static void *timer_main(void *arg) {
    (void)arg;
    sched_enter(g_timer_sched);
    GuestThread *t = guest_thread_new();
    t_self = t;
    CPU *c = &t->cpu;
    fpu_sync_rounding(c);
    if (g_timer_sched) {
        /* deterministic: sleep in the scheduler until the next timer is due, then fire every due one */
        for (;;) {
            sched_wait(NULL, NULL, 0, timers_next_due_us);
            for (;;) {
                double now = guest_peek_seconds();
                int best = -1;
                for (int i = 0; i < 64; i++)
                    if (g_timers[i].id && g_timers[i].due <= now && (best < 0 || g_timers[i].due < g_timers[best].due)) best = i;
                if (best < 0) break;
                Timer tm = g_timers[best];
                if (tm.periodic) {
                    g_timers[best].due += tm.delay / 1000.0;
                    if (g_timers[best].due < now - 0.25) g_timers[best].due = now;
                } else {
                    g_timers[best].id = 0;
                }
                uint32_t a[5] = { tm.id, 0, tm.user, 0, 0 };
                guest_stdcall(c, tm.callback, 5, a);
            }
        }
    }
    pthread_mutex_lock(&g_timer_lock);
    for (;;) {
        int best = -1;
        for (int i = 0; i < 64; i++)
            if (g_timers[i].id && (best < 0 || g_timers[i].due < g_timers[best].due)) best = i;
        if (best < 0) { pthread_cond_wait(&g_timer_cv, &g_timer_lock); continue; }
        double now = guest_seconds();
        if (g_timers[best].due > now) {
            /* sleep in short steps and re-check (a timed condition wait never woke up in WebAssembly);
               new timers are picked up within a millisecond */
            double wait = g_timers[best].due - now;
            static double last_report;
            if (now - last_report > 5) { last_report = now; LOG("[winmm] timer thread: next timer due in %.3f s (now %.3f)\n", wait, now); }
            pthread_mutex_unlock(&g_timer_lock);
            usleep(wait > 0.001 ? 1000 : (useconds_t)(wait * 1e6) + 1);
            pthread_mutex_lock(&g_timer_lock);
            continue;
        }
        Timer tm = g_timers[best];
        if (tm.periodic) {
            g_timers[best].due += tm.delay / 1000.0;
            if (g_timers[best].due < now - 0.25) g_timers[best].due = now;  /* don't spiral if we fall behind */
        } else {
            g_timers[best].id = 0;
        }
        pthread_mutex_unlock(&g_timer_lock);
        uint32_t a[5] = { tm.id, 0, tm.user, 0, 0 };
        static unsigned fired;
        if (fired++ % 500 == 0) LOG("[winmm] timer callbacks so far: %u\n", fired);
        guest_stdcall(c, tm.callback, 5, a);
        pthread_mutex_lock(&g_timer_lock);
    }
    return NULL;
}

W(timeSetEvent) {
    uint32_t delay = ARG(0), cb = ARG(2), user = ARG(3), flags = ARG(4);
    if (flags & 0x30 /* TIME_CALLBACK_EVENT_SET / PULSE */) { STUB("timeSetEvent event callbacks\n"); RET(5, 0); }
    pthread_mutex_lock(&g_timer_lock);
    int slot = -1;
    for (int i = 0; i < 64; i++) if (!g_timers[i].id) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_timer_lock); RET(5, 0); }
    uint32_t id = g_next_timer_id++;
    if (!g_next_timer_id) g_next_timer_id = 1;
    g_timers[slot] = (Timer){ id, delay ? delay : 1, cb, user, flags & 1, guest_seconds() + (delay ? delay : 1) / 1000.0 };
    static int nset;
    if (nset++ < 5) LOG("[winmm] timeSetEvent(%u ms) at %.3f on thread %x\n", delay, host_seconds(), t_self ? t_self->id : 0);
    if (!g_timer_thread_started) {
        g_timer_thread_started = 1;
        g_timer_sched = sched_new("timer", NULL, NULL);
        pthread_t pt;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 64u << 20);
        pthread_create(&pt, &at, timer_main, NULL);
        pthread_detach(pt);
    }
    pthread_cond_signal(&g_timer_cv);
    pthread_mutex_unlock(&g_timer_lock);
    RET(5, id);
}
W(timeKillEvent) {
    uint32_t id = ARG(0);
    pthread_mutex_lock(&g_timer_lock);
    for (int i = 0; i < 64; i++) if (g_timers[i].id == id) g_timers[i].id = 0;
    pthread_mutex_unlock(&g_timer_lock);
    RET(1, 0);
}

#define M(name) IMPORT("WINMM.DLL", name)
static const ImportHandler mm_list[] = {
    M(timeGetTime), M(timeBeginPeriod), M(timeEndPeriod), M(timeGetDevCaps), M(waveOutGetNumDevs),
    M(timeSetEvent), M(timeKillEvent),
};
const ImportTable winmm_imports = { mm_list, sizeof mm_list / sizeof mm_list[0] };

/* ================================================================== DirectX entry points
 * Direct3D 8 lives in d3d8.c.
 */
void w_Direct3DCreate8(CPU *c);  /* d3d8.c */
W(DirectDrawCreate) {
    LOG("[ddraw] DirectDrawCreate -> fails (game should fall back)\n");
    RET(3, 0x80004005u /* E_FAIL */);
}
void w_DirectInputCreateA(CPU *c);  /* dinput.c */
void w_DirectSoundCreate(CPU *c);         /* dsound.c */
void w_DirectSoundCaptureCreate(CPU *c);  /* dsound.c */

/* Extra d3d8.dll exports that D3DX looks up with GetProcAddress */
static void w_DebugSetMute(CPU *c) { RET(0, 0); }
static void w_ValidateVertexShader(CPU *c) { if (ARG(4)) W32(ARG(4), 0); RET(5, 0); }
static void w_ValidatePixelShader(CPU *c) { if (ARG(3)) W32(ARG(3), 0); RET(4, 0); }

static const ImportHandler dx_list[] = {
    { "D3D8.DLL!DebugSetMute", w_DebugSetMute },
    { "D3D8.DLL!ValidateVertexShader", w_ValidateVertexShader },
    { "D3D8.DLL!ValidatePixelShader", w_ValidatePixelShader },
    { "D3D8.DLL!Direct3DCreate8", w_Direct3DCreate8 },
    { "DDRAW.DLL!DirectDrawCreate", w_DirectDrawCreate },
    { "DINPUT.DLL!DirectInputCreateA", w_DirectInputCreateA },
    { "DSOUND.DLL!#1", w_DirectSoundCreate },
    { "DSOUND.DLL!#6", w_DirectSoundCaptureCreate },
};
const ImportTable directx_imports = { dx_list, sizeof dx_list / sizeof dx_list[0] };
