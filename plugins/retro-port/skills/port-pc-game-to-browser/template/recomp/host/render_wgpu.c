/* Render executor: owns every WebGPU object and executes the recorder's commands (render_cmd.h).
 *
 * Natively (wgpu-native, Metal) commands run as they are recorded and frames go to the SDL window.
 * In the browser (Dawn via Emscripten) this runs on the page's thread: the game thread queues command
 * chunks, and the page pumps them, presenting at most one frame per animation frame; a readback
 * pauses the pump until the GPU copy is mapped.
 *
 * One WGSL "uber shader" implements Direct3D 8's fixed-function stages; ps.1.1 pixel shaders are
 * translated to WGSL (pshader.c). Pipelines are cached by render state; textures are RGBA8 by id. */
#include "render.h"
#include "render_cmd.h"

#include <math.h>
#include <webgpu/webgpu.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

extern int g_window_w, g_window_h;
#ifndef __EMSCRIPTEN__
extern void *g_metal_layer;
#endif

/* ------------------------------------------------------------------ shader */

static const char *WGSL_COMMON =
"struct U {\n"
"  mvp: mat4x4<f32>,\n"
"  vp: vec4<f32>,        // D3D viewport x, y, w, h in back-buffer pixels\n"
"  flags: vec4<u32>,     // x: 1 = pre-transformed, 2 = clip space (vertex shader), y: alpha func (0 = test off), z: specular enable, w: lighting\n"
"  st0: vec4<u32>,       // colorop, colorarg1, colorarg2, alphaop\n"
"  st0b: vec4<u32>,      // alphaarg1, alphaarg2, texcoord index, colorarg0 | alphaarg0 << 8\n"
"  st1: vec4<u32>,\n"
"  st1b: vec4<u32>,\n"
"  tfactor: vec4<f32>,\n"
"  misc: vec4<f32>,      // x: alpha ref (0..1)\n"
"  mat_diffuse: vec4<f32>,\n"
"  ambient: vec4<f32>,\n"
"  psc: array<vec4<f32>, 8>,   // pixel shader constants c0-c7\n"
"  bem: array<vec4<f32>, 4>,   // bump-environment matrices per stage (00, 01, 10, 11)\n"
"  bel: array<vec4<f32>, 4>,   // bump luminance scale, offset per stage\n"
"};\n"
"@group(0) @binding(0) var<uniform> u: U;\n"
"@group(1) @binding(0) var tex0: texture_2d<f32>;\n"
"@group(1) @binding(1) var smp0: sampler;\n"
"@group(1) @binding(2) var tex1: texture_2d<f32>;\n"
"@group(1) @binding(3) var smp1: sampler;\n"
"@group(1) @binding(4) var tex2: texture_2d<f32>;\n"
"@group(1) @binding(5) var smp2: sampler;\n"
"@group(1) @binding(6) var tex3: texture_2d<f32>;\n"
"@group(1) @binding(7) var smp3: sampler;\n"
"struct VI { @location(0) pos: vec4<f32>, @location(1) diffuse: u32, @location(2) specular: u32,\n"
"            @location(3) tc0: vec4<f32>, @location(4) tc1: vec4<f32>, @location(5) tc2: vec4<f32>, @location(6) tc3: vec4<f32> };\n"
"struct VO { @builtin(position) pos: vec4<f32>, @location(0) diffuse: vec4<f32>, @location(1) specular: vec4<f32>,\n"
"            @location(2) tc0: vec4<f32>, @location(3) tc1: vec4<f32>, @location(4) tc2: vec4<f32>, @location(5) tc3: vec4<f32> };\n"
"fn d3dcolor(c: u32) -> vec4<f32> { return unpack4x8unorm(c).zyxw; }\n"
"@vertex fn vs(v: VI) -> VO {\n"
"  var o: VO;\n"
"  if (u.flags.x == 1u) {\n"
"    let w = select(1.0 / v.pos.w, 1.0, v.pos.w == 0.0);\n"
"    let nx = (v.pos.x + 0.5 - u.vp.x) / u.vp.z * 2.0 - 1.0;\n"
"    let ny = 1.0 - (v.pos.y + 0.5 - u.vp.y) / u.vp.w * 2.0;\n"
"    o.pos = vec4<f32>(nx * w, ny * w, v.pos.z * w, w);\n"
"  } else {\n"
"    var p = u.mvp * vec4<f32>(v.pos.xyz, 1.0);\n"
"    if (u.flags.x == 2u) { p = v.pos; }   // vertex shader output: already in clip space\n"
"    p.x = p.x + p.w / u.vp.z;\n"
"    p.y = p.y - p.w / u.vp.w;\n"
"    o.pos = p;\n"
"  }\n"
"  o.diffuse = d3dcolor(v.diffuse);\n"
"  if (u.flags.w == 1u) { o.diffuse = clamp(o.diffuse * u.mat_diffuse + u.ambient, vec4<f32>(0.0), vec4<f32>(1.0)); }\n"
"  o.specular = d3dcolor(v.specular);\n"
"  o.tc0 = v.tc0; o.tc1 = v.tc1; o.tc2 = v.tc2; o.tc3 = v.tc3;\n"
"  return o;\n"
"}\n"
"fn alpha_ok(a: f32) -> bool {\n"
"  let r = u.misc.x;\n"
"  switch (u.flags.y) {\n"
"    case 1u: { return false; } case 2u: { return a < r; } case 3u: { return abs(a - r) < 0.002; }\n"
"    case 4u: { return a <= r; } case 5u: { return a > r; } case 6u: { return abs(a - r) >= 0.002; }\n"
"    case 7u: { return a >= r; } default: { return true; }\n"
"  }\n"
"}\n";

/* fixed-function texture stages */
static const char *WGSL_FF =
"fn arg(a: u32, cur: vec4<f32>, tex: vec4<f32>, dif: vec4<f32>, spc: vec4<f32>) -> vec4<f32> {\n"
"  var v = cur;\n"
"  switch (a & 7u) { case 0u: { v = dif; } case 2u: { v = tex; } case 3u: { v = u.tfactor; } case 4u: { v = spc; } default: {} }\n"
"  if ((a & 0x10u) != 0u) { v = vec4<f32>(1.0) - v; }\n"
"  if ((a & 0x20u) != 0u) { v = vec4<f32>(v.a); }\n"
"  return v;\n"
"}\n"
"fn top(o: u32, a0: vec4<f32>, a1: vec4<f32>, a2: vec4<f32>, cur: vec4<f32>, tex: vec4<f32>, dif: vec4<f32>) -> vec4<f32> {\n"
"  switch (o) {\n"
"    case 2u: { return a1; }  case 3u: { return a2; }\n"
"    case 4u: { return a1 * a2; }  case 5u: { return a1 * a2 * 2.0; }  case 6u: { return a1 * a2 * 4.0; }\n"
"    case 7u: { return a1 + a2; }  case 8u: { return a1 + a2 - 0.5; }  case 9u: { return (a1 + a2 - 0.5) * 2.0; }\n"
"    case 10u: { return a1 - a2; }  case 11u: { return a1 + a2 - a1 * a2; }\n"
"    case 12u: { return mix(a2, a1, dif.a); }  case 13u: { return mix(a2, a1, tex.a); }\n"
"    case 14u: { return mix(a2, a1, u.tfactor.a); }  case 15u: { return a1 + a2 * (1.0 - tex.a); }\n"
"    case 16u: { return mix(a2, a1, cur.a); }\n"
"    case 18u: { return vec4<f32>(a1.rgb + a1.a * a2.rgb, a1.a); }\n"
"    case 19u: { return vec4<f32>(a1.rgb * a2.rgb + vec3<f32>(a1.a), a1.a); }\n"
"    case 20u: { return vec4<f32>((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a); }\n"
"    case 21u: { return vec4<f32>((vec3<f32>(1.0) - a1.rgb) * a2.rgb + vec3<f32>(a1.a), a1.a); }\n"
"    case 24u: { let d = saturate(dot(a1.rgb - 0.5, a2.rgb - 0.5) * 4.0); return vec4<f32>(d); }\n"
"    case 25u: { return a0 + a1 * a2; }\n"
"    case 26u: { return a0 * a1 + (vec4<f32>(1.0) - a0) * a2; }\n"
"    default: { return a1; }\n"
"  }\n"
"}\n"
"fn stage(s: vec4<u32>, sb: vec4<u32>, cur: vec4<f32>, tex: vec4<f32>, dif: vec4<f32>, spc: vec4<f32>) -> vec4<f32> {\n"
"  let c0 = arg(sb.w & 255u, cur, tex, dif, spc);\n"
"  let c = top(s.x, c0, arg(s.y, cur, tex, dif, spc), arg(s.z, cur, tex, dif, spc), cur, tex, dif);\n"
"  var a = cur.a;\n"
"  if (s.w > 1u) {\n"
"    let a0 = arg(sb.w >> 8u, cur, tex, dif, spc);\n"
"    a = top(s.w, a0, arg(sb.x, cur, tex, dif, spc), arg(sb.y, cur, tex, dif, spc), cur, tex, dif).a;\n"
"  }\n"
"  return saturate(vec4<f32>(c.rgb, a));\n"
"}\n"
"@fragment fn fs(i: VO) -> @location(0) vec4<f32> {\n"
"  var cur = i.diffuse;\n"
"  let uv0 = select(i.tc0.xy, i.tc1.xy, u.st0b.z == 1u);\n"
"  let uv1 = select(i.tc0.xy, i.tc1.xy, u.st1b.z == 1u);\n"
"  let tx0 = textureSample(tex0, smp0, uv0);\n"
"  let tx1 = textureSample(tex1, smp1, uv1);\n"
"  if (u.st0.x != 1u) {\n"
"    cur = stage(u.st0, u.st0b, cur, tx0, i.diffuse, i.specular);\n"
"    if (u.st1.x != 1u) { cur = stage(u.st1, u.st1b, cur, tx1, i.diffuse, i.specular); }\n"
"  }\n"
"  if (u.flags.z == 1u) { cur = vec4<f32>(saturate(cur.rgb + i.specular.rgb), cur.a); }\n"
"  if (!alpha_ok(cur.a)) { discard; }\n"
"  return cur;\n"
"}\n";

