/* Render recorder: the game-thread half of the renderer (see render_cmd.h).
 *
 * Implements the Direct3D backend interface by doing all CPU work here - expanding primitives to
 * lists, running vertex shaders, converting changed textures to RGBA, building uniforms and pipeline
 * keys - and emitting self-contained commands for the executor (render_wgpu.c). Natively every
 * command runs immediately; in the browser commands are queued per frame for the page's thread. */
#include "render.h"
#include "render_cmd.h"
#include "vshader.h"

#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

int texture_to_rgba8(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, uint8_t *dst,
                     const uint8_t *palette);
extern uint8_t g_palettes[][1024];

/* WGPUPrimitiveTopology values (kept numeric so this file doesn't need webgpu.h) */
enum { TOPO_POINTS = 1, TOPO_LINES = 2, TOPO_TRIANGLES = 4 };

/* ------------------------------------------------------------------ command buffer
 * One recorder for the whole process: Direct3D calls come from the game thread, but objects can be
 * released from others, so every entry point holds g_rec_lock (recursive). */

#include <pthread.h>
static pthread_mutex_t g_rec_lock;
static pthread_once_t g_rec_once = PTHREAD_ONCE_INIT;
static void rec_lock_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_rec_lock, &a);
}
static void rec_enter(void) { pthread_once(&g_rec_once, rec_lock_init); pthread_mutex_lock(&g_rec_lock); }
static void rec_leave(void) { pthread_mutex_unlock(&g_rec_lock); }

static uint8_t *g_cb;
static size_t g_cb_len, g_cb_cap;

static void *cmd_alloc(uint32_t type, size_t size) {
    size = (size + 7) & ~(size_t)7;
    if (g_cb_len + size > g_cb_cap) {
        g_cb_cap = (g_cb_len + size) * 2 + (1u << 20);
        g_cb = realloc(g_cb, g_cb_cap);
    }
    CmdHdr *h = (CmdHdr *)(g_cb + g_cb_len);
    memset(h, 0, size);
    h->type = type;
    h->size = (uint32_t)size;
    g_cb_len += size;
    return h;
}

#ifdef __EMSCRIPTEN__
/* browser: completed command chunks go to the page's thread (web_gpu.c) */
void web_queue_chunk(uint8_t *buf, size_t len, int ends_frame);
int web_queued_frames(void);
int web_gpu_ready(void);

static void cmd_end(void) {}

static void cmd_flush(int ends_frame) {
    if (!g_cb_len && !ends_frame) return;
    web_queue_chunk(g_cb, g_cb_len, ends_frame);   /* takes ownership */
    g_cb = NULL; g_cb_len = g_cb_cap = 0;
}
#else
/* native: run each command as soon as it is recorded */
static void cmd_end(void) {
    size_t off = 0;
    while (off < g_cb_len) {
        const CmdHdr *h = (const CmdHdr *)(g_cb + off);
        gpu_exec(h);
        off += h->size;
    }
    g_cb_len = 0;
}
static void cmd_flush(int ends_frame) { (void)ends_frame; }
#endif

/* ------------------------------------------------------------------ debugging aids
 * PORT_SCREENSHOTS="120,600" saves /tmp/port_frame_N.png; `touch /tmp/port_snap` or PORT_CAPTURE=N logs
 * every draw of a frame and saves the back buffer after each one (native only). */

static int g_snap_detail;
static uint32_t g_f_drawn;
static uint32_t g_trace_vs;

