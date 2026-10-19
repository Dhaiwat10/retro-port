/* Debugging aid: a write watchpoint on guest memory. watch_arm(addr) write-protects the page holding
 * `addr`; the next write that lands within 16 bytes of it is reported with a host backtrace (the
 * recompiled frames are named f_XXXXXXXX after their guest address). Any fault on the page
 * unprotects it so the write proceeds; call watch_arm again to catch the next one. */
#include "win32.h"

#include <execinfo.h>
#include <signal.h>
#include <sys/mman.h>

static volatile uint32_t g_watch_addr;
static volatile int g_watch_armed;
static volatile int g_watch_hits;

static void on_fault(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    uint8_t *p = si->si_addr;
    if (!g_watch_armed || p < g_mem || p >= g_mem + 0x100000000ull) {
        signal(sig, SIG_DFL);   /* not ours: crash as usual */
        return;
    }
    uint32_t a = (uint32_t)(p - g_mem);
    uint32_t page = g_watch_addr & ~0x3FFFu;
    if (a >= g_watch_addr && a < g_watch_addr + 16 && g_watch_hits < 20) {
        g_watch_hits++;
        fprintf(stderr, "[watch] write to %08x (watching %08x):\n", a, g_watch_addr);
        void *bt[24];
        int n = backtrace(bt, 24);
        backtrace_symbols_fd(bt, n, 2);
    }
    mprotect(g_mem + page, 0x4000, PROT_READ | PROT_WRITE);
    g_watch_armed = 0;
}

void watch_arm(uint32_t addr) {
    static int installed;
    if (!installed) {
        installed = 1;
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_fault;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
    if (g_watch_armed) return;
    g_watch_addr = addr;
    g_watch_armed = 1;
    mprotect(g_mem + (addr & ~0x3FFFu), 0x4000, PROT_READ);
}
