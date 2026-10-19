/* Host services shared by the Windows API layer: guest heap, handles, path mapping, time. */
#include "win32.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

int g_log;
char g_game_root[1024];
char g_user_root[1024];

/* ------------------------------------------------------------------ guest heap
 * Lives entirely in guest memory so the game can hold pointers into it. 16-byte header before each
 * block: [size][magic][0][0]. Size classes in 16-byte steps up to 4 KiB; larger blocks go on a list.
 */

#define HEAP_LO GUEST_HEAP_BASE
#define HEAP_HI GUEST_HEAP_HI
#define HEAP_MAGIC 0xC07A11C8u
#define NCLASS 256
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = HEAP_LO;
static uint32_t g_free[NCLASS + 1];  /* singly linked through the first payload word */

static uint32_t round16(uint32_t n) { return (n + 15) & ~15u; }

uint32_t heap_alloc(uint32_t size, int zero) {
    uint32_t sz = round16(size ? size : 1), a = 0;
    pthread_mutex_lock(&g_heap_lock);
    uint32_t cls = sz / 16;
    if (cls <= NCLASS && g_free[cls]) {
        a = g_free[cls];
        g_free[cls] = R32(a);
    } else if (cls > NCLASS) {  /* first fit among large free blocks */
        uint32_t prev = 0, p = g_free[0];
        while (p && R32(p - 16) < sz) { prev = p; p = R32(p); }
        if (p) {
            a = p;
            if (prev) W32(prev, R32(p)); else g_free[0] = R32(p);
            sz = R32(p - 16);
        }
    }
    if (!a) {
        if (g_heap_top + 16 + sz > HEAP_HI) {
            pthread_mutex_unlock(&g_heap_lock);
            fprintf(stderr, "guest heap exhausted (%u bytes requested)\n", size);
            abort();
        }
        a = g_heap_top + 16;
        g_heap_top += 16 + sz;
        W32(a - 16, sz);
    }
    W32(a - 12, HEAP_MAGIC);
    pthread_mutex_unlock(&g_heap_lock);
    if (zero) memset(HP(a), 0, R32(a - 16));
    return a;
}

void heap_free(uint32_t a) {
    if (!a) return;
    if (a < HEAP_LO || a >= HEAP_HI || R32(a - 12) != HEAP_MAGIC) {
        LOG("[heap] free of non-heap pointer %08x ignored\n", a);
        return;
    }
    pthread_mutex_lock(&g_heap_lock);
    W32(a - 12, 0);
    uint32_t cls = R32(a - 16) / 16;
    if (cls > NCLASS) cls = 0;
    W32(a, g_free[cls]);
    g_free[cls] = a;
    pthread_mutex_unlock(&g_heap_lock);
}

uint32_t heap_size(uint32_t a) {
    return (a >= HEAP_LO && a < HEAP_HI && R32(a - 12) == HEAP_MAGIC) ? R32(a - 16) : 0;
}

uint32_t heap_realloc(uint32_t a, uint32_t size, int zero) {
    if (!a) return heap_alloc(size, zero);
    uint32_t old = heap_size(a);
    if (round16(size) <= old) return a;
    uint32_t b = heap_alloc(size, 0);
    memcpy(HP(b), HP(a), old);
    if (zero) memset(HP(b + old), 0, R32(b - 16) - old);
    heap_free(a);
    return b;
}

#define VALLOC_LO GUEST_VALLOC_LO
#define VALLOC_HI GUEST_VALLOC_HI
static uint32_t g_valloc_top = VALLOC_LO;

uint32_t guest_valloc(uint32_t size) {
    size = (size + 0xffff) & ~0xffffu;  /* Windows allocation granularity */
    pthread_mutex_lock(&g_heap_lock);
    uint32_t a = g_valloc_top;
    if (a + size > VALLOC_HI) { pthread_mutex_unlock(&g_heap_lock); return 0; }
    g_valloc_top += size;
    pthread_mutex_unlock(&g_heap_lock);
    return a;  /* never handed out before, so still zero */
}

void guest_mem_ranges(uint32_t r[4]) { r[0] = HEAP_LO; r[1] = g_heap_top; r[2] = VALLOC_LO; r[3] = g_valloc_top; }
void guest_mem_stats(uint32_t *heap_used, uint32_t *valloc_used) {
    *heap_used = g_heap_top - HEAP_LO;
    *valloc_used = g_valloc_top - VALLOC_LO;
}