static int want_screenshot(uint32_t frame) {
    static uint32_t last_check;
    static int cap_frame = -1;
    if (cap_frame < 0) {
        cap_frame = getenv("PORT_CAPTURE") ? atoi(getenv("PORT_CAPTURE")) : 0;
        g_trace_vs = getenv("PORT_TRACE_VS") ? (uint32_t)strtoul(getenv("PORT_TRACE_VS"), NULL, 16) : 0;
    }
    if (cap_frame && frame + 1 == (uint32_t)cap_frame) { g_snap_detail = 1; return 0; }
    if (frame - last_check >= 15) {
        last_check = frame;
        if (!access("/tmp/port_snap", F_OK)) {
            unlink("/tmp/port_snap");
            g_snap_detail = 1;
            return 0;
        }
    }
    if (g_snap_detail) {
        g_snap_detail = 0;
        fprintf(stderr, "[snap] frame %u: %u draws\n", frame, g_f_drawn);
        return 1;
    }
    static const char *list = (const char *)1;
    if (list == (const char *)1) list = getenv("PORT_SCREENSHOTS");
    if (!list) return 0;
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%u", frame);
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        if (n == strlen(tmp) && !strncmp(p, tmp, n)) return 1;
        p += n + (p[n] == ',');
    }
    return 0;
}

/* ------------------------------------------------------------------ render target tracking */

static CmdTarget g_last_target;   /* zero: back buffer, not yet sent */
static int g_target_sent;
static uint32_t g_tgt_lw, g_tgt_lh;

static void select_target(Device *d) {
    const DevState *st = &d->st;
    Surface *rt = st->render_target && st->render_target != d->backbuffer ? com_data(st->render_target) : NULL;
    Surface *ds = st->depth_stencil && st->depth_stencil != d->depth ? com_data(st->depth_stencil) : NULL;
    CmdTarget t;
    memset(&t, 0, sizeof t);
    if (rt) { t.color_id = rt->gpu_id; t.color_w = rt->width; t.color_h = rt->height; t.lw = rt->width; t.lh = rt->height; }
    else { t.lw = d->width; t.lh = d->height; }
    if (ds) { t.depth_id = ds->gpu_id; t.depth_w = ds->width; t.depth_h = ds->height; }
    g_tgt_lw = t.lw; g_tgt_lh = t.lh;
    if (g_target_sent && !memcmp(&t.color_id, &g_last_target.color_id, sizeof t - sizeof t.h)) return;
    if (g_snap_detail)
        fprintf(stderr, "[target] color %u (%ux%u, logical %ux%u) depth %u\n", t.color_id, t.color_w, t.color_h, t.lw, t.lh,
                t.depth_id);
    CmdTarget *c = cmd_alloc(CMD_TARGET, sizeof *c);
    memcpy(&c->color_id, &t.color_id, sizeof t - sizeof t.h);
    cmd_end();
    g_last_target = t;
    g_target_sent = 1;
}

/* ------------------------------------------------------------------ vertices */

typedef struct { uint32_t stride, pos_type, has_normal, has_psize, has_diffuse, has_specular, ntex,
                 off_normal, off_diffuse, off_specular, off_tex[8]; } FvfLayout;

static void fvf_layout(uint32_t fvf, FvfLayout *L) {
    memset(L, 0, sizeof *L);
    uint32_t off = 0;
    L->pos_type = fvf & 0xE;
    switch (L->pos_type) {
    case 0x2: off = 12; break;              /* XYZ */
    case 0x4: off = 16; break;              /* XYZRHW */
    case 0x6: off = 16; break; case 0x8: off = 20; break; case 0xA: off = 24; break;   /* XYZB1-3 */
    case 0xC: off = 28; break; case 0xE: off = 32; break;
    }
    if (fvf & 0x10) { L->has_normal = 1; L->off_normal = off; off += 12; }
    if (fvf & 0x20) { L->has_psize = 1; off += 4; }
    if (fvf & 0x40) { L->has_diffuse = 1; L->off_diffuse = off; off += 4; }
    if (fvf & 0x80) { L->has_specular = 1; L->off_specular = off; off += 4; }
    L->ntex = (fvf >> 8) & 0xF;
    for (uint32_t i = 0; i < L->ntex && i < 8; i++) {
        L->off_tex[i] = off;
        uint32_t f = (fvf >> (16 + 2 * i)) & 3;
        off += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
    }
    L->stride = off;
}

