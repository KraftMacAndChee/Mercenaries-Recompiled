/* Plugin Interface 2 example: hook points and guest calls.
 *
 * - "weapon.player_fire": optionally fire player turrets along the barrel
 *   instead of snapping shots to the crosshair (FireAlongBarrel=1).
 * - "turret.update": count turret updates and log the first one seen.
 * - Guest calls: read the HUD resolution through the game's own
 *   RedRenderer::GetHUDResolutionX/Y (no arguments, result in EAX).
 */
#define WIN32_LEAN_AND_MEAN
#define MERC_PLUGIN_IMPLEMENTATION
#include <windows.h>
#include "../mercenaries_plugin.h"
#include <stdio.h>

#define GET_HUD_RESOLUTION_X 0x0020F920u
#define GET_HUD_RESOLUTION_Y 0x0020F910u

static const MercPluginHost *host_api;
static int fire_along_barrel;
static unsigned long long turret_updates;
static ULONGLONG next_report;

static void MERC_PLUGIN_CALL on_player_fire(void *userdata, MercPluginHookEvent *event)
{
    (void)userdata;
    /* args[0]: 0 = vehicle weapon, 1 = gunner seat. */
    if (fire_along_barrel) event->handled = 1;
}

static void MERC_PLUGIN_CALL on_turret_update(void *userdata, MercPluginHookEvent *event)
{
    (void)userdata;
    if (turret_updates++ == 0) {
        char text[128];
        snprintf(text, sizeof(text), "first turret.update: turret=%08X dt=%.4f",
                 event->args[0], (double)merc_plugin_f32(event->args[1]));
        host_api->log(host_api->context, text);
    }
}

static int MERC_PLUGIN_CALL update(void *userdata, const MercPluginUpdate *event)
{
    (void)userdata;
    if (event->api_version != MERCENARIES_PLUGIN_API_VERSION_2) return 0;
    ULONGLONG now = GetTickCount64();
    if (now < next_report) return 1;
    next_report = now + 10000u;
    uint32_t scratch = 0, width = 0, height = 0;
    char text[160];
    if (host_api->guest_call_begin(host_api->context, 0, &scratch)) {
        int ok = host_api->guest_call(host_api->context, GET_HUD_RESOLUTION_X, 0, NULL, 0, &width) &&
                 host_api->guest_call(host_api->context, GET_HUD_RESOLUTION_Y, 0, NULL, 0, &height);
        host_api->guest_call_end(host_api->context);
        if (ok)
            snprintf(text, sizeof(text), "HUD %ux%u; turret updates so far: %llu", width, height, turret_updates);
        else
            snprintf(text, sizeof(text), "guest call failed; turret updates so far: %llu", turret_updates);
        host_api->log(host_api->context, text);
    }
    return 1;
}

static void MERC_PLUGIN_CALL shutdown_plugin(void *userdata)
{
    (void)userdata;
    host_api->log(host_api->context, "Hooks example shut down.");
    host_api = NULL;
}

MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(
    const MercPluginHost *host, MercPlugin *plugin, char *error, uint32_t capacity)
{
    wchar_t settings[32768];
    if (!host || host->api_version != MERCENARIES_PLUGIN_API_VERSION_2 ||
        host->struct_size < sizeof(MercPluginHost) || !plugin || plugin->struct_size < sizeof(MercPlugin)) {
        if (error && capacity) snprintf(error, capacity, "Plugin Interface 2 required (set ApiVersion=2).");
        return 0;
    }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, host->settings_directory, -1, settings, 32700)) {
        if (error && capacity) snprintf(error, capacity, "Invalid settings directory.");
        return 0;
    }
    wcscat_s(settings, 32768, L"\\hooks_example.ini");
    fire_along_barrel = GetPrivateProfileIntW(L"HooksExample", L"FireAlongBarrel", 0, settings) == 1;
    host_api = host;
    turret_updates = 0;
    next_report = 0;
    if (!host->register_hook(host->context, "weapon.player_fire", on_player_fire, NULL) ||
        !host->register_hook(host->context, "turret.update", on_turret_update, NULL)) {
        if (error && capacity) snprintf(error, capacity, "Hook points unavailable in this build.");
        return 0;
    }
    plugin->userdata = NULL;
    plugin->on_update = update;
    plugin->on_shutdown = shutdown_plugin;
    host->log(host->context, fire_along_barrel ? "Hooks example initialized; firing along barrels."
                                               : "Hooks example initialized.");
    return 1;
}