/* ------------------------------------------------------------------ state */

static void release_view(WGPUTextureView view);   /* drops cached bind groups using it, then releases it */

typedef struct { WGPUTexture tex; WGPUTextureView view; uint32_t used_in_batch, w, h, levels, fmt; size_t bytes; } GTex;
int g_gpu_bc;               /* device has texture-compression-bc */
size_t g_gpu_tex_bytes;     /* GPU memory in game textures, for the [mem] log */
typedef struct { WGPUTexture tex; WGPUTextureView view; uint32_t w, h; int depth, gpu_dirty; } GSurf;

static WGPUInstance g_inst;
static WGPUAdapter g_adapter;
static WGPUDevice g_dev;
static WGPUQueue g_queue;
static WGPUSurface g_surface;
static WGPUTextureFormat g_surf_fmt;
static int g_surf_w, g_surf_h;   /* back buffer: the largest 4:3 area that fits the window */
static int g_win_w, g_win_h;     /* window (swap chain) size; the back buffer is centred in it */
static WGPUTexture g_depth;
static WGPUTextureView g_depth_view;
static WGPUTexture g_bb;          /* offscreen back buffer: always rendered, copied to the window when visible */
static WGPUTextureView g_bb_view;
static WGPUShaderModule g_shader;   /* shared header + fixed-function fragment stage */
static WGPUBindGroupLayout g_bgl_u, g_bgl_t;
static WGPUPipelineLayout g_pl;
static WGPUBuffer g_vbuf, g_ubuf;
static uint32_t g_vpos, g_upos;
#define VBUF_SIZE (32u << 20)
#define UBUF_SIZE (8u << 20)
#define USTRIDE 512u
static WGPUBindGroup g_bg_u;
static GTex g_white, g_black;
static WGPUSampler g_samplers[1024];   /* indexed by sampler key: never evicted while in use */

static WGPUCommandEncoder g_enc;
static WGPURenderPassEncoder g_pass;
static uint32_t g_batch = 1;
static int g_frame_open;

static WGPUStringView sv(const char *s) { return (WGPUStringView){ s, WGPU_STRLEN }; }

/* ids (shared counter for textures and surfaces) -> GTex / GSurf */
#define OBJ_CAP 16384
static struct { uint32_t id; void *p; } g_objs[OBJ_CAP];
static void *obj_get(uint32_t id) {
    for (uint32_t i = 0, h = id * 2654435761u; i < OBJ_CAP; i++) {
        uint32_t k = (h + i) & (OBJ_CAP - 1);
        if (g_objs[k].id == id) return g_objs[k].p;
        if (!g_objs[k].id) return NULL;
    }
    return NULL;
}
static void obj_put(uint32_t id, void *p) {
    for (uint32_t i = 0, h = id * 2654435761u; i < OBJ_CAP; i++) {
        uint32_t k = (h + i) & (OBJ_CAP - 1);
        if (g_objs[k].id == id || !g_objs[k].id || g_objs[k].id == 0xFFFFFFFFu) { g_objs[k].id = id; g_objs[k].p = p; return; }
    }
    fprintf(stderr, "[wgpu] object table full\n");
}
static void obj_del(uint32_t id) {
    for (uint32_t i = 0, h = id * 2654435761u; i < OBJ_CAP; i++) {
        uint32_t k = (h + i) & (OBJ_CAP - 1);
        if (g_objs[k].id == id) { g_objs[k].id = 0xFFFFFFFFu; g_objs[k].p = NULL; return; }   /* tombstone */
        if (!g_objs[k].id) return;
    }
}

/* ------------------------------------------------------------------ init */

static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2) {
    (void)u2;
    if (st != WGPURequestAdapterStatus_Success) fprintf(stderr, "[wgpu] adapter: %.*s\n", (int)msg.length, msg.data);
    *(WGPUAdapter *)u1 = a;
}
static void on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2) {
    (void)u2;
    if (st != WGPURequestDeviceStatus_Success) fprintf(stderr, "[wgpu] device: %.*s\n", (int)msg.length, msg.data);
    *(WGPUDevice *)u1 = d;
}
static void on_error(WGPUDevice const *d, WGPUErrorType t, WGPUStringView msg, void *u1, void *u2) {
    (void)d; (void)u1; (void)u2;
    static int count;
    if (count++ < 20) fprintf(stderr, "[wgpu] error %d: %.*s\n", (int)t, (int)msg.length, msg.data);
}

static void make_depth(void) {
    if (g_bb) { release_view(g_bb_view); wgpuTextureRelease(g_bb); }
    WGPUTextureDescriptor bd = WGPU_TEXTURE_DESCRIPTOR_INIT;
    bd.size = (WGPUExtent3D){ (uint32_t)g_surf_w, (uint32_t)g_surf_h, 1 };
    bd.format = g_surf_fmt;
    bd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    g_bb = wgpuDeviceCreateTexture(g_dev, &bd);
    g_bb_view = wgpuTextureCreateView(g_bb, NULL);
    if (g_depth) { release_view(g_depth_view); wgpuTextureRelease(g_depth); }
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = (WGPUExtent3D){ (uint32_t)g_surf_w, (uint32_t)g_surf_h, 1 };
    td.format = WGPUTextureFormat_Depth24PlusStencil8;
    td.usage = WGPUTextureUsage_RenderAttachment;
    g_depth = wgpuDeviceCreateTexture(g_dev, &td);
    g_depth_view = wgpuTextureCreateView(g_depth, NULL);
}

static void configure_surface(void) {
    g_win_w = g_window_w > 0 ? g_window_w : 1024;
    g_win_h = g_window_h > 0 ? g_window_h : 768;
    /* the game is 4:3: render at the largest 4:3 size that fits, letterboxed (no stretching) */
    if (g_win_w * 3 > g_win_h * 4) { g_surf_h = g_win_h; g_surf_w = g_win_h * 4 / 3; }
    else { g_surf_w = g_win_w; g_surf_h = g_win_w * 3 / 4; }
    WGPUSurfaceConfiguration sc = WGPU_SURFACE_CONFIGURATION_INIT;
    sc.device = g_dev;
    sc.format = g_surf_fmt;
    sc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;  /* back buffer is copied in */
    sc.width = (uint32_t)g_win_w;
    sc.height = (uint32_t)g_win_h;
    sc.presentMode = WGPUPresentMode_Fifo;   /* vsync, like D3DPRESENT_INTERVAL_ONE */
#ifdef __EMSCRIPTEN__
    sc.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst;
#endif
    sc.alphaMode = WGPUCompositeAlphaMode_Opaque;
    wgpuSurfaceConfigure(g_surface, &sc);
    make_depth();
}

