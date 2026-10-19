/* The host window (SDL2) and input: runs on the process's main thread, as macOS requires.
 * Keyboard events become DirectInput scan codes (g_dik), Windows virtual-key state (g_keys) and
 * WM_KEYDOWN/WM_KEYUP messages for the game's window procedure. */
#include "win32.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_metal.h>
#include <objc/message.h>
#include <objc/runtime.h>

/* Opt out of App Nap: when the window is hidden, macOS otherwise stretches our 16 ms frame sleeps
   to ~100 ms and the game slows to a crawl. */
static void disable_app_nap(void) {
    id info = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
    id reason = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
                                                                  sel_registerName("stringWithUTF8String:"), "game");
    /* NSActivityUserInitiated | NSActivityLatencyCritical */
    id act = ((id (*)(id, SEL, uint64_t, id))objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                           0x00FFFFFFULL | 0xFF00000000ULL, reason);
    ((id (*)(id, SEL))objc_msgSend)(act, sel_registerName("retain"));
}

void *g_metal_layer;           /* CAMetalLayer the renderer draws into */
int g_window_w = 1024, g_window_h = 768;   /* drawable size in pixels */
static SDL_Window *g_window;

void input_key_hid(int usage, int down);   /* input.c */
void input_mouse_button(int b, int down);
void dinput_mouse_move(int dx, int dy);
void input_gamepad(int idx, int connected, uint32_t buttons, int lx, int ly, int rx, int ry);

/* Game controllers. Pads SDL has a GameController mapping for are read in that layout (= the W3C standard
   layout input_gamepad() takes). Others (older DirectInput-era pads such as the Logitech Precision) are
   read as raw joysticks: buttons in HID order, as Windows DirectInput presents them and as the game's
   controltypes.csv expects; axes 0/1 = left stick, 2/3 = right stick; hats = d-pad.
   Slots are DirectInput pad numbers; a controller keeps its slot until it is unplugged. */
#define PAD_SLOTS 4
static SDL_GameController *g_pad[PAD_SLOTS];
static SDL_Joystick *g_joy[PAD_SLOTS];   /* the slot's joystick (also set for GameControllers) */

/* raw button n -> standard-layout bit that host/dinput.c turns back into DirectInput button n */
static const int g_std_for_raw[12] = { 2, 0, 1, 3, 4, 5, 6, 7, 9, 8, 10, 11 };

static void pad_report(int slot) {
    SDL_GameController *gc = g_pad[slot];
    SDL_Joystick *js = g_joy[slot];
    if (!js) { input_gamepad(slot, 0, 0, 0, 0, 0, 0); return; }
    uint32_t b = 0;
    if (!gc) {
        int nb = SDL_JoystickNumButtons(js), na = SDL_JoystickNumAxes(js);
        for (int i = 0; i < nb && i < 12; i++)
            if (SDL_JoystickGetButton(js, i)) b |= 1u << g_std_for_raw[i];
        for (int h = 0; h < SDL_JoystickNumHats(js); h++) {
            Uint8 v = SDL_JoystickGetHat(js, h);
            if (v & SDL_HAT_UP) b |= 1u << 12;
            if (v & SDL_HAT_DOWN) b |= 1u << 13;
            if (v & SDL_HAT_LEFT) b |= 1u << 14;
            if (v & SDL_HAT_RIGHT) b |= 1u << 15;
        }
        int ax[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < na && i < 4; i++) ax[i] = SDL_JoystickGetAxis(js, i);
        input_gamepad(slot, 1, b, ax[0], ax[1], ax[2], ax[3]);
        return;
    }
    static const SDL_GameControllerButton order[17] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_GUIDE,
    };
    for (int i = 0; i < 17; i++)
        if (order[i] != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(gc, order[i])) b |= 1u << i;
    /* triggers are analogue (0..32767): pressed past a third of their travel */
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 10000) b |= 1u << 6;
    if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 10000) b |= 1u << 7;
    input_gamepad(slot, 1, b, SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX),
                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY),
                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX),
                  SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));
}
static int pad_slot_of(SDL_JoystickID id) {
    for (int i = 0; i < PAD_SLOTS; i++)
        if (g_joy[i] && SDL_JoystickInstanceID(g_joy[i]) == id) return i;
    return -1;
}
static void pad_added(int device_index) {
    if (pad_slot_of(SDL_JoystickGetDeviceInstanceID(device_index)) >= 0) return;   /* already open */
    for (int i = 0; i < PAD_SLOTS; i++)
        if (!g_joy[i]) {
            if (SDL_IsGameController(device_index)) {
                g_pad[i] = SDL_GameControllerOpen(device_index);
                g_joy[i] = g_pad[i] ? SDL_GameControllerGetJoystick(g_pad[i]) : NULL;
            } else {
                g_joy[i] = SDL_JoystickOpen(device_index);
            }
            if (!g_joy[i]) { fprintf(stderr, "controller: open failed: %s\n", SDL_GetError()); return; }
            fprintf(stderr, "controller %d: %s (%s, %d buttons, %d axes, %d hats)\n", i, SDL_JoystickName(g_joy[i]),
                    g_pad[i] ? "mapped" : "raw joystick", SDL_JoystickNumButtons(g_joy[i]),
                    SDL_JoystickNumAxes(g_joy[i]), SDL_JoystickNumHats(g_joy[i]));
            pad_report(i);
            return;
        }
}
static void pad_removed(SDL_JoystickID id) {
    int i = pad_slot_of(id);
    if (i < 0) return;
    if (g_pad[i]) SDL_GameControllerClose(g_pad[i]); else SDL_JoystickClose(g_joy[i]);
    g_pad[i] = NULL;
    g_joy[i] = NULL;
    fprintf(stderr, "controller %d disconnected\n", i);
    pad_report(i);
}

