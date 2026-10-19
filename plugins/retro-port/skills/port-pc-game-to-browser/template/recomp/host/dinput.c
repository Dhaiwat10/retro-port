/* DirectInput (DirectX 7 era): keyboard, mouse and gamepad devices. Device state comes from g_keys / the
 * window backend; until there is a real window everything reads as "nothing pressed". */
#include "com.h"

#include <pthread.h>

#define W(name) static void name(CPU *c)

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOYSTICK = 3 };
/* Joystick axis ranges (DIPROP_RANGE/DEADZONE) are per axis, indexed by DIJOYSTATE offset / 4 (X Y Z Rx Ry Rz). */
typedef struct { int kind, pad; uint32_t format_size; int acquired; int32_t lo[6], hi[6], dead[6]; } DIDev;

static const uint8_t GUID_SysMouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7,
                                           0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7,
                                              0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };

/* DirectInput keyboard state is indexed by DIK scan codes; the window backend fills this. */
uint8_t g_dik[256];
int32_t g_mouse_dx, g_mouse_dy, g_mouse_buttons;
/* A press counts as held for at least 50 ms: the game samples state once per frame, so a very quick
   tap (or an automated click) would otherwise fall between two samples. */
static double g_dik_down_at[256], g_button_down_at[4];
#define MIN_HOLD (deterministic() ? 0.0 : 0.05)   /* deterministic runs: state is exactly what was pressed */

/* Buffered input (GetDeviceData): DIDEVICEOBJECTDATA records per device kind. */
typedef struct { uint32_t ofs, data, time, seq; } DIEvent;
static DIEvent g_queue[4][256];   /* by device kind; joysticks are polled only, so theirs stays empty */
static int g_qhead[4], g_qtail[4];
static uint32_t g_seq;
static pthread_mutex_t g_di_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_di_lock_pads = PTHREAD_MUTEX_INITIALIZER;

/* Gamepads: always-present virtual joysticks (the game enumerates devices only once, at startup, so a
 * controller connected later must already exist). Each one is presented as a "Logitech Dual Action USB",
 * which Data/controltypes.csv maps as a dual-analog pad: buttons 1-4 = Square Cross Circle Triangle,
 * 5-8 = L1 R1 L2 R2, 9 = Start, 10 = Select, 11-12 = L3 R3; left stick X/Y, right stick Z/Rz, d-pad POV.
 * The feeders (SDL natively, the Gamepad API in the browser) report the W3C "standard gamepad" layout
 * through input_gamepad() in input.c. PORT_PADS (default 1, at most 4) sets how many are exposed. */
#define MAX_PADS 4
typedef struct { int connected; uint32_t buttons; int32_t axis[4]; } PadState;  /* axes -32768..32767 */
static PadState g_pads[MAX_PADS];
static int g_npads = -1;
static int num_pads(void) {
    if (g_npads < 0) {
        const char *e = getenv("PORT_PADS");
        g_npads = e ? atoi(e) : 1;
        if (g_npads < 0) g_npads = 0;
        if (g_npads > MAX_PADS) g_npads = MAX_PADS;
    }
    return g_npads;
}
void dinput_pad(int idx, int connected, uint32_t std_buttons, const int32_t axis[4]) {
    if (idx < 0 || idx >= MAX_PADS) return;
    pthread_mutex_lock(&g_di_lock_pads);
    if (connected && !g_pads[idx].connected) LOG("[dinput] gamepad %d connected\n", idx);
    if (!connected && g_pads[idx].connected) LOG("[dinput] gamepad %d disconnected\n", idx);
    g_pads[idx].connected = connected;
    g_pads[idx].buttons = connected ? std_buttons : 0;
    for (int i = 0; i < 4; i++) g_pads[idx].axis[i] = connected ? axis[i] : 0;
    pthread_mutex_unlock(&g_di_lock_pads);
}
/* The pad identity comes from port.json (default: Logitech Dual Action, USB 046D:C216). DirectInput product
   GUIDs are {PIDVID-0000-0000-0000-"PIDVID"}. */