static void make_solid(GTex *g, uint8_t v) {
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = (WGPUExtent3D){ 1, 1, 1 };
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    g->tex = wgpuDeviceCreateTexture(g_dev, &td);
    g->view = wgpuTextureCreateView(g->tex, NULL);
    uint8_t px[4] = { v, v, v, 255 };
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = g->tex;
    WGPUTexelCopyBufferLayout lay = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    lay.bytesPerRow = 4; lay.rowsPerImage = 1;
    WGPUExtent3D ex = { 1, 1, 1 };
    wgpuQueueWriteTexture(g_queue, &dst, px, 4, &lay, &ex);
}

static WGPUShaderModule make_module(const char *fragment) {
    size_t n = strlen(WGSL_COMMON) + strlen(fragment) + 1;
    char *code = malloc(n);
    snprintf(code, n, "%s%s", WGSL_COMMON, fragment);
    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = sv(code);
    WGPUShaderModuleDescriptor smd = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    smd.nextInChain = &src.chain;
    WGPUShaderModule m = wgpuDeviceCreateShaderModule(g_dev, &smd);
    free(code);
    return m;
}

/* everything after the device exists: surface format, back buffer, shaders, layouts, buffers */
static void create_resources(void) {
    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps);
    g_surf_fmt = caps.formatCount ? caps.formats[0] : WGPUTextureFormat_BGRA8Unorm;
    for (size_t i = 0; i < caps.formatCount; i++)   /* prefer a non-sRGB format: D3D8 blends in gamma space */
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm) g_surf_fmt = caps.formats[i];
    configure_surface();

    g_shader = make_module(WGSL_FF);

    WGPUBindGroupLayoutEntry ue = WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
    ue.binding = 0;
    ue.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    ue.buffer.type = WGPUBufferBindingType_Uniform;
    ue.buffer.hasDynamicOffset = 1;
    ue.buffer.minBindingSize = sizeof(Uniforms);
    WGPUBindGroupLayoutDescriptor bd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    bd.entryCount = 1; bd.entries = &ue;
    g_bgl_u = wgpuDeviceCreateBindGroupLayout(g_dev, &bd);

    WGPUBindGroupLayoutEntry te[8];
    for (int i = 0; i < 8; i++) {
        te[i] = (WGPUBindGroupLayoutEntry)WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT;
        te[i].binding = (uint32_t)i;
        te[i].visibility = WGPUShaderStage_Fragment;
        if (i % 2 == 0) {
            te[i].texture.sampleType = WGPUTextureSampleType_Float;
            te[i].texture.viewDimension = WGPUTextureViewDimension_2D;
        } else {
            te[i].sampler.type = WGPUSamplerBindingType_Filtering;
        }
    }
    bd.entryCount = 8; bd.entries = te;
    g_bgl_t = wgpuDeviceCreateBindGroupLayout(g_dev, &bd);
    WGPUBindGroupLayout ls[2] = { g_bgl_u, g_bgl_t };
    WGPUPipelineLayoutDescriptor pld = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pld.bindGroupLayoutCount = 2; pld.bindGroupLayouts = ls;
    g_pl = wgpuDeviceCreatePipelineLayout(g_dev, &pld);

    WGPUBufferDescriptor bdesc = WGPU_BUFFER_DESCRIPTOR_INIT;
    bdesc.size = VBUF_SIZE; bdesc.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    g_vbuf = wgpuDeviceCreateBuffer(g_dev, &bdesc);
    bdesc.size = UBUF_SIZE; bdesc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    g_ubuf = wgpuDeviceCreateBuffer(g_dev, &bdesc);
    WGPUBindGroupEntry be = WGPU_BIND_GROUP_ENTRY_INIT;
    be.binding = 0; be.buffer = g_ubuf; be.size = sizeof(Uniforms);
    WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bgd.layout = g_bgl_u; bgd.entryCount = 1; bgd.entries = &be;
    g_bg_u = wgpuDeviceCreateBindGroup(g_dev, &bgd);
    make_solid(&g_white, 255);
    make_solid(&g_black, 0);   /* pixel shaders sampling an empty stage read (0, 0, 0, 1) */
    fprintf(stderr, "[wgpu] renderer ready: rendering at %dx%d in a %dx%d window\n", g_surf_w, g_surf_h, g_win_w, g_win_h);
}


#ifndef __EMSCRIPTEN__
int gpu_init_native(void) {
    if (g_dev) return 1;
    if (!g_metal_layer) { fprintf(stderr, "[wgpu] no window\n"); return 0; }
    g_inst = wgpuCreateInstance(NULL);
    WGPUSurfaceSourceMetalLayer ml = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    ml.layer = g_metal_layer;
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &ml.chain;
    g_surface = wgpuInstanceCreateSurface(g_inst, &sd);

    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    ao.compatibleSurface = g_surface;
    ao.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowProcessEvents;
    aci.callback = on_adapter;
    aci.userdata1 = &g_adapter;
    wgpuInstanceRequestAdapter(g_inst, &ao, aci);
    while (!g_adapter) wgpuInstanceProcessEvents(g_inst);

    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    static const WGPUFeatureName bc = WGPUFeatureName_TextureCompressionBC;
    g_gpu_bc = wgpuAdapterHasFeature(g_adapter, bc) && !getenv("PORT_NO_BC");   /* DXT textures stay compressed */
    if (g_gpu_bc) { dd.requiredFeatureCount = 1; dd.requiredFeatures = &bc; }
    fprintf(stderr, "[wgpu] BC texture compression: %s\n", g_gpu_bc ? "yes" : "no (DXT textures are converted to RGBA8)");
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowProcessEvents;
    dci.callback = on_device;
    dci.userdata1 = &g_dev;
    wgpuAdapterRequestDevice(g_adapter, &dd, dci);
    while (!g_dev) wgpuInstanceProcessEvents(g_inst);
    g_queue = wgpuDeviceGetQueue(g_dev);
    create_resources();
    return 1;
}
#endif


#ifndef __EMSCRIPTEN__
#include <zlib.h>
static void write_png(const char *path, const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride) {
    size_t raw_len = (size_t)(w * 3 + 1) * h;
    uint8_t *raw = malloc(raw_len);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (w * 3 + 1);
        r[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = bgra + (size_t)y * stride + 4 * x;
            int is_bgra = g_surf_fmt == WGPUTextureFormat_BGRA8Unorm || g_surf_fmt == WGPUTextureFormat_BGRA8UnormSrgb;
            r[1 + 3 * x] = is_bgra ? p[2] : p[0]; r[2 + 3 * x] = p[1]; r[3 + 3 * x] = is_bgra ? p[0] : p[2];
        }
    }
    uLongf zlen = compressBound((uLong)raw_len);
    uint8_t *z = malloc(zlen);
    compress2(z, &zlen, raw, (uLong)raw_len, 6);
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                         (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
    const struct { const char *t; const uint8_t *d; uint32_t n; } chunks[3] = { { "IHDR", ihdr, 13 }, { "IDAT", z, (uint32_t)zlen }, { "IEND", NULL, 0 } };
    for (int i = 0; i < 3; i++) {
        uint8_t len[4] = { (uint8_t)(chunks[i].n >> 24), (uint8_t)(chunks[i].n >> 16), (uint8_t)(chunks[i].n >> 8), (uint8_t)chunks[i].n };
        fwrite(len, 1, 4, f);
        fwrite(chunks[i].t, 1, 4, f);
        if (chunks[i].n) fwrite(chunks[i].d, 1, chunks[i].n, f);
        uLong crc = crc32(0, (const Bytef *)chunks[i].t, 4);
        if (chunks[i].n) crc = crc32(crc, chunks[i].d, chunks[i].n);
        uint8_t cb[4] = { (uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc };
        fwrite(cb, 1, 4, f);
    }
    fclose(f);
    free(raw); free(z);
}

#endif
static int g_map_done;
static void on_mapped(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
    (void)st; (void)msg; (void)u1; (void)u2;
    g_map_done = 1;
}

/* Reads the back buffer back and writes it as a PNG (work recorded so far is submitted first). */
#ifndef __EMSCRIPTEN__
static void capture_png(const char *path) {
    uint32_t w = (uint32_t)g_surf_w, h = (uint32_t)g_surf_h, stride = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t)stride * h;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(g_dev, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_dev, NULL);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = g_bb;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf;
    dst.layout.bytesPerRow = stride;
    dst.layout.rowsPerImage = h;
    WGPUExtent3D ex = { w, h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ex);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    mci.mode = WGPUCallbackMode_AllowProcessEvents;
    mci.callback = on_mapped;
    g_map_done = 0;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, bd.size, mci);
    while (!g_map_done) wgpuInstanceProcessEvents(g_inst);
    const uint8_t *data = wgpuBufferGetConstMappedRange(buf, 0, bd.size);
    write_png(path, data, w, h, stride);
    fprintf(stderr, "[wgpu] screenshot %s\n", path);
    wgpuBufferUnmap(buf);
    wgpuBufferDestroy(buf);
    wgpuBufferRelease(buf);
}


