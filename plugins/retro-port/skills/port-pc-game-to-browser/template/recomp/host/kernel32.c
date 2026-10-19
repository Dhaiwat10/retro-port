/* KERNEL32.DLL */
#include "win32.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define W(name) static void w_##name(CPU *c)

const char *guest_cwd(void);
void guest_set_cwd(const char *p);
static void set_error(uint32_t e) { if (t_self) t_self->last_error = e; }

enum { ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6,
       ERROR_NO_MORE_FILES = 18, ERROR_ALREADY_EXISTS = 183 };

/* ------------------------------------------------------------------ version, modules, process */

W(GetVersionExA) {
    uint32_t p = ARG(0);  /* OSVERSIONINFOA: report Windows XP SP2 */
    W32(p + 4, 5); W32(p + 8, 1); W32(p + 12, 2600); W32(p + 16, 2 /* VER_PLATFORM_WIN32_NT */);
    strcpy((char *)HP(p + 20), "Service Pack 2");
    RET(1, 1);
}

#define FAKE_MODULE(i) (0xF1000000u + (i) * 0x10000u)
static const char *g_known_dlls[] = { "kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll",
                                      "d3d8.dll", "ddraw.dll", "dinput.dll", "dsound.dll", "version.dll", NULL };

static uint32_t module_for(const char *name) {
    if (!name) return 0x400000;
    const char *b = strrchr(name, '\\');
    b = b ? b + 1 : name;
    for (int i = 0; g_known_dlls[i]; i++)
        if (!strncasecmp(b, g_known_dlls[i], strlen(g_known_dlls[i]) - 4)) return FAKE_MODULE(i + 1);
    return 0;
}

W(GetModuleHandleA) { RET(1, ARG(0) ? module_for(GSTR(ARG(0))) : 0x400000); }
W(LoadLibraryA) {
    uint32_t m = module_for(GSTR(ARG(0)));
    LOG("[kernel32] LoadLibraryA(%s) -> %08x\n", GSTR(ARG(0)), m);
    set_error(m ? 0 : 126);
    RET(1, m);
}
W(FreeLibrary) { RET(1, 1); }

W(GetProcAddress) {
    uint32_t mod = ARG(0), name = ARG(1);
    char full[160];
    const char *dll = "KERNEL32.DLL";
    for (int i = 0; g_known_dlls[i]; i++)
        if (mod == FAKE_MODULE(i + 1)) dll = g_known_dlls[i];
    char up[32];
    snprintf(up, sizeof up, "%s", dll);
    for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    if (name < 0x10000) { RET(2, 0); }  /* by ordinal: not supported */
    snprintf(full, sizeof full, "%s!%s", up, GSTR(name));
    GuestFn fn = find_import_handler(full);
    uint32_t r = fn ? add_dynamic_import(full, fn) : 0;
    LOG("[kernel32] GetProcAddress(%s) -> %08x\n", full, r);
    RET(2, r);
}

W(GetModuleFileNameA) {
    const char *p = GUEST_ROOT "\\" PORT_GUEST_EXE;
    uint32_t buf = ARG(1), n = ARG(2);
    snprintf((char *)HP(buf), n, "%s", p);
    RET(3, strlen(p) < n ? strlen(p) : n);
}

static uint32_t g_cmdline, g_env;
W(GetCommandLineA) {
    if (!g_cmdline) g_cmdline = guest_strdup("\"" GUEST_ROOT "\\" PORT_GUEST_EXE "\"");
    RET(0, g_cmdline);
}
W(GetEnvironmentStrings) {
    if (!g_env) { g_env = heap_alloc(64, 1); strcpy((char *)HP(g_env), "PATH=C:\\WINDOWS"); }
    RET(0, g_env);
}
W(GetEnvironmentStringsW) {
    uint32_t a = heap_alloc(8, 1);
    RET(0, a);
}
W(FreeEnvironmentStringsA) { RET(1, 1); }
W(FreeEnvironmentStringsW) { RET(1, 1); }
W(SetEnvironmentVariableA) { RET(2, 1); }

W(GetStartupInfoA) {
    uint32_t p = ARG(0);
    memset(HP(p), 0, 68);
    W32(p, 68);
    RET(1, 0);
}

#define STD_IN 0x0F0u
#define STD_OUT 0x0F4u
#define STD_ERR 0x0F8u
W(GetStdHandle) {
    int32_t which = (int32_t)ARG(0);
    RET(1, which == -10 ? STD_IN : which == -11 ? STD_OUT : STD_ERR);
}
W(SetStdHandle) { RET(2, 1); }
W(GetFileType) {
    uint32_t h = ARG(0);
    if (h == STD_IN || h == STD_OUT || h == STD_ERR) RET(1, 2 /* FILE_TYPE_CHAR */);
    RET(1, handle_get(h, H_FILE) ? 1 /* FILE_TYPE_DISK */ : 0);
}
W(SetHandleCount) { RET(1, ARG(0)); }

W(GetCurrentProcess) { RET(0, 0xFFFFFFFFu); }
W(GetCurrentThread) { RET(0, 0xFFFFFFFEu); }
W(GetCurrentProcessId) { RET(0, 0x1234); }
W(GetCurrentThreadId) { RET(0, t_self ? t_self->id : 0x100); }
W(GetLastError) { RET(0, t_self ? t_self->last_error : 0); }
W(SetLastError) { set_error(ARG(0)); RET(1, 0); }

W(ExitProcess) {
    fprintf(stderr, "[kernel32] ExitProcess(%d) from thread %u, stack:", (int)ARG(0), t_self ? t_self->id : 0);
    for (int k = 0, n = 0; k < 4096 && n < 32; k++) {
        uint32_t v = R32(c->esp + 4 * k);
        if (looks_like_code(v)) { fprintf(stderr, " %08x", v); n++; }
    }
    fprintf(stderr, "\n");
    /* strings still on the stack (the fatal error's message and arguments) */
    for (int k = 0, n = 0; k < 1024 && n < 12; k++) {
        uint32_t v = R32(c->esp + 4 * k);
        if (v < 0x401000 || v >= GUEST_VALLOC_HI) continue;
        const uint8_t *p = HP(v);
        int len = 0;
        while (len < 120 && p[len] >= 0x20 && p[len] < 0x7f) len++;
        if (len >= 6 && (p[len] == 0 || p[len] == '\n')) { fprintf(stderr, "    \"%.*s\"\n", len, (const char *)p); n++; }
    }
    exit((int)ARG(0));
}
W(TerminateProcess) {
    fprintf(stderr, "[kernel32] TerminateProcess(%d)\n", (int)ARG(1));
    exit((int)ARG(1));
}
W(FatalAppExitA) {
    fprintf(stderr, "[kernel32] FatalAppExit: %s\n", GSTR(ARG(1)));
    exit(3);
}

