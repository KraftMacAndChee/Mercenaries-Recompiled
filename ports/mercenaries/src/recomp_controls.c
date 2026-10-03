#include "recomp_controls.h"
#include "controls_mapping.h"
#include "recomp/recomp_types.h"
#include "dev_menu.h"
#include "free_cam.h"
#include "xinput_xbox.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define CONTROL_ACTIONS 26
#define MENU_CONTEXT 12
#define BIND_INHERIT 0xfffeu
#define WHEEL_UP 0x100u
#define WHEEL_DOWN 0x101u
static HWND g_window;
static volatile LONG g_hud_hidden;
static char g_path[MAX_PATH];
static unsigned short g_binds[2][CONTROL_CONTEXTS][CONTROL_ACTIONS];
static int g_initialized, g_mouse, g_sensitivity=5, g_invert, g_aim_assist=1;
static int g_shared_inputs=1;
static int g_in_game, g_shell_menu, g_modal, g_support, g_modal_back_y;
static unsigned char g_satellite_back_active[2];
static int g_result_screen, g_result_slowdown, g_waiting_controller;
static float g_scope_wheel_zoom;
static unsigned g_wheel_serial, g_scope_wheel_serial;
static uint32_t g_polled_main,g_polled_sub,g_polled_joystick,g_polled_joysub;
/* Menu-consumed inputs remain blocked for discrete gameplay actions until
 * release. Continuous movement/camera destinations resume immediately. */
static unsigned char g_menu_held[2][WHEEL_DOWN+1];
static volatile LONG g_wheel_pending;
static int g_wheel_active, g_wheel_release, g_wheel_remainder;
static int g_prompt_gameplay_body;
void recomp_controls_prompt_gameplay_body(int enabled){g_prompt_gameplay_body=enabled;}
static int g_last_gameplay_context=3; /* Prompt owner while retail disables input for an animation. */
static int g_joystick_context=-1; /* Retail aim mode, independent of binding aliases. */
static int g_mapping_active, g_gameplay, g_context=-1, g_menu, g_pause_navigation, g_page, g_device, g_edit_context=3;
static int g_listen=-1, g_armed, g_release, g_captured, g_isolated;
/* Inputs held when capture starts are excluded until individually released. */
static unsigned char g_capture_held[256];
static unsigned g_release_key;
static int g_release_device;
static POINT g_restore;
static RECT g_clip_rect;
static const char *g_test_mouse_path, *g_test_keys_path;
static unsigned g_test_keys[4], g_test_keys_sequence;
static ULONGLONG g_test_keys_until, g_test_keys_poll;
static LONG g_test_dx,g_test_dy;
static unsigned g_test_sequence;
static ULONGLONG g_test_poll_ms;
static volatile LONG g_dx, g_dy;
static const unsigned short keys[CONTROL_CHANNELS]={
    VK_UP,VK_DOWN,VK_LEFT,VK_RIGHT,VK_RETURN,VK_ESCAPE,'N','M',
    'Z','X','C','V',VK_SHIFT,VK_CONTROL,'Q','E',
    'A','D','S','W','J','L','K','I'};
static const char *contexts[CONTROL_CONTEXTS]={"HELICOPTER","CAR","TANK","ON FOOT",
    "SNIPER ZOOM",NULL,NULL,NULL,
    NULL,NULL,NULL,"PDA","MENUS"};
/* Retail Preset A / xboxPblJoystick button translation. Shared press/hold
 * actions stay together; different gameplay contexts have independent maps. */
static const char *foot[CONTROL_CHANNELS]={"SUPPORT ITEM UP","SUPPORT ITEM DOWN","TOGGLE FLAGS","OPEN SHOP",
    "PAUSE","OPEN PDA","CROUCH","AIM / SCOPE","RELOAD","JUMP","MELEE","USE / ENTER / CANCEL",
    "SWITCH WEAPON","SWITCH GRENADE","THROW GRENADE","FIRE",
    "WALK LEFT","WALK RIGHT","WALK BACK","WALK FORWARD","AIM LEFT","AIM RIGHT","AIM DOWN","AIM UP"};
static const char *heli[CONTROL_CHANNELS]={"RETRACT WINCH","DEPLOY WINCH","TOGGLE FLAGS","OPEN SHOP",
    "PAUSE","OPEN PDA","L3","R3","ASCEND","HORN","DESCEND","EXIT / DISEMBARK",
    "SWITCH WEAPON / RIDER FIRE","EJECT PASSENGERS","SECONDARY FIRE / EXIT","FIRE",
    "BANK LEFT","BANK RIGHT","PITCH BACK","PITCH FORWARD","TURN LEFT","TURN RIGHT","LOOK DOWN","LOOK UP"};
static const char *car[CONTROL_CHANNELS]={"SUPPORT ITEM UP","SUPPORT ITEM DOWN","TOGGLE FLAGS","OPEN SHOP",
    "PAUSE","OPEN PDA","L3","R3","ACCELERATE","HORN","BRAKE / REVERSE","EXIT / DISEMBARK",
    "RIDER FIRE","EJECT PASSENGERS","PLAYER EXIT","HANDBRAKE",
    "STEER LEFT","STEER RIGHT","REVERSE","THROTTLE","LOOK LEFT","LOOK RIGHT","LOOK DOWN","LOOK UP"};
static const char *tank[CONTROL_CHANNELS]={"SUPPORT ITEM UP","SUPPORT ITEM DOWN","TOGGLE FLAGS","OPEN SHOP",
    "PAUSE","OPEN PDA","L3","R3","A","HORN","X","EXIT / DISEMBARK",
    "SWITCH WEAPON","EJECT PASSENGERS","PLAYER EXIT","FIRE",
    "STEER LEFT","STEER RIGHT","REVERSE","DRIVE FORWARD","TURRET LEFT","TURRET RIGHT","TURRET DOWN","TURRET UP"};
static const char *pda[CONTROL_CHANNELS]={"UP","DOWN","LEFT","RIGHT","START","CLOSE PDA","L3","R3",
    "CONFIRM","B","X","BACK","RB / BLACK","LB / WHITE","PREVIOUS TAB","NEXT TAB",
    "PAN LEFT","PAN RIGHT","PAN DOWN","PAN UP","RS LEFT","RS RIGHT","RS DOWN","RS UP"};
/* Movement / aim first, then buttons. A four-row viewport keeps retail text
 * inside its safe area at both 4:3 and widescreen. */
static const unsigned order[CONTROL_ACTIONS]={19,18,16,17,23,22,20,21,15,14,8,9,10,11,12,13,6,7,0,1,2,3,4,5,24,25};
static const unsigned menu_order[]={0,1,2,3,8,11,15,9,4,10};
/* Only expose implemented vehicle actions. RsActorVehicleCar ignores
 * AnalogThrottle; tanks enable TrackedAcceleration, not Accelerator/Brake.
 * Vehicle L3/R3 have no retail binding. D-pad-left toggles HUD flags
 * directly in StatePlay, outside the logical joystick table. Up/down only operate
 * the helicopter winch. Tank rows are also the mounted-gun binding set.
 * Keep channel IDs/INI keys intact so existing working binds survive. */