#endif

static WGPUAddressMode addr_mode(uint32_t m) {
    switch (m) { case 2: return WGPUAddressMode_MirrorRepeat; case 3: case 4: case 5: return WGPUAddressMode_ClampToEdge;
                 default: return WGPUAddressMode_Repeat; }
}
static WGPUSampler sampler_for(uint32_t key) {
    key &= 1023;
    uint32_t mag = key & 1, min = (key >> 1) & 1, mip = (key >> 2) & 3, au = (key >> 4) & 7, av = (key >> 7) & 7;
    if (g_samplers[key]) return g_samplers[key];
    WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
    sd.magFilter = mag ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.minFilter = min ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    sd.mipmapFilter = mip == 2 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    sd.lodMaxClamp = mip ? 32.f : 0.f;
    sd.addressModeU = addr_mode(au);
    sd.addressModeV = addr_mode(av);
    g_samplers[key] = wgpuDeviceCreateSampler(g_dev, &sd);
    return g_samplers[key];
}

/* ------------------------------------------------------------------ pixel shaders */

char *ps_to_wgsl(const uint32_t *code, uint32_t n, char *why, size_t why_n);
typedef struct { WGPUShaderModule module; } PsProgram;

/* NULL when the shader can't be translated: such draws use the fixed-function stage instead */
static PsProgram *ps_program(uint32_t handle) {
    Shader *sh = shader_get(handle);
    if (!sh || !sh->pixel) return NULL;
    if (sh->backend) return sh->backend == (void *)1 ? NULL : sh->backend;
    char why[96] = "";
    char *fs = ps_to_wgsl(sh->code, sh->code_len, why, sizeof why);
    if (!fs) {
        fprintf(stderr, "[wgpu] pixel shader %x not translated: %s\n", handle, why);
        sh->backend = (void *)1;
        return NULL;
    }
    PsProgram *p = calloc(1, sizeof *p);
    p->module = make_module(fs);
    if (getenv("PORT_DUMP_PS")) fprintf(stderr, "[ps %x]\n%s", handle, fs);
    free(fs);
    sh->backend = p;
    return p;
}
static WGPUShaderModule module_for_ps(uint32_t ps) {
    PsProgram *p = ps ? ps_program(ps) : NULL;
    return p ? p->module : g_shader;
}

/* ------------------------------------------------------------------ pipelines */

static WGPUShaderModule module_for_ps(uint32_t ps);
typedef struct { PKey k; WGPURenderPipeline p; } PEntry;
static PEntry g_pipes[512];

static WGPUBlendFactor bf(uint32_t b) {
    switch (b) {
    case 1: return WGPUBlendFactor_Zero; case 2: return WGPUBlendFactor_One;
    case 3: return WGPUBlendFactor_Src; case 4: return WGPUBlendFactor_OneMinusSrc;
    case 5: return WGPUBlendFactor_SrcAlpha; case 6: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 7: return WGPUBlendFactor_DstAlpha; case 8: return WGPUBlendFactor_OneMinusDstAlpha;
    case 9: return WGPUBlendFactor_Dst; case 10: return WGPUBlendFactor_OneMinusDst;
    case 11: return WGPUBlendFactor_SrcAlphaSaturated;
    case 12: return WGPUBlendFactor_OneMinusSrcAlpha;  /* BOTHSRCALPHA (approximation) */
    case 13: return WGPUBlendFactor_SrcAlpha;          /* BOTHINVSRCALPHA (approximation) */
    default: return WGPUBlendFactor_One;
    }
}
static WGPUCompareFunction cmpf(uint32_t f) {
    switch (f) {
    case 1: return WGPUCompareFunction_Never; case 2: return WGPUCompareFunction_Less;
    case 3: return WGPUCompareFunction_Equal; case 4: return WGPUCompareFunction_LessEqual;
    case 5: return WGPUCompareFunction_Greater; case 6: return WGPUCompareFunction_NotEqual;
    case 7: return WGPUCompareFunction_GreaterEqual; default: return WGPUCompareFunction_Always;
    }
}
static WGPUBlendOperation bop(uint32_t o) {
    switch (o) { case 2: return WGPUBlendOperation_Subtract; case 3: return WGPUBlendOperation_ReverseSubtract;
                 case 4: return WGPUBlendOperation_Min; case 5: return WGPUBlendOperation_Max;
                 default: return WGPUBlendOperation_Add; }
}

static WGPURenderPipeline pipeline_for(const PKey *k) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof *k / 4; i++) h = (h ^ ((const uint32_t *)k)[i]) * 16777619u;
    for (uint32_t i = 0; i < 512; i++) {
        PEntry *e = &g_pipes[(h + i) % 512];
        if (e->p && !memcmp(&e->k, k, sizeof *k)) return e->p;
        if (!e->p) {
            WGPUVertexAttribute at[7] = {
                { NULL, WGPUVertexFormat_Float32x4, 0, 0 }, { NULL, WGPUVertexFormat_Uint32, 16, 1 },
                { NULL, WGPUVertexFormat_Uint32, 20, 2 }, { NULL, WGPUVertexFormat_Float32x4, 24, 3 },
                { NULL, WGPUVertexFormat_Float32x4, 40, 4 }, { NULL, WGPUVertexFormat_Float32x4, 56, 5 },
                { NULL, WGPUVertexFormat_Float32x4, 72, 6 },
            };
            WGPUVertexBufferLayout vbl = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
            vbl.arrayStride = sizeof(CVert); vbl.attributeCount = 7; vbl.attributes = at;
            vbl.stepMode = WGPUVertexStepMode_Vertex;
            WGPUBlendState bs = WGPU_BLEND_STATE_INIT;
            bs.color.srcFactor = bf(k->src); bs.color.dstFactor = bf(k->dst); bs.color.operation = bop(k->op);
            bs.alpha = bs.color;
            WGPUColorTargetState ct = WGPU_COLOR_TARGET_STATE_INIT;
            ct.format = g_surf_fmt;
            ct.blend = k->blend ? &bs : NULL;
            ct.writeMask = (k->cmask & 1 ? WGPUColorWriteMask_Red : 0) | (k->cmask & 2 ? WGPUColorWriteMask_Green : 0) |
                           (k->cmask & 4 ? WGPUColorWriteMask_Blue : 0) | (k->cmask & 8 ? WGPUColorWriteMask_Alpha : 0);
            WGPUFragmentState fs = WGPU_FRAGMENT_STATE_INIT;
            WGPUShaderModule mod = module_for_ps(k->ps);
            fs.module = mod; fs.entryPoint = sv("fs"); fs.targetCount = 1; fs.targets = &ct;
            WGPUDepthStencilState ds = WGPU_DEPTH_STENCIL_STATE_INIT;
            ds.format = WGPUTextureFormat_Depth24PlusStencil8;
            ds.depthWriteEnabled = k->zen && k->zwrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            ds.depthCompare = k->zen ? cmpf(k->zfunc) : WGPUCompareFunction_Always;
            WGPURenderPipelineDescriptor pd = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
            pd.layout = g_pl;
            pd.vertex.module = mod; pd.vertex.entryPoint = sv("vs");
            pd.vertex.bufferCount = 1; pd.vertex.buffers = &vbl;
            pd.primitive.topology = (WGPUPrimitiveTopology)k->topo;
            pd.primitive.frontFace = WGPUFrontFace_CW;
            pd.primitive.cullMode = k->cull == 2 ? WGPUCullMode_Front : k->cull == 3 ? WGPUCullMode_Back : WGPUCullMode_None;
            pd.depthStencil = &ds;
            pd.fragment = &fs;
            pd.multisample.count = 1; pd.multisample.mask = 0xFFFFFFFFu;
            e->k = *k;
            e->p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
            return e->p;
        }
    }
    fprintf(stderr, "[wgpu] pipeline cache full\n");
    return g_pipes[0].p;
}

/* ------------------------------------------------------------------ frames and passes */

static WGPUTextureView g_tgt_color, g_tgt_depth;   /* attachments of the current pass target (NULL: back buffer) */
static uint32_t g_tgt_w, g_tgt_h, g_tgt_lw = 640, g_tgt_lh = 480;   /* GPU size and Direct3D size */
static GSurf *g_tgt_gs;                                 /* NULL when drawing to the back buffer */