W(GetSystemInfo) {
    uint32_t p = ARG(0);
    memset(HP(p), 0, 36);
    W32(p + 4, 0x1000);        /* page size */
    W32(p + 8, 0x10000);       /* min app address */
    W32(p + 12, 0x7FFEFFFF);   /* max app address */
    W32(p + 16, 1);            /* active processor mask */
    W32(p + 20, 1);            /* number of processors */
    W32(p + 24, 586);          /* processor type */
    W32(p + 28, 0x10000);      /* allocation granularity */
    W16(p + 32, 6);            /* level */
    RET(1, 0);
}

W(IsProcessorFeaturePresent) { RET(1, 0); }  /* consistent with our CPUID: no MMX/SSE/3DNow! */
W(IsDebuggerPresent) { RET(0, 0); }
W(OutputDebugStringA) { LOG("[debug] %s", GSTR(ARG(0))); RET(1, 0); }
W(DebugBreak) { guest_trap(c, R32(c->esp), "DebugBreak"); }
W(IsBadReadPtr) { RET(2, ARG(0) == 0); }
W(IsBadWritePtr) { RET(2, ARG(0) == 0); }
W(IsBadCodePtr) { RET(1, ARG(0) == 0); }
W(SetUnhandledExceptionFilter) { RET(1, 0); }
W(UnhandledExceptionFilter) { RET(1, 0); }
W(RaiseException) {
    fprintf(stderr, "[kernel32] RaiseException code=%08x flags=%08x nargs=%u (from %08x)\n", ARG(0), ARG(1),
            ARG(2), R32(c->esp));
    if (ARG(0) == 0xE06D7363u && ARG(2) >= 3) {  /* MSVC C++ exception: show the thrown type */
        uint32_t info = R32(ARG(3) + 8);
        if (info) {
            uint32_t cta = R32(info + 12), ct = R32(cta + 4), td = R32(ct + 4);
            fprintf(stderr, "    C++ exception of type %s\n", GSTR(td + 8));
        }
    }
    guest_trap(c, R32(c->esp), "RaiseException (exceptions are not supported yet)");
}
W(RtlUnwind) { guest_trap(c, R32(c->esp), "RtlUnwind (exceptions are not supported yet)"); }

W(FormatMessageA) {
    uint32_t buf = ARG(4), n = ARG(5);
    if (ARG(0) & 0x100) RET(7, 0);  /* FORMAT_MESSAGE_ALLOCATE_BUFFER: unsupported */
    snprintf((char *)HP(buf), n, "error %u", ARG(2));
    RET(7, strlen(GSTR(buf)));
}

/* ------------------------------------------------------------------ code pages and locale (CRT) */

W(GetACP) { RET(0, 1252); }
W(GetOEMCP) { RET(0, 437); }
W(IsValidCodePage) { RET(1, 1); }
W(GetCPInfo) {
    uint32_t p = ARG(1);
    memset(HP(p), 0, 20);
    W32(p, 1);       /* MaxCharSize */
    W8(p + 4, '?');  /* DefaultChar */
    RET(2, 1);
}

W(MultiByteToWideChar) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET(6, n);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) W16(dst + 2 * i, R8(src + i));
    RET(6, k);
}

W(WideCharToMultiByte) {
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (!cap) RET(8, n);
    int k = n < cap ? n : cap;
    for (int i = 0; i < k; i++) { uint16_t w = R16(src + 2 * i); W8(dst + i, w < 256 ? (uint8_t)w : '?'); }
    RET(8, k);
}

W(GetStringTypeA) {
    uint32_t src = ARG(2), out = ARG(4);
    int32_t n = (int32_t)ARG(3);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    for (int i = 0; i < n; i++) {
        int ch = R8(src + i), t = 0;
        if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
        if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
        if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
        W16(out + 2 * i, (uint16_t)t);
    }
    RET(5, 1);
}
W(GetStringTypeW) {
    uint32_t src = ARG(1), out = ARG(3);
    int32_t n = (int32_t)ARG(2);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) {
        int ch = R16(src + 2 * i), t = 0;
        if (ch < 128) {
            if (isupper(ch)) t |= 1; if (islower(ch)) t |= 2; if (isdigit(ch)) t |= 4; if (isspace(ch)) t |= 8;
            if (ispunct(ch)) t |= 0x10; if (iscntrl(ch)) t |= 0x20; if (ch == ' ' || ch == '\t') t |= 0x40;
            if (isxdigit(ch)) t |= 0x80; if (isalpha(ch)) t |= 0x100;
        }
        W16(out + 2 * i, (uint16_t)t);
    }
    RET(4, 1);
}

W(LCMapStringA) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen(GSTR(src)) + 1;
    if (!cap) RET(6, n);
    for (int i = 0; i < n && i < cap; i++) {
        int ch = R8(src + i);
        W8(dst + i, (uint8_t)((flags & 0x100) ? tolower(ch) : (flags & 0x200) ? toupper(ch) : ch));
    }
    RET(6, n < cap ? n : cap);
}
W(LCMapStringW) {
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (R16(src + 2 * n)) n++; n++; }
    if (!cap) RET(6, n);
    for (int i = 0; i < n && i < cap; i++) {
        int ch = R16(src + 2 * i);
        if (ch < 128) ch = (flags & 0x100) ? tolower(ch) : (flags & 0x200) ? toupper(ch) : ch;
        W16(dst + 2 * i, (uint16_t)ch);
    }
    RET(6, n < cap ? n : cap);
}