static int action_available(int context,unsigned action) {
    context=controls_binding_context(context);
    if(action>=24)return context==SCOPE_CONTEXT && action<CONTROL_ACTIONS;
    if(context==SCOPE_CONTEXT)
        return action==2 || action==3 || action==4 || action==5 ||
            action==6 || action==7 || action==8 || action==12 ||
            action==13 || action==15 || (action>=20 && action<24);
    if(context>=0 && context<=2){
        if(action==6 || action==7)return 0;
        if(context!=0 && action<2)return 0;
        if(context==1 && (action==18 || action==19))return 0;
        if(context==2 && (action==8 || action==10))return 0;
    }
    return 1;
}
static const unsigned scope_order[]={24,25,23,22,20,21,15,8,7,6,12,13,2,3,4,5};
static unsigned action_count(void) {
    if(g_edit_context==MENU_CONTEXT)return sizeof(menu_order)/sizeof(menu_order[0]);
    if(g_edit_context==SCOPE_CONTEXT)return sizeof(scope_order)/sizeof(scope_order[0]);
    unsigned n=0;
    for(unsigned i=0;i<CONTROL_ACTIONS;++i)if(action_available(g_edit_context,order[i]))++n;
    return n;
}
static unsigned page_count(void) { return (action_count()+3)/4; }
static unsigned visible_actions(void) { unsigned n=action_count()-g_page*4;return n<4?n:4; }
static unsigned action_at(unsigned row) {
    unsigned index=g_page*4+row;
    if(g_edit_context==MENU_CONTEXT)return menu_order[index];
    if(g_edit_context==SCOPE_CONTEXT)return scope_order[index];
    for(unsigned i=0;i<CONTROL_ACTIONS;++i)if(action_available(g_edit_context,order[i])){
        if(!index--)return order[i];
    }
    return CONTROL_ACTIONS;
}
/* Keep the original fallback for saved profiles that predate QWERTY defaults.
 * Reset These Binds deliberately uses the new defaults, never this function. */
static unsigned short legacy_default_binding(int device,int context,unsigned action) {
    if(context==MENU_CONTEXT){
        if(action<4)return BIND_INHERIT;
        if(action==4)return device?4:0xffffu;
        if(action==8)return device?8:VK_RETURN;
        if(action==9)return device?9:VK_ESCAPE;
        if(action==10)return device?10:VK_DELETE;
        if(action==11)return device?11:legacy_default_binding(0,3,11);
        if(action==15)return device?15:legacy_default_binding(0,3,15);
        return 0xffffu;
    }
    if(action>=24)return device?(action==24?19:18):(action==24?WHEEL_UP:WHEEL_DOWN);
    if(device)return (unsigned short)action;
    /* Defaults follow US physical key positions on the active layout. Saved
     * bindings remain explicit logical keys and are never silently migrated. */
    static const unsigned scan[24]={0,0,0,0,0,0,0x31,0x32,0x2c,0x2d,0x2e,0x2f,0,0,0x10,0x12,0x1e,0x20,0x1f,0x11,0x24,0x26,0x25,0x17};
    unsigned vk=scan[action]?MapVirtualKeyW(scan[action],MAPVK_VSC_TO_VK_EX):0;
    return (unsigned short)(vk && vk<256?vk:keys[action]);
}
static unsigned short default_binding(int device,int context,unsigned action) {
    if(device)return legacy_default_binding(device,context,action);
    if(context==MENU_CONTEXT){
        if(action<4)return BIND_INHERIT;
        if(action==8)return VK_SPACE;
        if(action==9 || action==11)return 'R';
        if(action==10)return VK_DELETE;
        if(action==15)return 'E';
        return 0xffffu;
    }
    if(action>=24)return action==24?WHEEL_UP:WHEEL_DOWN;
    /* QWERTY letters are explicit; AZERTY suggestions have been converted. */
    static const unsigned short on_foot[CONTROL_CHANNELS]={
        VK_XBUTTON2,VK_XBUTTON1,VK_LEFT,VK_RIGHT,VK_ESCAPE,VK_TAB,VK_LCONTROL,'Q',
        'R',VK_SPACE,VK_MBUTTON,'E',WHEEL_UP,WHEEL_DOWN,VK_RBUTTON,VK_LBUTTON,
        'A','D','S','W','J','L','K','I'};
    context=controls_binding_context(context);
    if(context==11){
        switch(action){
        case 5:return VK_TAB;
        case 8:return VK_SPACE;
        case 11:return 'R';
        case 14:return 'Q';
        case 15:return 'E';
        case 16:return 'A';case 17:return 'D';case 18:return 'S';case 19:return 'W';
        default:return keys[action]; /* Other PDA controls were unspecified. */
        }
    }
    if(context>=0 && context<=2){
        switch(action){
        case 0:return context==0?'F':keys[action];
        case 1:return context==0?'V':keys[action];
        case 8:return context==0?VK_SPACE:context==1?'W':keys[action];
        case 9:return VK_MBUTTON;
        case 10:return context==0?'R':context==1?'S':keys[action];
        case 11:return 'E';
        case 12:return context==1?VK_RBUTTON:WHEEL_UP;
        case 13:return VK_XBUTTON1;
        case 14:return VK_XBUTTON2;
        case 15:return context==1?VK_SPACE:VK_LBUTTON;
        }
    }
    return on_foot[action];
}
/* Older INIs store only edited contexts/actions. Preserve the implicit old
 * defaults too, without rewriting any personal binding (including UNBOUND).
 * A per-profile version keeps subsequent launches and context resets stable. */
static int keyboard_defaults_version(void) {
    int version=GetPrivateProfileIntA("Controls","KeyboardDefaultsVersion",0,g_path);
    if(version>=1)return version;
    int existing=0;
    for(int context=0;context<CONTROL_CONTEXTS && !existing;++context){
        char section[32],entries[32];
        snprintf(section,sizeof(section),"Keyboard.%d",context+1);
        existing=GetPrivateProfileStringA(section,NULL,"",entries,sizeof(entries),g_path)>0;
    }
    version=existing?1:2;
    char value[8];snprintf(value,sizeof(value),"%d",version);
    WritePrivateProfileStringA("Controls","KeyboardDefaultsVersion",value,g_path);
    return version;
}
/* Normal support selection is an overlay on gameplay. Retail movement cancels
 * it; item-add animations, result screens and full menus still own all input. */
static int support_movement_active(void) {
    return g_support && g_polled_joysub==1u &&
        (g_polled_sub==0xC2CBD863u || g_polled_sub==0xC739FD0Fu) &&
        g_context>=0 && g_context!=11 && !g_menu && !g_pause_navigation &&
        !g_shell_menu && !g_waiting_controller && !g_result_screen;
}
static unsigned short resolved_binding(int device,int context,unsigned action) {
    context=controls_binding_context(context);
    if(!action_available(context,action))return 0xffffu;
    unsigned short b=g_binds[device][context][action];
    if(context==MENU_CONTEXT && b==BIND_INHERIT){
        static const unsigned inherit[]={19,18,16,17,4};
        /* Support uses the actual support/D-pad bindings, not movement as
         * additional navigation. Explicit Menu direction rebinds still apply. */
        if(action<4 && support_movement_active())return g_binds[device][3][action];
        return action<=4?g_binds[device][3][inherit[action]]:0xffffu;
    }
    return b;
}
/* Retail RsHudMessageWindow is distinct from frontend save confirmations,
 * tutorials (Continue), and support selection. Read its live type so reopening
 * a different message cannot retain a previous Yes/No mapping. */
static int mission_yes_no_active(void) {
    return g_in_game && g_polled_sub==0xDDFB69D8u && g_polled_joysub==5 &&
        !g_menu && !g_shell_menu && !g_pause_navigation &&
        MEM32(0x35F798u+0x30u)!=3 && MEM32(0x35F798u+0x4B4u)==1;
}
static int menu_active(void) { return g_pause_navigation || g_shell_menu || g_modal || (g_result_screen && !g_result_slowdown) || g_menu || g_waiting_controller; }
/* Render-only visibility: never deactivate retail canvases or their updates.
 * Keep menus, support selection, PDA and screen-transition brushes available. */
