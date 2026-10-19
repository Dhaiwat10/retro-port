/* Rendering backend interface used by the Direct3D 8 layer. */
#ifndef PORT_RENDER_H
#define PORT_RENDER_H

#include "d3d8.h"

typedef struct RenderBackend {
    const char *name;
    int (*init)(Device *d);
    void (*clear)(Device *d, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color, float z,
                  uint32_t stencil);
    /* prim: D3DPRIMITIVETYPE; for indexed draws `indices` points at the index data (host pointer) */
    void (*draw)(Device *d, uint32_t prim, uint32_t prim_count, uint32_t start_vertex, uint32_t num_vertices,
                 const uint8_t *vertices_up, uint32_t up_stride,
                 const void *indices, int index32, uint32_t start_index);
    void (*present)(Device *d);
    void (*destroy_texture)(Texture *t);
    /* render targets: copy GPU contents into the surface's guest memory before the CPU reads it, and
       push CPU writes back to the GPU */
    void (*readback)(Device *d, Surface *s);
    void (*upload_surface)(Device *d, Surface *s);
    void (*destroy_surface)(Surface *s);
} RenderBackend;

extern RenderBackend *g_render;
extern RenderBackend g_render_headless, g_render_gpu;   /* g_render_gpu: render_rec.c */

#endif