static void end_pass(void) {
    if (g_pass) { wgpuRenderPassEncoderEnd(g_pass); wgpuRenderPassEncoderRelease(g_pass); g_pass = NULL; }
}

/* Textures the game freed. Releasing a WebGPU texture only drops a reference: its GPU memory goes when the
   browser garbage-collects it, and screens that recreate textures every frame (team select) run the GPU out
   of memory first (Firefox). destroy() frees it at once, but draws recorded into the open command encoder
   may still use it, so the destroy waits until that work has been submitted. */
/* Bind groups of draws, cached by their 4 texture views and 4 sampler keys. A WebGPU bind group can't be
   destroyed, only dropped for the browser's garbage collector, and a new one per draw (hundreds a frame)
   outran it on Firefox/D3D12, where they hold descriptor-heap slots: "Out of Memory", then invalid bind
   groups. Entries using a view are dropped when that view is released (pointers can be reused). */
#define BG_CACHE 4096
#define BG_PROBES 8
typedef struct { WGPUTextureView v[4]; uint32_t s[4]; WGPUBindGroup bg; uint32_t stamp; } BgEntry;
static BgEntry g_bgc[BG_CACHE];
static uint32_t g_bg_stamp;
uint32_t g_gpu_bind_groups_made;   /* for the [mem] log */
static uint32_t bg_hash(WGPUTextureView const v[4], const uint32_t s[4]) {
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < 4; i++) { h = (h ^ (uint64_t)(uintptr_t)v[i]) * 1099511628211ull; h = (h ^ s[i]) * 1099511628211ull; }
    return (uint32_t)(h ^ (h >> 32));
}
static void bg_forget_view(WGPUTextureView view) {
    for (int i = 0; i < BG_CACHE; i++) {
        BgEntry *e = &g_bgc[i];
        if (e->bg && (e->v[0] == view || e->v[1] == view || e->v[2] == view || e->v[3] == view)) {
            wgpuBindGroupRelease(e->bg);   /* recorded commands keep their own reference */
            e->bg = NULL;
        }
    }
}
static void release_view(WGPUTextureView view) { bg_forget_view(view); wgpuTextureViewRelease(view); }

static WGPUTexture *g_retired;
static size_t g_nretired, g_capretired;
static void retire_texture(WGPUTexture t) {
    if (g_nretired == g_capretired) {
        g_capretired = g_capretired ? g_capretired * 2 : 64;
        g_retired = realloc(g_retired, g_capretired * sizeof *g_retired);
    }
    g_retired[g_nretired++] = t;
}
static void destroy_retired(void) {
    for (size_t i = 0; i < g_nretired; i++) { wgpuTextureDestroy(g_retired[i]); wgpuTextureRelease(g_retired[i]); }
    g_nretired = 0;
}

static void submit(void) {
    end_pass();
    if (g_enc) {
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g_enc, NULL);
        wgpuQueueSubmit(g_queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(g_enc);
        g_enc = NULL;
    }
    destroy_retired();   /* nothing recorded can use them any more */
    g_vpos = g_upos = 0;
    g_batch++;
}

static void begin_frame(void) {
    if (g_frame_open) return;
    if (g_window_w != g_win_w || g_window_h != g_win_h) {
        end_pass();
        configure_surface();
        g_tgt_color = g_tgt_depth = NULL;
        g_tgt_w = (uint32_t)g_surf_w; g_tgt_h = (uint32_t)g_surf_h;
    }
    g_frame_open = 1;
}

/* Opens a render pass on the current target; clear flags: 1 color, 2 depth, 4 stencil. */
static void begin_pass(uint32_t clear, uint32_t color, float z, uint32_t stencil) {
    end_pass();
    if (!g_enc) g_enc = wgpuDeviceCreateCommandEncoder(g_dev, NULL);
    WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ca.view = g_tgt_color ? g_tgt_color : g_bb_view;
    ca.loadOp = (clear & 1) ? WGPULoadOp_Clear : WGPULoadOp_Load;
    ca.storeOp = WGPUStoreOp_Store;
    ca.clearValue = (WGPUColor){ ((color >> 16) & 255) / 255.0, ((color >> 8) & 255) / 255.0, (color & 255) / 255.0,
                                 (color >> 24) / 255.0 };
    WGPURenderPassDepthStencilAttachment da = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    da.view = g_tgt_depth ? g_tgt_depth : g_depth_view;
    da.depthLoadOp = (clear & 2) ? WGPULoadOp_Clear : WGPULoadOp_Load;
    da.depthStoreOp = WGPUStoreOp_Store;
    da.depthClearValue = z;
    da.stencilLoadOp = (clear & 4) ? WGPULoadOp_Clear : WGPULoadOp_Load;
    da.stencilStoreOp = WGPUStoreOp_Store;
    da.stencilClearValue = stencil;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
    rp.depthStencilAttachment = &da;
    g_pass = wgpuCommandEncoderBeginRenderPass(g_enc, &rp);
}

/* ------------------------------------------------------------------ render targets */

static GSurf *gsurf(uint32_t id, uint32_t w, uint32_t h, int depth) {
    GSurf *g = obj_get(id);
    if (g) return g;
    g = calloc(1, sizeof *g);
    g->w = w ? w : 1; g->h = h ? h : 1; g->depth = depth;
    WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
    td.size = (WGPUExtent3D){ g->w, g->h, 1 };
    td.format = depth ? WGPUTextureFormat_Depth24PlusStencil8 : g_surf_fmt;
    td.usage = WGPUTextureUsage_RenderAttachment |
               (depth ? 0 : WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst);
    g->tex = wgpuDeviceCreateTexture(g_dev, &td);
    g->view = wgpuTextureCreateView(g->tex, NULL);
    obj_put(id, g);
    return g;
}

/* depth buffer for targets whose depth surface doesn't match their size (WebGPU needs equal sizes) */
static WGPUTextureView scratch_depth(uint32_t w, uint32_t h) {
    static struct { uint32_t w, h; WGPUTexture t; WGPUTextureView v; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].t && cache[i].w == w && cache[i].h == h) return cache[i].v;
    for (int i = 0; i < 8; i++)
        if (!cache[i].t) {
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.size = (WGPUExtent3D){ w, h, 1 };
            td.format = WGPUTextureFormat_Depth24PlusStencil8;
            td.usage = WGPUTextureUsage_RenderAttachment;
            cache[i].t = wgpuDeviceCreateTexture(g_dev, &td);
            cache[i].v = wgpuTextureCreateView(cache[i].t, NULL);
            cache[i].w = w; cache[i].h = h;
            return cache[i].v;
        }
    return cache[0].v;
}

static void exec_target(const CmdTarget *c) {
    WGPUTextureView color, depth;
    GSurf *gs = NULL;
    uint32_t w, h;
    if (c->color_id) { gs = gsurf(c->color_id, c->color_w, c->color_h, 0); color = gs->view; w = gs->w; h = gs->h; }
    else { color = g_bb_view; w = (uint32_t)g_surf_w; h = (uint32_t)g_surf_h; }
    if (!c->color_id && !c->depth_id) depth = g_depth_view;
    else if (c->depth_id) {
        GSurf *gd = gsurf(c->depth_id, c->depth_w, c->depth_h, 1);
        depth = gd->w == w && gd->h == h ? gd->view : scratch_depth(w, h);
    } else depth = scratch_depth(w, h);
    if (color != (g_tgt_color ? g_tgt_color : g_bb_view) || depth != (g_tgt_depth ? g_tgt_depth : g_depth_view)) {
        end_pass();
        g_tgt_color = color; g_tgt_depth = depth;
    }
    g_tgt_w = w; g_tgt_h = h; g_tgt_lw = c->lw; g_tgt_lh = c->lh; g_tgt_gs = gs;
}

static void use_backbuffer(void) {
    if (g_tgt_color && g_tgt_color != g_bb_view) end_pass();
    g_tgt_color = g_tgt_depth = NULL;
    g_tgt_gs = NULL;
    g_tgt_w = (uint32_t)g_surf_w; g_tgt_h = (uint32_t)g_surf_h;
}

/* one BGRA8 pixel -> a Direct3D format; returns bytes written (0: unsupported) */
static uint32_t pack_pixel(uint32_t fmt, const uint8_t *bgra, uint8_t *o) {
    uint32_t b = bgra[0], g = bgra[1], r = bgra[2], a = bgra[3];
    switch (fmt) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: memcpy(o, bgra, 4); if (fmt == FMT_X8R8G8B8) o[3] = 255; return 4;
    case FMT_R5G6B5: { uint16_t v = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3); memcpy(o, &v, 2); return 2; }
    case FMT_X1R5G5B5: case FMT_A1R5G5B5: {
        uint16_t v = (uint16_t)((a >= 128 ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3); memcpy(o, &v, 2); return 2; }
    case FMT_A4R4G4B4: { uint16_t v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4); memcpy(o, &v, 2); return 2; }
    default: return 0;
    }
}