static int cmp_strings(const char *a, int na, const char *b, int nb, int nocase) {
    int i = 0;
    for (; i < na && i < nb; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (nocase) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
W(CompareStringA) {
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    const char *a = GSTR(ARG(2)), *b = GSTR(ARG(4));
    if (na < 0) na = (int32_t)strlen(a);
    if (nb < 0) nb = (int32_t)strlen(b);
    RET(6, cmp_strings(a, na, b, nb, ARG(1) & 1));
}
W(CompareStringW) {
    int32_t na = (int32_t)ARG(3), nb = (int32_t)ARG(5);
    char a[1024], b[1024];
    uint32_t pa = ARG(2), pb = ARG(4);
    if (na < 0) { na = 0; while (R16(pa + 2 * na)) na++; }
    if (nb < 0) { nb = 0; while (R16(pb + 2 * nb)) nb++; }
    for (int i = 0; i < na && i < 1023; i++) a[i] = (char)R16(pa + 2 * i);
    for (int i = 0; i < nb && i < 1023; i++) b[i] = (char)R16(pb + 2 * i);
    RET(6, cmp_strings(a, na < 1023 ? na : 1023, b, nb < 1023 ? nb : 1023, ARG(1) & 1));
}

W(GetUserDefaultLCID) { RET(0, 0x0809); }  /* en-GB */
W(IsValidLocale) { RET(2, 1); }
W(EnumSystemLocalesA) { RET(2, 1); }
static int locale_info(uint32_t type, char *out, size_t n) {
    switch (type & 0xffff) {
    case 0x1004: return snprintf(out, n, "1252");      /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: return snprintf(out, n, "850");       /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0001: return snprintf(out, n, "0809");      /* LOCALE_ILANGUAGE */
    case 0x0003: return snprintf(out, n, "ENG");       /* LOCALE_SABBREVLANGNAME */
    case 0x0007: return snprintf(out, n, "GBR");       /* LOCALE_SABBREVCTRYNAME */
    case 0x000E: return snprintf(out, n, ".");         /* LOCALE_SDECIMAL */
    case 0x000F: return snprintf(out, n, ",");         /* LOCALE_STHOUSAND */
    case 0x0014: return snprintf(out, n, "\xa3");      /* LOCALE_SCURRENCY */
    default: return snprintf(out, n, "0");
    }
}
W(GetLocaleInfoA) {
    char tmp[64];
    int k = locale_info(ARG(1), tmp, sizeof tmp) + 1;
    if (ARG(3)) snprintf((char *)HP(ARG(2)), ARG(3), "%s", tmp);
    RET(4, k);
}
W(GetLocaleInfoW) {
    char tmp[64];
    int k = locale_info(ARG(1), tmp, sizeof tmp) + 1;
    for (int i = 0; ARG(3) && i < k && i < (int)ARG(3); i++) W16(ARG(2) + 2 * i, (uint8_t)tmp[i]);
    RET(4, k);
}

/* ------------------------------------------------------------------ time */

W(GetTickCount) { RET(0, (uint32_t)(guest_seconds() * 1000.0)); }
W(QueryPerformanceFrequency) { W64(ARG(0), 1000000000ull); RET(1, 1); }
W(QueryPerformanceCounter) { W64(ARG(0), (uint64_t)(guest_seconds() * 1e9)); RET(1, 1); }
W(GetSystemTimeAsFileTime) { W64(ARG(0), guest_time_100ns()); RET(1, 0); }
W(FileTimeToLocalFileTime) { W64(ARG(1), R64(ARG(0))); RET(2, 1); }
W(FileTimeToSystemTime) {
    uint64_t ft = R64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t p = ARG(1);
    W16(p, (uint16_t)(tmv.tm_year + 1900)); W16(p + 2, (uint16_t)(tmv.tm_mon + 1)); W16(p + 4, (uint16_t)tmv.tm_wday);
    W16(p + 6, (uint16_t)tmv.tm_mday); W16(p + 8, (uint16_t)tmv.tm_hour); W16(p + 10, (uint16_t)tmv.tm_min);
    W16(p + 12, (uint16_t)tmv.tm_sec); W16(p + 14, (uint16_t)((ft / 10000) % 1000));
    RET(2, 1);
}
W(GetTimeZoneInformation) { memset(HP(ARG(0)), 0, 172); RET(1, 0 /* TIME_ZONE_ID_UNKNOWN */); }
W(GetDateFormatA) {
    const char *s = "15/10/2026";
    if (ARG(5)) snprintf((char *)HP(ARG(4)), ARG(5), "%s", s);
    RET(6, strlen(s) + 1);
}
W(GetTimeFormatA) {
    const char *s = "12:00:00";
    if (ARG(5)) snprintf((char *)HP(ARG(4)), ARG(5), "%s", s);
    RET(6, strlen(s) + 1);
}

/* ------------------------------------------------------------------ memory */

#define HEAP_HANDLE 0x00A70000u
W(HeapCreate) { RET(3, HEAP_HANDLE); }
W(HeapDestroy) { RET(1, 1); }
W(GetProcessHeap) { RET(0, HEAP_HANDLE); }
W(HeapAlloc) { RET(3, heap_alloc(ARG(2), ARG(1) & 8)); }
W(HeapFree) { heap_free(ARG(2)); RET(3, 1); }
W(HeapReAlloc) {
    if (ARG(1) & 0x10 /* HEAP_REALLOC_IN_PLACE_ONLY */) RET(4, heap_size(ARG(2)) >= ARG(3) ? ARG(2) : 0);
    RET(4, heap_realloc(ARG(2), ARG(3), ARG(1) & 8));
}
W(HeapSize) { RET(3, heap_size(ARG(2))); }
W(GlobalAlloc) { RET(2, heap_alloc(ARG(1), ARG(0) & 0x40)); }
W(GlobalFree) { heap_free(ARG(0)); RET(1, 0); }
W(LocalFree) { heap_free(ARG(0)); RET(1, 0); }

W(VirtualAlloc) {
    uint32_t addr = ARG(0), size = ARG(1);
    if (addr) RET(4, addr);  /* committing inside a region we already handed out */
    RET(4, guest_valloc(size));
}
W(VirtualFree) { RET(3, 1); }
W(VirtualProtect) {
    /* Executable protections mark code the game generated or loaded itself (AEMS sound modules). */
    if (ARG(2) & 0xf0) { LOG("[kernel32] VirtualProtect(%08x, %x, exec)\n", ARG(0), ARG(1)); dyncode_register(ARG(0), ARG(1)); }
    if (ARG(3)) W32(ARG(3), 4 /* PAGE_READWRITE */);
    RET(4, 1);
}
W(VirtualQuery) {
    uint32_t a = ARG(0), p = ARG(1);
    memset(HP(p), 0, 28);
    W32(p, a & ~0xfffu); W32(p + 4, a & ~0xffffu); W32(p + 8, 4); W32(p + 12, 0x1000);
    W32(p + 16, 0x1000 /* MEM_COMMIT */); W32(p + 20, 4); W32(p + 24, 0x20000 /* MEM_PRIVATE */);
    RET(3, 28);
}

/* ------------------------------------------------------------------ TLS */

static uint32_t g_tls_used;  /* bitmask of allocated slots */
W(TlsAlloc) {
    for (int i = 0; i < 64; i++)
        if (!(g_tls_used & (1ull << i)) && i < 32) { g_tls_used |= 1u << i; RET(0, i); }
    RET(0, 0xFFFFFFFFu);
}
W(TlsFree) { if (ARG(0) < 32) g_tls_used &= ~(1u << ARG(0)); RET(1, 1); }
W(TlsGetValue) { set_error(0); RET(1, ARG(0) < 64 ? t_self->tls[ARG(0)] : 0); }
W(TlsSetValue) { if (ARG(0) < 64) t_self->tls[ARG(0)] = ARG(1); RET(2, 1); }

/* ------------------------------------------------------------------ critical sections and interlocked */

typedef struct CS { uint32_t key; pthread_mutex_t m; struct CS *next; GuestThread *owner; int depth; } CS;
static CS *g_cs[1024];
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

static CS *cs_get(uint32_t key) {
    uint32_t h = (key >> 2) & 1023;
    pthread_mutex_lock(&g_cs_lock);
    CS *x = g_cs[h];
    while (x && x->key != key) x = x->next;
    if (!x) {
        x = calloc(1, sizeof *x);
        x->key = key;
        pthread_mutexattr_t at;
        pthread_mutexattr_init(&at);
        pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&x->m, &at);
        x->next = g_cs[h];
        g_cs[h] = x;
    }
    pthread_mutex_unlock(&g_cs_lock);
    return x;
}
static pthread_mutex_t *cs_for(uint32_t key) { return &cs_get(key)->m; }
/* Deterministic mode: ownership is tracked here and a contended Enter blocks in the scheduler (a host
   mutex would block while holding the scheduler's baton). */
static int cs_free(void *p) { return ((CS *)p)->owner == NULL; }
W(InitializeCriticalSection) { memset(HP(ARG(0)), 0, 24); W32(ARG(0) + 4, 0xFFFFFFFFu); cs_for(ARG(0)); RET(1, 0); }
W(DeleteCriticalSection) { RET(1, 0); }
W(EnterCriticalSection) {
    if (!sched_active()) { pthread_mutex_lock(cs_for(ARG(0))); RET(1, 0); }
    CS *x = cs_get(ARG(0));
    if (x->owner != t_self) sched_wait(cs_free, x, UINT64_MAX, NULL);
    x->owner = t_self;
    x->depth++;
    RET(1, 0);
}
W(LeaveCriticalSection) {
    if (!sched_active()) { pthread_mutex_unlock(cs_for(ARG(0))); RET(1, 0); }
    CS *x = cs_get(ARG(0));
    if (x->owner == t_self && --x->depth == 0) x->owner = NULL;
    RET(1, 0);
}

/* same global lock as the translated lock-prefixed instructions, so both agree */
W(InterlockedIncrement) { guest_lock(); uint32_t v = R32(ARG(0)) + 1; W32(ARG(0), v); guest_unlock(); RET(1, v); }
W(InterlockedDecrement) { guest_lock(); uint32_t v = R32(ARG(0)) - 1; W32(ARG(0), v); guest_unlock(); RET(1, v); }
W(InterlockedExchange) { guest_lock(); uint32_t v = R32(ARG(0)); W32(ARG(0), ARG(1)); guest_unlock(); RET(2, v); }

/* ------------------------------------------------------------------ sync objects */

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int manual, signaled;      /* event */
    int count, max;            /* semaphore */
    pthread_t owner; int owned, depth;  /* mutex */
    int waiters, handoff;               /* mutex: threads blocked on it; released to one of them */
    int finished; uint32_t exit_code;   /* thread */
} Sync;