static void pad_guid(uint8_t *g, int instance, int idx) {
    static const uint8_t product[16] = { PORT_PAD_VID & 0xff, PORT_PAD_VID >> 8, PORT_PAD_PID & 0xff, PORT_PAD_PID >> 8,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 };
    static const uint8_t inst[16] = { 0x00, 0x7A, 0x6A, 0x52, 0x06, 0x00, 0xD1, 0x11, 0x80, 0x01,
                                      0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    memcpy(g, instance ? inst : product, 16);
    if (instance) g[0] = (uint8_t)idx;
}
static int pad_from_guid(const uint8_t *g) {
    uint8_t t[16];
    for (int i = 0; i < num_pads(); i++) {
        pad_guid(t, 1, i);
        if (!memcmp(g, t, 16)) return i;
    }
    pad_guid(t, 0, 0);
    return !memcmp(g, t, 16) ? 0 : -1;   /* some games create by product GUID */
}
#define PAD_NAME PORT_PAD_NAME   /* port.json: ideally a name the game's own controller table matches */
#define PAD_DEVTYPE 0x00010404u   /* DIDEVTYPE_JOYSTICK | DIDEVTYPEJOYSTICK_GAMEPAD << 8 | DIDEVTYPE_HID */
#define PAD_BUTTONS 12


static void push(int kind, uint32_t ofs, uint32_t data) {
    pthread_mutex_lock(&g_di_lock);
    int next = (g_qtail[kind] + 1) % 256;
    if (next != g_qhead[kind]) {
        g_queue[kind][g_qtail[kind]] = (DIEvent){ ofs, data, (uint32_t)(guest_seconds() * 1000), ++g_seq };
        g_qtail[kind] = next;
    }
    pthread_mutex_unlock(&g_di_lock);
}
void dinput_key(uint8_t dik, int down) {
    g_dik[dik] = (uint8_t)down;
    if (down) g_dik_down_at[dik] = host_seconds();
    push(1, dik, down ? 0x80 : 0);
}
void dinput_mouse_button(int b, int down) {
    if (down) { g_mouse_buttons |= 1 << b; if (b < 4) g_button_down_at[b] = host_seconds(); }
    else g_mouse_buttons &= ~(1 << b);
    push(2, 12u + (uint32_t)b, down ? 0x80 : 0);
}
void dinput_mouse_move(int dx, int dy) {
    g_mouse_dx += dx; g_mouse_dy += dy;
    if (dx) push(2, 0, (uint32_t)dx);
    if (dy) push(2, 4, (uint32_t)dy);
}

static ComInterface IDirectInputDevice_i;

/* ------------------------------------------------------------------ joystick objects */

typedef struct { uint32_t ofs; uint8_t guid0; uint32_t type; const char *name; uint16_t page, usage; } PadObj;
/* guid0: last byte of the low word of DirectInput's object GUIDs {A36D02xx-C9F3-11CF-BFC7-444553540000} */
static const PadObj g_pad_axes[] = {
    {  0, 0xE0, 2u | 0u << 8, "X Axis", 1, 0x30 },        /* DIDFT_ABSAXIS | instance */
    {  4, 0xE1, 2u | 1u << 8, "Y Axis", 1, 0x31 },
    {  8, 0xE2, 2u | 2u << 8, "Z Axis", 1, 0x32 },
    { 20, 0xE3, 2u | 3u << 8, "Z Rotation", 1, 0x35 },
    { 32, 0xF2, 0x10u | 0u << 8, "Hat Switch", 1, 0x39 }, /* DIDFT_POV */
};
#define PAD_NOBJ (5 + PAD_BUTTONS)
static PadObj pad_obj(int i) {
    if (i < 5) return g_pad_axes[i];
    static char names[PAD_BUTTONS][16];
    int b = i - 5;
    snprintf(names[b], sizeof names[b], "Button %d", b + 1);
    return (PadObj){ 48u + (uint32_t)b, 0xF0, 4u | (uint32_t)b << 8 /* DIDFT_PSHBUTTON */, names[b], 9, (uint16_t)(b + 1) };
}
static void write_object_instance(uint32_t p, PadObj o) {
    uint32_t sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    static const uint8_t base[16] = { 0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    memcpy(HP(p + 4), base, 16);
    W8(p + 4, o.guid0);
    W32(p + 20, o.ofs);
    W32(p + 24, o.type);
    snprintf((char *)HP(p + 32), 260, "%s", o.name);
    if (sz >= 308) { W16(p + 304, o.page); W16(p + 306, o.usage); }   /* DirectX 5+ fields */
}
/* Axis slot (DIJOYSTATE offset / 4) addressed by a property header's dwObj/dwHow; -1 = whole device. */
static int pad_axis_slot(uint32_t obj, uint32_t how) {
    if (how == 0) return -1;                                      /* DIPH_DEVICE */
    if (how == 1) return obj < 24 && obj % 4 == 0 ? (int)(obj / 4) : -2;   /* DIPH_BYOFFSET */
    if (how == 2) {                                               /* DIPH_BYID */
        if ((obj & 3) == 0) return -2;
        static const int inst_slot[4] = { 0, 1, 2, 5 };
        uint32_t inst = (obj >> 8) & 0xFFFF;
        return inst < 4 ? inst_slot[inst] : -2;
    }
    return -2;
}
static int32_t pad_axis_value(const DIDev *d, int slot, int32_t raw) {
    int32_t mag = raw < 0 ? -raw : raw;
    if (d->dead[slot] > 0 && (int64_t)mag * 10000 / 32768 < d->dead[slot]) raw = 0;
    return d->lo[slot] + (int32_t)(((int64_t)raw + 32768) * ((int64_t)d->hi[slot] - d->lo[slot]) / 65535);
}
/* W3C standard gamepad buttons -> the Dual Action's DirectInput buttons (controltypes.csv order) */
static const int g_std_for_di[PAD_BUTTONS] = { 2, 0, 1, 3, 4, 5, 6, 7, 9, 8, 10, 11 };
static void pad_write_state(const DIDev *d, uint32_t p, uint32_t n) {
    PadState st;
    pthread_mutex_lock(&g_di_lock_pads);
    st = g_pads[d->pad];
    pthread_mutex_unlock(&g_di_lock_pads);
    uint8_t buf[272];
    memset(buf, 0, sizeof buf);
    int32_t v[6] = { st.axis[0], st.axis[1], st.axis[2], 0, 0, st.axis[3] };
    /* The game takes digital directions for this pad type from the left stick and ignores the POV hat,
       so the d-pad also deflects the left stick fully. */
    if (st.buttons >> 12 & 1) v[1] = -32768;
    if (st.buttons >> 13 & 1) v[1] = 32767;
    if (st.buttons >> 14 & 1) v[0] = -32768;
    if (st.buttons >> 15 & 1) v[0] = 32767;
    for (int k = 0; k < 6; k++) { int32_t a = pad_axis_value(d, k, v[k]); memcpy(buf + 4 * k, &a, 4); }
    uint32_t pov = 0xFFFFFFFFu;
    int up = st.buttons >> 12 & 1, down = st.buttons >> 13 & 1, left = st.buttons >> 14 & 1, right = st.buttons >> 15 & 1;
    if (up || down || left || right) {
        int dx = right - left, dy = down - up;
        static const uint32_t dirs[3][3] = { { 31500, 0, 4500 }, { 27000, 0xFFFFFFFFu, 9000 }, { 22500, 18000, 13500 } };
        pov = dirs[dy + 1][dx + 1];
    }
    memcpy(buf + 32, &pov, 4);
    memset(buf + 36, 0xFF, 12);   /* POVs 1-3 centred */
    for (int b = 0; b < PAD_BUTTONS; b++) buf[48 + b] = (st.buttons >> g_std_for_di[b] & 1) ? 0x80 : 0;
    memcpy(HP(p), buf, n < sizeof buf ? n : sizeof buf);
}

/* ------------------------------------------------------------------ IDirectInputDevice */

W(dev_GetCapabilities) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1);
    uint32_t sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    W32(p + 4, 1 /* DIDC_ATTACHED */);
    if (d && d->kind == DEV_JOYSTICK) {
        W32(p + 4, 1 | 2 /* DIDC_ATTACHED | DIDC_POLLEDDEVICE */);
        W32(p + 8, PAD_DEVTYPE); W32(p + 12, 4); W32(p + 16, PAD_BUTTONS); W32(p + 20, 1);
        RET(2, S_OK);
    }
    W32(p + 8, d && d->kind == DEV_KEYBOARD ? 0x0403 : 0x0202);  /* dev type */
    W32(p + 12, d && d->kind == DEV_KEYBOARD ? 0 : 3);            /* axes */
    W32(p + 16, d && d->kind == DEV_KEYBOARD ? 128 : 4);          /* buttons */
    RET(2, S_OK);
}
W(dev_EnumObjects) {
    DIDev *d = com_data(THIS);
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3) & 0xFF;
    if (!d || d->kind != DEV_JOYSTICK) RET(4, S_OK);
    uint32_t p = heap_alloc(316, 1);
    for (int i = 0; i < PAD_NOBJ; i++) {
        PadObj o = pad_obj(i);
        if (flags && !(o.type & flags)) continue;   /* DIDFT_ALL = 0, else DIDFT_AXIS / BUTTON / POV masks */
        W32(p, 316);
        write_object_instance(p, o);
        uint32_t a[2] = { p, ref };
        if (!guest_stdcall(c, cb, 2, a)) break;     /* DIENUM_STOP */
    }
    heap_free(p);
    RET(4, S_OK);
}
/* Property GUIDs are small integers in DirectInput (MAKEDIPROPERTY): 4 = RANGE, 5 = DEADZONE. */
W(dev_GetProperty) {
    DIDev *d = com_data(THIS);
    uint32_t prop = ARG(1), p = ARG(2);
    if (d && d->kind == DEV_JOYSTICK && (prop == 4 || prop == 5)) {
        int slot = pad_axis_slot(R32(p + 8), R32(p + 12));
        if (slot == -2) RET(3, 0x80070002u /* DIERR_OBJECTNOTFOUND */);
        if (slot < 0) slot = 0;
        if (prop == 4) { W32(p + 16, (uint32_t)d->lo[slot]); W32(p + 20, (uint32_t)d->hi[slot]); }
        else W32(p + 16, (uint32_t)d->dead[slot]);
        RET(3, S_OK);
    }
    RET(3, 0x80070057u /* E_INVALIDARG -> use defaults */);
}
W(dev_SetProperty) {
    DIDev *d = com_data(THIS);
    uint32_t prop = ARG(1), p = ARG(2);
    if (d && d->kind == DEV_JOYSTICK && (prop == 4 || prop == 5)) {
        int slot = pad_axis_slot(R32(p + 8), R32(p + 12));
        if (slot == -2) RET(3, 0x80070002u);
        for (int k = 0; k < 6; k++) {
            if (slot >= 0 && k != slot) continue;
            if (prop == 4) { d->lo[k] = (int32_t)R32(p + 16); d->hi[k] = (int32_t)R32(p + 20); }
            else d->dead[k] = (int32_t)R32(p + 16);
        }
        LOG("[dinput] pad %d %s(slot %d) = %d %d\n", d->pad, prop == 4 ? "RANGE" : "DEADZONE", slot,
            (int)R32(p + 16), prop == 4 ? (int)R32(p + 20) : 0);
    }
    RET(3, S_OK);
}
W(dev_Acquire) { DIDev *d = com_data(THIS); if (d) d->acquired = 1; LOG("[dinput] Acquire\n"); RET(1, S_OK); }
W(dev_Unacquire) { DIDev *d = com_data(THIS); if (d) d->acquired = 0; RET(1, S_OK); }
W(dev_GetDeviceState) {
    DIDev *d = com_data(THIS);
    { static int n; if (n++ < 2) LOG("[dinput] GetDeviceState(%s, %u bytes)\n", d && d->kind == DEV_KEYBOARD ? "keyboard" : "mouse", ARG(1)); }
    uint32_t n = ARG(1), p = ARG(2);
    memset(HP(p), 0, n);
    if (d && d->kind == DEV_KEYBOARD) {
        double now = host_seconds();
        for (uint32_t i = 0; i < n && i < 256; i++)
            W8(p + i, g_dik[i] || now - g_dik_down_at[i] < MIN_HOLD ? 0x80 : 0);
    } else if (d && d->kind == DEV_JOYSTICK) {
        pad_write_state(d, p, n);
    } else if (d && d->kind == DEV_MOUSE && n >= 16) {
        W32(p, (uint32_t)g_mouse_dx); W32(p + 4, (uint32_t)g_mouse_dy);
        g_mouse_dx = g_mouse_dy = 0;
        double now = host_seconds();
        for (int b = 0; b < 4; b++)
            W8(p + 12 + b, ((g_mouse_buttons >> b) & 1) || now - g_button_down_at[b] < MIN_HOLD ? 0x80 : 0);
    }
    RET(3, S_OK);
}
W(dev_GetDeviceData) {
    DIDev *d = com_data(THIS);
    { static int n; if (n++ < 2) LOG("[dinput] GetDeviceData(%s)\n", d && d->kind == DEV_KEYBOARD ? "keyboard" : "mouse"); }
    uint32_t size = ARG(1), out = ARG(2), pn = ARG(3), peek = ARG(4) & 1;
    int kind = d ? d->kind : 0;
    uint32_t max = pn ? R32(pn) : 0, n = 0;
    pthread_mutex_lock(&g_di_lock);
    int h = g_qhead[kind];
    while (h != g_qtail[kind] && n < max) {
        if (out) {
            memset(HP(out + n * size), 0, size);
            memcpy(HP(out + n * size), &g_queue[kind][h], size < 16 ? size : 16);
        }
        h = (h + 1) % 256;
        n++;
    }
    if (!peek) g_qhead[kind] = h;
    pthread_mutex_unlock(&g_di_lock);
    if (pn) W32(pn, n);
    RET(5, S_OK);
}
W(dev_SetDataFormat) {
    DIDev *d = com_data(THIS);
    if (d) d->format_size = R32(ARG(1) + 12);
    /* Joysticks: only the standard c_dfDIJoystick / c_dfDIJoystick2 layouts are supported */
    if (d && d->kind == DEV_JOYSTICK) {
        LOG("[dinput] pad %d SetDataFormat(%u bytes, %u objects)\n", d->pad, d->format_size, R32(ARG(1) + 16));
        if (d->format_size != 80 && d->format_size != 272)
            fprintf(stderr, "[dinput] unsupported joystick data format (%u bytes)\n", d->format_size);
    }
    RET(2, S_OK);
}
W(dev_SetEventNotification) { RET(2, S_OK); }
W(dev_SetCooperativeLevel) { RET(3, S_OK); }
static void guid_axis(uint8_t *g, int n) {   /* GUID_XAxis/YAxis/ZAxis = {A36D02E0+n-C9F3-11CF-BFC7-444553540000} */
    static const uint8_t base[16] = { 0xE0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    memcpy(g, base, 16);
    g[0] = (uint8_t)(0xE0 + n);
}
W(dev_GetObjectInfo) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1), obj = ARG(2), how = ARG(3);
    uint32_t sz = R32(p);
    LOG("[dinput] GetObjectInfo(%s, obj %u, how %u)\n", !d ? "?" : d->kind == DEV_KEYBOARD ? "keyboard" : d->kind == DEV_JOYSTICK ? "gamepad" : "mouse", obj, how);
    uint32_t ofs = obj;
    if (d && d->kind == DEV_JOYSTICK) {
        for (int i = 0; i < PAD_NOBJ; i++) {
            PadObj o = pad_obj(i);
            if ((how == 1 && o.ofs == obj) || (how == 2 && (o.type & 0xFFFF1F) == (obj & 0xFFFF1F))) {
                write_object_instance(p, o);
                RET(4, S_OK);
            }
        }
        RET(4, 0x80070002u);
    }
    if (how != 1 /* DIPH_BYOFFSET */) RET(4, 0x80070002u /* DIERR_OBJECTNOTFOUND */);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    uint8_t guid[16];
    uint32_t type;
    char name[32];
    if (d && d->kind == DEV_MOUSE) {
        if (ofs <= 8 && ofs % 4 == 0) {
            guid_axis(guid, (int)(ofs / 4));
            type = 1u | ((ofs / 4) << 8);                   /* DIDFT_RELAXIS | instance */
            snprintf(name, sizeof name, "%c Axis", "XYZ"[ofs / 4]);
        } else if (ofs >= 12 && ofs < 16) {
            static const uint8_t button[16] = { 0xF0, 0x02, 0x6D, 0xA3, 0xF3, 0xC9, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
            memcpy(guid, button, 16);
            type = 4u | ((ofs - 12) << 8);                  /* DIDFT_PSHBUTTON | instance */
            snprintf(name, sizeof name, "Button %u", ofs - 12);
        } else RET(4, 0x80070002u);
    } else {
        if (ofs > 255) RET(4, 0x80070002u);
        static const uint8_t key[16] = { 0x20, 0x82, 0x72, 0x55, 0x3C, 0xD3, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
        memcpy(guid, key, 16);
        type = 0xC | (ofs << 8);                            /* DIDFT_BUTTON | instance */
        snprintf(name, sizeof name, "Key %u", ofs);
    }
    memcpy(HP(p + 4), guid, 16);
    W32(p + 20, ofs);
    W32(p + 24, type);
    snprintf((char *)HP(p + 32), 260, "%s", name);
    RET(4, S_OK);
}
W(dev_GetDeviceInfo) {
    DIDev *d = com_data(THIS);
    uint32_t p = ARG(1), sz = R32(p);
    memset(HP(p + 4), 0, sz > 4 ? sz - 4 : 0);
    if (d && d->kind == DEV_JOYSTICK) {
        pad_guid(HP(p + 4), 1, d->pad);
        pad_guid(HP(p + 20), 0, d->pad);
        W32(p + 36, PAD_DEVTYPE);
        strcpy((char *)HP(p + 40), PAD_NAME);
        strcpy((char *)HP(p + 300), PAD_NAME);
        if (sz >= 580) { W16(p + 576, 1); W16(p + 578, 5); }   /* HID usage page 1, usage 5 = game pad */
        RET(2, S_OK);
    }
    int kb = d && d->kind == DEV_KEYBOARD;
    memcpy(HP(p + 4), kb ? GUID_SysKeyboard : GUID_SysMouse, 16);
    memcpy(HP(p + 20), kb ? GUID_SysKeyboard : GUID_SysMouse, 16);
    W32(p + 36, kb ? 0x0403 : 0x0202);
    strcpy((char *)HP(p + 40), kb ? "Keyboard" : "Mouse");
    strcpy((char *)HP(p + 300), kb ? "Keyboard" : "Mouse");
    RET(2, S_OK);
}
W(dev_RunControlPanel) { RET(3, S_OK); }
W(dev_Initialize) { RET(4, S_OK); }
W(dev_Poll) { RET(1, S_OK); }
#define DIERR_UNSUPPORTED 0x80004001u
W(dev_CreateEffect) { if (ARG(3)) W32(ARG(3), 0); RET(5, DIERR_UNSUPPORTED); }
W(dev_EnumEffects) { RET(4, S_OK); }
W(dev_GetEffectInfo) { RET(3, DIERR_UNSUPPORTED); }
W(dev_GetForceFeedbackState) { RET(2, DIERR_UNSUPPORTED); }
W(dev_SendForceFeedbackCommand) { RET(2, DIERR_UNSUPPORTED); }
W(dev_EnumCreatedEffectObjects) { RET(4, S_OK); }

static const ComMethod didev_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCapabilities", 1, dev_GetCapabilities }, { "EnumObjects", 3, dev_EnumObjects },
    { "GetProperty", 2, dev_GetProperty }, { "SetProperty", 2, dev_SetProperty }, { "Acquire", 0, dev_Acquire },
    { "Unacquire", 0, dev_Unacquire }, { "GetDeviceState", 2, dev_GetDeviceState },
    { "GetDeviceData", 4, dev_GetDeviceData }, { "SetDataFormat", 1, dev_SetDataFormat },
    { "SetEventNotification", 1, dev_SetEventNotification }, { "SetCooperativeLevel", 2, dev_SetCooperativeLevel },
    { "GetObjectInfo", 3, dev_GetObjectInfo }, { "GetDeviceInfo", 1, dev_GetDeviceInfo },
    { "RunControlPanel", 2, dev_RunControlPanel }, { "Initialize", 3, dev_Initialize },
    /* IDirectInputDevice2 / 7 */
    /* no force feedback: the pads don't report DIDC_FORCEFEEDBACK */
    { "CreateEffect", 4, dev_CreateEffect }, { "EnumEffects", 3, dev_EnumEffects }, { "GetEffectInfo", 2, dev_GetEffectInfo },
    { "GetForceFeedbackState", 1, dev_GetForceFeedbackState }, { "SendForceFeedbackCommand", 1, dev_SendForceFeedbackCommand },
    { "EnumCreatedEffectObjects", 3, dev_EnumCreatedEffectObjects }, { "Escape", 1, NULL }, { "Poll", 0, dev_Poll },
    { "SendDeviceData", 4, NULL }, { "EnumEffectsInFile", 4, NULL }, { "WriteEffectToFile", 4, NULL },
};
static ComInterface IDirectInputDevice_i = { "IDirectInputDeviceA", didev_methods, 29, 0 };

