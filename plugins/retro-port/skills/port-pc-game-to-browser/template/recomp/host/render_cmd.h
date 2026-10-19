/* Rendering is split in two (see render_rec.c and render_wgpu.c):
 *   recorder  - runs on the game thread; does all CPU work (primitive expansion, vertex shaders,
 *               texture conversion, uniforms) and emits self-contained commands;
 *   executor  - owns every WebGPU object and executes the commands.
 * Natively each command is executed right away. In the browser, WebGPU must live on the page's
 * thread (which returns to its event loop), so commands are queued per frame and replayed there.
 * Commands never point at game objects: textures and surfaces are referred to by numeric ids. */
#ifndef PORT_RENDER_CMD_H
#define PORT_RENDER_CMD_H

#include <stdint.h>

typedef struct { float x, y, z, w; uint32_t diffuse, specular; float tc[4][4]; } CVert;  /* 88 bytes */

typedef struct {
    float mvp[16];
    float vp[4];
    uint32_t flags[4];
    uint32_t st0[4], st0b[4], st1[4], st1b[4];
    float tfactor[4];
    float misc[4];
    float mat_diffuse[4];
    float ambient[4];
    float psc[8][4];
    float bem[4][4];
    float bel[4][4];
} Uniforms;   /* 416 bytes */

typedef struct PKey { uint32_t blend, src, dst, op, zen, zwrite, zfunc, cull, cmask, topo, ps; } PKey;

enum { CMD_TARGET = 1, CMD_CLEAR, CMD_DRAW, CMD_UPLOAD_TEX, CMD_DESTROY_TEX, CMD_UPLOAD_SURF, CMD_DESTROY_SURF,
       CMD_READBACK, CMD_PRESENT };

typedef struct { uint32_t type, size; } CmdHdr;   /* size includes the header; 8-byte aligned */

/* Render target for the following clears/draws. color_id 0 = the back buffer. depth_id 0 = the
   device's own depth buffer (back buffer) or a scratch one (render targets). */
typedef struct { CmdHdr h; uint32_t color_id, color_w, color_h, depth_id, depth_w, depth_h, lw, lh; } CmdTarget;

typedef struct { CmdHdr h; uint32_t flags, color, stencil; float z; } CmdClear;

/* texture slot kinds */
enum { TS_NONE = 0, TS_TEX, TS_RTSURF };

typedef struct {
    CmdHdr h;
    PKey key;
    float vp[4];              /* Direct3D viewport (target logical pixels) */
    float minz, maxz;
    uint32_t full_viewport;   /* clear quads: ignore vp, cover the whole target */
    uint32_t slot_kind[4], slot_id[4], samp_key[4];
    uint32_t pixel_shader;    /* empty slots read black (pixel shaders) or white (fixed function) */
    int32_t capture_index;    /* debugging: save the back buffer after this draw (native only); -1 none */
    Uniforms u;
    uint32_t nverts;
    /* CVert verts[nverts] follow */
} CmdDraw;

/* RGBA8 levels follow: for each level, w*h*4 bytes */
/* fmt: TEXFMT_RGBA8, or a BC format whose blocks follow as stored (DXT textures, when the GPU samples BC) */
enum { TEXFMT_RGBA8 = 0, TEXFMT_BC1, TEXFMT_BC2, TEXFMT_BC3 };
typedef struct { CmdHdr h; uint32_t id, levels, fmt, w[14], h_[14]; } CmdUploadTex;
extern int g_gpu_bc;   /* the device can sample BC1-3 (set before the game draws) */
typedef struct { CmdHdr h; uint32_t id; } CmdDestroy;
/* level data follows: RGBA8 rows, or BC blocks (rows of (w+3)/4 blocks) */
typedef struct { CmdHdr h; uint32_t id, w, h_; } CmdUploadSurf;

/* Copies a render target (id 0: back buffer, scaled to w x h) into guest memory in format `fmt`,
   then sets *done (a host address) to 1. */
typedef struct { CmdHdr h; uint32_t id, w, h_, fmt, pitch, guest_mem; uint64_t done_ptr; } CmdReadback;

typedef struct { CmdHdr h; uint32_t frame, screenshot; } CmdPresent;

/* executor entry points */
void gpu_exec(const CmdHdr *c);              /* native: run one command now */
int gpu_init_native(void);                   /* native: create device, surface and pipelines */

#endif