static Sync *sync_new(void) {
    Sync *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    return s;
}

W(CreateEventA) {
    uint32_t h = handle_new(H_EVENT);
    Sync *s = sync_new();
    s->manual = ARG(1) != 0;
    s->signaled = ARG(2) != 0;
    handle_get(h, H_EVENT)->ptr = s;
    RET(4, h);
}
/* Host-side SetEvent, used by DirectSound position notifications. */
void kernel_set_event(uint32_t h) {
    WinHandle *x = handle_get(h, H_EVENT);
    if (!x) return;
    Sync *s = x->ptr;
    pthread_mutex_lock(&s->m); s->signaled = 1; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->m);
}

W(SetEvent) {
    WinHandle *x = handle_get(ARG(0), H_EVENT);
    if (!x) RET(1, 0);
    Sync *s = x->ptr;
    pthread_mutex_lock(&s->m); s->signaled = 1; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->m);
    RET(1, 1);
}
W(ResetEvent) {
    WinHandle *x = handle_get(ARG(0), H_EVENT);
    if (!x) RET(1, 0);
    Sync *s = x->ptr;
    pthread_mutex_lock(&s->m); s->signaled = 0; pthread_mutex_unlock(&s->m);
    RET(1, 1);
}
W(CreateMutexA) {
    uint32_t h = handle_new(H_MUTEX);
    Sync *s = sync_new();
    if (ARG(1)) { s->owned = 1; s->owner = pthread_self(); s->depth = 1; }
    handle_get(h, H_MUTEX)->ptr = s;
    RET(3, h);
}
W(ReleaseMutex) {
    WinHandle *x = handle_get(ARG(0), H_MUTEX);
    if (!x) RET(1, 0);
    Sync *s = x->ptr;
    pthread_mutex_lock(&s->m);
    if (s->owned && pthread_equal(s->owner, pthread_self()) && --s->depth == 0) {
        s->owned = 0;
        /* Hand over to a thread that is already waiting rather than letting the releasing thread
           barge back in: EA's sound code locks and unlocks in a tight loop, which otherwise starves
           the timer thread (in WebAssembly it never gets the lock at all). */
        if (s->waiters) s->handoff = 1;
        pthread_cond_broadcast(&s->cv);
    }
    pthread_mutex_unlock(&s->m);
    RET(1, 1);
}
W(CreateSemaphoreA) {
    uint32_t h = handle_new(H_SEMAPHORE);
    Sync *s = sync_new();
    s->count = (int)ARG(1);
    s->max = (int)ARG(2);
    handle_get(h, H_SEMAPHORE)->ptr = s;
    RET(4, h);
}
W(ReleaseSemaphore) {
    WinHandle *x = handle_get(ARG(0), H_SEMAPHORE);
    if (!x) RET(3, 0);
    Sync *s = x->ptr;
    pthread_mutex_lock(&s->m);
    if (ARG(2)) W32(ARG(2), (uint32_t)s->count);
    s->count += (int)ARG(1);
    if (s->count > s->max) s->count = s->max;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    RET(3, 1);
}

/* Returns 1 when the object became signaled (and consumes it where that's the semantics). */
static int try_acquire(WinHandle *x, int waited) {
    Sync *s = x->ptr;
    switch (x->type) {
    case H_EVENT: if (s->signaled) { if (!s->manual) s->signaled = 0; return 1; } return 0;
    case H_SEMAPHORE: if (s->count > 0) { s->count--; return 1; } return 0;
    case H_MUTEX:
        if (!s->owned && (!s->handoff || waited)) {
            s->owned = 1; s->owner = pthread_self(); s->depth = 1; s->handoff = 0;
            return 1;
        }
        if (s->owned && pthread_equal(s->owner, pthread_self())) { s->depth++; return 1; }  /* recursion */
        return 0;
    case H_THREAD: return s->finished;
    default: return 1;
    }
}

/* Deterministic mode: could try_acquire succeed now? (Evaluated by the scheduler for a parked waiter.) */
static int can_acquire(void *p) {
    WinHandle *x = p;
    Sync *s = x->ptr;
    switch (x->type) {
    case H_EVENT: return s->signaled;
    case H_SEMAPHORE: return s->count > 0;
    case H_MUTEX: return !s->owned;
    case H_THREAD: return s->finished;
    default: return 1;
    }
}
static uint32_t wait_one_det(WinHandle *x, uint32_t ms) {
    Sync *s = x->ptr;
    uint64_t deadline = ms == 0xFFFFFFFFu ? UINT64_MAX : sched_now_us() + (uint64_t)ms * 1000ull;
    for (;;) {
        pthread_mutex_lock(&s->m);
        int ok = try_acquire(x, 1);
        pthread_mutex_unlock(&s->m);
        if (ok) return 0;
        if (ms == 0 || !sched_wait(can_acquire, x, deadline, NULL)) return 0x102;   /* WAIT_TIMEOUT */
    }
}

