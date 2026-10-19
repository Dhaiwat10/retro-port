/* Browser/WebAssembly platform layer (Emscripten): the page owns the canvas (render_wgpu.c replays
 * frames there), input and audio come in through exported functions. */
#ifdef __EMSCRIPTEN__
#include "render.h"
#include "win32.h"

int g_window_w = 1024, g_window_h = 768;

int window_create(const char *title) { (void)title; return 0; }   /* 0: run headless */
void window_event_loop(void) {}


void watch_arm(uint32_t addr) { (void)addr; }   /* write watchpoints need signals: native debugging only */
#endif