int recomp_controls_hide_hud_brush(uint32_t brush,uint32_t canvas) {
    if(!InterlockedCompareExchange(&g_hud_hidden,0,0) || !g_gameplay || menu_active())return 0;
    if(brush==0x35CFB8u)return 0; /* RsHudScreenTransitions in instrument canvas. */
    switch(canvas){
    case 0x363F30u: /* 3D reticle */
    case 0x363F80u: /* reticle */
    case 0x363FD0u: /* instruments */
    case 0x364028u: /* primary instruments */
    case 0x364080u: /* artist gauges */
    case 0x3640D0u: /* action buttons and world icons */
    case 0x364120u: /* popup HUD */
    case 0x364170u: /* scope overlay */
    case 0x3641C8u: /* satellite reticle */
    case 0x364218u: /* messages */
    case 0x364268u: /* subtitles */
    case 0x3642B8u: /* briefing radar */
    case 0x364360u: /* faction scope */
        return 1;
    default:return 0;
    }
}
static void binding_release(int device,unsigned key) {
    g_listen=-1;g_armed=0;g_release_device=device;g_release_key=key;g_release=1;
}
static void hud_hotkey(void) {
    if(g_listen>=0 || g_release){binding_release(0,VK_F10);return;}
    if(!g_in_game || recomp_dev_menu_visible())return;
    LONG hidden=!InterlockedCompareExchange(&g_hud_hidden,0,0);
    InterlockedExchange(&g_hud_hidden,hidden);
    fprintf(stderr,"[HUD] %s (F10)\n",hidden?"hidden":"visible");
}
static const char *action_name(int context,unsigned action) {
    if(context==MENU_CONTEXT){
        switch(action){case 0:return "UP";case 1:return "DOWN";case 2:return "LEFT";case 3:return "RIGHT";case 8:return "CONFIRM";case 9:return "BACK";case 10:return "SECONDARY / DELETE SAVE";case 11:return "DECLINE SIDE MISSION";case 15:return "SELECT SUPPORT OPTION";default:return "PAUSE";}
    }
    if(context==SCOPE_CONTEXT && action==7)return "EXIT SCOPE";
    if(context==SCOPE_CONTEXT && action==13)return "SWITCH GRENADE (SNIPER ONLY)";
    if(action==24)return "ZOOM IN";
    if(action==25)return "ZOOM OUT";
    if(context==0)return heli[action];
    if(context==1)return car[action];
    if(context==2)return tank[action];
    if(context==11)return pda[action];
    return foot[action];
}
static void write_value(const char *section,const char *key,int value) {
    char text[24];snprintf(text,sizeof(text),"%d",value);
    if(!WritePrivateProfileStringA(section,key,text,g_path))
        fprintf(stderr,"[CONTROLS] Cannot save %s/%s: %lu\n",section,key,(unsigned long)GetLastError());
}
static void save_binding(int device,int context,unsigned action) {
    char section[48],key[24];
    snprintf(section,sizeof(section),"%s.%d",device ? "Gamepad" : "Keyboard",context+1);
    snprintf(key,sizeof(key),"Action%u",action);
    write_value(section,key,g_binds[device][context][action]);
}
static void save_context(void) {
    for(unsigned i=0;i<CONTROL_ACTIONS;++i)save_binding(g_device,g_edit_context,i);
}
static void unpack(const XBOX_GAMEPAD *pad,float raw[CONTROL_CHANNELS]) {
    short axes[4]={pad->sThumbLX,pad->sThumbLY,pad->sThumbRX,pad->sThumbRY};
    for(unsigned i=0;i<8;++i){raw[i]=(pad->wButtons&(1u<<i))?1.f:0.f;raw[i+8]=pad->bAnalogButtons[i]/255.f;}
    for(unsigned i=0;i<4;++i){float v=axes[i]/32767.f;if(v< -1.f)v=-1.f;raw[16+i*2]=v<0?-v:0;raw[17+i*2]=v>0?v:0;}
}
static short axis_value(float value) { if(value>1)value=1;if(value< -1)value=-1;return (short)(value*32767.f); }
static void pack(XBOX_GAMEPAD *pad,const float value[CONTROL_CHANNELS]) {
    memset(pad,0,sizeof(*pad));
    for(unsigned i=0;i<8;++i){if(value[i]>.25f)pad->wButtons|=(WORD)(1u<<i);pad->bAnalogButtons[i]=(BYTE)(value[i+8]*255.f);}
    pad->sThumbLX=axis_value(value[17]-value[16]);pad->sThumbLY=axis_value(value[19]-value[18]);
    pad->sThumbRX=axis_value(value[21]-value[20]);pad->sThumbRY=axis_value(value[23]-value[22]);
}
static int focused(void){return g_window && GetForegroundWindow()==g_window && !IsIconic(g_window);}
static void release_mouse(void) {
    if(g_captured){ClipCursor(NULL);SetCursor(LoadCursor(NULL,IDC_ARROW));
        if(focused())SetCursorPos(g_restore.x,g_restore.y);g_captured=0;}
    InterlockedExchange(&g_dx,0);InterlockedExchange(&g_dy,0);
}
static void update_mouse(void) {
    int want=controls_capture_allowed(g_mouse || recomp_freecam_enabled(),g_gameplay,focused(),recomp_dev_menu_visible(),g_menu || g_modal);
    if(g_isolated)want=0;
    if(!want){release_mouse();return;}
    RECT rect;POINT a={0,0},b;
    if(!GetClientRect(g_window,&rect) || rect.right<=0 || rect.bottom<=0){release_mouse();return;}
    b.x=rect.right;b.y=rect.bottom;ClientToScreen(g_window,&a);ClientToScreen(g_window,&b);
    int entering=!g_captured;
    if(!g_captured){GetCursorPos(&g_restore);InterlockedExchange(&g_dx,0);InterlockedExchange(&g_dy,0);g_captured=1;}
    rect.left=a.x;rect.top=a.y;rect.right=b.x;rect.bottom=b.y;
    if(entering || memcmp(&g_clip_rect,&rect,sizeof(rect))){ClipCursor(&rect);g_clip_rect=rect;}
    if(entering)SetCursor(NULL);
}
static int freecam_overlay_input(int keyboard) {
    return recomp_freecam_enabled() && g_gameplay && !menu_active() && recomp_dev_menu_camera_input(keyboard);
}
static int key_pressed(unsigned key) {
    if(key==WHEEL_UP)return g_wheel_active>0;
    if(key==WHEEL_DOWN)return g_wheel_active<0;
    if(g_test_keys_path){
        if(GetTickCount64()<g_test_keys_until)for(unsigned i=0;i<4;++i)if(key && key==g_test_keys[i])return 1;
        return 0;
    }
    if(freecam_overlay_input(1))return !g_isolated && key>0 && key<256 && (GetAsyncKeyState((int)key)&0x8000)!=0;
    return key<256 && xbox_InputKeyDown((int)key);
}
static int capture_key(unsigned key) {
    return key!=VK_F8 && key!=VK_F9 && key!=VK_F10 && key!=VK_F11 &&
        key!=VK_LWIN && key!=VK_RWIN && key!=VK_MENU && key!=VK_LMENU && key!=VK_RMENU &&
        !(key>=VK_LSHIFT && key<=VK_RCONTROL);
}
static void capture_snapshot(int keyboard,const float *raw) {
    memset(g_capture_held,0,sizeof(g_capture_held));
    if(keyboard){
        for(unsigned key=1;key<256;++key)
            if(capture_key(key))g_capture_held[key]=(unsigned char)key_pressed(key);
    }else for(unsigned i=0;i<CONTROL_CHANNELS;++i)g_capture_held[i]=raw[i]>.25f;
    g_armed=1;
}
static void begin_binding(unsigned action) {
    g_listen=(int)action;g_armed=0;
    if(!g_device)capture_snapshot(1,NULL);
}
static void accept_binding(unsigned short key) {
    unsigned short *map=g_binds[g_device][g_edit_context];
    if(!g_shared_inputs && key!=0xffffu){
        unsigned short previous=resolved_binding(g_device,g_edit_context,g_listen);
        for(unsigned action=0;action<CONTROL_ACTIONS;++action){
            if(action==(unsigned)g_listen || !action_available(g_edit_context,action))continue;
            if(g_edit_context==MENU_CONTEXT){
                unsigned i=0;
                while(i<sizeof(menu_order)/sizeof(menu_order[0]) && menu_order[i]!=action)++i;
                if(i==sizeof(menu_order)/sizeof(menu_order[0]))continue;
            }
            /* Resolve inherited menu directions before swapping. Copying the
             * inheritance marker would refer to the destination's action. */
            if(resolved_binding(g_device,g_edit_context,action)==key)map[action]=previous;
        }
    }
    map[g_listen]=key;
    save_context();
    /* Clearing is always initiated by Backspace, even in the gamepad editor. */
    binding_release(key==0xffffu?0:g_device,key==0xffffu?VK_BACK:key);
}
static float menu_source(int keyboard,unsigned short binding,const float *raw) {
    float value=keyboard?(key_pressed(binding)?1.f:0.f):(binding<24?raw[binding]:0.f);
    if(binding<=WHEEL_DOWN && value>.25f)g_menu_held[keyboard?0:1][binding]=1;
    return value;
}
static int continuous_action(int context,unsigned action) {
    return (context==11 && action>=16 && action<24) ||
        (context==SCOPE_CONTEXT && (action==24 || action==25)) ||
        (context>=0 && context<=4 && ((action>=16 && action<24) ||
        ((context==0 || context==1) && (action==8 || action==10))));
}
static int gameplay_source_allowed(int keyboard,int context,unsigned action,unsigned binding) {
    if(binding>WHEEL_DOWN)return 0;
    unsigned held=g_menu_held[keyboard?0:1][binding];
    if(!held || (context==11 && held==2))return 1;
    /* Classify destinations: a button may steer, a stick may fire. */
    return continuous_action(context,action);
}
static int gameplay_key_pressed(unsigned binding,int context,unsigned action) {
    /* F9 closing keys have a separate latch in the host input layer. A key
     * rebound to movement must bypass that latch too, but never UI capture. */
    if(!g_test_keys_path && !freecam_overlay_input(1) && binding<256 && continuous_action(context,action))
        return xbox_InputKeyDownContinuous((int)binding);
    return key_pressed(binding);
}
static int satellite_camera_active(void) {
    return g_gameplay && !menu_active() && g_polled_joystick==7u &&
        g_polled_sub==0xC2CBD863u && (g_polled_joysub==0u || g_polled_joysub==3u) &&
        !recomp_freecam_enabled();
}
static BOOL mapper(XBOX_INPUT_STATE *state,BOOL keyboard) {
    float raw[CONTROL_CHANNELS],out[CONTROL_CHANNELS];
    if(!g_initialized)return FALSE;
    if(!satellite_camera_active())memset(g_satellite_back_active,0,sizeof(g_satellite_back_active));
    /* Focus owns keyboard/mouse capture, not controller availability. Keep
     * remapping gamepads in the background, but never capture a new binding
     * or poll desktop keys while another application has focus. */
    if((recomp_dev_menu_visible() && !freecam_overlay_input(keyboard)) ||
       (!focused() && !freecam_overlay_input(keyboard) && !g_test_keys_path && (keyboard || g_listen>=0 || g_release))){
        if(g_listen>=0)g_armed=0;
        memset(state,0,sizeof(*state));return TRUE;
    }
    if(!keyboard)unpack(&state->Gamepad,raw);
    /* Releasing one input must not wait for other held keys/sticks. */
    if(keyboard){for(unsigned k=1;k<=WHEEL_DOWN;++k)if(g_menu_held[0][k] && !key_pressed(k))g_menu_held[0][k]=0;}
    else {for(unsigned k=0;k<24;++k)if(raw[k]<=.25f)g_menu_held[1][k]=0;}
    if(keyboard && (g_listen>=0 || g_release) && key_pressed(VK_F10)){
        hud_hotkey();memset(state,0,sizeof(*state));return TRUE;
    }
    if(g_listen>=0 || g_release) {
        if(g_release){
            /* An unrelated held modifier, button or stick must not lock the
             * entire editor after the newly captured input is released. */
            if(keyboard==!g_release_device){
                int held=keyboard?key_pressed(g_release_key):
                    g_release_key<CONTROL_CHANNELS && raw[g_release_key]>.25f;
                if(!held)g_release=0;
            }
        }else if(keyboard==!g_device){
            if(!g_armed)capture_snapshot(keyboard,raw);
            if(keyboard){
                for(unsigned key=1;key<256;++key){
                    if(!capture_key(key))continue;
                    int down=key_pressed(key);
                    if(!down)g_capture_held[key]=0;
                    else if(!g_capture_held[key]){
                        accept_binding(key==VK_BACK?0xffffu:(unsigned short)key);break;
                    }
                }
            }else for(unsigned i=0;i<CONTROL_CHANNELS;++i){
                if(raw[i]<=.25f)g_capture_held[i]=0;
                else if(raw[i]>.65f && !g_capture_held[i]){accept_binding((unsigned short)i);break;}
            }
        }
        memset(state,0,sizeof(*state));return TRUE;
    }
    if(menu_active()){
        const int mission_yes_no=mission_yes_no_active();
        const int support_select=g_support && !g_menu && !g_pause_navigation &&
            !g_shell_menu && !g_waiting_controller;
        memset(out,0,sizeof(out));
        if(!keyboard)unpack(&state->Gamepad,raw);
        for(unsigned i=0;i<sizeof(menu_order)/sizeof(menu_order[0]);++i){
            unsigned a=menu_order[i];
            if((mission_yes_no && a==9) || (!mission_yes_no && a==11) || (!support_select && a==15))continue;
            unsigned short b=resolved_binding(keyboard?0:1,MENU_CONTEXT,a);
            out[a]=menu_source(keyboard,b,raw);
        }
        for(unsigned a=0;a<4;++a)if(g_binds[keyboard?0:1][MENU_CONTEXT][a]==BIND_INHERIT){
            unsigned short b=resolved_binding(keyboard?0:1,3,a);
            float v=menu_source(keyboard,b,raw);
            if(v>out[a])out[a]=v;
        }
        if(support_movement_active()){
            /* Preserve the active gameplay context's analog movement/look.
             * The original AI handles cancellation and movement; do not forge
             * a Back press or pass through combat buttons. Track these inputs
             * so a shared attack binding cannot fire on the closing frame. */
            for(unsigned a=16;a<24;++a)
                out[a]=menu_source(keyboard,resolved_binding(keyboard?0:1,g_context,a),raw);
        }
        /* Both selection bindings feed one retail accept channel. Sharing a
         * source or pressing both cannot dispatch two selection events. */
        if(support_select){if(out[15]>out[8])out[8]=out[15];out[15]=0;}
        /* Opposite directions cancel. No gameplay fire/grenade channels leak
         * into modal dialogs or the original unbound-key fallback. */
        if(out[0]>.5f && out[1]>.5f)out[0]=out[1]=0;
        if(out[2]>.5f && out[3]>.5f)out[2]=out[3]=0;
        if(!mission_yes_no && g_modal_back_y){out[11]=out[9];out[9]=0;} /* Retail support cancels with Y. */
        /* Retail startup/disconnect accepts Start, not menu Choose. Use the
         * user's Confirm binding only during this handshake, never both. */
        if(g_waiting_controller){float start=out[8]>out[4]?out[8]:out[4];memset(out,0,sizeof(out));out[4]=start;}
        pack(&state->Gamepad,out);return TRUE;
    }
    if(!g_mapping_active || g_context<0)return FALSE;
    int map_context=recomp_freecam_enabled()?3:g_context; /* Free flight uses On Foot binds. */
    if(keyboard){
        memset(state,0,sizeof(*state));
        for(unsigned i=0;i<CONTROL_CHANNELS;++i){unsigned short b=g_binds[0][map_context][i];
            out[i]=b<=WHEEL_DOWN && gameplay_source_allowed(1,map_context,i,b) && gameplay_key_pressed(b,map_context,i)?1.f:0.f;}
    }else{
        unpack(&state->Gamepad,raw);
        controls_map_channels(raw,g_binds[1][map_context],out);
        for(unsigned i=0;i<CONTROL_CHANNELS;++i)
            if(!gameplay_source_allowed(0,map_context,i,g_binds[1][map_context][i]))out[i]=0;
    }
    /* Free Cam keyboard ascent is Space, independent of the reload binding.
     * The controller's mapped A channel and normal gameplay binds are intact. */
    if(keyboard && recomp_freecam_enabled()){
        for(unsigned i=0;i<CONTROL_CHANNELS;++i)
            if(g_binds[0][map_context][i]==VK_SPACE || g_binds[0][map_context][i]=='1' || g_binds[0][map_context][i]=='2' ||
               g_binds[0][map_context][i]=='Q' || g_binds[0][map_context][i]=='E')out[i]=0;
        out[8]=gameplay_key_pressed(VK_SPACE,0,8)?1.f:0.f;
    }
    /* Hidden legacy slots must not emit old defaults or collide with a new
     * binding. Filter destinations, not sources: any physical key, trigger
     * or stick may still bind a working action. */
    for(unsigned i=0;i<CONTROL_CHANNELS;++i)if(!action_available(map_context,i))out[i]=0;
    /* A separate pair of bindings changes scope magnification without
     * changing On Foot movement or the action that enters the scope. */
    if((g_joystick_context==9 || g_joystick_context==4) && !recomp_freecam_enabled()){
        out[18]=out[19]=0;
        if(g_joystick_context==4)out[13]=0; /* Retail faction scope disables grenade switching. */
        for(unsigned a=24;a<26;++a){unsigned short b=resolved_binding(keyboard?0:1,SCOPE_CONTEXT,a);
            float v=keyboard?(b<=WHEEL_DOWN && gameplay_source_allowed(1,SCOPE_CONTEXT,a,b) && gameplay_key_pressed(b,SCOPE_CONTEXT,a)?1.f:0.f):(b<24 && gameplay_source_allowed(0,SCOPE_CONTEXT,a,b)?raw[b]:0.f);
            out[a==24?19:18]=v;
            if(keyboard && b>=WHEEL_UP && b<=WHEEL_DOWN){
                /* Reusing the wheel for weapon switching must not switch a
                 * weapon while the same detent changes magnification. */
                for(unsigned i=0;i<24;++i)if(i!=18 && i!=19 && g_binds[0][map_context][i]==b)out[i]=0;
                if(v && g_scope_wheel_serial!=g_wheel_serial){
                    g_scope_wheel_serial=g_wheel_serial;g_scope_wheel_zoom=a==24?1.f:-1.f;
                }
            }
        }
    }
    /* HumanGPSCamera (7) cancels through the original R3 logical action.
     * Menus Back is an additional source only while this camera owns input. */
    if(satellite_camera_active()){
        unsigned device=keyboard?0:1;
        unsigned short back=resolved_binding(device,MENU_CONTEXT,9);
        float cancel=keyboard?(key_pressed(back)?1.f:0.f):(back<24?raw[back]:0.f);
        if(cancel<=.25f)g_satellite_back_active[device]=0;
        else if(g_satellite_back_active[device] || gameplay_source_allowed(keyboard,map_context,7,back)){
            g_satellite_back_active[device]=1;
            menu_source(keyboard,back,raw);
            /* A shared Back/Fire binding must cancel rather than order a strike.
             * The menu-held latch also consumes it after returning On Foot. */
            for(unsigned a=0;a<CONTROL_CHANNELS;++a)
                if(a!=7 && g_binds[keyboard?0:1][map_context][a]==back)out[a]=0;
            if(cancel>out[7])out[7]=cancel;
        }
    }
    /* PDA uses its own retail map, but closing it must consume its input too. */
    if(g_context==11)for(unsigned a=0;a<CONTROL_CHANNELS;++a){
        unsigned short b=g_binds[keyboard?0:1][11][a];
        if(b<=WHEEL_DOWN && out[a]>.25f)g_menu_held[keyboard?0:1][b]=2;
    }
    pack(&state->Gamepad,out);return TRUE;
}
void recomp_controls_init(HWND window) {
    g_window=window;
    if(g_initialized)return;
    DWORD n=GetModuleFileNameA(NULL,g_path,MAX_PATH);char *slash=strrchr(g_path,'\\');
    if(!n || n>=MAX_PATH || !slash)strcpy(g_path,".\\mercenaries_recomp.ini");
    else strcpy(slash+1,"mercenaries_recomp.ini");
    char configured_root[MAX_PATH];
    DWORD configured_length=GetEnvironmentVariableA("MERCENARIES_CONFIG_ROOT",configured_root,MAX_PATH);
    if(configured_length && configured_length+sizeof("\\mercenaries_recomp.ini")<=MAX_PATH)
        snprintf(g_path,MAX_PATH,"%s\\mercenaries_recomp.ini",configured_root);
    const int legacy_keyboard=keyboard_defaults_version()<2;
    g_mouse=GetPrivateProfileIntA("Controls","MouseAim",legacy_keyboard?0:1,g_path)==1;
    g_sensitivity=(int)GetPrivateProfileIntA("Controls","MouseSensitivity",5,g_path);
    if(g_sensitivity<1 || g_sensitivity>20)g_sensitivity=5;
    g_invert=GetPrivateProfileIntA("Controls","InvertMouseY",0,g_path)==1;
    g_aim_assist=GetPrivateProfileIntA("Controls","AimAssist",1,g_path)!=0;
    g_shared_inputs=GetPrivateProfileIntA("Controls","AllowSharedInputs",1,g_path)!=0;
    for(int device=0;device<2;++device)for(int context=0;context<CONTROL_CONTEXTS;++context){
        char section[48];snprintf(section,sizeof(section),"%s.%d",device?"Gamepad":"Keyboard",context+1);
        for(unsigned i=0;i<CONTROL_ACTIONS;++i){
            char key[24];snprintf(key,sizeof(key),"Action%u",i);
            unsigned fallback=legacy_keyboard?legacy_default_binding(device,context,i):default_binding(device,context,i);unsigned value=GetPrivateProfileIntA(section,key,(int)fallback,g_path);
            if(value!=0xffffu && !(context==MENU_CONTEXT && i<=4 && value==BIND_INHERIT) && (device ? value>=CONTROL_CHANNELS : value==0 || value>WHEEL_DOWN))value=fallback;
            g_binds[device][context][i]=(unsigned short)value;
        }
    }
    /* The former scope slots were hidden aliases. Import the effective On
     * Foot bindings once, including explicit unbinding and wheel zoom, rather
     * than revive stale per-scope INI entries from an older editor. */
    if(GetPrivateProfileIntA("Controls","ScopeBindingsVersion",0,g_path)<1){
        for(int device=0;device<2;++device)for(unsigned a=0;a<CONTROL_ACTIONS;++a){
            if(!action_available(SCOPE_CONTEXT,a))continue;
            g_binds[device][SCOPE_CONTEXT][a]=g_binds[device][3][a];
            save_binding(device,SCOPE_CONTEXT,a);
        }
        write_value("Controls","ScopeBindingsVersion",1);
    }
    /* Earlier menu editors persisted unused Action10 as UNBOUND. It is the
     * retail secondary choice (Delete Save), now explicitly bindable. Migrate
     * that old placeholder once; preserve later intentional unbinding. */
    if(GetPrivateProfileIntA("Controls","MenuBindingsVersion",0,g_path)<2){
        for(int device=0;device<2;++device)if(g_binds[device][MENU_CONTEXT][10]==0xffffu){
            g_binds[device][MENU_CONTEXT][10]=legacy_keyboard?legacy_default_binding(device,MENU_CONTEXT,10):default_binding(device,MENU_CONTEXT,10);
            save_binding(device,MENU_CONTEXT,10);
        }
        write_value("Controls","MenuBindingsVersion",2);
    }
    /* Older editors saved every unused menu slot as UNBOUND. Initialize only
     * that legacy placeholder; subsequent intentional unbinding is preserved. */
    if(GetPrivateProfileIntA("Controls","MenuBindingsVersion",0,g_path)<3){
        for(int device=0;device<2;++device)if(g_binds[device][MENU_CONTEXT][11]==0xffffu){
            g_binds[device][MENU_CONTEXT][11]=legacy_keyboard?legacy_default_binding(device,MENU_CONTEXT,11):default_binding(device,MENU_CONTEXT,11);
            save_binding(device,MENU_CONTEXT,11);
        }
        write_value("Controls","MenuBindingsVersion",3);
    }
    if(GetPrivateProfileIntA("Controls","MenuBindingsVersion",0,g_path)<4){
        for(int device=0;device<2;++device)if(g_binds[device][MENU_CONTEXT][15]==0xffffu){
            g_binds[device][MENU_CONTEXT][15]=legacy_keyboard?legacy_default_binding(device,MENU_CONTEXT,15):default_binding(device,MENU_CONTEXT,15);
            save_binding(device,MENU_CONTEXT,15);
        }
        write_value("Controls","MenuBindingsVersion",4);
    }
    g_isolated=getenv("MERCENARIES_TEST_ISOLATE_INPUT")!=NULL;
    if(g_isolated){g_test_mouse_path=getenv("MERCENARIES_TEST_MOUSE_FILE");
        g_test_keys_path=getenv("MERCENARIES_TEST_KEYS_FILE");}
    g_initialized=1;xbox_InputSetMapper(mapper);
    RAWINPUTDEVICE mouse={1,2,0,window};
    if(!RegisterRawInputDevices(&mouse,1,sizeof(mouse)))fprintf(stderr,"[CONTROLS] Raw mouse registration failed: %lu\n",(unsigned long)GetLastError());
    SetTimer(window,0x5244,16,NULL);
}
void recomp_controls_state(uint32_t main,uint32_t sub,uint32_t joystick,int enabled) {
    g_polled_main=main;g_polled_sub=sub;g_polled_joystick=joystick;
    recomp_freecam_context(main);
    g_joystick_context=joystick>=1 && joystick<=12 ? (int)joystick-1 : -1;
    g_context=controls_context(joystick);
    if(main==0x4249D707u && sub==0xC2CBD863u && g_context>=0 && g_context!=11)
        g_last_gameplay_context=g_context;
    g_pause_navigation=main==0x4249D707u && (sub==0x7084D38Du || sub==0xA9A8BBBAu ||
        sub==0x9F19476Eu || sub==0x2D154491u || sub==0xB540BE88u || sub==0x53ACCC57u);
    g_in_game=main==0x4249D707u;g_modal=g_support=g_modal_back_y=0;
    g_shell_menu=main==0x59C00449u || main==0x11E1FC01u || main==0xAEF39CCFu;
    g_result_screen=g_in_game && (sub==0x65872ADEu || sub==0xA64B64FCu ||
                                   sub==0xA11AED03u || sub==0xF5A9EFA7u);
    g_result_slowdown=g_result_screen && joystick!=15u; /* retail Disabled */
    if(g_in_game && sub!=0xC2CBD863u && sub!=0x7084D38Du)recomp_freecam_set(0);
    g_gameplay=main==0x4249D707u && (sub==0xC2CBD863u || g_result_slowdown) && enabled && g_context>=0 && joystick!=12;
    g_mapping_active=g_gameplay || (main==0x4249D707u && joystick==12u);
    g_scope_wheel_zoom=0;
    if(g_wheel_release){g_wheel_active=0;g_wheel_release=0;}
    else {LONG pending=InterlockedExchange(&g_wheel_pending,0);
        g_wheel_active=pending>0?1:pending<0?-1:0;
        if(g_wheel_active){++g_wheel_serial;InterlockedExchangeAdd(&g_wheel_pending,pending-g_wheel_active);g_wheel_release=1;}}
    /* Private replay feeds physical keys through the production mapper;
     * unlike the legacy gamepad override it tests rebinding itself. */
    if(g_test_keys_path && GetTickCount64()-g_test_keys_poll>=25){
        g_test_keys_poll=GetTickCount64();
        FILE *file=fopen(g_test_keys_path,"r");
        if(file){unsigned sequence,duration,k[4]={0};
            int n=fscanf(file,"%u %u %u %u %u %u",&sequence,&duration,&k[0],&k[1],&k[2],&k[3]);fclose(file);
            if(n>=3 && sequence!=g_test_keys_sequence && duration<=10000 &&
               k[0]<256 && k[1]<256 && k[2]<256 && k[3]<256){
                g_test_keys_sequence=sequence;memcpy(g_test_keys,k,sizeof(k));g_test_keys_until=g_test_keys_poll+duration;
                xbox_InputNotifyKeyboardActivity();
                for(unsigned i=0;i<4;++i){
                    if(k[i]==VK_F10)hud_hotkey();
                    /* Isolated replay exercises the same developer time handler. */
                    if((k[i]=='1' || k[i]=='2') && recomp_freecam_enabled() && g_gameplay && !g_modal && !g_menu)
                        recomp_freecam_time_key(k[i]);
                }
                fprintf(stderr,"[CONTROLS-TEST] keys sequence=%u duration=%u keys=%u,%u,%u,%u\n",sequence,duration,k[0],k[1],k[2],k[3]);
            }
        }
    }
    if(g_test_mouse_path && GetTickCount64()-g_test_poll_ms>=25){
        g_test_poll_ms=GetTickCount64();
        FILE *file=fopen(g_test_mouse_path,"r");
        if(file){unsigned sequence;int dx,dy;
            int parsed=fscanf(file,"%u %d %d",&sequence,&dx,&dy);fclose(file);
            if(parsed==3 && sequence!=g_test_sequence && abs(dx)<=4096 && abs(dy)<=4096){
                g_test_sequence=sequence;
                if(g_gameplay && (g_mouse || recomp_freecam_enabled()) && !g_menu){g_test_dx+=dx;g_test_dy+=dy;}
                fprintf(stderr,"[CONTROLS-TEST] sequence=%u state=%u gameplay=%d enabled=%d dx=%d dy=%d\n",sequence,joystick,g_gameplay,g_mouse,dx,dy);
            }
        }
    }
    if(!g_gameplay || g_menu){g_test_dx=0;g_test_dy=0;}
    /* The UI thread handles capture; this call may come from an input worker. */
}
/* RsJoystickManager: base 0x323608, state +0x4c4, substate +0x4c8. */
void recomp_controls_substate(uint32_t substate) {
    g_polled_joysub=substate;
    /* The support-add screen reads raw A/Start even if finishing a takedown
     * or re-equipping a weapon restores the joystick table's Default substate.
     * Keep its Menus bindings until the actual screen returns to gameplay. */
    g_support=g_in_game && (g_polled_sub==0xC739FD0Fu || substate==1 || substate==2);
    g_modal=g_in_game && (g_support || (substate==4 && !g_result_slowdown) || substate==5);
    g_modal_back_y=g_modal && substate!=4;
    if(g_modal){InterlockedExchange(&g_dx,0);InterlockedExchange(&g_dy,0);}
}
/* The retail event handler can run after Pause/Use changed its action table,
 * but before another XInput sample. Never reinterpret an old mapped packet
 * using that new table. The next poll supplies correctly remapped input. */
