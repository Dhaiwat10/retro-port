/* Small C API over the recompiled code so a Python driver can run single functions
 * (see difftest.py, which compares them against the original x86 under Unicorn). */
#include "runtime.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

static sigjmp_buf g_alarm_jb;
static void on_alarm(int sig) { (void)sig; siglongjmp(g_alarm_jb, 1); }

static CPU g_cpu;
static LoadedImage g_img;

int dt_init(const char *exe) {
    runtime_init(&g_cpu);
    if (load_image(exe, &g_img)) return -1;
    register_all_functions();
    return 0;
}

uint8_t *dt_mem(void) { return g_mem; }
const char *dt_trap_msg(void) { return g_trap_msg; }

/* regs: eax ecx edx ebx esp ebp esi edi (in/out). Returns 0 = returned normally, 1 = trapped,
   2 = no such function, 3 = timed out. The caller has already pushed the arguments and a return address. */
int dt_run(uint32_t addr, uint32_t *regs, uint16_t fpu_cw) {
    GuestFn fn = lookup_function(addr);
    if (!fn) return 2;
    CPU *c = &g_cpu;
    memset(c, 0, sizeof *c);
    c->eax = regs[0]; c->ecx = regs[1]; c->edx = regs[2]; c->ebx = regs[3];
    c->esp = regs[4]; c->ebp = regs[5]; c->esi = regs[6]; c->edi = regs[7];
    c->fpu_cw = fpu_cw; c->mxcsr = 0x1f80; c->fs_base = GUEST_TIB;
    c->pf_byte = 1; /* EFLAGS = 0x202: PF clear */
    fpu_sync_rounding(c);
    jmp_buf jb;
    g_trap_jmp = &jb;
    int rc = 0;
    /* Watchdog: a recompiled function that diverges into an endless loop reports rc = 3. */
    signal(SIGALRM, on_alarm);
    struct itimerval tv = { {0, 0}, {2, 0} }, off = { {0, 0}, {0, 0} };
    if (sigsetjmp(g_alarm_jb, 1)) {
        rc = 3;
    } else {
        setitimer(ITIMER_REAL, &tv, NULL);
        if (setjmp(jb) == 0) fn(c);
        else rc = 1;
    }
    setitimer(ITIMER_REAL, &off, NULL);
    g_trap_jmp = NULL;
    c->fpu_cw = 0x027f;
    fpu_sync_rounding(c); /* back to round-to-nearest for the host */
    regs[0] = c->eax; regs[1] = c->ecx; regs[2] = c->edx; regs[3] = c->ebx;
    regs[4] = c->esp; regs[5] = c->ebp; regs[6] = c->esi; regs[7] = c->edi;
    return rc;
}

/* Tracing: record up to `cap` instructions of the next dt_run (needs a --trace build). */
void dt_trace_start(uint32_t cap) {
    free(g_trace);
    g_trace = calloc(cap, sizeof *g_trace);
    g_trace_cap = cap;
    g_trace_len = 0;
}
uint32_t dt_trace_len(void) { return g_trace_len; }
TraceRec *dt_trace_buf(void) { return g_trace; }