/* PORT_PROFILE_WAITS=1: counts the game thread's Sleep/SleepEx/WaitForSingleObject calls by call site (the
   first three return addresses on its stack) and prints the busiest every 3 s. A game stuck or slow in a
   polling loop shows up here as one call site with a huge count; disassemble its addresses next. */
static void profile_wait(CPU *c, const char *what, uint32_t arg) {
    static int on = -1;
    if (on < 0) on = getenv("PORT_PROFILE_WAITS") != NULL;
    if (!on || !on_game_thread()) return;
    typedef struct { const char *what; uint32_t site[3], arg, n; } Site;
    static Site tab[512];
    static int nt;
    static double last;
    uint32_t site[3] = { 0, 0, 0 };
    for (int k = 0, m = 0; k < 256 && m < 3; k++) { uint32_t v = R32(c->esp + 4 * k); if (looks_like_code(v)) site[m++] = v; }
    int i;
    for (i = 0; i < nt; i++)
        if (tab[i].what == what && tab[i].arg == arg && !memcmp(tab[i].site, site, sizeof site)) break;
    if (i == nt && nt < 512) { tab[nt] = (Site){ what, { site[0], site[1], site[2] }, arg, 0 }; nt++; }
    if (i < 512) tab[i].n++;
    double now = host_seconds();
    if (now - last < 3) return;
    last = now;
    fprintf(stderr, "[profile] busiest wait/sleep call sites (count, call, argument, return addresses):\n");
    for (int shown = 0; shown < 8; shown++) {
        int b = -1;
        for (int k = 0; k < nt; k++) if (tab[k].n && (b < 0 || tab[k].n > tab[b].n)) b = k;
        if (b < 0) break;
        fprintf(stderr, "  %9u %-20s %8x  %08x %08x %08x\n", tab[b].n, tab[b].what, tab[b].arg, tab[b].site[0], tab[b].site[1], tab[b].site[2]);
        tab[b].n = 0;   /* counts restart each report */
    }
    for (int k = 0; k < nt; k++) tab[k].n = 0;
}

static uint32_t wait_one(uint32_t h, uint32_t ms) {
    WinHandle *x = handle_get(h, -1);
    if (!x || !x->ptr) return 0;  /* unknown objects count as signaled */
    if (sched_active()) return wait_one_det(x, ms);
    Sync *s = x->ptr;
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    if (ms != 0xFFFFFFFFu) {
        dl.tv_sec += ms / 1000;
        dl.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&s->m);
    uint32_t r = 0;
    int waited = 0;
    while (!try_acquire(x, waited)) {
        if (ms == 0) { r = 0x102; break; }
        if (!waited) { waited = 1; s->waiters++; }
        int e = ms == 0xFFFFFFFFu ? pthread_cond_wait(&s->cv, &s->m) : pthread_cond_timedwait(&s->cv, &s->m, &dl);
        if (e == ETIMEDOUT && !try_acquire(x, waited)) { r = 0x102 /* WAIT_TIMEOUT */; break; }
        if (e == ETIMEDOUT) break;
    }
    if (waited) {
        s->waiters--;
        if (r && s->handoff && !s->waiters) { s->handoff = 0; pthread_cond_broadcast(&s->cv); }  /* nobody left to take it */
    }
    pthread_mutex_unlock(&s->m);
    return r;
}
W(WaitForSingleObject) {
    extern uint32_t g_det_waits;
    if (on_game_thread()) g_det_waits++;
    profile_wait(c, "WaitForSingleObject", ARG(1));
    RET(2, wait_one(ARG(0), ARG(1)));
}

/* Deterministic mode: a timed block. A zero-length sleep (a yield) also lets PORT_DET_YIELD_US (default
   1000) of virtual time pass: Cricket 07's intro-movie player (VideoPlayer.cpp) spins on
   SleepEx(0) until the next movie frame is due, and with only the 1 us per clock read that took ~6,600
   scheduler hand-offs per frame (about 2 fps). */
static int det_sleep(uint32_t ms) {
    if (!sched_active()) return 0;
    extern uint32_t g_det_sleeps;
    if (on_game_thread()) g_det_sleeps++;
    static int64_t yield_us = -1;
    if (yield_us < 0) { const char *e = getenv("PORT_DET_YIELD_US"); yield_us = e ? atoll(e) : 1000; }
    uint64_t us = ms ? (uint64_t)ms * 1000ull : (uint64_t)yield_us;
    if (!us) sched_yield_det();
    else sched_wait(NULL, NULL, sched_now_us() + us, NULL);
    return 1;
}
W(Sleep) {
    uint32_t ms = ARG(0);
    profile_wait(c, "Sleep", ms);
    if (det_sleep(ms)) RET(1, 0);
    if (ms == 0) sched_yield(); else usleep(ms * 1000);
    RET(1, 0);
}
W(SleepEx) {
    uint32_t ms = ARG(0);
    profile_wait(c, "SleepEx", ms);
    if (det_sleep(ms)) RET(2, 0);
    if (ms == 0) sched_yield(); else usleep(ms * 1000);
    RET(2, 0);
}

/* ------------------------------------------------------------------ threads */

#define THREAD_STACK_SIZE 0x100000u
static uint32_t g_next_tid = 0x200;

typedef struct { GuestThread *t; uint32_t start, param; Sync *sync; int suspended; pthread_mutex_t go_m;
                 pthread_cond_t go_cv; void *sched; } ThreadStart;
static int thread_resumed(void *p) { return !((ThreadStart *)p)->suspended; }

static void *thread_main(void *arg) {
    ThreadStart *ts = arg;
    t_self = ts->t;
    CPU *c = &ts->t->cpu;
    fpu_sync_rounding(c);
    sched_enter(ts->sched);   /* deterministic mode: runs once resumed and scheduled */
    pthread_mutex_lock(&ts->go_m);
    while (ts->suspended) pthread_cond_wait(&ts->go_cv, &ts->go_m);
    pthread_mutex_unlock(&ts->go_m);
    LOG("[kernel32] thread %x starts at %08x\n", ts->t->id, ts->start);
    PUSH32(c, ts->param);
    PUSH32(c, GUEST_RETURN_SENTINEL);
    call_indirect(c, ts->start);
    pthread_mutex_lock(&ts->sync->m);
    ts->sync->finished = 1;
    ts->sync->exit_code = c->eax;
    pthread_cond_broadcast(&ts->sync->cv);
    pthread_mutex_unlock(&ts->sync->m);
    LOG("[kernel32] thread %x exits with %u\n", ts->t->id, c->eax);
    sched_exit();
    return NULL;
}

static ThreadStart *g_threads_by_handle[4096];

/* A guest thread context: its own CPU state, guest stack and thread information block. */
GuestThread *g_all_threads[64];   /* for the watchdog */
int g_nthreads;