/* mapped BGRA pixels -> guest memory in the surface's format (nearest scaling: the back buffer is scaled) */
static void store_pixels(const CmdReadback *c, const uint8_t *px, uint32_t gw, uint32_t gh, uint32_t stride) {
    uint8_t tmp[4];
    if (!pack_pixel(c->fmt, (const uint8_t[4]){ 0, 0, 0, 0 }, tmp)) {
        static int warned;
        if (!warned++) fprintf(stderr, "[wgpu] render target readback: format %x not supported\n", c->fmt);
        return;
    }
    for (uint32_t y = 0; y < c->h_; y++) {
        const uint8_t *row = px + (size_t)(y * gh / c->h_) * stride;
        uint8_t *o = HP(c->guest_mem + y * c->pitch);
        for (uint32_t x = 0; x < c->w; x++) o += pack_pixel(c->fmt, row + 4 * (x * gw / c->w), o);
    }
}

/* Starts copying a readback source into a mappable buffer; returns the buffer (or NULL: nothing to read). */
static WGPUBuffer start_readback(const CmdReadback *c, uint32_t *gw, uint32_t *gh, uint32_t *stride) {
    GSurf *gs = c->id ? obj_get(c->id) : NULL;
    if (c->id && (!gs || gs->depth || !gs->gpu_dirty)) return NULL;   /* CPU copy is current */
    WGPUTexture tex = c->id ? gs->tex : g_bb;
    *gw = c->id ? gs->w : (uint32_t)g_surf_w;
    *gh = c->id ? gs->h : (uint32_t)g_surf_h;
    *stride = (*gw * 4 + 255) & ~255u;
    submit();
    if (g_frame_open) begin_pass(0, 0, 1.f, 0);   /* keep drawing into the same target afterwards */
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = (uint64_t)*stride * *gh;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(g_dev, &bd);
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_dev, NULL);
    WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    src.texture = tex;
    WGPUTexelCopyBufferInfo dst = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    dst.buffer = buf; dst.layout.bytesPerRow = *stride; dst.layout.rowsPerImage = *gh;
    WGPUExtent3D ex = { *gw, *gh, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &ex);
    WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(g_queue, 1, &cb);
    wgpuCommandBufferRelease(cb);
    wgpuCommandEncoderRelease(enc);
    if (gs) gs->gpu_dirty = 0;
    return buf;
}

