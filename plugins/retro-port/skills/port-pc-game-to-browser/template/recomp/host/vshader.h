/* Direct3D 8 vertex shaders on the CPU (see vshader.c). */
#ifndef PORT_VSHADER_H
#define PORT_VSHADER_H

#include "d3d8.h"

typedef struct {
    const uint8_t *data[16];   /* per stream: host pointer to vertex 0 */
    uint32_t stride[16];
    const float *ff_mvp;       /* world*view*projection, for declaration-only (fixed-function) shaders */
} VsStreams;

typedef struct { float pos[4], d0[4], d1[4], tex[4][4]; } VsOut;

void vs_run(Shader *sh, const float consts[96][4], const VsStreams *st, uint32_t index, VsOut *out);
int vs_is_fixed_function(Shader *sh);

#endif