/* ------------------------------------------------------------------ IDirectInput */

static uint32_t create_device(const uint8_t *guid) {
    DIDev *d = calloc(1, sizeof *d);
    d->kind = !memcmp(guid, GUID_SysKeyboard, 16) ? DEV_KEYBOARD : !memcmp(guid, GUID_SysMouse, 16) ? DEV_MOUSE : 0;
    int pad = d->kind ? -1 : pad_from_guid(guid);
    if (pad >= 0) {
        d->kind = DEV_JOYSTICK;
        d->pad = pad;
        for (int k = 0; k < 6; k++) { d->lo[k] = 0; d->hi[k] = 65535; }   /* DirectInput's default range */
    }
    if (!d->kind) { free(d); return 0; }
    if (pad >= 0) LOG("[dinput] CreateDevice(gamepad %d)\n", pad);
    else LOG("[dinput] CreateDevice(%s)\n", d->kind == DEV_KEYBOARD ? "keyboard" : "mouse");
    return com_new(&IDirectInputDevice_i, 0, d);
}

W(di_CreateDevice) {
    uint32_t dev = create_device(HP(ARG(1)));
    W32(ARG(2), dev);
    RET(4, dev ? S_OK : 0x80040154u /* DIERR_DEVICENOTREG */);
}
W(di_CreateDeviceEx) {
    uint32_t dev = create_device(HP(ARG(1)));
    W32(ARG(3), dev);
    RET(5, dev ? S_OK : 0x80040154u);
}

