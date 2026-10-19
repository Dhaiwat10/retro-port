/* Minimal COM support: guest-visible objects whose vtables point at host functions.
 *
 * A guest COM object is a small block in guest memory: [0] vtable pointer, [4] host object index.
 * Each vtable slot is a host "import" slot, so a guest `call [ecx+N]` lands in com_dispatch, which
 * finds the method from the slot and runs its handler (stdcall: `this` is argument 0).
 */
#ifndef PORT_COM_H
#define PORT_COM_H

#include "win32.h"

typedef struct ComMethod {
    const char *name;
    int nargs;        /* arguments after `this` */
    GuestFn impl;     /* NULL: log once and return S_OK */
} ComMethod;

typedef struct ComInterface {
    const char *name;
    const ComMethod *methods;
    int count;
    uint32_t vtable;  /* guest address, built on first use */
    void (*destroy)(void *data);  /* called when the last reference is released (may be NULL) */
} ComInterface;

typedef struct ComObj {
    ComInterface *iface;
    int32_t refs;
    int type;
    void *data;       /* host-side state */
    uint32_t guest;   /* guest address of the object */
} ComObj;

uint32_t com_new(ComInterface *it, int type, void *data);  /* returns the guest object pointer */
ComObj *com_get(uint32_t guest);                           /* NULL if not one of ours */
void *com_data(uint32_t guest);

/* Standard IUnknown handlers usable in any interface's table. */
void com_QueryInterface(CPU *c);
void com_AddRef(CPU *c);
void com_Release(CPU *c);

extern int g_log_d3d;  /* PORT_LOG_D3D: 1 = object creation and unusual calls, 2 = every call */

#define THIS ARG(0)
#define S_OK 0u
#define E_FAIL 0x80004005u
#define E_NOINTERFACE 0x80004002u

#endif