GuestThread *guest_thread_new(void) {
    GuestThread *t = calloc(1, sizeof *t);
    int slot = __atomic_fetch_add(&g_nthreads, 1, __ATOMIC_SEQ_CST);
    if (slot < 64) g_all_threads[slot] = t;
    t->id = __atomic_fetch_add(&g_next_tid, 4, __ATOMIC_SEQ_CST);
    uint32_t stack = guest_valloc(THREAD_STACK_SIZE);
    t->tib = guest_valloc(0x1000);
    W32(t->tib + 0x00, 0xFFFFFFFFu);
    W32(t->tib + 0x04, stack + THREAD_STACK_SIZE);
    W32(t->tib + 0x08, stack);
    W32(t->tib + 0x18, t->tib);
    CPU *nc = &t->cpu;
    nc->esp = stack + THREAD_STACK_SIZE - 16;
    nc->fpu_cw = 0x027f;
    nc->mxcsr = 0x1f80;
    nc->fs_base = t->tib;
    nc->pf_byte = 1;
    return t;
}

W(CreateThread) {
    uint32_t start = ARG(2), param = ARG(3), flags = ARG(4), tid_out = ARG(5);
    GuestThread *t = guest_thread_new();

    uint32_t h = handle_new(H_THREAD);
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->t = t; ts->start = start; ts->param = param; ts->sync = sync_new();
    ts->suspended = (flags & 4) != 0;  /* CREATE_SUSPENDED */
    pthread_mutex_init(&ts->go_m, NULL);
    pthread_cond_init(&ts->go_cv, NULL);
    handle_get(h, H_THREAD)->ptr = ts->sync;
    g_threads_by_handle[(h - 0x1000) / 4] = ts;
    if (tid_out) W32(tid_out, t->id);
    char name[24];
    snprintf(name, sizeof name, "thread %x", t->id);
    ts->sched = sched_new(name, ts->suspended ? thread_resumed : NULL, ts);
    pthread_t pt;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 64u << 20);  /* host stack: recompiled frames are large (more so in wasm) */
    pthread_create(&pt, &at, thread_main, ts);
    pthread_detach(pt);
    LOG("[kernel32] CreateThread(start=%08x param=%08x flags=%x) -> handle %x tid %x\n", start, param, flags, h, t->id);
    RET(6, h);
}
W(ResumeThread) {
    uint32_t h = ARG(0);
    ThreadStart *ts = (h >= 0x1000 && (h - 0x1000) / 4 < 4096) ? g_threads_by_handle[(h - 0x1000) / 4] : NULL;
    if (ts) {
        pthread_mutex_lock(&ts->go_m);
        int was = ts->suspended;
        ts->suspended = 0;
        pthread_cond_broadcast(&ts->go_cv);
        pthread_mutex_unlock(&ts->go_m);
        RET(1, was);
    }
    RET(1, 0);
}
W(ExitThread) {
    LOG("[kernel32] ExitThread(%u)\n", ARG(0));
    sched_exit();
    pthread_exit(NULL);
}
W(TerminateThread) { STUB("TerminateThread(%x) ignored\n", ARG(0)); RET(2, 1); }
W(GetExitCodeThread) {
    WinHandle *x = handle_get(ARG(0), H_THREAD);
    Sync *s = x ? x->ptr : NULL;
    W32(ARG(1), s && s->finished ? s->exit_code : 259 /* STILL_ACTIVE */);
    RET(2, 1);
}
W(SetThreadPriority) { RET(2, 1); }
W(GetThreadPriority) { RET(1, 0); }
W(SetThreadAffinityMask) { RET(2, 1); }
W(DuplicateHandle) {
    if (ARG(3)) W32(ARG(3), ARG(1) == 0xFFFFFFFEu ? 0x1FFFFFFCu : ARG(1));
    RET(7, 1);
}
W(CloseHandle) { handle_close(ARG(0)); RET(1, 1); }

/* ------------------------------------------------------------------ files */

W(CreateFileA) {
    const char *name = GSTR(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char host[1024];
    int exists = map_path(name, host, sizeof host);
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
    case 1: fl |= O_CREAT | O_EXCL; break;   /* CREATE_NEW */
    case 2: fl |= O_CREAT | O_TRUNC; break;  /* CREATE_ALWAYS */
    case 3: break;                           /* OPEN_EXISTING */
    case 4: fl |= O_CREAT; break;            /* OPEN_ALWAYS */
    case 5: fl |= O_TRUNC; break;            /* TRUNCATE_EXISTING */
    }
    if (fl & O_CREAT) {  /* make sure the parent folder exists for files the game writes */
        char dir[1024];
        snprintf(dir, sizeof dir, "%s", host);
        for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
    }
    int fd = open(host, fl, 0644);
    LOG("[file] CreateFileA(%s -> %s, disp %u) = %s\n", name, host, disp, fd >= 0 ? "ok" : strerror(errno));
    if (fd < 0) { set_error(exists ? ERROR_ACCESS_DENIED : ERROR_FILE_NOT_FOUND); RET(7, 0xFFFFFFFFu); }
    uint32_t h = handle_new(H_FILE);
    WinHandle *x = handle_get(h, H_FILE);
    x->fd = fd;
    snprintf(x->path, sizeof x->path, "%s", host);
    set_error((disp == 4 || disp == 2) && exists ? ERROR_ALREADY_EXISTS : 0);
    RET(7, h);
}

W(CreateFileW) {
    char name[512];
    uint32_t p = ARG(0);
    int i = 0;
    for (; i < 511 && R16(p + 2 * i); i++) name[i] = (char)R16(p + 2 * i);
    name[i] = 0;
    uint32_t tmp = guest_strdup(name);
    W32(c->esp + 4, tmp);
    w_CreateFileA(c);
    heap_free(tmp);
}

