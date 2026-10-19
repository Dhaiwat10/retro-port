#include "com.h"

#include <pthread.h>

int g_log_d3d;

#define MAX_SLOTS 8192
static struct { ComInterface *it; int idx; } g_slots[MAX_SLOTS];

#define MAX_OBJS 65536
static ComObj g_objs[MAX_OBJS];
static int g_nobjs = 1;
static int g_free_idx[MAX_OBJS], g_nfree;  /* recycled object slots */
static pthread_mutex_t g_com_lock = PTHREAD_MUTEX_INITIALIZER;

static void com_dispatch(CPU *c) {
    uint32_t slot = (g_import_target - IMPORT_BASE) / 16;
    ComInterface *it = slot < MAX_SLOTS ? g_slots[slot].it : NULL;
    if (!it) guest_trap(c, g_import_target, "COM dispatch through an unknown slot");
    const ComMethod *m = &it->methods[g_slots[slot].idx];
    if (g_log_d3d >= 2)
        fprintf(stderr, "[com] %s::%s(this=%08x, %08x %08x %08x %08x)\n", it->name, m->name, ARG(0), ARG(1),
                ARG(2), ARG(3), ARG(4));
    if (m->impl) { m->impl(c); return; }
    static char warned[MAX_SLOTS];
    if (!warned[slot]) {
        warned[slot] = 1;
        fprintf(stderr, "[com] %s::%s not implemented: returning S_OK\n", it->name, m->name);
    }
    c->eax = S_OK;
    c->esp += 4 + 4 * (uint32_t)(m->nargs + 1);
}

static uint32_t build_vtable(ComInterface *it) {
    if (it->vtable) return it->vtable;
    uint32_t vt = heap_alloc(4u * (uint32_t)it->count, 1);
    for (int i = 0; i < it->count; i++) {
        char name[128];
        snprintf(name, sizeof name, "%s::%s", it->name, it->methods[i].name);
        uint32_t magic = add_dynamic_import(name, com_dispatch);
        uint32_t slot = (magic - IMPORT_BASE) / 16;
        g_slots[slot].it = it;
        g_slots[slot].idx = i;
        W32(vt + 4u * (uint32_t)i, magic);
    }
    it->vtable = vt;
    return vt;
}

uint32_t com_new(ComInterface *it, int type, void *data) {
    pthread_mutex_lock(&g_com_lock);
    uint32_t vt = build_vtable(it);
    int idx = g_nfree ? g_free_idx[--g_nfree] : g_nobjs++;
    if (idx >= MAX_OBJS) { fprintf(stderr, "too many live COM objects\n"); abort(); }
    uint32_t g = heap_alloc(16, 1);
    W32(g, vt);
    W32(g + 4, (uint32_t)idx);
    W32(g + 8, 0xC0C0C0C0u);  /* marker so com_get can reject foreign pointers */
    g_objs[idx] = (ComObj){ it, 1, type, data, g };
    pthread_mutex_unlock(&g_com_lock);
    if (g_log_d3d) fprintf(stderr, "[com] new %s %08x\n", it->name, g);
    return g;
}

ComObj *com_get(uint32_t g) {
    if (!g || R32(g + 8) != 0xC0C0C0C0u) return NULL;
    uint32_t idx = R32(g + 4);
    return (idx > 0 && idx < MAX_OBJS && g_objs[idx].guest == g) ? &g_objs[idx] : NULL;
}

void *com_data(uint32_t g) {
    ComObj *o = com_get(g);
    return o ? o->data : NULL;
}

void com_QueryInterface(CPU *c) {
    ComObj *o = com_get(THIS);
    if (ARG(2)) W32(ARG(2), THIS);
    if (o) o->refs++;
    RET(3, S_OK);
}

void com_AddRef(CPU *c) {
    ComObj *o = com_get(THIS);
    RET(1, o ? (uint32_t)++o->refs : 1);
}

void com_Release(CPU *c) {
    ComObj *o = com_get(THIS);
    if (!o) RET(1, 0);
    int32_t r = --o->refs;
    if (r <= 0 && !o->iface->destroy) o->refs = r = 0;  /* singletons (devices, interfaces): keep alive */
    else if (r <= 0) {
        o->refs = r = 0;
        if (o->data) {
            o->iface->destroy(o->data);
            o->data = NULL;
        }
        W32(o->guest + 8, 0);   /* no longer a live object */
        heap_free(o->guest);
        pthread_mutex_lock(&g_com_lock);
        o->guest = 0;
        g_free_idx[g_nfree++] = (int)(o - g_objs);
        pthread_mutex_unlock(&g_com_lock);
    }
    RET(1, (uint32_t)r);
}