int recomp_controls_event_current(uint32_t main,uint32_t sub,uint32_t joystick,uint32_t joysub) {
    if(!g_initialized)return 1;
    if(main!=g_polled_main || sub!=g_polled_sub || joystick!=g_polled_joystick)return 0;
    if(joysub==g_polled_joysub)return 1;
    /* GPS acquisition enters/leaves ManualTargeting within an update. Both
     * substates use the same On Foot physical map and need continuous Fire. */
    return (joystick==6u || joystick==7u) && (joysub==0u || joysub==3u) &&
           (g_polled_joysub==0u || g_polled_joysub==3u);
}
void recomp_controls_waiting_controller(int waiting) { g_waiting_controller=waiting; }
int recomp_controls_aim_assist(void) { return g_aim_assist; }
int recomp_controls_message(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(focused() && !g_isolated) {
        if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lp & (1L<<30)))
            xbox_InputNotifyKeyboardActivity();
        if(message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN || message==WM_MBUTTONDOWN ||
           message==WM_XBUTTONDOWN || message==WM_MOUSEWHEEL)
            xbox_InputNotifyKeyboardActivity();
    }
    if(message==WM_TIMER && wp==0x5244){update_mouse();return 1;}
    if(message==WM_KILLFOCUS || message==WM_DESTROY || message==WM_ACTIVATEAPP && !wp){release_mouse();g_armed=0;InterlockedExchange(&g_wheel_pending,0);g_wheel_active=g_wheel_release=g_wheel_remainder=0;}
    if(message==WM_KEYDOWN && wp==VK_F9 && recomp_dev_menu_allowed()){release_mouse();g_listen=-1;g_release=0;}
    if((message==WM_KEYDOWN || message==WM_KEYUP) && (wp=='1' || wp=='2') &&
       recomp_freecam_enabled() && g_gameplay && !g_modal && !g_menu && focused()){
        if(message==WM_KEYDOWN && !(lp & (1L<<30)))recomp_freecam_time_key((unsigned)wp);
        return 1;
    }
    if(message==WM_KEYDOWN && wp==VK_BACK && g_listen>=0){accept_binding(0xffffu);return 1;}
    /* F10 is a Win32 menu key: consume down AND up to avoid activating the
     * window menu. Autorepeat must not toggle repeatedly while held. */
    if(wp==VK_F10 && (message==WM_KEYDOWN || message==WM_SYSKEYDOWN ||
                     message==WM_KEYUP || message==WM_SYSKEYUP)){
        if((message==WM_KEYDOWN || message==WM_SYSKEYDOWN) && !(lp & (1L<<30)) && focused() && !g_isolated)hud_hotkey();
        return 1;
    }
    if(message==WM_SETCURSOR && LOWORD(lp)==HTCLIENT && g_captured){SetCursor(NULL);return 1;}
    if(message==WM_INPUT){
        RAWINPUT raw;UINT size=sizeof(raw);
        if(GetRawInputData((HRAWINPUT)lp,RID_INPUT,&raw,&size,sizeof(RAWINPUTHEADER))==sizeof(raw) &&
           raw.header.dwType==RIM_TYPEMOUSE && !(raw.data.mouse.usFlags&MOUSE_MOVE_ABSOLUTE)){
            if(focused() && !g_isolated && (raw.data.mouse.lLastX || raw.data.mouse.lLastY))
                xbox_InputNotifyKeyboardActivity();
            if(g_captured){InterlockedExchangeAdd(&g_dx,raw.data.mouse.lLastX);InterlockedExchangeAdd(&g_dy,raw.data.mouse.lLastY);}
        }
        /* DefWindowProc must still perform foreground raw-input cleanup. */
    }
    if(message==WM_MOUSEWHEEL && focused() && !g_isolated){
        int delta=GET_WHEEL_DELTA_WPARAM(wp);
        if(g_listen>=0){if(g_device==0 && g_armed && delta)accept_binding(delta>0?WHEEL_UP:WHEEL_DOWN);return 1;}
        if(g_menu==2){g_page=(g_page+page_count()+(delta>0?-1:1))%page_count();return 1;}
        if(!recomp_dev_menu_visible()){
            g_wheel_remainder+=delta;
            int steps=g_wheel_remainder/WHEEL_DELTA;g_wheel_remainder%=WHEEL_DELTA;
            InterlockedExchangeAdd(&g_wheel_pending,steps);
        }
        return 1;
    }
    return 0;
}
void recomp_controls_open(void){g_menu=1;g_page=0;g_listen=-1;g_release=0;release_mouse();}
void recomp_controls_close(void){g_menu=0;g_listen=-1;g_release=0;}
unsigned recomp_controls_count(void){return g_menu==2?visible_actions()+5:7;}
int recomp_controls_is_binding(void){return g_listen>=0 || g_release;}
/* Resolve a virtual retail button using the exact same maps as input. */
static int prompt_binding_context(unsigned *action_inout,int device,int context) {
    unsigned action=*action_inout;
    const int keyboard=device==XBOX_PROMPT_KEYBOARD;
    if(context<0)context=g_context>=0?g_context:(g_in_game?g_last_gameplay_context:-1);
    /* Carry pickup switches to cinematic before the action HUD finishes
     * fading. Y/Use and other gameplay glyphs still belong to the actor's
     * binding map; cinematic Confirm/Back/Start belong to Menus. Do not turn
     * a valid Use binding into an unbound menu channel for that final frame. */
    const int cinematic_gameplay_prompt=g_in_game && g_polled_sub==0x9F19476Eu &&
        action!=4 && action!=8 && action!=9 && !g_menu && !g_modal;
    if(g_prompt_gameplay_body || cinematic_gameplay_prompt){
        if(context<0)context=g_last_gameplay_context;
    }else if(mission_yes_no_active() && action==11){
        context=MENU_CONTEXT; /* No has its own bind, independent of ordinary Back. */
    }else if(menu_active()) {
        if(action==4 && (g_waiting_controller ||
            (g_shell_menu && keyboard && resolved_binding(0,MENU_CONTEXT,4)==0xffffu)))action=8;
        if(g_modal_back_y && (action==11 || action==9))action=9;
        context=MENU_CONTEXT;
    }
    /* Scope tutorials and explicit context prompts use the same semantic
     * zoom bindings as live input, even while the modal disables the actor. */
    if(controls_binding_context(context)==SCOPE_CONTEXT && (action==18 || action==19))
        action=action==19?24:25;
    *action_inout=action;
    return context<0 || context>=CONTROL_CONTEXTS?-1:controls_binding_context(context);
}
int recomp_controls_prompt_action_available(unsigned action,int device) {
    if(action>=CONTROL_ACTIONS)return 0;
    int context=prompt_binding_context(&action,device,-1);
    return context>=0 && action_available(context,action);
}
unsigned short recomp_controls_prompt_binding(unsigned action,int device,int context) {
    if(action>=CONTROL_ACTIONS)return 0xffffu;
    context=prompt_binding_context(&action,device,context);
    if(context<0 || !action_available(context,action))return 0xffffu;
    const int keyboard=device==XBOX_PROMPT_KEYBOARD;
    return g_initialized?resolved_binding(keyboard?0:1,context,action):default_binding(keyboard?0:1,context,action);
}
/* The carousel advertises its dedicated select binding, including its
 * closing fade after gameplay input has resumed. Equipped Fire is separate. */
