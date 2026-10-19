/* USER32.DLL: windows, messages, input state. For now there is one virtual window; a real host
 * window (and real input) arrives with the renderer. */
#include "win32.h"

#include <pthread.h>

#define W(name) static void w_##name(CPU *c)

int g_screen_w = 1024, g_screen_h = 768;  /* 4:3, matching the game's 640x480 HUD layout */

static uint32_t g_wndproc;       /* guest WNDPROC of the registered class */
static uint32_t g_window_created;
static uint32_t g_hook_proc;

/* Calls a guest stdcall function from host code on the current thread's CPU. */
uint32_t guest_stdcall(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t sp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(c, args[i]);
    PUSH32(c, GUEST_RETURN_SENTINEL);
    call_indirect(c, fn);
    if (c->esp != sp) {
        LOG("[user32] callback %08x left esp off by %d\n", fn, (int)(c->esp - sp));
        c->esp = sp;
    }
    return c->eax;
}

static uint32_t send_to_wndproc(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_wndproc) return 0;
    uint32_t a[4] = { hwnd, msg, wp, lp };
    return guest_stdcall(c, g_wndproc, 4, a);
}

/* ------------------------------------------------------------------ message queue */

typedef struct { uint32_t hwnd, msg, wp, lp; } Msg;
static Msg g_queue[256];
static int g_qhead, g_qtail;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    int next = (g_qtail + 1) % 256;
    if (next != g_qhead) { g_queue[g_qtail] = (Msg){ hwnd, msg, wp, lp }; g_qtail = next; }
    pthread_mutex_unlock(&g_qlock);
}

static int pop_message(Msg *m, int remove) {
    pthread_mutex_lock(&g_qlock);
    int have = g_qhead != g_qtail;
    if (have) { *m = g_queue[g_qhead]; if (remove) g_qhead = (g_qhead + 1) % 256; }
    pthread_mutex_unlock(&g_qlock);
    return have;
}

void (*g_pump_host_events)(void);  /* set by the window backend */

static void write_msg(uint32_t p, const Msg *m) {
    W32(p, m->hwnd); W32(p + 4, m->msg); W32(p + 8, m->wp); W32(p + 12, m->lp);
    W32(p + 16, (uint32_t)(guest_seconds() * 1000)); W32(p + 20, 0); W32(p + 24, 0);
}

W(PeekMessageA) {
    if (g_pump_host_events) g_pump_host_events();
    Msg m;
    if (!pop_message(&m, ARG(4) & 1 /* PM_REMOVE */)) RET(5, 0);
    write_msg(ARG(0), &m);
    RET(5, 1);
}
W(GetMessageA) {
    Msg m;
    while (!pop_message(&m, 1)) {
        if (g_pump_host_events) g_pump_host_events();
        if (sched_active()) { sched_wait(NULL, NULL, sched_now_us() + 2000, NULL); continue; }
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
    }
    write_msg(ARG(0), &m);
    RET(4, m.msg != 0x12 /* WM_QUIT */);
}
W(TranslateMessage) { RET(1, 0); }
W(DispatchMessageA) {
    uint32_t p = ARG(0);
    uint32_t r = send_to_wndproc(c, R32(p), R32(p + 4), R32(p + 8), R32(p + 12));
    RET(1, r);
}
W(PostMessageA) { post_message(ARG(0), ARG(1), ARG(2), ARG(3)); RET(4, 1); }
W(SendMessageA) {
    LOG("[user32] SendMessageA(msg %x)\n", ARG(1));
    if (ARG(1) == 0x80 /* WM_SETICON */) RET(4, 0);
    uint32_t r = send_to_wndproc(c, ARG(0), ARG(1), ARG(2), ARG(3));
    RET(4, r);
}
W(SendNotifyMessageA) { post_message(ARG(0), ARG(1), ARG(2), ARG(3)); RET(4, 1); }
W(PostQuitMessage) { post_message(MAIN_HWND, 0x12, ARG(0), 0); RET(1, 0); }
W(DefWindowProcA) { RET(4, 0); }

/* ------------------------------------------------------------------ window */

