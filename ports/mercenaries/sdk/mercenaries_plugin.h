#ifndef MERCENARIES_PLUGIN_H
#define MERCENARIES_PLUGIN_H
#include <stdint.h>
#include <stddef.h>
/* Interface versions. mod.ini ApiVersion selects one; the host passes that
 * value in every api_version field it gives the plugin. */
#define MERCENARIES_PLUGIN_API_VERSION_1 1u
#define MERCENARIES_PLUGIN_API_VERSION_2 2u
#define MERCENARIES_PLUGIN_API_VERSION MERCENARIES_PLUGIN_API_VERSION_2 /* newest */
#define MERC_PLUGIN_CALL __cdecl
#define MERC_PLUGIN_EXPORT __declspec(dllexport)
#ifdef __cplusplus
extern "C" {
#endif
/* Windows x64 C ABI. Existing v1 fields/signatures must never change; later
 * versions only append members. */
typedef struct MercPluginUpdate {
    uint32_t struct_size, api_version;
    uint64_t update_index;
    double wall_delta_seconds; /* Real time, NOT simulation time; advances while paused. */
} MercPluginUpdate;

/* ---- Interface 2: hook points ----
 * A hook point is a named place inside the game where the host stops and
 * offers documented values to subscribed plugins (see sdk/README.md for the
 * list). args[] hold integers, guest addresses or IEEE-754 float bits as
 * documented per hook. A plugin may change args marked writable, and set
 * handled=1 (with result, where documented) to replace the retail
 * behaviour. Subscribers run in load order; each sees earlier changes. */
#define MERC_PLUGIN_HOOK_ARGS 8u
typedef struct MercPluginHookEvent {
    uint32_t struct_size, api_version;
    const char *name;           /* the hook point, e.g. "turret.update" */
    uint32_t arg_count;
    uint32_t args[MERC_PLUGIN_HOOK_ARGS];
    uint32_t result;            /* documented per hook; used when handled */
    int32_t handled;            /* 1 = replace the retail behaviour */
} MercPluginHookEvent;
typedef void (MERC_PLUGIN_CALL *MercPluginHookFn)(void *userdata, MercPluginHookEvent *event);

typedef struct MercPluginHost {
    uint32_t struct_size, api_version;
    void *context;
    const char *plugin_id;          /* UTF-8 strings, valid until shutdown. */
    const char *plugin_directory;   /* Absolute private staged files. */
    const char *settings_directory; /* Absolute persistent folder outside mod-cache. */
    void (MERC_PLUGIN_CALL *log)(void *, const char *);
    int (MERC_PLUGIN_CALL *read_guest)(void *, uint32_t, void *, size_t);
    int (MERC_PLUGIN_CALL *write_guest)(void *, uint32_t, const void *, size_t);
    /* ---- Interface 2 (present when api_version >= 2) ---- */
    /* Subscribe to a hook point by name. Returns 0 for an unknown name.
     * Call from init. read_guest/write_guest/guest calls work inside it. */
    int (MERC_PLUGIN_CALL *register_hook)(void *, const char *name, MercPluginHookFn fn, void *userdata);
    /* Call translated guest functions. Only inside a hook or update callback.
     * begin reserves scratch_bytes (<= 4096, 16-byte aligned) on the guest
     * stack and returns its guest address, for sret/out parameters; write it
     * with write_guest. guest_call pushes args (args[0] = first parameter),
     * sets ECX = this_ptr (thiscall; pass 0 otherwise) and returns EAX.
     * Callee stack cleanup is undone after each call. end restores the guest
     * CPU state. Returns 0 if the address is not a translated function or the
     * call is not allowed now. Calling game code is at the plugin's risk. */
    int (MERC_PLUGIN_CALL *guest_call_begin)(void *, uint32_t scratch_bytes, uint32_t *scratch_address);
    int (MERC_PLUGIN_CALL *guest_call)(void *, uint32_t function, uint32_t this_ptr,
                                       const uint32_t *args, uint32_t arg_count, uint32_t *eax);
    void (MERC_PLUGIN_CALL *guest_call_end)(void *);
} MercPluginHost;
typedef struct MercPlugin {
    uint32_t struct_size, api_version;
    void *userdata;
    int (MERC_PLUGIN_CALL *on_update)(void *, const MercPluginUpdate *);
    void (MERC_PLUGIN_CALL *on_shutdown)(void *);
} MercPlugin;
/* Export with C linkage. Different from the legacy MercenariesMod_Init ABI.
 * Return 1 on success, 0 on failure; error is UTF-8/NUL terminated.
 * Fill callbacks/userdata. A supplied shutdown callback is also called if init fails.
 */
typedef int (MERC_PLUGIN_CALL *MercPluginEntry)(const MercPluginHost *,
    MercPlugin *, char *, uint32_t);
#ifdef MERC_PLUGIN_IMPLEMENTATION
MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(
    const MercPluginHost *, MercPlugin *, char *, uint32_t);
#endif
/* Float helpers for hook args. */
static __inline float merc_plugin_f32(uint32_t bits) { union { uint32_t u; float f; } v; v.u = bits; return v.f; }
static __inline uint32_t merc_plugin_bits(float value) { union { uint32_t u; float f; } v; v.f = value; return v.u; }
#ifdef __cplusplus
}
#endif
#endif