W(ReadFile) {
    WinHandle *x = handle_get(ARG(0), H_FILE);
    if (!x) { set_error(ERROR_INVALID_HANDLE); RET(5, 0); }
    /* Windows only returns short reads at end of file; host reads can stop early, so loop */
    uint32_t want = ARG(2), got = 0;
    ssize_t n = 0;
    while (got < want && (n = read(x->fd, HP(ARG(1) + got), want - got)) > 0) got += (uint32_t)n;
    if (ARG(3)) W32(ARG(3), got);
    RET(5, n >= 0);
}
W(WriteFile) {
    uint32_t h = ARG(0);
    if (h == STD_OUT || h == STD_ERR) {
        fwrite(HP(ARG(1)), 1, ARG(2), stderr);
        if (ARG(3)) W32(ARG(3), ARG(2));
        RET(5, 1);
    }
    WinHandle *x = handle_get(h, H_FILE);
    if (!x) { set_error(ERROR_INVALID_HANDLE); RET(5, 0); }
    ssize_t n = write(x->fd, HP(ARG(1)), ARG(2));
    if (ARG(3)) W32(ARG(3), n > 0 ? (uint32_t)n : 0);
    RET(5, n >= 0);
}
W(SetFilePointer) {
    WinHandle *x = handle_get(ARG(0), H_FILE);
    if (!x) { set_error(ERROR_INVALID_HANDLE); RET(4, 0xFFFFFFFFu); }
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)R32(ARG(2)) << 32) | ARG(1));
    off_t r = lseek(x->fd, off, ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END);
    if (ARG(2)) W32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    RET(4, (uint32_t)r);
}
W(GetFileSize) {
    WinHandle *x = handle_get(ARG(0), H_FILE);
    if (!x) RET(2, 0xFFFFFFFFu);
    struct stat st;
    fstat(x->fd, &st);
    if (ARG(1)) W32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET(2, (uint32_t)st.st_size);
}
W(FlushFileBuffers) { RET(1, 1); }
W(SetEndOfFile) {
    WinHandle *x = handle_get(ARG(0), H_FILE);
    if (!x) RET(1, 0);
    RET(1, ftruncate(x->fd, lseek(x->fd, 0, SEEK_CUR)) == 0);
}
W(DeleteFileA) {
    char host[1024];
    map_path(GSTR(ARG(0)), host, sizeof host);
    RET(1, unlink(host) == 0);
}
W(MoveFileA) {
    char a[1024], b[1024];
    map_path(GSTR(ARG(0)), a, sizeof a);
    map_path(GSTR(ARG(1)), b, sizeof b);
    RET(2, rename(a, b) == 0);
}
W(CreateDirectoryA) {
    char host[1024];
    map_path(GSTR(ARG(0)), host, sizeof host);
    int r = mkdir(host, 0755);
    if (r && errno == EEXIST) set_error(ERROR_ALREADY_EXISTS);
    RET(2, r == 0);
}
W(RemoveDirectoryA) {
    char host[1024];
    map_path(GSTR(ARG(0)), host, sizeof host);
    RET(1, rmdir(host) == 0);
}

typedef struct { char dir[1024]; char pattern[256]; DIR *d; } FindState;