W(RegisterClassA) {
    g_wndproc = R32(ARG(0) + 4);  /* WNDCLASSA.lpfnWndProc */
    LOG("[user32] RegisterClassA(%s) wndproc=%08x\n", GSTR(R32(ARG(0) + 36)), g_wndproc);
    RET(1, 0xC001);
}
W(RegisterClassExA) {
    g_wndproc = R32(ARG(0) + 8);  /* WNDCLASSEXA.lpfnWndProc */
    LOG("[user32] RegisterClassExA wndproc=%08x\n", g_wndproc);
    RET(1, 0xC001);
}
W(UnregisterClassA) { RET(2, 1); }

W(CreateWindowExA) {
    LOG("[user32] CreateWindowExA(class %s, title %s, %dx%d)\n", ARG(1) > 0xffff ? GSTR(ARG(1)) : "#atom",
        ARG(2) ? GSTR(ARG(2)) : "", (int)ARG(6), (int)ARG(7));
    g_window_created = 1;
    /* Deliver the creation messages a real window would get. */
    uint32_t cs = heap_alloc(48, 1);  /* CREATESTRUCTA */
    W32(cs + 28, ARG(6)); W32(cs + 24, ARG(7)); W32(cs + 36, ARG(2)); W32(cs + 40, ARG(1)); W32(cs + 44, ARG(0));
    send_to_wndproc(c, MAIN_HWND, 0x81 /* WM_NCCREATE */, 0, cs);
    send_to_wndproc(c, MAIN_HWND, 0x01 /* WM_CREATE */, 0, cs);
    RET(12, MAIN_HWND);
}
W(DestroyWindow) { RET(1, 1); }
W(ShowWindow) {
    if (ARG(1)) {
        send_to_wndproc(c, MAIN_HWND, 0x18 /* WM_SHOWWINDOW */, 1, 0);
        send_to_wndproc(c, MAIN_HWND, 0x06 /* WM_ACTIVATE */, 1, 0);
        send_to_wndproc(c, MAIN_HWND, 0x07 /* WM_SETFOCUS */, 0, 0);
    }
    RET(2, 0);
}
W(UpdateWindow) { RET(1, 1); }
W(MoveWindow) { RET(6, 1); }
W(SetWindowPos) { RET(7, 1); }
W(SetWindowTextA) { LOG("[user32] SetWindowTextA(%s)\n", GSTR(ARG(1))); RET(2, 1); }
W(GetWindowLongA) {
    int32_t idx = (int32_t)ARG(1);
    RET(2, idx == -16 /* GWL_STYLE */ ? 0x10CF0000u : idx == -4 /* GWL_WNDPROC */ ? g_wndproc : 0);
}
W(FindWindowA) { RET(2, 0); }  /* no other instance running */
W(GetDesktopWindow) { RET(0, 0x00010001u); }
W(GetForegroundWindow) { RET(0, MAIN_HWND); }
W(SetForegroundWindow) { RET(1, 1); }
W(SetActiveWindow) { RET(1, MAIN_HWND); }
W(SetFocus) { RET(1, MAIN_HWND); }
W(IsIconic) { RET(1, 0); }
W(GetClientRect) {
    uint32_t r = ARG(1);
    W32(r, 0); W32(r + 4, 0); W32(r + 8, (uint32_t)g_screen_w); W32(r + 12, (uint32_t)g_screen_h);
    RET(2, 1);
}
W(AdjustWindowRect) { RET(3, 1); }
W(AdjustWindowRectEx) { RET(4, 1); }
W(ClientToScreen) { RET(2, 1); }
W(ScreenToClient) { RET(2, 1); }
W(SetRect) {
    uint32_t r = ARG(0);
    W32(r, ARG(1)); W32(r + 4, ARG(2)); W32(r + 8, ARG(3)); W32(r + 12, ARG(4));
    RET(5, 1);
}
W(BeginPaint) { memset(HP(ARG(1)), 0, 64); RET(2, 0x00020001u); }
W(EndPaint) { RET(2, 1); }
W(GetSystemMetrics) {
    switch (ARG(0)) {
    case 0: RET(1, g_screen_w);      /* SM_CXSCREEN */
    case 1: RET(1, g_screen_h);      /* SM_CYSCREEN */
    case 4: RET(1, 19);              /* SM_CYCAPTION */
    case 5: case 6: RET(1, 1);       /* borders */
    case 32: case 33: RET(1, 4);     /* SM_CXFRAME / SM_CYFRAME */
    default: RET(1, 0);
    }
}
W(SystemParametersInfoA) { RET(4, 1); }
W(MessageBoxA) {
    fprintf(stderr, "[MessageBox] %s: %s\n", ARG(2) ? GSTR(ARG(2)) : "", ARG(1) ? GSTR(ARG(1)) : "");
    RET(4, 1 /* IDOK */);
}