static void convert_vertex(const uint8_t *v, const FvfLayout *L, CVert *o) {
    float f[4] = { 0, 0, 0, 1 };
    memcpy(f, v, L->pos_type == 0x4 ? 16 : 12);
    o->x = f[0]; o->y = f[1]; o->z = f[2]; o->w = L->pos_type == 0x4 ? f[3] : 1.f;
    o->diffuse = 0xFFFFFFFFu; o->specular = 0;
    if (L->has_diffuse) memcpy(&o->diffuse, v + L->off_diffuse, 4);
    if (L->has_specular) memcpy(&o->specular, v + L->off_specular, 4);
    memset(o->tc, 0, sizeof o->tc);
    for (uint32_t t = 0; t < 4; t++) {
        o->tc[t][3] = 1.f;
        if (t < L->ntex) memcpy(o->tc[t], v + L->off_tex[t], 8);
    }
}

static void mat_mul(const float *a, const float *b, float *r) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
}

static uint32_t pack_color(const float *f) {   /* RGBA floats -> D3DCOLOR (ARGB) */
    uint32_t b[4];
    for (int i = 0; i < 4; i++) { float v = f[i] < 0 ? 0 : f[i] > 1 ? 1 : f[i]; b[i] = (uint32_t)(v * 255.f + 0.5f); }
    return b[2] | b[1] << 8 | b[0] << 16 | b[3] << 24;
}

static void unpack_color(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 255) / 255.f; o[1] = ((c >> 8) & 255) / 255.f; o[2] = (c & 255) / 255.f; o[3] = (c >> 24) / 255.f;
}

/* ------------------------------------------------------------------ textures */

static uint32_t sampler_key(const uint32_t *tss) {
    uint32_t mag = tss[16] >= 2, min = tss[17] >= 2, mip = tss[18];
    uint32_t au = tss[13] & 7, av = tss[14] & 7;
    return mag | min << 1 | (mip & 3) << 2 | au << 4 | av << 7;   /* < 1024 */
}

/* send a texture's contents if they changed since the last upload */
/* DXT textures go to the GPU as BC blocks when it can sample them: 4-8x less GPU memory than RGBA8 (a match
   needs hundreds of MB uncompressed, more than some GPUs/browsers give a page). WebGPU wants whole blocks,
   so the base level must be a multiple of 4; other textures are converted. */
static uint32_t bc_format(const Texture *t) {
    if (!g_gpu_bc || !format_is_dxt(t->format) || t->lw[0] % 4 || t->lh[0] % 4) return TEXFMT_RGBA8;
    return t->format == FMT_DXT1 ? TEXFMT_BC1 : t->format <= FMT_DXT3 ? TEXFMT_BC2 : TEXFMT_BC3;
}