uint32_t guest_strdup(const char *s) {
    uint32_t a = heap_alloc((uint32_t)strlen(s) + 1, 0);
    strcpy((char *)HP(a), s);
    return a;
}

/* ------------------------------------------------------------------ handles */

#define MAX_HANDLES 4096
static WinHandle g_handles[MAX_HANDLES];
static pthread_mutex_t g_handle_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type) {
    pthread_mutex_lock(&g_handle_lock);
    for (int i = 16; i < MAX_HANDLES; i++) {
        if (g_handles[i].type == H_FREE) {
            memset(&g_handles[i], 0, sizeof g_handles[i]);
            g_handles[i].type = type;
            g_handles[i].fd = -1;
            pthread_mutex_unlock(&g_handle_lock);
            return 0x1000u + 4u * (uint32_t)i;
        }
    }
    pthread_mutex_unlock(&g_handle_lock);
    fprintf(stderr, "out of handles\n");
    abort();
}

WinHandle *handle_get(uint32_t h, int type) {
    if (h < 0x1000u || (h & 3)) return NULL;
    uint32_t i = (h - 0x1000u) / 4;
    if (i >= MAX_HANDLES || g_handles[i].type == H_FREE) return NULL;
    if (type != -1 && g_handles[i].type != type) return NULL;
    return &g_handles[i];
}

void handle_close(uint32_t h) {
    WinHandle *x = handle_get(h, -1);
    if (!x) return;
    if (x->type == H_FILE && x->fd >= 0) close(x->fd);
    if (x->type == H_FIND && x->ptr) free(x->ptr);
    x->type = H_FREE;
}

/* ------------------------------------------------------------------ paths */

/* Finds `name` in directory `dir` ignoring case; writes the real name into `out`. */
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(*dir ? dir : ".");
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, n, "%s", e->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static char g_cwd[1024] = GUEST_ROOT;  /* the game's current directory, in Windows form */

const char *guest_cwd(void) { return g_cwd; }
void guest_set_cwd(const char *p) { snprintf(g_cwd, sizeof g_cwd, "%s", p); }

int map_path(const char *guest, char *host, size_t n) {
    char full[1024];
    /* make absolute in Windows form */
    if (isalpha((unsigned char)guest[0]) && guest[1] == ':')
        snprintf(full, sizeof full, "%s", guest);
    else if (guest[0] == '\\' || guest[0] == '/')
        snprintf(full, sizeof full, "C:%s", guest);
    else
        snprintf(full, sizeof full, "%s\\%s", g_cwd, guest);
    for (char *p = full; *p; p++) if (*p == '/') *p = '\\';

    const char *rest;
    const char *base;
    size_t rl = strlen(GUEST_ROOT);
    if (!strncasecmp(full, GUEST_ROOT, rl) && (full[rl] == '\\' || full[rl] == 0)) {
        base = g_game_root;
        rest = full + rl;
    } else {
        base = g_user_root;  /* anything outside the game folder: profiles, temp files, ... */
        rest = full + 2;
    }

    char out[1024];
    snprintf(out, sizeof out, "%s", base);
    int exists = 1;
    char comp[512];
    while (*rest) {
        while (*rest == '\\') rest++;
        if (!*rest) break;
        size_t k = 0;
        while (rest[k] && rest[k] != '\\') k++;
        snprintf(comp, sizeof comp, "%.*s", (int)k, rest);
        rest += k;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) {
            char *sl = strrchr(out, '/');
            if (sl && sl > out + strlen(base) - 1) *sl = 0;
            continue;
        }
        char real[512];
        if (exists && find_ci(out, comp, real, sizeof real)) {
            size_t L = strlen(out);
            snprintf(out + L, sizeof out - L, "/%s", real);
        } else {
            exists = 0;
            size_t L = strlen(out);
            snprintf(out + L, sizeof out - L, "/%s", comp);
        }
    }
    snprintf(host, n, "%s", out);
    struct stat st;
    return stat(host, &st) == 0;
}

/* ------------------------------------------------------------------ time */

uint64_t host_time_100ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}

/* Seconds since the process started, plus a minute: like uptime on a freshly booted PC. Games turn
   GetTickCount/timeGetTime into 32-bit floats, so the values must stay small; the raw monotonic clock
   is epoch-based in WebAssembly (~1.8e12 ms) and broke EA's timer scheduling there. */