/* ------------------------------------------------------------------ cursor, icons */

W(LoadCursorA) { RET(2, 0x00030001u); }
W(LoadIconA) { RET(2, 0x00030002u); }
W(LoadImageA) { RET(6, 0x00030003u); }
W(DestroyIcon) { RET(1, 1); }
W(SetCursor) { RET(1, 0); }
W(ShowCursor) { RET(1, ARG(0) ? 0 : -1); }
static int32_t g_mouse_x = 512, g_mouse_y = 384;
W(GetCursorPos) { W32(ARG(0), (uint32_t)g_mouse_x); W32(ARG(0) + 4, (uint32_t)g_mouse_y); RET(1, 1); }
W(SetCursorPos) { g_mouse_x = (int32_t)ARG(0); g_mouse_y = (int32_t)ARG(1); RET(2, 1); }

/* ------------------------------------------------------------------ keyboard */

uint8_t g_keys[256];  /* virtual-key state, filled by the window backend */
W(GetAsyncKeyState) { RET(1, g_keys[ARG(0) & 0xff] ? 0x8000 : 0); }
W(GetKeyState) { RET(1, g_keys[ARG(0) & 0xff] ? 0xFF80 : 0); }
W(GetKeyboardState) { for (int i = 0; i < 256; i++) W8(ARG(0) + i, g_keys[i] ? 0x80 : 0); RET(1, 1); }
W(GetKeyboardLayout) { RET(1, 0x08090809u); }
W(MapVirtualKeyExA) { RET(3, ARG(0)); }
W(ToAsciiEx) {
    uint32_t vk = ARG(0);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == ' ') { W16(ARG(3), (uint16_t)vk); RET(6, 1); }
    RET(6, 0);
}
W(keybd_event) { RET(4, 0); }
W(CharUpperA) {
    uint32_t p = ARG(0);
    if (p < 0x10000) RET(1, (uint32_t)toupper((int)p));
    for (uint32_t q = p; R8(q); q++) W8(q, (uint8_t)toupper(R8(q)));
    RET(1, p);
}
W(SetWindowsHookExA) { g_hook_proc = ARG(1); LOG("[user32] SetWindowsHookExA(%d)\n", (int)ARG(0)); RET(4, 0x00040001u); }
W(UnhookWindowsHookEx) { RET(1, 1); }
W(CallNextHookEx) { RET(4, 0); }

#define U(name) IMPORT("USER32.DLL", name)
static const ImportHandler list[] = {
    U(PeekMessageA), U(GetMessageA), U(TranslateMessage), U(DispatchMessageA), U(PostMessageA), U(SendMessageA),
    U(SendNotifyMessageA), U(PostQuitMessage), U(DefWindowProcA), U(RegisterClassA), U(RegisterClassExA),
    U(UnregisterClassA), U(CreateWindowExA), U(DestroyWindow), U(ShowWindow), U(UpdateWindow), U(MoveWindow),
    U(SetWindowPos), U(SetWindowTextA), U(GetWindowLongA), U(FindWindowA), U(GetDesktopWindow),
    U(GetForegroundWindow), U(SetForegroundWindow), U(SetActiveWindow), U(SetFocus), U(IsIconic),
    U(GetClientRect), U(AdjustWindowRect), U(AdjustWindowRectEx), U(ClientToScreen), U(ScreenToClient),
    U(SetRect), U(BeginPaint), U(EndPaint), U(GetSystemMetrics), U(SystemParametersInfoA), U(MessageBoxA),
    U(LoadCursorA), U(LoadIconA), U(LoadImageA), U(DestroyIcon), U(SetCursor), U(ShowCursor), U(GetCursorPos),
    U(SetCursorPos), U(GetAsyncKeyState), U(GetKeyState), U(GetKeyboardState), U(GetKeyboardLayout),
    U(MapVirtualKeyExA), U(ToAsciiEx), U(keybd_event), U(CharUpperA), U(SetWindowsHookExA),
    U(UnhookWindowsHookEx), U(CallNextHookEx),
};
const ImportTable user32_imports = { list, sizeof list / sizeof list[0] };
