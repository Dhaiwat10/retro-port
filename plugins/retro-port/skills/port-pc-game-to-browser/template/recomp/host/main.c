/* Native launcher: loads the game executable's image, wires up the Windows API layer and runs the game's
 * real entry point (the MSVC C runtime startup, which then calls WinMain).
 *
 * Usage: game [game-folder]      (default: ./game)
 * Env:   PORT_LOG=1          host-side log (files, registry, threads, ...)
 *        PORT_LOG_IMPORTS=1  every Windows call the game makes
 *        PORT_LOG_D3D=1|2    Direct3D object creation (1) or every Direct3D call (2)
 *        PORT_LOG_FPS=1      frames per second
 *        PORT_HEADLESS=1     no window (rendering skipped, Present paced at 60 Hz)
 *        PORT_NO_AUDIO=1     mute
 */
#include "win32.h"
#include "render.h"

#include <limits.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static LoadedImage g_img;
static CPU g_cpu;

/* PORT_WATCHDOG=secs: periodically print the game thread's guest call stack (return addresses found
   on its stack), to see where it is when it seems stuck. */
static void *watchdog(void *arg) {
    int secs = (int)(intptr_t)arg;
    for (;;) {
        sleep((unsigned)secs);
        if (deterministic()) {   /* a stuck game thread that doesn't move these spins on memory */
            extern uint32_t g_det_reads, g_det_sleeps, g_det_waits;
            extern uint64_t g_vtime_us;
            fprintf(stderr, "[watchdog] virtual time %.6f s, game thread: clock reads %u, sleeps %u, waits %u\n",
                    g_vtime_us * 1e-6, g_det_reads, g_det_sleeps, g_det_waits);
        }
        extern GuestThread *g_all_threads[64];
        extern int g_nthreads;
        for (int t = -1; t < g_nthreads && t < 64; t++) {
            CPU *c = t < 0 ? &g_cpu : &g_all_threads[t]->cpu;
            fprintf(stderr, "[watchdog] %s %x esp=%08x stack:", t < 0 ? "game thread" : "thread", t < 0 ? 0x100 : g_all_threads[t]->id,
                    c->esp);
            for (int k = 0, n = 0; k < 2048 && n < 16; k++) {
                uint32_t v = R32(c->esp + 4 * k);
                if (looks_like_code(v)) { fprintf(stderr, " %08x", v); n++; }
            }
            fprintf(stderr, "\n");
        }
    }
    return NULL;
}

static void *game_main(void *arg) {
    (void)arg;
    CPU *c = &g_cpu;
    thread_main_init(c);
    sched_main_init();
    t_self->cpu = *c;            /* keep the thread record consistent with the main CPU */
    fpu_sync_rounding(c);
    GuestFn entry = lookup_function(g_img.entry);
    if (!entry) { fprintf(stderr, "entry point %08x was not recompiled\n", g_img.entry); exit(1); }
    fprintf(stderr, "starting game at %08x\n", g_img.entry);
    PUSH32(c, GUEST_RETURN_SENTINEL);
    entry(c);
    fprintf(stderr, "game returned %d\n", (int)c->eax);
    exit((int)c->eax);
}

#if defined(__EMSCRIPTEN__) && !defined(PORT_NODE_BUILD)
#include <emscripten/wasmfs.h>
#endif

int main(int argc, char **argv) {
#if defined(__EMSCRIPTEN__) && !defined(PORT_NODE_BUILD)
    /* browser: the page copied the player's game folder into OPFS; reach it through /opfs */
    wasmfs_create_directory("/opfs", 0777, wasmfs_create_opfs_backend());
#endif
    const char *game = argc > 1 ? argv[1] : "game";
    if (!realpath(game, g_game_root)) { perror(game); return 1; }
    /* PORT_USERDATA: another folder for saves and settings (e.g. a fresh copy per test run) */
    if (getenv("PORT_USERDATA")) snprintf(g_user_root, sizeof g_user_root, "%s", getenv("PORT_USERDATA"));
    else snprintf(g_user_root, sizeof g_user_root, "%s/../userdata", g_game_root);
    mkdir(g_user_root, 0755);
    g_log = getenv("PORT_LOG") != NULL;
    extern int g_log_d3d;
    g_log_d3d = getenv("PORT_LOG_D3D") ? atoi(getenv("PORT_LOG_D3D")) : 0;

    static ImportHandler all[1024];
    uint32_t n = 0;
    const ImportTable *tables[] = { &kernel32_imports, &user32_imports, &gdi32_imports, &advapi32_imports,
                                    &winmm_imports, &directx_imports };
    for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++)
        for (uint32_t i = 0; i < tables[t]->count; i++) all[n++] = tables[t]->list[i];
    set_import_handlers(all, n);

    runtime_init(&g_cpu);
    vclock_init();
    char exe[1100];
    snprintf(exe, sizeof exe, "%s/%s", g_game_root, PORT_EXE);
    if (load_image(exe, &g_img)) return 1;
    register_all_functions();
    fprintf(stderr, "loaded %s: %u imports, %u of them implemented\n", exe, g_img.num_imports, n);

    /* The window and its event loop must live on the main thread (macOS); the game gets its own
       thread. PORT_HEADLESS=1 runs without a window (no rendering, 60 Hz pacing). */
    int headless = getenv("PORT_HEADLESS") != NULL;
    extern int window_create(const char *title);
    extern void window_event_loop(void);
#ifdef __EMSCRIPTEN__
    /* browser: the page owns the canvas and replays our frames; this thread just waits for the game */
    if (!headless) g_render = &g_render_gpu;
    headless = 1;
#else
    if (!headless && window_create(PORT_NAME)) g_render = &g_render_gpu;
    else headless = 1;
#endif

    /* Recompiled code nests one host frame per guest call, so give the game a generous host stack. */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u << 20);
    pthread_t th;
    pthread_create(&th, &at, game_main, NULL);
    if (getenv("PORT_WATCHDOG")) {
        pthread_t wd;
        pthread_create(&wd, NULL, watchdog, (void *)(intptr_t)atoi(getenv("PORT_WATCHDOG")));
    }
    if (headless) pthread_join(th, NULL);
    else window_event_loop();  /* never returns; closing the window exits */
    return 0;
}