static int find_next(FindState *fs, uint32_t out) {
    struct dirent *e;
    while (fs->d && (e = readdir(fs->d))) {
        if (fnmatch(fs->pattern, e->d_name, FNM_CASEFOLD)) continue;
        char full[1300];
        snprintf(full, sizeof full, "%s/%s", fs->dir, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;
        memset(HP(out), 0, 320);  /* WIN32_FIND_DATAA */
        W32(out, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        W32(out + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        W32(out + 32, (uint32_t)st.st_size);
        snprintf((char *)HP(out + 44), 260, "%s", e->d_name);
        return 1;
    }
    return 0;
}

W(FindFirstFileA) {
    const char *spec = GSTR(ARG(0));
    char guest_dir[1024], host[1024];
    snprintf(guest_dir, sizeof guest_dir, "%s", spec);
    char *sl = strrchr(guest_dir, '\\');
    char *sl2 = strrchr(guest_dir, '/');
    if (sl2 > sl) sl = sl2;
    FindState *fs = calloc(1, sizeof *fs);
    if (sl) { snprintf(fs->pattern, sizeof fs->pattern, "%s", sl + 1); *sl = 0; }
    else { snprintf(fs->pattern, sizeof fs->pattern, "%s", guest_dir); guest_dir[0] = 0; }
    if (!strcmp(fs->pattern, "*.*")) strcpy(fs->pattern, "*");
    map_path(guest_dir[0] ? guest_dir : ".", host, sizeof host);
    snprintf(fs->dir, sizeof fs->dir, "%s", host);
    fs->d = opendir(host);
    LOG("[file] FindFirstFileA(%s) in %s\n", spec, host);
    if (!find_next(fs, ARG(1))) {
        if (fs->d) closedir(fs->d);
        free(fs);
        set_error(ERROR_FILE_NOT_FOUND);
        RET(2, 0xFFFFFFFFu);
    }
    uint32_t h = handle_new(H_FIND);
    handle_get(h, H_FIND)->ptr = fs;
    RET(2, h);
}
W(FindNextFileA) {
    WinHandle *x = handle_get(ARG(0), H_FIND);
    if (!x || !find_next(x->ptr, ARG(1))) { set_error(ERROR_NO_MORE_FILES); RET(2, 0); }
    RET(2, 1);
}
W(FindClose) {
    WinHandle *x = handle_get(ARG(0), H_FIND);
    if (x && x->ptr) { FindState *fs = x->ptr; if (fs->d) closedir(fs->d); }
    handle_close(ARG(0));
    RET(1, 1);
}

W(GetCurrentDirectoryA) {
    const char *d = guest_cwd();
    if (ARG(0) > strlen(d)) strcpy((char *)HP(ARG(1)), d);
    RET(2, strlen(d));
}
W(SetCurrentDirectoryA) {
    LOG("[file] SetCurrentDirectoryA(%s)\n", GSTR(ARG(0)));
    guest_set_cwd(GSTR(ARG(0)));
    RET(1, 1);
}
W(GetFullPathNameA) {
    const char *p = GSTR(ARG(0));
    char full[1024];
    if (p[1] == ':') snprintf(full, sizeof full, "%s", p);
    else snprintf(full, sizeof full, "%s\\%s", guest_cwd(), p);
    if (ARG(1) > strlen(full)) strcpy((char *)HP(ARG(2)), full);
    if (ARG(3)) {
        char *b = strrchr((char *)HP(ARG(2)), '\\');
        W32(ARG(3), b ? ARG(2) + (uint32_t)(b + 1 - (char *)HP(ARG(2))) : ARG(2));
    }
    RET(4, strlen(full));
}

/* Drives: C: is the game's hard disk. D: is a CD-ROM holding the game disc, so disc checks pass. */
W(GetLogicalDrives) { RET(0, (1u << 2) | (1u << 3)); }
W(GetLogicalDriveStringsA) {
    const char s[] = "C:\\\0D:\\\0";
    if (ARG(0) >= sizeof s) memcpy(HP(ARG(1)), s, sizeof s);
    RET(2, sizeof s - 1);
}
W(GetDriveTypeA) {
    const char *r = ARG(0) ? GSTR(ARG(0)) : "C:\\";
    RET(1, (r[0] == 'D' || r[0] == 'd') ? 5 /* DRIVE_CDROM */ : 3 /* DRIVE_FIXED */);
}
W(GetDiskFreeSpaceA) {
    if (ARG(1)) W32(ARG(1), 8);
    if (ARG(2)) W32(ARG(2), 512);
    if (ARG(3)) W32(ARG(3), 0x100000);
    if (ARG(4)) W32(ARG(4), 0x200000);
    RET(5, 1);
}
W(GetVolumeInformationA) {
    const char *r = ARG(0) ? GSTR(ARG(0)) : "C:\\";
    const char *label = (r[0] == 'D' || r[0] == 'd') ? PORT_VOLUME_LABEL : "SYSTEM";
    LOG("[file] GetVolumeInformationA(%s)\n", r);
    if (ARG(1)) snprintf((char *)HP(ARG(1)), ARG(2), "%s", label);
    if (ARG(3)) W32(ARG(3), 0x07C07C07);
    if (ARG(4)) W32(ARG(4), 255);
    if (ARG(5)) W32(ARG(5), 0);
    if (ARG(6)) snprintf((char *)HP(ARG(6)), ARG(7), "%s", (r[0] == 'D' || r[0] == 'd') ? "CDFS" : "NTFS");
    RET(8, 1);
}

W(CreateFileMappingA) {
    WinHandle *f = handle_get(ARG(0), H_FILE);
    uint32_t h = handle_new(H_MAPPING);
    WinHandle *m = handle_get(h, H_MAPPING);
    m->fd = f ? dup(f->fd) : -1;
    RET(6, h);
}
W(MapViewOfFile) {
    WinHandle *m = handle_get(ARG(0), H_MAPPING);
    if (!m || m->fd < 0) RET(5, 0);
    struct stat st;
    fstat(m->fd, &st);
    uint32_t off = ARG(3), len = ARG(4) ? ARG(4) : (uint32_t)st.st_size - off;
    uint32_t a = guest_valloc(len);
    /* copy-in (views are only read by the game); loop: a single read may come back short */
    uint32_t got = 0;
    while (got < len) {
        ssize_t r = pread(m->fd, HP(a + got), len - got, (off_t)off + got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    LOG("[file] MapViewOfFile(%u bytes, read %u) -> %08x\n", len, got, a);
    RET(5, a);
}
W(UnmapViewOfFile) { RET(1, 1); }

/* ------------------------------------------------------------------ resources (none exposed yet) */

W(FindResourceA) { STUB("FindResourceA(%x)\n", ARG(1)); RET(3, 0); }
W(FindResourceW) { RET(3, 0); }
W(LoadResource) { RET(2, 0); }
W(LockResource) { RET(1, 0); }
W(SizeofResource) { RET(2, 0); }

/* ------------------------------------------------------------------ console (CRT only) */

W(GetConsoleMode) { RET(2, 0); }
W(SetConsoleMode) { RET(2, 1); }
W(SetConsoleCtrlHandler) { RET(2, 1); }
W(WriteConsoleA) { fwrite(HP(ARG(1)), 1, ARG(2), stderr); if (ARG(3)) W32(ARG(3), ARG(2)); RET(5, 1); }
W(ReadConsoleInputA) { RET(4, 0); }
W(PeekConsoleInputA) { if (ARG(3)) W32(ARG(3), 0); RET(4, 1); }
W(GetNumberOfConsoleInputEvents) { if (ARG(1)) W32(ARG(1), 0); RET(2, 1); }

#define K(name) IMPORT("KERNEL32.DLL", name)
static const ImportHandler list[] = {
    K(GetVersionExA), K(GetModuleHandleA), K(LoadLibraryA), K(FreeLibrary), K(GetProcAddress),
    K(GetModuleFileNameA), K(GetCommandLineA), K(GetEnvironmentStrings), K(GetEnvironmentStringsW),
    K(FreeEnvironmentStringsA), K(FreeEnvironmentStringsW), K(SetEnvironmentVariableA), K(GetStartupInfoA),
    K(GetStdHandle), K(SetStdHandle), K(GetFileType), K(SetHandleCount), K(GetCurrentProcess),
    K(GetCurrentThread), K(GetCurrentProcessId), K(GetCurrentThreadId), K(GetLastError), K(SetLastError),
    K(ExitProcess), K(TerminateProcess), K(FatalAppExitA), K(GetSystemInfo), K(IsProcessorFeaturePresent),
    K(IsDebuggerPresent), K(OutputDebugStringA), K(DebugBreak), K(IsBadReadPtr), K(IsBadWritePtr),
    K(IsBadCodePtr), K(SetUnhandledExceptionFilter), K(UnhandledExceptionFilter), K(RaiseException),
    K(RtlUnwind), K(FormatMessageA), K(GetACP), K(GetOEMCP), K(IsValidCodePage), K(GetCPInfo),
    K(MultiByteToWideChar), K(WideCharToMultiByte), K(GetStringTypeA), K(GetStringTypeW), K(LCMapStringA),
    K(LCMapStringW), K(CompareStringA), K(CompareStringW), K(GetUserDefaultLCID), K(IsValidLocale),
    K(EnumSystemLocalesA), K(GetLocaleInfoA), K(GetLocaleInfoW), K(GetTickCount), K(QueryPerformanceFrequency),
    K(QueryPerformanceCounter), K(GetSystemTimeAsFileTime), K(FileTimeToLocalFileTime), K(FileTimeToSystemTime),
    K(GetTimeZoneInformation), K(GetDateFormatA), K(GetTimeFormatA), K(HeapCreate), K(HeapDestroy),
    K(GetProcessHeap), K(HeapAlloc), K(HeapFree), K(HeapReAlloc), K(HeapSize), K(GlobalAlloc), K(GlobalFree),
    K(LocalFree), K(VirtualAlloc), K(VirtualFree), K(VirtualProtect), K(VirtualQuery), K(TlsAlloc), K(TlsFree),
    K(TlsGetValue), K(TlsSetValue), K(InitializeCriticalSection), K(DeleteCriticalSection),
    K(EnterCriticalSection), K(LeaveCriticalSection), K(InterlockedIncrement), K(InterlockedDecrement),
    K(InterlockedExchange), K(CreateEventA), K(SetEvent), K(ResetEvent), K(CreateMutexA), K(ReleaseMutex),
    K(CreateSemaphoreA), K(ReleaseSemaphore), K(WaitForSingleObject), K(Sleep), K(SleepEx), K(CreateThread),
    K(ResumeThread), K(ExitThread), K(TerminateThread), K(GetExitCodeThread), K(SetThreadPriority),
    K(GetThreadPriority), K(SetThreadAffinityMask), K(DuplicateHandle), K(CloseHandle), K(CreateFileA),
    K(CreateFileW), K(ReadFile), K(WriteFile), K(SetFilePointer), K(GetFileSize), K(FlushFileBuffers),
    K(SetEndOfFile), K(DeleteFileA), K(MoveFileA), K(CreateDirectoryA), K(RemoveDirectoryA), K(FindFirstFileA),
    K(FindNextFileA), K(FindClose), K(GetCurrentDirectoryA), K(SetCurrentDirectoryA), K(GetFullPathNameA),
    K(GetLogicalDrives), K(GetLogicalDriveStringsA), K(GetDriveTypeA), K(GetDiskFreeSpaceA),
    K(GetVolumeInformationA), K(CreateFileMappingA), K(MapViewOfFile), K(UnmapViewOfFile), K(FindResourceA),
    K(FindResourceW), K(LoadResource), K(LockResource), K(SizeofResource), K(GetConsoleMode), K(SetConsoleMode),
    K(SetConsoleCtrlHandler), K(WriteConsoleA), K(ReadConsoleInputA), K(PeekConsoleInputA),
    K(GetNumberOfConsoleInputEvents),
};
const ImportTable kernel32_imports = { list, sizeof list / sizeof list[0] };