static void sync_texture(Device *d, Texture *t) {
    if (t->sent_version == t->version) return;
    uint32_t fmt = bc_format(t), bs = fmt == TEXFMT_BC1 ? 8 : 16;
    size_t total = 0;
    for (uint32_t l = 0; l < t->levels && l < 14; l++)
        total += fmt ? (size_t)((t->lw[l] + 3) / 4) * ((t->lh[l] + 3) / 4) * bs : (size_t)t->lw[l] * t->lh[l] * 4;
    CmdUploadTex *c = cmd_alloc(CMD_UPLOAD_TEX, sizeof *c + total);
    c->id = t->gpu_id;
    c->levels = t->levels < 14 ? t->levels : 14;
    c->fmt = fmt;
    if (fmt) {
        uint8_t *out = (uint8_t *)(c + 1);
        for (uint32_t l = 0; l < c->levels; l++) {
            uint32_t bw = (t->lw[l] + 3) / 4, bh = (t->lh[l] + 3) / 4;
            c->w[l] = t->lw[l]; c->h_[l] = t->lh[l];
            for (uint32_t by = 0; by < bh; by++) memcpy(out + (size_t)by * bw * bs, HP(t->mem[0][l] + by * t->pitch[l]), (size_t)bw * bs);
            out += (size_t)bw * bh * bs;
        }
        cmd_end();
        t->sent_version = t->version;
        return;
    }
    uint8_t *out = (uint8_t *)(c + 1);
    const uint8_t *pal = d->st.palette_current < 1024 ? g_palettes[d->st.palette_current] : NULL;
    for (uint32_t l = 0; l < c->levels; l++) {
        uint32_t w = t->lw[l], h = t->lh[l];
        c->w[l] = w; c->h_[l] = h;
        if (!texture_to_rgba8(t->format, HP(t->mem[0][l]), t->pitch[l], w, h, out, pal)) {
            static int warned;
            if (!warned++) fprintf(stderr, "[render] unsupported texture format %x\n", t->format);
            memset(out, 255, (size_t)w * h * 4);
        }
        out += (size_t)w * h * 4;
    }
    cmd_end();
    t->sent_version = t->version;
}

/* ------------------------------------------------------------------ clears */

static void clear_quad(Device *d, float x1, float y1, float x2, float y2, uint32_t flags, uint32_t color, float z) {
    if (x2 <= x1 || y2 <= y1 || !(flags & 3)) return;
    CmdDraw *c = cmd_alloc(CMD_DRAW, sizeof *c + 6 * sizeof(CVert));
    c->key = (PKey){ 0, 2, 1, 1, 1, (flags & 2) ? 1u : 0u, 8, 1, (flags & 1) ? 0xFu : 0u, TOPO_TRIANGLES, 0 };
    c->full_viewport = 1;
    c->minz = 0.f; c->maxz = 1.f;
    c->capture_index = -1;
    for (int i = 0; i < 4; i++) c->samp_key[i] = sampler_key(d->st.tss[i]);
    c->u.vp[2] = (float)g_tgt_lw; c->u.vp[3] = (float)g_tgt_lh;
    c->u.flags[0] = 1;   /* pre-transformed */
    c->u.st0[0] = 1;     /* stage disabled: output = diffuse */
    c->nverts = 6;
    CVert *v = (CVert *)(c + 1);
    const float px[6] = { x1, x2, x1, x1, x2, x2 }, py[6] = { y1, y1, y2, y2, y1, y2 };
    for (int i = 0; i < 6; i++) { v[i].x = px[i] - 0.5f; v[i].y = py[i] - 0.5f; v[i].z = z; v[i].w = 1.f; v[i].diffuse = color; }
    cmd_end();
}