unsigned short recomp_controls_prompt_support_binding(int device) {
    int d=device==XBOX_PROMPT_KEYBOARD?0:1;
    return g_initialized?resolved_binding(d,MENU_CONTEXT,15):default_binding(d,MENU_CONTEXT,15);
}
/* Resolve the editor's selected page/context, never the live gameplay context.
 * Return false while capturing or unbound so those status messages stay text. */
int recomp_controls_row_binding(uint32_t hash,int *keyboard,unsigned short *binding) {
    if(g_menu!=2 || hash<RECOMP_CONTROLS_ROW+2 || hash>=RECOMP_CONTROLS_ROW+2+visible_actions())return 0;
    unsigned a=action_at(hash-RECOMP_CONTROLS_ROW-2);
    unsigned short b=resolved_binding(g_device,g_edit_context,a);
    if(g_listen==(int)a || b==0xffffu)return 0;
    if(keyboard)*keyboard=!g_device;
    if(binding)*binding=b;
    return 1;
}
const char *recomp_controls_label(uint32_t hash) {
    static char label[128];
    if(hash==RECOMP_CONTROLS_TITLE)return g_menu==2?"REBIND CONTROLS":"CONTROLS";
    if(hash<RECOMP_CONTROLS_ROW || hash>=RECOMP_CONTROLS_ROW+RECOMP_CONTROLS_ROWS)return NULL;
    unsigned row=hash-RECOMP_CONTROLS_ROW;
    if(g_menu==1){
        switch(row){
        case 0:snprintf(label,sizeof(label),"MOUSE AIM: %s",g_mouse?"ON":"OFF");break;
        case 1:snprintf(label,sizeof(label),"MOUSE SENSITIVITY: %d",g_sensitivity);break;
        case 2:snprintf(label,sizeof(label),"INVERT MOUSE Y: %s",g_invert?"ON":"OFF");break;
        case 3:snprintf(label,sizeof(label),"AIM ASSIST: %s",g_aim_assist?"ON":"OFF");break;
        case 4:snprintf(label,sizeof(label),"ALLOW SHARED INPUTS: %s",g_shared_inputs?"ON":"OFF");break;
        case 5:return "REBIND CONTROLS";default:return "BACK";}
    }else{
        if(row==0)snprintf(label,sizeof(label),"DEVICE: %s",g_device?"GAMEPAD":"KEYBOARD / MOUSE");
        else if(row==1)snprintf(label,sizeof(label),"CONTEXT: %s",contexts[g_edit_context]);
        else if(row<2+visible_actions()){unsigned a=action_at(row-2);
            snprintf(label,sizeof(label),recomp_controls_row_binding(hash,NULL,NULL)?"%s%s":"%s: %s",action_name(g_edit_context,a),g_listen==(int)a?"PRESS INPUT (F10 CANCEL)":g_binds[g_device][g_edit_context][a]==0xffffu?"UNBOUND":"");}
        else if(row==2+visible_actions())snprintf(label,sizeof(label),"PAGE: %d / %u (LEFT / RIGHT)",g_page+1,page_count());
        else if(row==3+visible_actions())return "RESET THESE BINDS";
        else return "BACK";
    }
    return label;
}
uint32_t recomp_controls_input(unsigned row,unsigned input,unsigned event) {
    if(g_listen>=0 || g_release)return 1;
    if(event!=1)return input>=2?1:0;
    if(input==4){if(g_menu==2){g_menu=1;g_page=0;return 1;}return 0;}
    if(input!=2 && input!=3 && input!=5)return 0;
    int direction=input==2?-1:1;
    if(g_menu==1){
        if(row==0){g_mouse=!g_mouse;write_value("Controls","MouseAim",g_mouse);}
        if(row==1){g_sensitivity+=direction;if(g_sensitivity<1)g_sensitivity=20;if(g_sensitivity>20)g_sensitivity=1;write_value("Controls","MouseSensitivity",g_sensitivity);}
        if(row==2){g_invert=!g_invert;write_value("Controls","InvertMouseY",g_invert);}
        if(row==3){g_aim_assist=!g_aim_assist;write_value("Controls","AimAssist",g_aim_assist);}
        if(row==4){g_shared_inputs=!g_shared_inputs;write_value("Controls","AllowSharedInputs",g_shared_inputs);}
        if(row==5 && input==5){g_menu=2;g_page=0;}
        if(row==6 && input==5)return 2;
    }else{
        if(row==0)g_device=!g_device;
        else if(row==1){
            do { g_edit_context=(g_edit_context+CONTROL_CONTEXTS+direction)%CONTROL_CONTEXTS; }
            while(!contexts[g_edit_context]);
            g_page=0;
        }
        else if(row<2+visible_actions() && input==5){begin_binding(action_at(row-2));}
        else if(row==2+visible_actions())g_page=(g_page+page_count()+direction)%page_count();
        else if(row==3+visible_actions() && input==5){for(unsigned i=0;i<CONTROL_ACTIONS;++i)g_binds[g_device][g_edit_context][i]=default_binding(g_device,g_edit_context,i);save_context();}
        else if(row==4+visible_actions() && input==5){g_menu=1;g_page=0;}
    }
    return 1;
}

