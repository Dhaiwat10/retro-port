/* Deterministic scheduler (PORT_DETERMINISTIC=1): makes a run a function of its inputs alone, the basis
 * for lockstep netplay (every peer simulates the same match from the same inputs).
 *
 * Exactly one participant runs at a time: guest threads, the multimedia timer thread and the sound mixer.
 * The running one keeps going until it blocks (Sleep, a wait, a contended critical section, Present) or
 * exits; the next is chosen in creation order, round-robin from the one that blocked, among those whose
 * wait condition now holds. When every participant is blocked, the virtual clock jumps to the earliest
 * deadline (a sleep, a timed wait, the next vsync, the next timer). Time is simulated, not measured, so
 * thread timing on the host can no longer leak into the game.
 *
 * Wait conditions are evaluated by whichever participant is switching away (everyone else is parked), so
 * they may read guest and host state freely, and a woken thread finds its condition still true. */
#include "win32.h"

#include <pthread.h>

extern uint64_t g_vtime_us;   /* core.c */

typedef struct SchedThread {
    int index;                      /* creation order */
    char name[24];
    enum { RUNNABLE, BLOCKED, DEAD } state;
    int (*pred)(void *);            /* blocked: wakes when pred(arg) holds (NULL: only the deadline) */
    void *arg;
    uint64_t deadline;              /* virtual microseconds; UINT64_MAX = none */
    uint64_t (*deadline_fn)(void);  /* or computed each time (the timer thread's next due timer) */
    int timed_out;
    pthread_cond_t cv;
} SchedThread;

#define MAX_SCHED 256
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static SchedThread *g_th[MAX_SCHED];
static int g_n;
static SchedThread *g_cur;
static __thread SchedThread *t_sched;

static uint64_t deadline_of(SchedThread *t) { return t->deadline_fn ? t->deadline_fn() : t->deadline; }

static void dump_and_abort(void) {
    fprintf(stderr, "[sched] deadlock at %.6f s: every participant is blocked with no deadline\n",
            60.0 + g_vtime_us * 1e-6);
    for (int i = 0; i < g_n; i++)
        fprintf(stderr, "  %d %-20s %s\n", i, g_th[i]->name, g_th[i]->state == DEAD ? "dead" : g_th[i]->state == BLOCKED ? "blocked" : "runnable");
    abort();
}

/* The next participant to run after `me` (which may be itself). Called with g_lock held. */
static SchedThread *pick(SchedThread *me) {
    for (;;) {
        for (int k = 1; k <= g_n; k++) {
            SchedThread *t = g_th[(me->index + k) % g_n];
            if (t->state == RUNNABLE) return t;
            if (t->state == BLOCKED && t->pred && t->pred(t->arg)) { t->state = RUNNABLE; t->timed_out = 0; return t; }
            /* the clock can also move while someone runs (preemption): wake a waiter whose deadline passed */
            if (t->state == BLOCKED && deadline_of(t) <= g_vtime_us) { t->state = RUNNABLE; t->timed_out = 1; return t; }
        }
        /* nobody can run: advance the clock to the earliest deadline */
        uint64_t best = UINT64_MAX;
        for (int i = 0; i < g_n; i++)
            if (g_th[i]->state == BLOCKED) { uint64_t d = deadline_of(g_th[i]); if (d < best) best = d; }
        if (best == UINT64_MAX) dump_and_abort();
        if (best > g_vtime_us) g_vtime_us = best;
        for (int k = 1; k <= g_n; k++) {
            SchedThread *t = g_th[(me->index + k) % g_n];
            if (t->state == BLOCKED && deadline_of(t) <= g_vtime_us) { t->state = RUNNABLE; t->timed_out = 1; return t; }
        }
    }
}

/* Hands the baton on and (unless `me` is done) waits for it to come back. Called with g_lock held. */
static void switch_from(SchedThread *me) {
    SchedThread *next = pick(me);
    if (next == me) return;
    g_cur = next;
    pthread_cond_signal(&next->cv);
    if (me->state == DEAD) return;
    while (g_cur != me) pthread_cond_wait(&me->cv, &g_lock);
}

static SchedThread *new_record(const char *name) {
    SchedThread *t = calloc(1, sizeof *t);
    snprintf(t->name, sizeof t->name, "%s", name);
    pthread_cond_init(&t->cv, NULL);
    t->deadline = UINT64_MAX;
    pthread_mutex_lock(&g_lock);
    if (g_n >= MAX_SCHED) { fprintf(stderr, "[sched] too many threads\n"); abort(); }
    t->index = g_n;
    g_th[g_n++] = t;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void sched_main_init(void) {
    if (!deterministic()) return;
    t_sched = new_record("game");
    g_cur = t_sched;
}

/* Called by the creating participant, so records are made in a deterministic order. A record created
   suspended waits for start(arg) before it first runs. */
void *sched_new(const char *name, int (*start)(void *), void *arg) {
    if (!deterministic()) return NULL;
    SchedThread *t = new_record(name);
    if (start) { t->state = BLOCKED; t->pred = start; t->arg = arg; }
    return t;
}

/* First thing a new host thread does: wait for its turn. */
void sched_enter(void *rec) {
    if (!rec) return;
    SchedThread *t = rec;
    t_sched = t;
    pthread_mutex_lock(&g_lock);
    while (g_cur != t) pthread_cond_wait(&t->cv, &g_lock);
    pthread_mutex_unlock(&g_lock);
}

void sched_exit(void) {
    if (!t_sched) return;
    pthread_mutex_lock(&g_lock);
    t_sched->state = DEAD;
    switch_from(t_sched);
    pthread_mutex_unlock(&g_lock);
    t_sched = NULL;
}

void sched_yield_det(void) {
    if (!t_sched) return;
    pthread_mutex_lock(&g_lock);
    switch_from(t_sched);
    pthread_mutex_unlock(&g_lock);
}

/* Blocks until pred(arg) holds (returns 1) or the virtual clock reaches the deadline (returns 0). */
int sched_wait(int (*pred)(void *), void *arg, uint64_t deadline_us, uint64_t (*deadline_fn)(void)) {
    SchedThread *me = t_sched;
    if (!me) return 1;
    pthread_mutex_lock(&g_lock);
    if (pred && pred(arg)) { pthread_mutex_unlock(&g_lock); return 1; }
    uint64_t d = deadline_fn ? deadline_fn() : deadline_us;
    if (d <= g_vtime_us) { pthread_mutex_unlock(&g_lock); return 0; }
    me->state = BLOCKED; me->pred = pred; me->arg = arg; me->deadline = deadline_us; me->deadline_fn = deadline_fn;
    switch_from(me);
    me->state = RUNNABLE; me->pred = NULL; me->deadline_fn = NULL; me->deadline = UINT64_MAX;
    int ok = !me->timed_out;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int sched_active(void) { return t_sched != NULL; }
uint64_t sched_now_us(void) { return __atomic_load_n(&g_vtime_us, __ATOMIC_RELAXED); }