static void rec_clear_impl(Device *d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z,
                      uint32_t stencil) {
    select_target(d);
    const DevState *st = &d->st;
    float vx1 = (float)st->viewport.x, vy1 = (float)st->viewport.y;
    float vx2 = vx1 + (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    float vy2 = vy1 + (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    int full = !count && vx1 <= 0 && vy1 <= 0 && vx2 >= (float)g_tgt_lw && vy2 >= (float)g_tgt_lh;
    if (full) {
        CmdClear *c = cmd_alloc(CMD_CLEAR, sizeof *c);
        c->flags = flags & 7; c->color = color; c->z = z; c->stencil = stencil;
        cmd_end();
        return;
    }
    /* Direct3D clears only the viewport (or the given rects): draw quads instead of a pass clear */
    for (uint32_t i = 0; i < (count ? count : 1); i++) {
        float x1 = vx1, y1 = vy1, x2 = vx2, y2 = vy2;
        if (count && rects) {
            int32_t r[4];
            memcpy(r, HP(rects + 16 * i), 16);
            if (r[0] > x1) x1 = (float)r[0];
            if (r[1] > y1) y1 = (float)r[1];
            if (r[2] < x2) x2 = (float)r[2];
            if (r[3] < y2) y2 = (float)r[3];
        }
        clear_quad(d, x1, y1, x2, y2, flags, color, z);
    }
}

/* ------------------------------------------------------------------ draw */

static void rec_draw_impl(Device *d, uint32_t prim, uint32_t prim_count, uint32_t start_vertex, uint32_t num_vertices,
                     const uint8_t *vertices_up, uint32_t up_stride, const void *indices, int index32,
                     uint32_t start_index) {
    DevState *st = &d->st;
    (void)num_vertices;
    if (!prim_count) return;
    Shader *sh = (st->vs & 1) ? shader_get(st->vs) : NULL;   /* vertex shader handle (FVFs have bit 0 clear) */
    if ((st->vs & 1) && !sh) return;
    g_f_drawn++;
    select_target(d);
    if (g_snap_detail) {
        Texture *t0 = com_data(st->texture[0]), *t1 = com_data(st->texture[1]);
        fprintf(stderr, "[draw] #%04u vs %x ps %x prim %u n %u | tex0 %s fmt %x tex1 %s fmt %x | tss0 op %u a %u,%u aop %u | "
                "tss1 op %u | blend %u %u/%u atest %u\n", g_f_drawn - 1, st->vs, st->ps, prim, prim_count,
                t0 ? (t0->type == RT_TEXTURE ? "2d" : "cube") : "-", t0 ? t0->format : 0,
                t1 ? (t1->type == RT_TEXTURE ? "2d" : "cube") : "-", t1 ? t1->format : 0,
                st->tss[0][1], st->tss[0][2], st->tss[0][3], st->tss[0][4], st->tss[1][1], st->rs[27], st->rs[19],
                st->rs[20], st->rs[15]);
    }

    FvfLayout L;
    fvf_layout(sh ? 0x2 /* XYZ: transformed by the shader */ : st->vs, &L);
    const uint8_t *vb = NULL;
    uint32_t stride;
    if (vertices_up) { vb = vertices_up; stride = up_stride; }
    else {
        Buffer *b = com_data(st->stream_vb[0]);
        if (!b && !sh) return;
        if (b) vb = HP(b->mem);
        stride = st->stream_stride[0];
    }
    if (!stride && !sh) stride = L.stride;
    uint32_t base = indices && !vertices_up ? st->base_vertex : 0;

    /* expand to a list topology */
    uint32_t topo, n = 0;
    uint32_t cap = prim_count * 3 + 4;
    uint32_t *idx = malloc(4 * cap);
#define IDX(i) (indices ? (index32 ? ((const uint32_t *)indices)[start_index + (i)] : ((const uint16_t *)indices)[start_index + (i)]) + base : start_vertex + (i))
    switch (prim) {
    case 1: topo = TOPO_POINTS; for (uint32_t i = 0; i < prim_count; i++) idx[n++] = IDX(i); break;
    case 2: topo = TOPO_LINES; for (uint32_t i = 0; i < 2 * prim_count; i++) idx[n++] = IDX(i); break;
    case 3: topo = TOPO_LINES;
        for (uint32_t i = 0; i < prim_count; i++) { idx[n++] = IDX(i); idx[n++] = IDX(i + 1); } break;
    case 4: topo = TOPO_TRIANGLES; for (uint32_t i = 0; i < 3 * prim_count; i++) idx[n++] = IDX(i); break;
    case 5: topo = TOPO_TRIANGLES;
        for (uint32_t i = 0; i < prim_count; i++) {
            if (i & 1) { idx[n++] = IDX(i + 1); idx[n++] = IDX(i); idx[n++] = IDX(i + 2); }
            else { idx[n++] = IDX(i); idx[n++] = IDX(i + 1); idx[n++] = IDX(i + 2); }
        }
        break;
    case 6: topo = TOPO_TRIANGLES;
        for (uint32_t i = 0; i < prim_count; i++) { idx[n++] = IDX(0); idx[n++] = IDX(i + 1); idx[n++] = IDX(i + 2); }
        break;
    default: free(idx); return;
    }
#undef IDX

    /* textures first: an upload is its own command and must precede the draw */
    for (int s = 0; s < 4; s++) {
        Texture *t = com_data(st->texture[s]);
        if (t && t->type == RT_TEXTURE && !(t->usage & 1)) sync_texture(d, t);
    }

    CmdDraw *c = cmd_alloc(CMD_DRAW, sizeof *c + (size_t)n * sizeof(CVert));
    CVert *cv = (CVert *)(c + 1);
    c->nverts = n;
    c->capture_index = g_snap_detail ? (int32_t)(g_f_drawn - 1) : -1;
    if (sh) {
        VsStreams vss;
        memset(&vss, 0, sizeof vss);
        if (vertices_up) { vss.data[0] = vertices_up; vss.stride[0] = up_stride; }
        else
            for (int s = 0; s < 16; s++) {
                Buffer *sb = com_data(st->stream_vb[s]);
                if (sb) { vss.data[s] = HP(sb->mem); vss.stride[s] = st->stream_stride[s]; }
            }
        float wv[16], ff_mvp[16];
        mat_mul(st->transform[256], st->transform[2], wv);
        mat_mul(wv, st->transform[3], ff_mvp);
        vss.ff_mvp = ff_mvp;
        /* run the shader once per distinct vertex: over the referenced range when it is compact */
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i < n; i++) { if (idx[i] < lo) lo = idx[i]; if (idx[i] > hi) hi = idx[i]; }
        uint32_t range = n ? hi - lo + 1 : 0;
        int use_range = range <= 2 * n + 64;
        CVert *tmp = use_range ? malloc(sizeof(CVert) * (range ? range : 1)) : NULL;
        for (uint32_t i = 0; i < (use_range ? range : n); i++) {
            VsOut o;
            extern int vs_trace;
            vs_trace = g_snap_detail && i == 0 && st->vs == g_trace_vs;
            vs_run(sh, (const float (*)[4])st->vs_const, &vss, use_range ? lo + i : idx[i], &o);
            vs_trace = 0;
            CVert *v = use_range ? &tmp[i] : &cv[i];
            v->x = o.pos[0]; v->y = o.pos[1]; v->z = o.pos[2]; v->w = o.pos[3];
            v->diffuse = pack_color(o.d0); v->specular = pack_color(o.d1);
            memcpy(v->tc, o.tex, sizeof v->tc);
        }
        if (use_range) { for (uint32_t i = 0; i < n; i++) cv[i] = tmp[idx[i] - lo]; free(tmp); }
    } else {
        for (uint32_t i = 0; i < n; i++) convert_vertex(vb + (size_t)idx[i] * stride, &L, &cv[i]);
    }
    free(idx);

    for (int s = 0; s < 4; s++) {
        Texture *t = com_data(st->texture[s]);
        c->samp_key[s] = sampler_key(st->tss[s]);
        if (t && t->type == RT_TEXTURE && (t->usage & 1)) {
            Surface *s0 = t->surfaces[0][0] ? com_data(t->surfaces[0][0]) : NULL;
            if (s0) { c->slot_kind[s] = TS_RTSURF; c->slot_id[s] = s0->gpu_id; }
        } else if (t && t->type == RT_TEXTURE) {
            c->slot_kind[s] = TS_TEX; c->slot_id[s] = t->gpu_id;
        }
    }
    c->pixel_shader = st->ps;

    Uniforms *u = &c->u;
    if (!sh && L.pos_type != 0x4) {
        float wv[16];
        mat_mul(st->transform[256], st->transform[2], wv);    /* WORLD * VIEW */
        mat_mul(wv, st->transform[3], u->mvp);                /* * PROJECTION */
    }
    u->vp[0] = (float)st->viewport.x; u->vp[1] = (float)st->viewport.y;
    u->vp[2] = (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    u->vp[3] = (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    u->flags[0] = sh ? 2 : L.pos_type == 0x4;
    u->flags[1] = st->rs[15] ? st->rs[25] : 0;                 /* ALPHATESTENABLE ? ALPHAFUNC */
    u->flags[2] = st->rs[29] != 0;                             /* SPECULARENABLE */
    u->flags[3] = !sh && L.pos_type != 0x4 && st->rs[137] && !L.has_normal;  /* LIGHTING without normals: ambient */
    for (int s = 0; s < 2; s++) {
        const uint32_t *t = st->tss[s];
        uint32_t *a = s ? u->st1 : u->st0, *b = s ? u->st1b : u->st0b;
        a[0] = t[1]; a[1] = t[2]; a[2] = t[3]; a[3] = t[4];
        b[0] = t[5]; b[1] = t[6]; b[2] = sh ? (uint32_t)s : t[11] & 7; b[3] = (t[26] & 255) | (t[27] & 255) << 8;
    }
    unpack_color(st->rs[60], u->tfactor);
    u->misc[0] = (st->rs[24] & 255) / 255.f;
    for (int i = 0; i < 4; i++) u->mat_diffuse[i] = 1.f;
    if (u->flags[3]) {
        /* no normals: lit color is emissive + ambient * material ambient (diffuse lighting needs a normal) */
        float amb[4];
        unpack_color(st->rs[139], amb);
        for (int i = 0; i < 3; i++) { u->mat_diffuse[i] = 0.f; u->ambient[i] = amb[i] * st->material[8 + i] + st->material[12 + i]; }
        u->mat_diffuse[3] = 1.f;
        if (st->rs[141] && L.has_diffuse && st->rs[147] == 1) {  /* COLORVERTEX + AMBIENTMATERIALSOURCE=COLOR1 */
            for (int i = 0; i < 3; i++) { u->mat_diffuse[i] = amb[i]; u->ambient[i] = st->material[12 + i]; }
        }
    }
    memcpy(u->psc, st->ps_const, sizeof u->psc);
    for (int s = 0; s < 4; s++) {
        memcpy(u->bem[s], &st->tss[s][7], 16);        /* BUMPENVMAT00, 01, 10, 11 (float bits) */
        memcpy(&u->bel[s][0], &st->tss[s][22], 4);    /* BUMPENVLSCALE */
        memcpy(&u->bel[s][1], &st->tss[s][23], 4);    /* BUMPENVLOFFSET */
    }

    PKey k = { st->rs[27] != 0, st->rs[19], st->rs[20], st->rs[171] ? st->rs[171] : 1, st->rs[7] != 0 && st->depth_stencil,
               st->rs[14], st->rs[23], st->rs[22], st->rs[168] & 0xF, topo, st->ps };
    if (L.pos_type == 0x4) k.cull = 1;  /* keep 2D simple: no culling for pre-transformed vertices */
    else if (k.cull == 2) k.cull = 3; else if (k.cull == 3) k.cull = 2;  /* D3D CW/CCW culls back/front with CW front */
    c->key = k;
    c->vp[0] = (float)st->viewport.x; c->vp[1] = (float)st->viewport.y;
    c->vp[2] = (float)(st->viewport.w ? st->viewport.w : g_tgt_lw);
    c->vp[3] = (float)(st->viewport.h ? st->viewport.h : g_tgt_lh);
    c->minz = st->viewport.minz;
    c->maxz = st->viewport.maxz > st->viewport.minz ? st->viewport.maxz : 1.f;
    cmd_end();
}

/* ------------------------------------------------------------------ frames, readback, resources */

static int rec_init(Device *d) {
    (void)d;
#ifdef __EMSCRIPTEN__
    while (!web_gpu_ready()) usleep(1000);   /* the page creates the WebGPU device before starting the game */
    return 1;
#else
    return gpu_init_native();
#endif
}

static void rec_present_impl(Device *d) {
    CmdPresent *c = cmd_alloc(CMD_PRESENT, sizeof *c);
    c->frame = d->frame;
    c->screenshot = (uint32_t)want_screenshot(d->frame);
    cmd_end();
    cmd_flush(1);
    g_target_sent = 0;   /* the executor ends every frame on the back buffer */
    g_f_drawn = 0;
#ifdef __EMSCRIPTEN__
    while (web_queued_frames() >= 2) usleep(1000);   /* don't run ahead of the display */
#endif
}

static void rec_readback_impl(Device *d, Surface *s) {
    int is_bb = com_data(d->backbuffer) == s;
    if (!is_bb && !s->is_rt) return;
    if (!s->mem) s->mem = heap_alloc(s->pitch * s->height + 16, 1);
    volatile int done = 0;
    CmdReadback *c = cmd_alloc(CMD_READBACK, sizeof *c);
    c->id = is_bb ? 0 : s->gpu_id;
    c->w = s->width; c->h_ = s->height; c->fmt = s->format; c->pitch = s->pitch; c->guest_mem = s->mem;
    c->done_ptr = (uint64_t)(uintptr_t)&done;
    cmd_end();
    cmd_flush(0);
    while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) usleep(200);   /* natively it already ran */
}

static void rec_upload_surface_impl(Device *d, Surface *s) {
    if (!s->mem || s->is_depth || com_data(d->backbuffer) == s) return;
    CmdUploadSurf *c = cmd_alloc(CMD_UPLOAD_SURF, sizeof *c + (size_t)s->width * s->height * 4);
    c->id = s->gpu_id; c->w = s->width; c->h_ = s->height;
    if (!texture_to_rgba8(s->format, HP(s->mem), s->pitch, s->width, s->height, (uint8_t *)(c + 1), NULL))
        memset(c + 1, 0, (size_t)s->width * s->height * 4);
    cmd_end();
}

static void rec_destroy_texture_impl(Texture *t) {
    if (t->sent_version == 0xFFFFFFFFu) return;   /* never reached the GPU */
    CmdDestroy *c = cmd_alloc(CMD_DESTROY_TEX, sizeof *c);
    c->id = t->gpu_id;
    cmd_end();
}

static void rec_destroy_surface_impl(Surface *s) {
    CmdDestroy *c = cmd_alloc(CMD_DESTROY_SURF, sizeof *c);
    c->id = s->gpu_id;
    cmd_end();
}

/* ------------------------------------------------------------------ locked entry points */

static void rec_clear(Device *d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    rec_enter(); rec_clear_impl(d, count, rects, flags, color, z, stencil); rec_leave();
}
static void rec_draw(Device *d, uint32_t prim, uint32_t prim_count, uint32_t start_vertex, uint32_t num_vertices,
                     const uint8_t *vertices_up, uint32_t up_stride, const void *indices, int index32, uint32_t start_index) {
    rec_enter();
    rec_draw_impl(d, prim, prim_count, start_vertex, num_vertices, vertices_up, up_stride, indices, index32, start_index);
    rec_leave();
}
static void rec_present(Device *d) { rec_enter(); rec_present_impl(d); rec_leave(); }
static void rec_readback(Device *d, Surface *s) { rec_enter(); rec_readback_impl(d, s); rec_leave(); }
static void rec_upload_surface(Device *d, Surface *s) { rec_enter(); rec_upload_surface_impl(d, s); rec_leave(); }
static void rec_destroy_texture(Texture *t) { rec_enter(); rec_destroy_texture_impl(t); rec_leave(); }
static void rec_destroy_surface(Surface *s) { rec_enter(); rec_destroy_surface_impl(s); rec_leave(); }

RenderBackend g_render_gpu = { "webgpu", rec_init, rec_clear, rec_draw, rec_present, rec_destroy_texture,
                               rec_readback, rec_upload_surface, rec_destroy_surface };