void recomp_controls_freecam_input(XBOX_INPUT_STATE *state) {
    if(recomp_dev_menu_visible()){
        XBOX_INPUT_STATE camera={0};
        if(freecam_overlay_input(0))xbox_InputGetOverlayState(0,&camera);
        recomp_freecam_input(&camera,g_gameplay && !menu_active(),0);
        memset(&state->Gamepad,0,sizeof(state->Gamepad));
        return;
    }
    recomp_freecam_input(state,g_gameplay && !g_modal && !g_menu,0);
}
int recomp_controls_freecam_menu_key(unsigned key) {
    if(key==VK_SPACE || key=='Q' || key=='E')return 1;
    for(unsigned i=16;i<24;++i)if(g_binds[0][3][i]==key)return 1;
    return g_binds[0][3][6]==key;
}
float recomp_controls_freecam_roll(void) {
    if(!recomp_freecam_enabled() || !g_gameplay || g_menu || g_modal ||
       (recomp_dev_menu_visible() && !freecam_overlay_input(1)) ||
       (!focused() && !freecam_overlay_input(1) && !g_test_keys_path))return 0.f;
    return (float)(gameplay_key_pressed('Q',3,16)-gameplay_key_pressed('E',3,16));
}
void recomp_controls_freecam_look(float *x,float *y) {
    *x=*y=0;
    if(!recomp_freecam_enabled() || (!g_captured && !g_test_mouse_path) || !g_gameplay || g_menu || g_modal || recomp_dev_menu_visible())return;
    LONG dx=g_test_mouse_path?g_test_dx:InterlockedExchange(&g_dx,0);
    LONG dy=g_test_mouse_path?g_test_dy:InterlockedExchange(&g_dy,0);
    if(g_test_mouse_path)g_test_dx=g_test_dy=0;
    *x=dx*(.0004f*g_sensitivity);*y=dy*(.0004f*g_sensitivity)*(g_invert?-1.f:1.f);
}
/* Called at the retail angular accumulation point, after saving the original
 * stick inertia. Mouse counts are angles, so FPS and the 1..6 retail substeps
 * cannot change sensitivity or replay a movement. */
