/* Direct3D 8 on the host: object model + full device state tracking. Rendering is delegated to a
 * backend (render.h); the headless backend only counts. */
#ifndef PORT_D3D8_H
#define PORT_D3D8_H

#include "com.h"

/* D3DFORMAT values the game is likely to use */
enum {
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_A8 = 28, FMT_P8 = 41, FMT_L8 = 50, FMT_A8L8 = 51, FMT_V8U8 = 60,
    FMT_D32 = 71, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D16 = 80, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_DXT1 = 0x31545844, FMT_DXT2 = 0x32545844, FMT_DXT3 = 0x33545844, FMT_DXT4 = 0x34545844,
    FMT_DXT5 = 0x35545844,
};

enum ResType { RT_SURFACE = 1, RT_VOLUME, RT_TEXTURE, RT_VOLUMETEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER,
               RT_INDEXBUFFER, RT_DEVICE = 100, RT_D3D, RT_SWAPCHAIN };

typedef struct Texture {
    int type;                     /* RT_TEXTURE or RT_CUBETEXTURE */
    uint32_t format, usage, pool;
    uint32_t width, height, levels, faces;
    uint32_t mem[6][14];          /* guest memory per face/level */
    uint32_t pitch[14], lw[14], lh[14], size[14];
    uint32_t surfaces[6][14];     /* guest surface objects, created on demand */
    uint32_t version;             /* bumped on every unlock: backends re-upload when it changes */
    uint32_t gpu_id;              /* renderer's name for it (stable, never reused) */
    uint32_t sent_version;        /* version last sent to the renderer */
    void *backend;
} Texture;

typedef struct Surface {
    uint32_t format, width, height, pitch, usage, pool;
    uint32_t mem;                 /* guest memory (0 for render targets / depth without storage) */
    uint32_t container;           /* owning texture object, if any */
    int face, level;
    int is_rt, is_depth;
    uint32_t gpu_id;              /* renderer's name for it (stable, never reused) */
    void *backend;
} Surface;

typedef struct Buffer {
    int index;                    /* index buffer? */
    uint32_t size, usage, pool, fvf_or_format;
    uint32_t mem;
    uint32_t version;
    void *backend;
} Buffer;

typedef struct Shader {
    int pixel;
    uint32_t *decl;  uint32_t decl_len;   /* vertex shaders: declaration tokens */
    uint32_t *code;  uint32_t code_len;   /* NULL for fixed-function declarations */
    void *backend;
} Shader;

#define MAX_TS 512
typedef struct DevState {
    uint32_t rs[256];
    uint32_t tss[8][32];
    float transform[MAX_TS][16];
    struct { uint32_t x, y, w, h; float minz, maxz; } viewport;
    float material[17];
    uint8_t light[8][104];
    uint32_t light_enabled[8];
    float clip_plane[6][4];
    uint32_t texture[8];          /* guest texture objects */
    uint32_t stream_vb[16], stream_stride[16];
    uint32_t ib, base_vertex;
    uint32_t vs, ps;              /* vertex shader handle or FVF; pixel shader handle */
    float vs_const[96][4];
    float ps_const[8][4];
    uint32_t render_target, depth_stencil;
    uint32_t palette_current;
} DevState;

typedef struct Device {
    DevState st;
    uint32_t width, height, backbuffer_format, depth_format, windowed, hwnd;
    uint32_t backbuffer, depth;   /* guest surface objects */
    uint32_t d3d;                 /* owning IDirect3D8 */
    uint32_t frame;
    uint32_t draws_this_frame;
    int recording_block;
} Device;

Shader *shader_get(uint32_t handle);
uint32_t gpu_new_id(void);
uint32_t format_bpp(uint32_t fmt);   /* bits per pixel; DXT1 = 4, DXT2-5 = 8 */
int format_is_dxt(uint32_t fmt);

#endif