static void finish_readback(const CmdReadback *c) {
    __atomic_store_n((volatile int *)(uintptr_t)c->done_ptr, 1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ commands */

static void exec_upload_tex(const CmdUploadTex *c) {
    GTex *g = obj_get(c->id);
    static const WGPUTextureFormat formats[4] = { WGPUTextureFormat_RGBA8Unorm, WGPUTextureFormat_BC1RGBAUnorm,
                                                  WGPUTextureFormat_BC2RGBAUnorm, WGPUTextureFormat_BC3RGBAUnorm };
    uint32_t bs = c->fmt == TEXFMT_BC1 ? 8 : 16;
    if (g && (g->w != c->w[0] || g->h != c->h_[0] || g->levels != c->levels || g->fmt != c->fmt)) {
        release_view(g->view); retire_texture(g->tex); g_gpu_tex_bytes -= g->bytes; free(g); obj_del(c->id); g = NULL;
    }
    if (!g) {
        g = calloc(1, sizeof *g);
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = (WGPUExtent3D){ c->w[0], c->h_[0], 1 };
        td.format = formats[c->fmt & 3];
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        td.mipLevelCount = c->levels;
        g->tex = wgpuDeviceCreateTexture(g_dev, &td);
        g->view = wgpuTextureCreateView(g->tex, NULL);
        g->w = c->w[0]; g->h = c->h_[0]; g->levels = c->levels; g->fmt = c->fmt;
        for (uint32_t l = 0; l < c->levels; l++)
            g->bytes += c->fmt ? (size_t)((c->w[l] + 3) / 4) * ((c->h_[l] + 3) / 4) * bs : (size_t)c->w[l] * c->h_[l] * 4;
        g_gpu_tex_bytes += g->bytes;
        obj_put(c->id, g);
    } else if (g->used_in_batch == g_batch) {
        /* already drawn with in the current batch: flush that work first, so the earlier draws see
           the old contents (Direct3D semantics) */
        submit();
        if (g_frame_open) begin_pass(0, 0, 1.f, 0);
    }
    const uint8_t *px = (const uint8_t *)(c + 1);
    for (uint32_t l = 0; l < c->levels; l++) {
        WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        dst.texture = g->tex; dst.mipLevel = l;
        WGPUTexelCopyBufferLayout lay = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
        WGPUExtent3D ex = { c->w[l], c->h_[l], 1 };
        size_t n;
        if (c->fmt) {   /* whole blocks: small mips are copied at their rounded-up (physical) size */
            uint32_t bw = (c->w[l] + 3) / 4, bh = (c->h_[l] + 3) / 4;
            lay.bytesPerRow = bw * bs; lay.rowsPerImage = bh;
            ex = (WGPUExtent3D){ bw * 4, bh * 4, 1 };
            n = (size_t)bw * bh * bs;
        } else {
            lay.bytesPerRow = c->w[l] * 4; lay.rowsPerImage = c->h_[l];
            n = (size_t)c->w[l] * c->h_[l] * 4;
        }
        wgpuQueueWriteTexture(g_queue, &dst, px, n, &lay, &ex);
        px += n;
    }
}

static void exec_destroy(uint32_t id, int surface) {
    void *p = obj_get(id);
    if (!p) return;
    if (surface) {
        GSurf *g = p;
        if (g->view == g_tgt_color || g->view == g_tgt_depth) use_backbuffer();
        release_view(g->view); retire_texture(g->tex);
    } else {
        GTex *g = p;
        release_view(g->view); retire_texture(g->tex);
        g_gpu_tex_bytes -= g->bytes;
    }
    free(p);
    obj_del(id);
}

static void exec_upload_surf(const CmdUploadSurf *c) {
    GSurf *gs = gsurf(c->id, c->w, c->h_, 0);
    uint8_t *px = malloc((size_t)gs->w * gs->h * 4);
    memcpy(px, c + 1, (size_t)gs->w * gs->h * 4);
    if (g_surf_fmt == WGPUTextureFormat_BGRA8Unorm)
        for (size_t k = 0; k < (size_t)gs->w * gs->h; k++) { uint8_t t = px[4 * k]; px[4 * k] = px[4 * k + 2]; px[4 * k + 2] = t; }
    submit();   /* keep ordering with draws already recorded */
    if (g_frame_open) begin_pass(0, 0, 1.f, 0);
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = gs->tex;
    WGPUTexelCopyBufferLayout lay = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    lay.bytesPerRow = gs->w * 4; lay.rowsPerImage = gs->h;
    WGPUExtent3D ex = { gs->w, gs->h, 1 };
    wgpuQueueWriteTexture(g_queue, &dst, px, (size_t)gs->w * gs->h * 4, &lay, &ex);
    free(px);
    gs->gpu_dirty = 0;
}

static void exec_clear(const CmdClear *c) {
    begin_frame();
    begin_pass(c->flags, c->color, c->z, c->stencil);
    if (g_tgt_gs) g_tgt_gs->gpu_dirty = 1;
}

static void exec_draw(const CmdDraw *c) {
    begin_frame();
    if (!g_pass) begin_pass(0, 0, 1.f, 0);
    uint32_t n = c->nverts, bytes = n * (uint32_t)sizeof(CVert);
    if (g_vpos + bytes > VBUF_SIZE || g_upos + USTRIDE > UBUF_SIZE) { submit(); begin_pass(0, 0, 1.f, 0); }

    PsProgram *ps = c->key.ps ? ps_program(c->key.ps) : NULL;
    WGPUTextureView views[4];
    for (int s = 0; s < 4; s++) {
        views[s] = ps ? g_black.view : g_white.view;
        if (c->slot_kind[s] == TS_TEX) {
            GTex *g = obj_get(c->slot_id[s]);
            if (g) { g->used_in_batch = g_batch; views[s] = g->view; }
        } else if (c->slot_kind[s] == TS_RTSURF) {
            GSurf *gs = obj_get(c->slot_id[s]);
            /* a render-target texture is never sampled while it is being drawn into */
            views[s] = gs && !gs->depth && gs->view != g_tgt_color ? gs->view : g_black.view;
        }
    }
    wgpuQueueWriteBuffer(g_queue, g_vbuf, g_vpos, c + 1, bytes);
    wgpuQueueWriteBuffer(g_queue, g_ubuf, g_upos, &c->u, sizeof c->u);

    WGPUBindGroupEntry te[8];
    for (int i = 0; i < 4; i++) {
        te[2 * i] = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
        te[2 * i].binding = (uint32_t)(2 * i); te[2 * i].textureView = views[i];
        te[2 * i + 1] = (WGPUBindGroupEntry)WGPU_BIND_GROUP_ENTRY_INIT;
        te[2 * i + 1].binding = (uint32_t)(2 * i + 1); te[2 * i + 1].sampler = sampler_for(c->samp_key[i]);
    }
    WGPUBindGroup bg = NULL;
    uint32_t hsh = bg_hash(views, c->samp_key);
    BgEntry *victim = NULL;
    for (int p = 0; p < BG_PROBES && !bg; p++) {
        BgEntry *e = &g_bgc[(hsh + (uint32_t)p) % BG_CACHE];
        if (e->bg && !memcmp(e->v, views, sizeof e->v) && !memcmp(e->s, c->samp_key, sizeof e->s)) { bg = e->bg; e->stamp = ++g_bg_stamp; }
        else if (!victim || !e->bg || (victim->bg && e->stamp < victim->stamp)) victim = e;
    }
    if (!bg) {
        WGPUBindGroupDescriptor bgd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bgd.layout = g_bgl_t; bgd.entryCount = 8; bgd.entries = te;
        bg = wgpuDeviceCreateBindGroup(g_dev, &bgd);
        g_gpu_bind_groups_made++;
        if (victim->bg) wgpuBindGroupRelease(victim->bg);
        memcpy(victim->v, views, sizeof victim->v);
        memcpy(victim->s, c->samp_key, sizeof victim->s);
        victim->bg = bg;
        victim->stamp = ++g_bg_stamp;
    }

    PKey k = c->key;
    if (!ps) k.ps = 0;
    WGPURenderPipeline p = pipeline_for(&k);

    float vx, vy, vw, vh;
    if (c->full_viewport) { vx = vy = 0; vw = (float)g_tgt_w; vh = (float)g_tgt_h; }
    else {
        float sx = (float)g_tgt_w / (float)g_tgt_lw, sy = (float)g_tgt_h / (float)g_tgt_lh;
        vx = c->vp[0] * sx; vy = c->vp[1] * sy; vw = c->vp[2] * sx; vh = c->vp[3] * sy;
        if (vx + vw > g_tgt_w) vw = g_tgt_w - vx;
        if (vy + vh > g_tgt_h) vh = g_tgt_h - vy;
    }
    if (vw <= 0 || vh <= 0) return;
    wgpuRenderPassEncoderSetViewport(g_pass, vx, vy, vw, vh, c->minz, c->maxz);
    wgpuRenderPassEncoderSetPipeline(g_pass, p);
    uint32_t off = g_upos;
    wgpuRenderPassEncoderSetBindGroup(g_pass, 0, g_bg_u, 1, &off);
    wgpuRenderPassEncoderSetBindGroup(g_pass, 1, bg, 0, NULL);   /* owned by the cache */
    wgpuRenderPassEncoderSetVertexBuffer(g_pass, 0, g_vbuf, g_vpos, bytes);
    wgpuRenderPassEncoderDraw(g_pass, n, 1, 0, 0);
    if (g_tgt_gs) g_tgt_gs->gpu_dirty = 1;
    g_vpos += (bytes + 255) & ~255u;
    g_upos += USTRIDE;
#ifndef __EMSCRIPTEN__
    if (c->capture_index >= 0) {
        /* frame capture: the back buffer after every draw, as /tmp/port_cap/NNNN.png */
        mkdir("/tmp/port_cap", 0755);
        char path[64];
        snprintf(path, sizeof path, "/tmp/port_cap/%04d.png", c->capture_index);
        submit();
        capture_png(path);
        begin_pass(0, 0, 1.f, 0);
    }
#endif
}

/* ------------------------------------------------------------------ present */

#ifndef __EMSCRIPTEN__
static void pace_60hz(void) {
    static double next;
    double now = host_seconds();
    if (next == 0 || now - next > 0.1) next = now;
    next += 1.0 / 60.0;
    if (next > now) usleep((useconds_t)((next - now) * 1e6));
}
#endif

static void exec_present(const CmdPresent *c) {
    begin_frame();
    use_backbuffer();
    if (!g_pass && !g_enc) begin_pass(1, 0, 1.f, 0);   /* nothing drawn: present black */
    submit();
#ifndef __EMSCRIPTEN__
    if (c->screenshot) { char path[256]; snprintf(path, sizeof path, "/tmp/port_frame_%u.png", c->frame); capture_png(path); }
#else
    (void)c;
#endif
    g_frame_open = 0;
    /* copy the back buffer to the window */
    WGPUSurfaceTexture cur = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(g_surface, &cur);
    if (cur.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal ||
        cur.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(g_dev, NULL);
        if (g_win_w != g_surf_w || g_win_h != g_surf_h) {   /* black bars around the 4:3 picture */
            WGPUTextureView v = wgpuTextureCreateView(cur.texture, NULL);
            WGPURenderPassColorAttachment ca = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
            ca.view = v; ca.loadOp = WGPULoadOp_Clear; ca.storeOp = WGPUStoreOp_Store;
            ca.clearValue = (WGPUColor){ 0, 0, 0, 1 };
            WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
            rp.colorAttachmentCount = 1; rp.colorAttachments = &ca;
            WGPURenderPassEncoder pe = wgpuCommandEncoderBeginRenderPass(enc, &rp);
            wgpuRenderPassEncoderEnd(pe);
            wgpuRenderPassEncoderRelease(pe);
            wgpuTextureViewRelease(v);
        }
        WGPUTexelCopyTextureInfo src = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        src.texture = g_bb;
        dst.texture = cur.texture;
        dst.origin = (WGPUOrigin3D){ (uint32_t)(g_win_w - g_surf_w) / 2, (uint32_t)(g_win_h - g_surf_h) / 2, 0 };
        WGPUExtent3D ex = { (uint32_t)g_surf_w, (uint32_t)g_surf_h, 1 };
        wgpuCommandEncoderCopyTextureToTexture(enc, &src, &dst, &ex);
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(enc, NULL);
        wgpuQueueSubmit(g_queue, 1, &cb);
        wgpuCommandBufferRelease(cb);
        wgpuCommandEncoderRelease(enc);
#ifndef __EMSCRIPTEN__
        wgpuSurfacePresent(g_surface);   /* FIFO: waits for vsync */
        wgpuTextureRelease(cur.texture);
        /* PORT_PACE60: fixed 60 Hz even on fast displays, so scripted input (PORT_INPUT) is repeatable */
        static int pace = -1;
        if (pace < 0) pace = getenv("PORT_PACE60") != NULL;
        if (pace) pace_60hz();
#else
        wgpuTextureRelease(cur.texture);   /* the browser presents the canvas when this task ends */
#endif
    } else {
        if (cur.texture) wgpuTextureRelease(cur.texture);
#ifndef __EMSCRIPTEN__
        if (cur.status != (WGPUSurfaceGetCurrentTextureStatus)0x00030001) configure_surface();  /* not just occluded */
        pace_60hz();
#endif
    }
}

/* ------------------------------------------------------------------ native: commands run immediately */

#ifndef __EMSCRIPTEN__
void gpu_exec(const CmdHdr *h) {
    switch (h->type) {
    case CMD_TARGET: exec_target((const CmdTarget *)h); break;
    case CMD_CLEAR: exec_clear((const CmdClear *)h); break;
    case CMD_DRAW: exec_draw((const CmdDraw *)h); break;
    case CMD_UPLOAD_TEX: exec_upload_tex((const CmdUploadTex *)h); break;
    case CMD_DESTROY_TEX: exec_destroy(((const CmdDestroy *)h)->id, 0); break;
    case CMD_UPLOAD_SURF: exec_upload_surf((const CmdUploadSurf *)h); break;
    case CMD_DESTROY_SURF: exec_destroy(((const CmdDestroy *)h)->id, 1); break;
    case CMD_PRESENT: exec_present((const CmdPresent *)h); break;
    case CMD_READBACK: {
        const CmdReadback *c = (const CmdReadback *)h;
        uint32_t gw, gh, stride;
        WGPUBuffer buf = start_readback(c, &gw, &gh, &stride);
        if (buf) {
            WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            mci.mode = WGPUCallbackMode_AllowProcessEvents;
            mci.callback = on_mapped;
            g_map_done = 0;
            wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, (uint64_t)stride * gh, mci);
            while (!g_map_done) wgpuInstanceProcessEvents(g_inst);
            store_pixels(c, wgpuBufferGetConstMappedRange(buf, 0, (uint64_t)stride * gh), gw, gh, stride);
            wgpuBufferUnmap(buf);
            wgpuBufferDestroy(buf);   /* free the staging memory now, not at garbage collection */
            wgpuBufferRelease(buf);
        }
        finish_readback(c);
        break;
    }
    }
}
#endif

/* ------------------------------------------------------------------ browser: queued commands */

#ifdef __EMSCRIPTEN__
#include <pthread.h>

typedef struct Chunk { uint8_t *buf; size_t len, off; int ends_frame; struct Chunk *next; } Chunk;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static Chunk *g_q_head, *g_q_tail;
static int g_q_frames, g_ready;
static WGPUBuffer g_rb_buf;
static CmdReadback g_rb_cmd;
static uint32_t g_rb_gw, g_rb_gh, g_rb_stride;
static int g_rb_pending;

/* game thread: hand over a finished chunk of commands (takes ownership of buf) */
void web_queue_chunk(uint8_t *buf, size_t len, int ends_frame) {
    Chunk *ch = calloc(1, sizeof *ch);
    ch->buf = buf; ch->len = len; ch->ends_frame = ends_frame;
    pthread_mutex_lock(&g_q_lock);
    if (g_q_tail) g_q_tail->next = ch; else g_q_head = ch;
    g_q_tail = ch;
    if (ends_frame) g_q_frames++;
    pthread_mutex_unlock(&g_q_lock);
}
int web_queued_frames(void) { return __atomic_load_n(&g_q_frames, __ATOMIC_ACQUIRE); }
int web_gpu_ready(void) { return __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE); }