/* Peek without consuming: the retail crouch transition holds a world-space
 * aim point until look input cancels it. Raw mouse angles are applied after
 * the stick code, so its earlier cancellation check must see them too. */
int recomp_controls_mouse_look_pending(void) {
    if(recomp_freecam_enabled())return 0;
    if((!g_captured && !g_test_mouse_path) || !g_gameplay || g_menu || !g_mouse)return 0;
    if(g_test_mouse_path)return g_test_dx!=0 || g_test_dy!=0;
    return InterlockedCompareExchange(&g_dx,0,0)!=0 || InterlockedCompareExchange(&g_dy,0,0)!=0;
}
float recomp_controls_mouse_delta(unsigned axis,float original) {
    if(recomp_freecam_enabled())return original;
    if((!g_captured && !g_test_mouse_path) || !g_gameplay || g_menu || !g_mouse)return original;
    LONG counts=g_test_mouse_path ? (axis?g_test_dy:g_test_dx) : InterlockedExchange(axis?&g_dy:&g_dx,0);
    if(g_test_mouse_path){if(axis)g_test_dy=0;else g_test_dx=0;
        if(counts)fprintf(stderr,"[CONTROLS-TEST] consume axis=%u counts=%ld\n",axis,counts);}

    float delta=(float)counts*(.0004f*g_sensitivity);
    if(axis)delta*=g_invert?1.f:-1.f;
    return original+delta;
}
/* Vehicles and zoomed scopes have bounded mechanical controls rather than
 * free camera yaw. Feed those through the real logical joystick event once
 * per simulation update, after controller deadzone processing. */
