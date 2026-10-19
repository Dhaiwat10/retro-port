/* Node test build: no page, no WebGPU executor. The game runs headless (PORT_HEADLESS is forced by
   node_env.js), so these are only here to satisfy the recorder's references. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
void web_queue_chunk(uint8_t *buf, size_t len, int ends_frame) { (void)len; (void)ends_frame; free(buf); }
int web_queued_frames(void) { return 0; }
int web_gpu_ready(void) { return 1; }
int g_gpu_bc;
size_t g_gpu_tex_bytes;
uint32_t g_gpu_bind_groups_made;