/* Returns 0 when the callback asks to stop (DIENUM_STOP). */
static int enum_one(CPU *c, uint32_t cb, uint32_t ref, const uint8_t *guid, const uint8_t *product, uint32_t type,
                    const char *name) {
    uint32_t p = heap_alloc(580, 1);
    W32(p, 580);
    memcpy(HP(p + 4), guid, 16);
    memcpy(HP(p + 20), product, 16);
    W32(p + 36, type);
    strcpy((char *)HP(p + 40), name);
    strcpy((char *)HP(p + 300), name);
    if (type == PAD_DEVTYPE) { W16(p + 576, 1); W16(p + 578, 5); }
    uint32_t a[2] = { p, ref };
    uint32_t r = guest_stdcall(c, cb, 2, a);
    heap_free(p);
    return r != 0;
}
W(di_EnumDevices) {
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3);
    LOG("[dinput] EnumDevices(type %u)\n", type);
    /* same order as Windows: system mouse first, then keyboard (the game's controller slots follow it) */
    uint32_t flags = ARG(4);
    if ((type == 0 || type == 2) && !enum_one(c, cb, ref, GUID_SysMouse, GUID_SysMouse, 0x0202, "Mouse")) RET(5, S_OK);
    if ((type == 0 || type == 3) && !enum_one(c, cb, ref, GUID_SysKeyboard, GUID_SysKeyboard, 0x0403, "Keyboard")) RET(5, S_OK);
    if ((type == 0 || type == 4) && !(flags & 0x100 /* DIEDFL_FORCEFEEDBACK */))
        for (int i = 0; i < num_pads(); i++) {
            uint8_t inst[16], prod[16];
            pad_guid(inst, 1, i);
            pad_guid(prod, 0, i);
            if (!enum_one(c, cb, ref, inst, prod, PAD_DEVTYPE, PAD_NAME)) break;
        }
    RET(5, S_OK);
}
W(di_GetDeviceStatus) { RET(2, S_OK); }
W(di_RunControlPanel) { RET(3, S_OK); }
W(di_Initialize) { RET(3, S_OK); }
W(di_FindDevice) { RET(4, 0x80070002u); }

static const ComMethod di_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "CreateDevice", 3, di_CreateDevice }, { "EnumDevices", 4, di_EnumDevices },
    { "GetDeviceStatus", 1, di_GetDeviceStatus }, { "RunControlPanel", 2, di_RunControlPanel },
    { "Initialize", 2, di_Initialize }, { "FindDevice", 3, di_FindDevice }, { "CreateDeviceEx", 4, di_CreateDeviceEx },
};
static ComInterface IDirectInput_i = { "IDirectInputA", di_methods, 10, 0 };

void w_DirectInputCreateA(CPU *c) {
    LOG("[dinput] DirectInputCreateA(version %x)\n", ARG(1));
    W32(ARG(2), com_new(&IDirectInput_i, 0, NULL));
    RET(4, S_OK);
}