float recomp_controls_mouse_axis(uint32_t hash,float original,float dt) {
    if(hash==0xD4F1922Bu && g_gameplay && !menu_active() && !recomp_freecam_enabled() &&
       (g_joystick_context==9 || g_joystick_context==4) && g_scope_wheel_zoom!=0){
        /* Xbox inverts physical LY before logical dispatch; preserve the
         * retail event's sign. One detent gives four button-sized steps. */
        if(original!=0){g_scope_wheel_zoom=0;return original*4.f;}
    }
    if(recomp_freecam_enabled())return original;
    if((!g_captured && !g_test_mouse_path) || !g_gameplay || g_menu || !g_mouse || !isfinite(dt) || dt<=0)return original;
    int axis=hash==0xEAA0CE27u?0:hash==0xE9A0CC94u?1:-1;
    if(axis<0)return original;
    if(!(g_joystick_context==2 || g_joystick_context==10 || g_joystick_context==4 || g_joystick_context==9 || (g_joystick_context==0 && !axis)))return original;
    LONG counts=g_test_mouse_path ? (axis?g_test_dy:g_test_dx) : InterlockedExchange(axis?&g_dy:&g_dx,0);
    if(g_test_mouse_path){if(axis)g_test_dy=0;else g_test_dx=0;
        if(counts)fprintf(stderr,"[CONTROLS-TEST] consume axis=%u counts=%ld\n",axis,counts);}

    float value=(float)counts*(.0004f*g_sensitivity)/(dt*2.f);
    if(axis)value*=g_invert?1.f:-1.f;
    if(value>1)value=1;if(value< -1)value=-1;
    /* FreeAim is squared by the original human controller. */
    if(g_joystick_context==4 || g_joystick_context==9)value=copysignf(sqrtf(fabsf(value)),value);
    value+=original;if(value>1)value=1;if(value< -1)value=-1;
    return value;
}