int window_create(const char *title) {
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    /* SDL's GameController.framework backend hides generic HID pads (e.g. the Logitech Precision) on current
       macOS; the IOKit and HIDAPI backends cover them and the common Xbox/PlayStation pads.
       SDL_JOYSTICK_MFI=1 in the environment turns it back on. */
    SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_window_w, g_window_h,
                                SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                (getenv("PORT_FULLSCREEN") ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_window) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); return 0; }
    SDL_RaiseWindow(g_window);
    disable_app_nap();
    SDL_MetalView view = SDL_Metal_CreateView(g_window);
    g_metal_layer = SDL_Metal_GetLayer(view);
    SDL_Metal_GetDrawableSize(g_window, &g_window_w, &g_window_h);
    /* fullscreen: relative mouse mode keeps the (hidden) system cursor from stopping at the edges */
    if (getenv("PORT_FULLSCREEN")) SDL_SetRelativeMouseMode(SDL_TRUE);
    fprintf(stderr, "window: %dx%d drawable\n", g_window_w, g_window_h);
    for (int i = 0; i < SDL_NumJoysticks(); i++) pad_added(i);   /* already connected (also re-sent as events) */
    return 1;
}

/* Runs forever on the main thread. */
void window_event_loop(void) {
    int scripted = getenv("PORT_INPUT") != NULL;   /* scripted runs ignore the real mouse and keyboard */
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 10)) continue;
        if (scripted && (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_MOUSEMOTION ||
                         e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP))
            continue;
        switch (e.type) {
        case SDL_QUIT:
            fprintf(stderr, "window closed\n");
            exit(0);
        case SDL_KEYDOWN: if (!e.key.repeat) input_key_hid((int)e.key.keysym.scancode, 1); break;
        case SDL_KEYUP: input_key_hid((int)e.key.keysym.scancode, 0); break;
        case SDL_MOUSEMOTION: dinput_mouse_move(e.motion.xrel, e.motion.yrel); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            input_mouse_button(b, e.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        /* the joystick events arrive for every pad (mapped ones also get CONTROLLER* events) */
        case SDL_JOYDEVICEADDED: pad_added(e.jdevice.which); break;
        case SDL_JOYDEVICEREMOVED: pad_removed(e.jdevice.which); break;
        case SDL_JOYAXISMOTION: case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYHATMOTION:
        case SDL_CONTROLLERAXISMOTION: case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            SDL_JoystickID id = e.type == SDL_JOYAXISMOTION ? e.jaxis.which : e.type == SDL_JOYHATMOTION ? e.jhat.which
                              : e.type == SDL_CONTROLLERAXISMOTION ? e.caxis.which
                              : e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP ? e.cbutton.which
                              : e.jbutton.which;
            int i = pad_slot_of(id);
            if (i >= 0 && !scripted) pad_report(i);
            break;
        }
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) SDL_Metal_GetDrawableSize(g_window, &g_window_w, &g_window_h);
            break;
        }
    }
}