static void on_rb_mapped(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) {
    (void)msg; (void)u1; (void)u2;
    uint64_t size = (uint64_t)g_rb_stride * g_rb_gh;
    if (st == WGPUMapAsyncStatus_Success)
        store_pixels(&g_rb_cmd, wgpuBufferGetConstMappedRange(g_rb_buf, 0, size), g_rb_gw, g_rb_gh, g_rb_stride);
    wgpuBufferUnmap(g_rb_buf);
    wgpuBufferDestroy(g_rb_buf);
    wgpuBufferRelease(g_rb_buf);
    finish_readback(&g_rb_cmd);
    g_rb_pending = 0;
}

/* runs a chunk from where it stopped; returns 1 when it paused for a readback */
static int run_chunk(Chunk *ch) {
    while (ch->off < ch->len) {
        const CmdHdr *h = (const CmdHdr *)(ch->buf + ch->off);
        ch->off += h->size;
        switch (h->type) {
        case CMD_TARGET: exec_target((const CmdTarget *)h); break;
        case CMD_CLEAR: exec_clear((const CmdClear *)h); break;
        case CMD_DRAW: exec_draw((const CmdDraw *)h); break;
        case CMD_UPLOAD_TEX: exec_upload_tex((const CmdUploadTex *)h); break;
        case CMD_DESTROY_TEX: exec_destroy(((const CmdDestroy *)h)->id, 0); break;
        case CMD_UPLOAD_SURF: exec_upload_surf((const CmdUploadSurf *)h); break;
        case CMD_DESTROY_SURF: exec_destroy(((const CmdDestroy *)h)->id, 1); break;
        case CMD_PRESENT: exec_present((const CmdPresent *)h); break;
        case CMD_READBACK: {
            const CmdReadback *c = (const CmdReadback *)h;
            WGPUBuffer buf = start_readback(c, &g_rb_gw, &g_rb_gh, &g_rb_stride);
            if (!buf) { finish_readback(c); break; }
            g_rb_cmd = *c;
            g_rb_buf = buf;
            g_rb_pending = 1;
            WGPUBufferMapCallbackInfo mci = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
            mci.mode = WGPUCallbackMode_AllowSpontaneous;
            mci.callback = on_rb_mapped;
            wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, (uint64_t)g_rb_stride * g_rb_gh, mci);
            return 1;
        }
        }
    }
    return 0;
}

/* page thread: run queued commands. A chunk that ends a frame (and so presents) only runs when
   allow_present is set - the page passes 1 from requestAnimationFrame. Returns frames presented. */
EMSCRIPTEN_KEEPALIVE int port_gpu_pump(int allow_present) {
    int presented = 0;
    if (!g_ready) return 0;
    while (!g_rb_pending) {
        pthread_mutex_lock(&g_q_lock);
        Chunk *ch = g_q_head;
        pthread_mutex_unlock(&g_q_lock);
        if (!ch || (ch->ends_frame && !allow_present)) break;
        if (run_chunk(ch)) break;   /* readback in flight: resume when it is mapped */
        pthread_mutex_lock(&g_q_lock);
        g_q_head = ch->next;
        if (!g_q_head) g_q_tail = NULL;
        if (ch->ends_frame) g_q_frames--;
        pthread_mutex_unlock(&g_q_lock);
        if (ch->ends_frame) { presented++; allow_present = 0; }
        free(ch->buf);
        free(ch);
    }
    return presented;
}

EMSCRIPTEN_KEEPALIVE void port_set_canvas_size(int w, int h) { g_window_w = w; g_window_h = h; }

static void on_web_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2) {
    (void)u1; (void)u2;
    if (st != WGPURequestDeviceStatus_Success) { fprintf(stderr, "[wgpu] device: %.*s\n", (int)msg.length, msg.data); return; }
    g_dev = d;
    g_queue = wgpuDeviceGetQueue(g_dev);
    create_resources();
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
}

static void request_web_adapter(int attempt);
static void on_web_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2) {
    (void)u2;
    int attempt = (int)(intptr_t)u1;
    if (st != WGPURequestAdapterStatus_Success) {
        fprintf(stderr, "[wgpu] adapter (attempt %d): %.*s\n", attempt, (int)msg.length, msg.data);
        if (attempt < 2) request_web_adapter(attempt + 1);   /* default preference, then the software fallback */
        return;
    }
    if (attempt == 2) fprintf(stderr, "[wgpu] using the browser's software (fallback) adapter: expect it to be slow\n");
    g_adapter = a;
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    static const WGPUFeatureName bc = WGPUFeatureName_TextureCompressionBC;
    g_gpu_bc = wgpuAdapterHasFeature(g_adapter, bc) && !getenv("PORT_NO_BC");   /* DXT textures stay compressed */
    if (g_gpu_bc) { dd.requiredFeatureCount = 1; dd.requiredFeatures = &bc; }
    fprintf(stderr, "[wgpu] BC texture compression: %s\n", g_gpu_bc ? "yes" : "no (DXT textures are converted to RGBA8)");
    WGPURequestDeviceCallbackInfo dci = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    dci.mode = WGPUCallbackMode_AllowSpontaneous;
    dci.callback = on_web_device;
    wgpuAdapterRequestDevice(g_adapter, &dd, dci);
}

/* page thread: create the device for <canvas id="canvas"> (asynchronous; the game waits for it) */
EMSCRIPTEN_KEEPALIVE void port_gpu_init(int w, int h) {
    g_window_w = w; g_window_h = h;
    g_inst = wgpuCreateInstance(NULL);
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector cs = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    cs.selector = sv("#canvas");
    WGPUSurfaceDescriptor sd = WGPU_SURFACE_DESCRIPTOR_INIT;
    sd.nextInChain = &cs.chain;
    g_surface = wgpuInstanceCreateSurface(g_inst, &sd);
    request_web_adapter(0);
}
/* attempt 0: high-performance GPU; 1: any GPU; 2: the browser's software fallback adapter, if it has one */
static void request_web_adapter(int attempt) {
    WGPURequestAdapterOptions ao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    if (attempt == 0) ao.powerPreference = WGPUPowerPreference_HighPerformance;
    if (attempt == 2) ao.forceFallbackAdapter = 1;
    WGPURequestAdapterCallbackInfo aci = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    aci.mode = WGPUCallbackMode_AllowSpontaneous;
    aci.callback = on_web_adapter;
    aci.userdata1 = (void *)(intptr_t)attempt;
    wgpuInstanceRequestAdapter(g_inst, &ao, aci);
}
#endif
