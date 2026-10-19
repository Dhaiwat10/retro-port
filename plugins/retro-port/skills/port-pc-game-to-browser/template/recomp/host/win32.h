/* Host implementations of the Windows API the game imports.
 *
 * Every import is a GuestFn: the game reached it with `call [IAT]`, so the return address is at
 * [esp] and the arguments follow. Windows API functions are stdcall: the callee pops its arguments.
 */
#ifndef PORT_WIN32_H
#define PORT_WIN32_H

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARG(i) R32(c->esp + 4 + 4 * (i))
/* return from a stdcall import with `nargs` 32-bit arguments */
#define RET(nargs, value) do { c->eax = (uint32_t)(value); c->esp += 4 + 4 * (nargs); return; } while (0)
/* cdecl (variadic) imports: caller cleans up the arguments */
#define RET_CDECL(value) do { c->eax = (uint32_t)(value); c->esp += 4; return; } while (0)

#define GSTR(a) ((const char *)HP(a))
#define GWSTR(a) ((const uint16_t *)HP(a))

/* Registration: each host module contributes a table of {"DLL!Name", handler}. */
#define IMPORT(dll, name) { dll "!" #name, w_##name }
typedef struct { const ImportHandler *list; uint32_t count; } ImportTable;
extern const ImportTable kernel32_imports, user32_imports, gdi32_imports, advapi32_imports, winmm_imports,
    directx_imports;

/* Logging of host-side events (PORT_LOG=1). */
extern int g_log;
#define MAIN_HWND 0x00010010u   /* the game's one window */
#define LOG(...) do { if (g_log) fprintf(stderr, __VA_ARGS__); } while (0)
#define STUB(...) LOG("[stub] " __VA_ARGS__)

/* ------------------------------------------------------------------ guest memory allocation */

uint32_t heap_alloc(uint32_t size, int zero);
void heap_free(uint32_t a);
uint32_t heap_realloc(uint32_t a, uint32_t size, int zero);
uint32_t heap_size(uint32_t a);
uint32_t guest_valloc(uint32_t size);              /* page-granular, zero-filled */
uint32_t guest_strdup(const char *s);

/* ------------------------------------------------------------------ handles */

enum HandleType { H_FREE, H_FILE, H_FIND, H_MAPPING, H_EVENT, H_MUTEX, H_SEMAPHORE, H_THREAD, H_STD, H_MISC };
typedef struct {
    int type;
    int fd;              /* H_FILE */
    void *ptr;           /* H_FIND: directory search state; sync objects: their host object */
    char path[1024];
} WinHandle;

uint32_t handle_new(int type);
WinHandle *handle_get(uint32_t h, int type);   /* NULL if not that type */
void handle_close(uint32_t h);

/* ------------------------------------------------------------------ paths */

extern char g_game_root[1024];   /* host folder holding the game's files */
extern char g_user_root[1024];   /* host folder for files the game writes outside its own folder */
#include "port_config.h"
#define GUEST_ROOT PORT_GUEST_ROOT

/* Maps a Windows path from the game to a host path, matching path components case-insensitively.
   Returns 1 if the host path exists. */
int map_path(const char *guest, char *host, size_t n);

/* ------------------------------------------------------------------ threads */

typedef struct GuestThread {
    CPU cpu;
    uint32_t id;
    uint32_t tib;
    uint32_t tls[64];
    uint32_t last_error;
} GuestThread;
extern __thread GuestThread *t_self;
GuestThread *thread_main_init(CPU *c);
GuestThread *guest_thread_new(void);                                  /* stack + TIB + CPU state */
uint32_t guest_stdcall(CPU *c, uint32_t fn, int nargs, const uint32_t *args);  /* call into the game */
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
extern int g_screen_w, g_screen_h;
extern uint8_t g_keys[256];

/* ------------------------------------------------------------------ misc */

uint64_t host_time_100ns(void);  /* FILETIME units since 1601 */
double host_seconds(void);       /* monotonic */
/* Time the game sees (core.c). Normally host_seconds(); with PORT_DETERMINISTIC=1 a virtual clock that only
   the game's main thread advances (per Present, per Sleep, 1 us per time read), so it is a function of
   that thread's execution rather than of wall time. */
double guest_seconds(void);
uint64_t guest_time_100ns(void); /* FILETIME units since 1601 */
int deterministic(void);
void vclock_frame(void);         /* Present */
void vclock_init(void);          /* main(), before the game starts */
double guest_peek_seconds(void); /* guest_seconds() without advancing the virtual clock */
int on_game_thread(void);         /* the game's main thread (the one that runs WinMain) */
/* sched.c (deterministic mode only; no-ops otherwise) */
void sched_main_init(void);                       /* on the game's thread, before it starts */
void *sched_new(const char *name, int (*start)(void *), void *arg);   /* by the creator; NULL when off */
void sched_enter(void *rec);                      /* first thing on the new host thread */
void sched_exit(void);
void sched_yield_det(void);
int sched_wait(int (*pred)(void *), void *arg, uint64_t deadline_us, uint64_t (*deadline_fn)(void));
int sched_active(void);                           /* the calling thread is scheduled */
uint64_t sched_now_us(void);

#endif