double host_seconds(void) {
    static double start = -1;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + ts.tv_nsec * 1e-9;
    if (start < 0) start = now;   /* first call happens during startup, before other threads exist */
    return now - start + 60.0;
}

/* ------------------------------------------------------------------ the game's clock */

/* With PORT_DETERMINISTIC=1 the game sees a virtual clock driven by the deterministic scheduler (sched.c):
   it jumps to the next deadline when every thread is blocked, and every read adds 1 us so a spin-wait on
   the clock still makes progress. Present blocks until the next 1/60 s tick (vsync). */
static int g_det = -1;
uint64_t g_vtime_us;             /* virtual microseconds (sched.c advances it) */
uint32_t g_det_sleeps, g_det_waits, g_det_reads;   /* game-thread counters for the hash log */
static GuestThread g_main_thread;

int deterministic(void) {
    if (g_det < 0) g_det = getenv("PORT_DETERMINISTIC") != NULL;
    return g_det;
}
int on_game_thread(void) { return t_self == &g_main_thread; }

double guest_seconds(void) {
    if (!deterministic()) return host_seconds();
    if (on_game_thread()) g_det_reads++;
    return 60.0 + (double)__atomic_add_fetch(&g_vtime_us, 1, __ATOMIC_RELAXED) * 1e-6;
}
double guest_peek_seconds(void) {   /* the game's clock without advancing it */
    if (!deterministic()) return host_seconds();
    return 60.0 + (double)__atomic_load_n(&g_vtime_us, __ATOMIC_RELAXED) * 1e-6;
}
uint64_t guest_time_100ns(void) {
    if (!deterministic()) return host_time_100ns();
    /* 2006-01-01 00:00 UTC in FILETIME units, plus the virtual clock */
    return 127804032000000000ull + (uint64_t)((guest_seconds() - 60.0) * 1e7);
}
/* Present: wait for the next vsync tick (exact multiples of 1/60 s); online, the frame lasts the time the
   host measured for it (netplay.c), counted from the previous frame's boundary. */
void vclock_frame(void) {
    if (!deterministic() || !on_game_thread()) return;
    extern int sched_wait(int (*)(void *), void *, uint64_t, uint64_t (*)(void));
    extern uint32_t netplay_frame_dt_us(void);
    uint64_t now = __atomic_load_n(&g_vtime_us, __ATOMIC_RELAXED);
    uint32_t dt = netplay_frame_dt_us();
    if (dt) {
        static uint64_t boundary;
        boundary += dt;
        if (boundary < now) boundary = now;   /* the frame already took longer in game time (loading) */
        sched_wait(NULL, NULL, boundary, NULL);
        return;
    }
    uint64_t k = now * 60ull / 1000000ull + 1;
    sched_wait(NULL, NULL, (k * 1000000ull + 59ull) / 60ull, NULL);
}
static uint64_t vclock_rdtsc(void) { return (uint64_t)(guest_seconds() * 2e9); }   /* 2 GHz, as the registry says */
/* A thread preempted after a full quantum has used CPU time, so the clock moves on by PORT_DET_PREEMPT_US
   (default 1000) before the next participant is picked. Without it a thread that spins on memory without
   reading the clock (Cricket's loading screen polling its async loader) never lets a timer come due, and
   the timer callback it waits for never runs. */
static void vclock_preempt(void) {
    static int64_t us = -1;
    if (us < 0) { const char *e = getenv("PORT_DET_PREEMPT_US"); us = e ? atoll(e) : 1000; }
    __atomic_add_fetch(&g_vtime_us, (uint64_t)us, __ATOMIC_RELAXED);
    sched_yield_det();
}
void vclock_init(void) {
    extern uint64_t (*g_rdtsc_hook)(void);
    extern int g_preempt_on;
    extern void (*g_preempt_fn)(void);
    if (deterministic()) {
        g_rdtsc_hook = vclock_rdtsc;
        g_preempt_fn = vclock_preempt;
        g_preempt_on = 1;
        fprintf(stderr, "deterministic mode: virtual clock + scheduler\n");
    }
}

/* ------------------------------------------------------------------ threads */

__thread GuestThread *t_self;

GuestThread *thread_main_init(CPU *c) {
    (void)c;
    g_main_thread.id = 0x100;
    g_main_thread.tib = GUEST_TIB;
    t_self = &g_main_thread;
    return t_self;
}
