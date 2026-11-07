#define WIN32_LEAN_AND_MEAN
#define MERC_PLUGIN_IMPLEMENTATION
#include <windows.h>
#include "../mercenaries_plugin.h"
#include <stdio.h>

static const MercPluginHost *host_api;
static ULONGLONG last_log;
static unsigned interval_ms=5000;
static int first_update=1;
static int MERC_PLUGIN_CALL update(void *userdata,const MercPluginUpdate *event)
{
    uint32_t magic=0;
    (void)userdata;
    if(event->api_version!=MERCENARIES_PLUGIN_API_VERSION_1)return 0;
    if(!host_api->read_guest(host_api->context,0x10000,&magic,sizeof(magic)) ||
       magic!=0x48454258u) {
        host_api->log(host_api->context,"Could not read the expected retail XBE header.");
        return 0;
    }
    ULONGLONG now=GetTickCount64();
    if(first_update || now-last_log>=interval_ms) {
        char text[160];
        snprintf(text,sizeof(text),"Update %llu: checked guest read passed; wall delta %.4f seconds.",
            (unsigned long long)event->update_index,event->wall_delta_seconds);
        host_api->log(host_api->context,text);last_log=now;first_update=0;
    }
    return 1;
}
static void MERC_PLUGIN_CALL shutdown_plugin(void *userdata)
{
    (void)userdata;
    host_api->log(host_api->context,"Example shut down.");
    host_api=NULL;
}
MERC_PLUGIN_EXPORT int MERC_PLUGIN_CALL MercenariesPlugin_Init(
    const MercPluginHost *host,MercPlugin *plugin,char *error,uint32_t capacity)
{
    wchar_t settings[32768];
    if(!host || host->api_version!=MERCENARIES_PLUGIN_API_VERSION_1 ||
       host->struct_size<sizeof(MercPluginHost) || !plugin ||
       plugin->struct_size<sizeof(MercPlugin)) {
        if(error && capacity)snprintf(error,capacity,"Unsupported plugin interface.");
        return 0;
    }
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,host->settings_directory,-1,settings,32700)) {
        if(error && capacity)snprintf(error,capacity,"Invalid settings directory.");
        return 0;
    }
    wcscat_s(settings,32768,L"\\example.ini");
    interval_ms=GetPrivateProfileIntW(L"Example",L"LogIntervalMs",5000,settings);
    if(interval_ms<250)interval_ms=250;
    if(interval_ms>60000)interval_ms=60000;
    host_api=host;first_update=1;last_log=0;
    plugin->userdata=NULL;
    plugin->on_update=update;
    plugin->on_shutdown=shutdown_plugin;
    host->log(host->context,"Example initialized; callbacks use the official interface.");
    return 1;
}

