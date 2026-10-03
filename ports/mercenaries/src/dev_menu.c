/* F9 developer tools. An owned, translucent native panel keeps the renderer's
 * state untouched. Commands are queued here and executed by the retail thread. */
#include "dev_menu.h"
#include "dev_battle.h"
#include "dev_weapons.h"
#include "dev_factions.h"
#include "dev_missions.h"
#include "free_cam.h"
#include "recomp_controls.h"
#include "dev_overlay.h"
#include "xinput_xbox.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <windowsx.h>
static const DevVehicle vehicles[] = {
#include "dev_vehicle_catalog.inc"
};
static const DevVehicle troops[] = {
#include "dev_troop_catalog.inc"
};
static volatile LONG battle_flags;
unsigned recomp_dev_battle_flags(void){return (unsigned)InterlockedCompareExchange(&battle_flags,0,0);}
void recomp_dev_battle_set(unsigned flags){InterlockedExchange(&battle_flags,(LONG)(flags&3u));}
unsigned recomp_dev_troop_count(void){return (unsigned)(sizeof(troops)/sizeof(troops[0]));}
const DevVehicle *recomp_dev_troop_at(unsigned i){return i<recomp_dev_troop_count()?troops+i:NULL;}
static HWND crew_mode, crew_faction, quantity, weapon;
static HWND relation_first, relation_second, relation_kind, relation_apply, relation_help, relation_and, relation_are;
static volatile LONG relation_pending;
static HWND owner, panel, search, faction, kind, list, details, spawn, status, close_after;
static HWND mission_faction, mission_list, mission_start, province_list, travel, world_label, mission_help;
static int mission_view, displayed_province = -2;
static HFONT font, title_font;
static HBRUSH background, field_background;
static void (*bookmark_callback)(void);
static volatile LONG pending=-1;
static volatile LONG boids_pending;
static int busy, selected=-1, shown_count;
static unsigned shown[512];
static int scale=100;
static void (*display_callback)(int);
void recomp_dev_menu_set_display_callback(void (*callback)(int)){display_callback=callback;}
static int developer_setting(const char *name) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
    if (!n || n >= sizeof(path)) return 0;
    char *slash = strrchr(path, '\\');
    if (!slash) return 0;
    slash[1] = 0;
    if (strcat_s(path, sizeof(path), "developer.ini")) return 0;
    char root[MAX_PATH];DWORD length=GetEnvironmentVariableA("MERCENARIES_CONFIG_ROOT",root,MAX_PATH);
    if(length && length+sizeof("\\developer.ini")<=MAX_PATH)snprintf(path,sizeof(path),"%s\\developer.ini",root);
    return GetPrivateProfileIntA("Developer", name, 0, path) == 1;
}
int recomp_dev_menu_allowed(void) {
    static int allowed = -1;
    if (allowed < 0) allowed = developer_setting("developer_menu");
    return allowed;
}
int recomp_developer_logging_enabled(void) {
    return developer_setting("logging");
}
#define C_BG RGB(25,29,35)
#define C_FIELD RGB(36,42,50)
#define C_TEXT RGB(224,230,237)
#define C_ACCENT RGB(99,211,219)
#define ID_SEARCH 101
#define ID_FACTION 102
#define ID_KIND 103
#define ID_LIST 104
#define ID_SPAWN 105
#define ID_CLOSE 106
#define ID_BOOKMARK 107
#define ID_AUTOCLOSE 108
#define ID_INSPECT 109
#define ID_FREECAM 110
#define ID_PERFORMANCE 111
#define ID_VEHICLES 112
#define ID_MISSIONS 113
#define ID_MISSION_FACTION 114
#define ID_MISSION_LIST 115
#define ID_MISSION_START 116
#define ID_PROVINCE 117
#define ID_TRAVEL 118
#define ID_TROOPS 119
#define ID_CREW_MODE 120
#define ID_CREW_FACTION 121
#define ID_QUANTITY 122
#define ID_UNTARGETABLE 123
#define ID_PASSIVE 124
#define ID_BOIDS 125
#define ID_WEAPON 126
#define ID_RELATIONS 127
#define ID_RELATION_FIRST 128
#define ID_RELATION_SECOND 129
#define ID_RELATION_KIND 130
#define ID_RELATION_APPLY 131
static void filter_changed(void);
static const DevVehicle *display_item(unsigned i){return mission_view==2?recomp_dev_troop_at(i):recomp_dev_vehicle_at(i);}
void recomp_dev_freecam_toggle(void)
{
 if(recomp_dev_menu_allowed())recomp_freecam_set(!recomp_freecam_enabled());
}
static void mission_choices(void)
{
 int province=recomp_dev_current_province();
 unsigned faction_index=(unsigned)SendMessageA(mission_faction,CB_GETCURSEL,0,0);
 SendMessageA(mission_list,CB_RESETCONTENT,0,0);
 for(unsigned i=0;i<recomp_dev_mission_count(province,faction_index);++i){
  unsigned number=recomp_dev_mission_number(province,faction_index,i);char text[96];
  const char *special="";
  if(faction_index==0 && number==4)special=province==0?" - Ace of Clubs":" - Ace of Hearts";
  if(faction_index==0 && number==8)special=province==0?" - Ace of Diamonds":" - Ace of Spades";
  snprintf(text,sizeof(text),"Mission %u%s",number,special);
  LRESULT row=SendMessageA(mission_list,CB_ADDSTRING,0,(LPARAM)text);
  SendMessageA(mission_list,CB_SETITEMDATA,row,number);
 }
 SendMessageA(mission_list,CB_SETCURSEL,0,0);
 SetWindowTextA(world_label,province==0?"Current province: Southern":"" );
 if(province==1)SetWindowTextA(world_label,"Current province: Northern");
 if(province<0)SetWindowTextA(world_label,"Enter a normal province to use these tools.");
 EnableWindow(mission_start,!busy && province>=0);
 EnableWindow(travel,!busy && province>=0);
 if(province>=0)SendMessageA(province_list,CB_SETCURSEL,1-province,0);
 displayed_province=province;
}
/* Build filters from the current catalogue so no empty placeholder categories appear. */
static void refresh_catalog_filters(void)
{
 char previous_faction[64],previous_kind[64];
 GetWindowTextA(faction,previous_faction,sizeof(previous_faction));
 GetWindowTextA(kind,previous_kind,sizeof(previous_kind));
 SendMessageA(faction,CB_RESETCONTENT,0,0);SendMessageA(kind,CB_RESETCONTENT,0,0);
 SendMessageA(faction,CB_ADDSTRING,0,(LPARAM)"All factions");
 SendMessageA(kind,CB_ADDSTRING,0,(LPARAM)"All types");
 unsigned count=mission_view==2?recomp_dev_troop_count():recomp_dev_vehicle_count();
 for(unsigned i=0;i<count;++i){
  const DevVehicle *v=display_item(i);
  if(SendMessageA(faction,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)v->faction)==CB_ERR)
   SendMessageA(faction,CB_ADDSTRING,0,(LPARAM)v->faction);
  if(SendMessageA(kind,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)v->kind)==CB_ERR)
   SendMessageA(kind,CB_ADDSTRING,0,(LPARAM)v->kind);
 }
 LRESULT f=SendMessageA(faction,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)previous_faction);
 LRESULT k=SendMessageA(kind,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)previous_kind);
 SendMessageA(faction,CB_SETCURSEL,f==CB_ERR?0:f,0);
 SendMessageA(kind,CB_SETCURSEL,k==CB_ERR?0:k,0);
}
static void switch_view(int missions)
{
 mission_view=missions;
 HWND vehicle_controls[]={search,faction,kind,list,details,spawn,close_after,crew_mode,crew_faction,quantity,weapon};
 HWND mission_controls[]={mission_faction,mission_list,mission_start,province_list,travel,world_label,mission_help};
 for(unsigned i=0;i<sizeof(vehicle_controls)/sizeof(vehicle_controls[0]);++i)ShowWindow(vehicle_controls[i],(missions==0 || missions==2)?SW_SHOW:SW_HIDE);
 for(unsigned i=0;i<sizeof(mission_controls)/sizeof(mission_controls[0]);++i)ShowWindow(mission_controls[i],missions==1?SW_SHOW:SW_HIDE);
 ShowWindow(crew_mode,missions==0?SW_SHOW:SW_HIDE);
 ShowWindow(crew_faction,missions==0?SW_SHOW:SW_HIDE);
 ShowWindow(quantity,missions==2?SW_SHOW:SW_HIDE);
 ShowWindow(weapon,missions==2?SW_SHOW:SW_HIDE);
 HWND relation_controls[]={relation_first,relation_second,relation_kind,relation_apply,relation_help,relation_and,relation_are};
 for(unsigned i=0;i<sizeof(relation_controls)/sizeof(relation_controls[0]);++i)ShowWindow(relation_controls[i],missions==3?SW_SHOW:SW_HIDE);
 ShowWindow(kind,missions==0?SW_SHOW:SW_HIDE);
 if(missions==3){SetWindowTextA(status,"Select two different factions and a relationship, then Apply.");}
 else if(missions==1)mission_choices();else {refresh_catalog_filters();filter_changed();}
 SetFocus(missions==3?relation_first:missions==1?mission_faction:search);InvalidateRect(panel,NULL,TRUE);
}
static void request_transition(int province_travel)
{
 if(busy)return;
 int accepted=0;
 if(province_travel){
  LRESULT destination=SendMessageA(province_list,CB_GETCURSEL,0,0);
  if(destination>=0)accepted=recomp_dev_request_travel((unsigned)destination);
 }else{
  LRESULT row=SendMessageA(mission_list,CB_GETCURSEL,0,0);
  LRESULT faction_index=SendMessageA(mission_faction,CB_GETCURSEL,0,0);
  if(row>=0 && faction_index>=0)accepted=recomp_dev_request_mission((unsigned)faction_index,
       (unsigned)SendMessageA(mission_list,CB_GETITEMDATA,row,0));
 }
 if(!accepted){SetWindowTextA(status,"Choose an available mission or a different province.");return;}
 busy=1;EnableWindow(spawn,FALSE);EnableWindow(mission_start,FALSE);EnableWindow(travel,FALSE);
 SetWindowTextA(status,"Request queued for the game thread...");
}
static int px(int n){return MulDiv(n,scale,100);}
unsigned recomp_dev_vehicle_count(void){return (unsigned)(sizeof(vehicles)/sizeof(vehicles[0]));}
const DevVehicle *recomp_dev_vehicle_at(unsigned i){return i<recomp_dev_vehicle_count()?vehicles+i:NULL;}
int recomp_dev_menu_visible(void){return panel&&IsWindowVisible(panel);}
int recomp_dev_menu_camera_input(int keyboard)
{
 if(!recomp_dev_menu_visible() || !recomp_freecam_enabled() || IsIconic(owner))return 0;
 HWND foreground=GetForegroundWindow();
 if(foreground!=owner && foreground!=panel)return 0;
 if(keyboard){
  GUITHREADINFO info={0};info.cbSize=sizeof(info);
  if(!GetGUIThreadInfo(GetWindowThreadProcessId(owner,NULL),&info))return 0;
  char cls[32]={0};GetClassNameA(info.hwndFocus,cls,sizeof(cls));
  /* Text entry and drop-down navigation own their keys. */
  if(!strcmp(cls,"Edit") || !strcmp(cls,"ComboBox") || !strcmp(cls,"ComboLBox"))return 0;
 }
 return 1;
}
int recomp_dev_take_boids(void){return InterlockedExchange(&boids_pending,0)!=0;}
void recomp_dev_request_boids(void){InterlockedExchange(&boids_pending,1);}
int recomp_dev_take_spawn(unsigned *i){LONG command=InterlockedExchange(&pending,-1);if(command<0)return 0;*i=(unsigned)command;return 1;}
static int contains(const char *text,const char *query){
 for(;*text;text++){const char *a=text,*b=query;while(*a&&*b&&tolower((unsigned char)*a)==tolower((unsigned char)*b)){a++;b++;}if(!*b)return 1;}return !*query;
}
static void selection_changed(void){
 int row=(int)SendMessageA(list,LB_GETCURSEL,0,0);char text[1024];
 selected=row>=0&&row<shown_count?(int)shown[row]:-1;
 EnableWindow(spawn,selected>=0&&!busy);
 if(selected<0){SetWindowTextA(details,"No matching units.\r\n\r\nTry another search or filter.");return;}
 const DevVehicle *v=display_item((unsigned)selected);
 snprintf(text,sizeof(text),"%s\r\n\r\nFaction: %s\r\nType: %s\r\n\r\n%s\r\n\r\nUse open, level ground.",v->name,v->faction,v->kind,v->template_name);
 SetWindowTextA(details,text);
}
static void filter_changed(void){
 char query[160],f[64],k[64];GetWindowTextA(search,query,sizeof(query));GetWindowTextA(faction,f,sizeof(f));GetWindowTextA(kind,k,sizeof(k));
 shown_count=0;SendMessageA(list,WM_SETREDRAW,FALSE,0);SendMessageA(list,LB_RESETCONTENT,0,0);
 unsigned total=mission_view==2?recomp_dev_troop_count():recomp_dev_vehicle_count();
 for(unsigned i=0;i<total;i++){
  const DevVehicle *v=display_item(i);
  if(f[0]&&strcmp(f,"All factions")&&strcmp(f,v->faction))continue;
  if(mission_view!=2&&k[0]&&strcmp(k,"All types")&&strcmp(k,v->kind))continue;
  if(!contains(v->name,query)&&!contains(v->template_name,query)&&!contains(v->faction,query))continue;
  if(shown_count>=512)break;
  shown[shown_count++]=i;SendMessageA(list,LB_ADDSTRING,0,(LPARAM)v->name);
 }
 SendMessageA(list,LB_SETCURSEL,0,0);SendMessageA(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,NULL,TRUE);selection_changed();
 char text[160];snprintf(text,sizeof(text),"%d / %u variants  |  World remains live",shown_count,total);if(!busy)SetWindowTextA(status,text);
}
static void request_spawn(void){
 if(selected<0||busy)return;
 unsigned command=(unsigned)selected;
 if(mission_view==2){
  unsigned counts[]={1,4,8,12};LRESULT q=SendMessageA(quantity,CB_GETCURSEL,0,0);
  LRESULT w=SendMessageA(weapon,CB_GETCURSEL,0,0);
  command|=(unsigned)(w>=0 && w<DEV_WEAPON_COUNT?w:0)<<DEV_SPAWN_WEAPON_SHIFT;
  command|=DEV_SPAWN_TROOP|((counts[q>=0&&q<4?q:0]-1u)<<DEV_SPAWN_COUNT_SHIFT);
 }else{
  LRESULT mode=SendMessageA(crew_mode,CB_GETCURSEL,0,0),f=SendMessageA(crew_faction,CB_GETCURSEL,0,0);
  command|=(unsigned)(mode>=0&&mode<=2?mode:0)<<DEV_SPAWN_CREW_SHIFT;
  command|=(unsigned)(f>=0&&f<5?f:0)<<DEV_SPAWN_FACTION_SHIFT;
 }
 busy=1;InterlockedExchange(&pending,(LONG)command);EnableWindow(spawn,FALSE);EnableWindow(mission_start,FALSE);EnableWindow(travel,FALSE);SetWindowTextA(status,"Finding a clear position...");
}
int recomp_dev_request_relation(unsigned first,unsigned second,unsigned relation)
{
 unsigned command=dev_relation_command(first,second,relation);
 return command && recomp_dev_menu_allowed() &&
        InterlockedCompareExchange(&relation_pending,(LONG)command,0)==0;
}
unsigned recomp_dev_take_relation(void){return (unsigned)InterlockedExchange(&relation_pending,0);}
void recomp_dev_relation_result(int success,const char *message)
{
 (void)success;busy=0;SetWindowTextA(status,message);
 EnableWindow(spawn,selected>=0);EnableWindow(relation_apply,TRUE);mission_choices();
}
static void request_relation(void)
{
 if(busy)return;
 unsigned a=(unsigned)SendMessageA(relation_first,CB_GETCURSEL,0,0);
 unsigned b=(unsigned)SendMessageA(relation_second,CB_GETCURSEL,0,0);
 unsigned v=(unsigned)SendMessageA(relation_kind,CB_GETCURSEL,0,0);
 if(!recomp_dev_request_relation(a,b,v)){SetWindowTextA(status,"Choose two different factions and a relationship.");return;}
 busy=1;EnableWindow(relation_apply,FALSE);EnableWindow(spawn,FALSE);
 EnableWindow(mission_start,FALSE);EnableWindow(travel,FALSE);
 SetWindowTextA(status,"Updating faction relations...");
}
static void hide_panel(void){
 ShowWindow(panel,SW_HIDE);xbox_InputSetOverlayCapture(FALSE);SetForegroundWindow(owner);SetFocus(owner);if(display_callback)display_callback(0);
}
void recomp_dev_spawn_progress(const char *message){SetWindowTextA(status,message);}
void recomp_dev_spawn_result(int success,const char *message){
 busy=0;SetWindowTextA(status,message);EnableWindow(spawn,selected>=0);mission_choices();
 if(success&&SendMessageA(close_after,BM_GETCHECK,0,0)==BST_CHECKED)hide_panel();
}
void recomp_dev_transition_result(int success,const char *message)
{
 busy=0;SetWindowTextA(status,message);EnableWindow(spawn,selected>=0);mission_choices();
 if(success && recomp_dev_menu_visible())hide_panel();
}
static LRESULT CALLBACK child_proc(HWND h,UINT m,WPARAM w,LPARAM l){
 if(m==WM_KEYDOWN){
  if(w==VK_F11){if(!(l&(1L<<30)))recomp_dev_freecam_toggle();return 0;}
  if(w==VK_F9||w==VK_ESCAPE){if(!(l&(1L<<30)))hide_panel();return 0;}
  if(w==VK_RETURN){
   if(!(l&(1L<<30))){
    if(h==search||h==list||h==spawn)request_spawn();
    else {char cls[32];GetClassNameA(h,cls,sizeof(cls));if(!strcmp(cls,"Button"))SendMessageA(h,BM_CLICK,0,0);}
   }
   return 0;
  }
  if(w==VK_TAB){HWND next=GetNextDlgTabItem(panel,h,(GetKeyState(VK_SHIFT)&0x8000)!=0);if(next)SetFocus(next);return 0;}
 }
 /* Movement must not also activate buttons or type-select another troop. */
 if((m==WM_KEYDOWN || m==WM_KEYUP || m==WM_CHAR) &&
    recomp_dev_menu_camera_input(1) && recomp_controls_freecam_menu_key((unsigned)(m==WM_CHAR?toupper((unsigned char)w):w)))return 0;
 return CallWindowProcA((WNDPROC)GetWindowLongPtrA(h,GWLP_USERDATA),h,m,w,l);
}
static HWND control(const char *cls,const char *text,DWORD style,int id){
 HWND h=CreateWindowExA(0,cls,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,panel,(HMENU)(INT_PTR)id,GetModuleHandleA(NULL),NULL);
 SendMessageA(h,WM_SETFONT,(WPARAM)font,FALSE);
 if(style&WS_TABSTOP){WNDPROC old=(WNDPROC)SetWindowLongPtrA(h,GWLP_WNDPROC,(LONG_PTR)child_proc);SetWindowLongPtrA(h,GWLP_USERDATA,(LONG_PTR)old);}
 return h;
}
static void place(HWND h,int x,int y,int w,int height){MoveWindow(h,px(x),px(y),px(w),px(height),TRUE);}
static void layout(void){
 RECT r;GetClientRect(panel,&r);int width=MulDiv(r.right,100,scale),height=MulDiv(r.bottom,100,scale),left=170,body=width-left-24,split=(body*3)/5;
 place(search,left,78,body,32);place(faction,left,124,split-8,250);place(kind,left+split,124,body-split,250);
 place(list,left,169,split-8,height-241);place(details,left+split+8,177,body-split-16,height-411);
 place(crew_mode,left+split+8,height-220,body-split-12,160);
 place(crew_faction,left+split+8,height-181,body-split-12,220);
 place(weapon,left+split+8,height-220,body-split-12,300);
 place(quantity,left+split+8,height-181,body-split-12,180);
 place(close_after,left+split+8,height-139,body-split-12,26);place(spawn,left+split+8,height-100,body-split-12,38);
 place(status,left,height-46,body,32);place(GetDlgItem(panel,ID_CLOSE),width-88,15,66,30);place(GetDlgItem(panel,ID_BOOKMARK),20,268,130,36);place(GetDlgItem(panel,ID_INSPECT),20,314,130,36);
 place(GetDlgItem(panel,ID_FREECAM),20,360,130,28);
 place(GetDlgItem(panel,ID_PERFORMANCE),20,397,145,28);
 place(GetDlgItem(panel,ID_VEHICLES),20,76,130,32);place(GetDlgItem(panel,ID_MISSIONS),20,154,130,32);
 place(GetDlgItem(panel,ID_TROOPS),20,115,130,32);
 place(GetDlgItem(panel,ID_UNTARGETABLE),20,437,145,28);
 place(GetDlgItem(panel,ID_PASSIVE),20,474,145,28);
 place(GetDlgItem(panel,ID_BOIDS),20,515,145,36);
 place(GetDlgItem(panel,ID_RELATIONS),20,193,130,32);
 place(relation_first,left,100,(body-50)/2,250);
 place(relation_and,left+(body-50)/2+8,104,34,25);
 place(relation_second,left+(body+50)/2,100,(body-50)/2,250);
 place(relation_are,left,153,38,25);place(relation_kind,left+42,149,220,200);
 place(relation_apply,left+278,149,120,34);
 place(relation_help,left,212,body,height-290);
 place(world_label,left,80,body,30);place(mission_faction,left,123,210,220);
 place(mission_list,left+225,123,body-225,240);place(mission_start,left,170,200,36);
 place(province_list,left,237,250,160);place(travel,left+266,237,170,36);
 place(mission_help,left,300,body,height-365);
 InvalidateRect(panel,NULL,TRUE);
}
static void draw_button(const DRAWITEMSTRUCT *d){
 RECT r=d->rcItem;COLORREF color=(d->itemState&ODS_SELECTED)?RGB(57,89,102):C_FIELD;
 HBRUSH brush=CreateSolidBrush(color);FillRect(d->hDC,&r,brush);DeleteObject(brush);
 if(d->itemState&ODS_FOCUS){RECT f=r;InflateRect(&f,-3,-3);DrawFocusRect(d->hDC,&f);}
 char text[80];GetWindowTextA(d->hwndItem,text,sizeof(text));SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,(d->itemState&ODS_DISABLED)?RGB(110,118,127):C_TEXT);SelectObject(d->hDC,font);DrawTextA(d->hDC,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
}
static void paint_panel(HWND h,HDC dc)
{
 RECT r;GetClientRect(h,&r);FillRect(dc,&r,background);SetBkMode(dc,TRANSPARENT);SelectObject(dc,title_font);SetTextColor(dc,C_TEXT);TextOutA(dc,px(22),px(22),"Developer tools",15);SelectObject(dc,font);SetTextColor(dc,C_ACCENT);SetTextColor(dc,RGB(148,161,174));TextOutA(dc,px(22),px(238),"DIAGNOSTICS",11);TextOutA(dc,px(22),r.bottom-px(40),"F9 / Esc to close",17);if(mission_view==3)TextOutA(dc,px(170),px(52),"Faction relationships",21);else if(mission_view==1)TextOutA(dc,px(170),px(52),"Mission selection and province travel",36);else TextOutA(dc,px(170),px(52),"Search name, faction or variant",30);
}
static LRESULT CALLBACK panel_proc(HWND h,UINT m,WPARAM w,LPARAM l){
 switch(m){
 case WM_NCHITTEST:{LRESULT hit=DefWindowProcA(h,m,w,l);if(hit==HTCLIENT){POINT pt={GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&pt);if(pt.y<px(48)&&pt.x<px(800))return HTCAPTION;}return hit;}
 case WM_TIMER:if(w==ID_FREECAM){if(displayed_province!=recomp_dev_current_province())mission_choices();SendDlgItemMessageA(h,ID_FREECAM,BM_SETCHECK,recomp_freecam_enabled()?BST_CHECKED:BST_UNCHECKED,0);return 0;}break;
 case WM_CLOSE:hide_panel();return 0;
 case WM_KEYDOWN:if(w==VK_F11){if(!(l&(1L<<30)))recomp_dev_freecam_toggle();return 0;}if(w==VK_F9||w==VK_ESCAPE){hide_panel();return 0;}break;
 case WM_SIZE:if(list)layout();return 0;
 case WM_GETMINMAXINFO:((MINMAXINFO*)l)->ptMinTrackSize.x=px(860);((MINMAXINFO*)l)->ptMinTrackSize.y=px(620);return 0;
 case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:
  SetTextColor((HDC)w,C_TEXT);SetBkColor((HDC)w,m==WM_CTLCOLORSTATIC?C_BG:C_FIELD);return (LRESULT)(m==WM_CTLCOLORSTATIC?background:field_background);
 case WM_COMMAND:
  if(LOWORD(w)==ID_VEHICLES){switch_view(0);return 0;}
  if(LOWORD(w)==ID_RELATIONS){switch_view(3);return 0;}
  if(LOWORD(w)==ID_RELATION_APPLY){request_relation();return 0;}
  if(LOWORD(w)==ID_TROOPS){switch_view(2);return 0;}
  if(LOWORD(w)==ID_UNTARGETABLE || LOWORD(w)==ID_PASSIVE){
   recomp_dev_battle_set((SendDlgItemMessageA(h,ID_UNTARGETABLE,BM_GETCHECK,0,0)==BST_CHECKED?1u:0u)|(SendDlgItemMessageA(h,ID_PASSIVE,BM_GETCHECK,0,0)==BST_CHECKED?2u:0u));return 0;
  }
  if(LOWORD(w)==ID_MISSIONS){switch_view(1);return 0;}
  if(LOWORD(w)==ID_CREW_MODE && HIWORD(w)==CBN_SELCHANGE){EnableWindow(crew_faction,SendMessageA(crew_mode,CB_GETCURSEL,0,0)>0);return 0;}
  if(LOWORD(w)==ID_MISSION_FACTION && HIWORD(w)==CBN_SELCHANGE){mission_choices();return 0;}
  if(LOWORD(w)==ID_MISSION_START){request_transition(0);return 0;}
  if(LOWORD(w)==ID_TRAVEL){request_transition(1);return 0;}
  if(LOWORD(w)==ID_PERFORMANCE && HIWORD(w)==BN_CLICKED){recomp_dev_overlay_set(SendDlgItemMessageA(h,ID_PERFORMANCE,BM_GETCHECK,0,0)==BST_CHECKED);return 0;}
  if(LOWORD(w)==ID_FREECAM && HIWORD(w)==BN_CLICKED){recomp_freecam_set(SendDlgItemMessageA(h,ID_FREECAM,BM_GETCHECK,0,0)==BST_CHECKED);return 0;}
  if(LOWORD(w)==ID_CLOSE){hide_panel();return 0;}
  if(LOWORD(w)==ID_BOOKMARK){if(bookmark_callback)bookmark_callback();SetWindowTextA(status,"Diagnostic bookmark saved (F8).");return 0;}
  if(LOWORD(w)==ID_BOIDS){
   if(!busy && recomp_dev_menu_allowed()){
    busy=1;EnableWindow(spawn,FALSE);EnableWindow(mission_start,FALSE);EnableWindow(travel,FALSE);
    SetWindowTextA(status,"Creating a flock near the player...");recomp_dev_request_boids();
   }
   return 0;
  }
  if(LOWORD(w)==ID_INSPECT){if(!busy){busy=1;InterlockedExchange(&pending,0x7fffffff);EnableWindow(spawn,FALSE);}return 0;}
  if(LOWORD(w)==ID_SPAWN){request_spawn();return 0;}
  if((LOWORD(w)==ID_SEARCH&&HIWORD(w)==EN_CHANGE)||((LOWORD(w)==ID_FACTION||LOWORD(w)==ID_KIND)&&HIWORD(w)==CBN_SELCHANGE)){filter_changed();return 0;}
  if(LOWORD(w)==ID_LIST){if(HIWORD(w)==LBN_SELCHANGE)selection_changed();else if(HIWORD(w)==LBN_DBLCLK)request_spawn();return 0;}break;
 case WM_DRAWITEM:{const DRAWITEMSTRUCT *d=(const DRAWITEMSTRUCT*)l;
  if(d->CtlID!=ID_LIST){draw_button(d);return TRUE;}if(d->itemID==(UINT)-1||d->itemID>=(unsigned)shown_count)return TRUE;
  const DevVehicle *v=display_item(shown[d->itemID]);RECT r=d->rcItem;HBRUSH b=CreateSolidBrush(d->itemState&ODS_SELECTED?RGB(44,78,89):C_FIELD);FillRect(d->hDC,&r,b);DeleteObject(b);
  SelectObject(d->hDC,font);SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,C_TEXT);r.left+=px(10);r.top+=px(5);DrawTextA(d->hDC,v->name,-1,&r,DT_SINGLELINE|DT_END_ELLIPSIS);
  char text[240];snprintf(text,sizeof(text),"%s  /  %s",v->faction,v->template_name);r.top+=px(22);SetTextColor(d->hDC,RGB(153,169,181));DrawTextA(d->hDC,text,-1,&r,DT_SINGLELINE|DT_END_ELLIPSIS);return TRUE;}
 case WM_PRINTCLIENT:paint_panel(h,(HDC)w);return 0;
 case WM_PAINT:{PAINTSTRUCT ps;HDC dc=BeginPaint(h,&ps);paint_panel(h,dc);EndPaint(h,&ps);return 0;}
 }
 return DefWindowProcA(h,m,w,l);
}
void recomp_dev_menu_init(HWND h,void (*bookmark)(void)){owner=h;bookmark_callback=bookmark;}
void recomp_dev_menu_toggle(void){
 if(!recomp_dev_menu_allowed())return;
 if(recomp_dev_menu_visible()){hide_panel();return;}
 if(!owner)return;
 if(!panel){
  MONITORINFO monitor={sizeof(monitor)};GetMonitorInfoA(MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST),&monitor);
  HDC dc=GetDC(owner);scale=MulDiv(GetDeviceCaps(dc,LOGPIXELSX),100,96);ReleaseDC(owner,dc);
  int available_width=monitor.rcWork.right-monitor.rcWork.left-24,available_height=monitor.rcWork.bottom-monitor.rcWork.top-24;
  if(px(980)>available_width)scale=MulDiv(available_width,100,980);
  if(px(650)>available_height)scale=MulDiv(available_height,100,650);
  if(scale<50)scale=50;
  font=CreateFontA(-px(15),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");title_font=CreateFontA(-px(21),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,"Segoe UI");
  background=CreateSolidBrush(C_BG);field_background=CreateSolidBrush(C_FIELD);WNDCLASSA c={0};c.lpfnWndProc=panel_proc;c.hInstance=GetModuleHandleA(NULL);c.hCursor=LoadCursorA(NULL,IDC_ARROW);c.lpszClassName="MercenariesDeveloperTools";RegisterClassA(&c);
  RECT r;GetWindowRect(owner,&r);
  int x=r.left+px(35),y=r.top+px(65);
  if(x+px(980)>monitor.rcWork.right)x=monitor.rcWork.right-px(980);
  if(y+px(650)>monitor.rcWork.bottom)y=monitor.rcWork.bottom-px(650);
  if(x<monitor.rcWork.left)x=monitor.rcWork.left;if(y<monitor.rcWork.top)y=monitor.rcWork.top;
  panel=CreateWindowExA(WS_EX_TOOLWINDOW|WS_EX_LAYERED,c.lpszClassName,"Mercenaries - Developer tools",WS_POPUP|WS_THICKFRAME,x,y,px(980),px(650),owner,NULL,c.hInstance,NULL);
  if(!panel)return;SetLayeredWindowAttributes(panel,0,238,LWA_ALPHA);
  search=control("EDIT","",WS_TABSTOP|ES_AUTOHSCROLL|WS_BORDER,ID_SEARCH);SendMessageA(search,EM_SETLIMITTEXT,150,0);
  faction=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_FACTION);kind=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_KIND);
  list=control("LISTBOX","",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,ID_LIST);SendMessageA(list,LB_SETITEMHEIGHT,0,px(52));
  details=control("STATIC","",SS_LEFT,0);spawn=control("BUTTON","Spawn",WS_TABSTOP|BS_OWNERDRAW,ID_SPAWN);status=control("STATIC","",SS_LEFT,0);
  close_after=control("BUTTON","Close after spawning",WS_TABSTOP|BS_AUTOCHECKBOX,ID_AUTOCLOSE);control("BUTTON","Close",WS_TABSTOP|BS_OWNERDRAW,ID_CLOSE);control("BUTTON","Bookmark log",WS_TABSTOP|BS_OWNERDRAW,ID_BOOKMARK);control("BUTTON","Inspect last spawn",WS_TABSTOP|BS_OWNERDRAW,ID_INSPECT);control("BUTTON","Free Cam (F11)",WS_TABSTOP|BS_AUTOCHECKBOX,ID_FREECAM);control("BUTTON","FPS / Frametime",WS_TABSTOP|BS_AUTOCHECKBOX,ID_PERFORMANCE);control("BUTTON","Vehicles",WS_TABSTOP|BS_OWNERDRAW,ID_VEHICLES);
  control("BUTTON","Missions / Travel",WS_TABSTOP|BS_OWNERDRAW,ID_MISSIONS);
  control("BUTTON","Troops",WS_TABSTOP|BS_OWNERDRAW,ID_TROOPS);
  control("BUTTON","Untargetable",WS_TABSTOP|BS_AUTOCHECKBOX,ID_UNTARGETABLE);
  control("BUTTON","Passive Mode",WS_TABSTOP|BS_AUTOCHECKBOX,ID_PASSIVE);
  control("BUTTON","Boids Simulation",WS_TABSTOP|BS_OWNERDRAW,ID_BOIDS);
  crew_mode=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_CREW_MODE);
  const char *modes[]={"Empty vehicle","Driver / pilot only","Full crew and passengers"};
  for(unsigned i=0;i<3;i++)SendMessageA(crew_mode,CB_ADDSTRING,0,(LPARAM)modes[i]);
  SendMessageA(crew_mode,CB_SETCURSEL,0,0);
  crew_faction=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_CREW_FACTION);
  const char *crew_factions[]={"Crew: Allied Nations","Crew: Chinese","Crew: Russian Mafia","Crew: North Korean","Crew: South Korean"};
  for(unsigned i=0;i<5;i++)SendMessageA(crew_faction,CB_ADDSTRING,0,(LPARAM)crew_factions[i]);
  SendMessageA(crew_faction,CB_SETCURSEL,3,0);EnableWindow(crew_faction,FALSE);
  quantity=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_QUANTITY);
  const char *quantities[]={"1 soldier","4 soldiers","8 soldiers","12 soldiers"};
  for(unsigned i=0;i<4;i++)SendMessageA(quantity,CB_ADDSTRING,0,(LPARAM)quantities[i]);
  SendMessageA(quantity,CB_SETCURSEL,0,0);
  weapon=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_WEAPON);
  for(unsigned i=0;i<DEV_WEAPON_COUNT;++i)SendMessageA(weapon,CB_ADDSTRING,0,(LPARAM)dev_weapons[i].name);
  SendMessageA(weapon,CB_SETCURSEL,0,0);
  control("BUTTON","Factions",WS_TABSTOP|BS_OWNERDRAW,ID_RELATIONS);
  relation_first=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_RELATION_FIRST);
  relation_second=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_RELATION_SECOND);
  relation_kind=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_RELATION_KIND);
  for(unsigned i=0;i<7u;++i){
   SendMessageA(relation_first,CB_ADDSTRING,0,(LPARAM)dev_faction_names[i]);
   SendMessageA(relation_second,CB_ADDSTRING,0,(LPARAM)dev_faction_names[i]);
  }
  for(unsigned i=0;i<4u;++i)SendMessageA(relation_kind,CB_ADDSTRING,0,(LPARAM)dev_relation_names[i]);
  SendMessageA(relation_first,CB_SETCURSEL,0,0);SendMessageA(relation_second,CB_SETCURSEL,5,0);
  SendMessageA(relation_kind,CB_SETCURSEL,3,0);
  relation_and=control("STATIC","and",SS_LEFT,0);relation_are=control("STATIC","are",SS_LEFT,0);
  relation_apply=control("BUTTON","Apply",WS_TABSTOP|BS_OWNERDRAW,ID_RELATION_APPLY);
  relation_help=control("STATIC","Apply changes both factions' attitudes toward each other.",SS_LEFT,0);
  world_label=control("STATIC","",SS_LEFT,0);
  mission_faction=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_MISSION_FACTION);
  const char *mission_factions[]={"Allied Nations","Chinese","Russian Mafia","South Korean"};
  for(unsigned i=0;i<4;++i)SendMessageA(mission_faction,CB_ADDSTRING,0,(LPARAM)mission_factions[i]);
  SendMessageA(mission_faction,CB_SETCURSEL,0,0);
  mission_list=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,ID_MISSION_LIST);
  mission_start=control("BUTTON","Start mission",WS_TABSTOP|BS_OWNERDRAW,ID_MISSION_START);
  province_list=control("COMBOBOX","",WS_TABSTOP|CBS_DROPDOWNLIST,ID_PROVINCE);
  SendMessageA(province_list,CB_ADDSTRING,0,(LPARAM)"Southern Province");
  SendMessageA(province_list,CB_ADDSTRING,0,(LPARAM)"Northern Province");
  travel=control("BUTTON","Travel",WS_TABSTOP|BS_OWNERDRAW,ID_TRAVEL);
  mission_help=control("STATIC","Select a faction and mission in the current province. Travel first to select missions in the other province.\r\n\r\nMission selection uses the original developer skip route and changes the active campaign state. Use a test save.\r\n\r\nProvince travel requires no active contract and uses the normal Allied arrival point. Free Cam and time controls reset when a transition starts.",SS_LEFT,0);
  SetTimer(panel,ID_FREECAM,200,NULL);layout();filter_changed();switch_view(0);
 }
 if(display_callback)display_callback(1);
 xbox_InputSetOverlayCapture(TRUE);ShowWindow(panel,SW_SHOW);SetForegroundWindow(panel);SetFocus(recomp_freecam_enabled()?panel:(mission_view==3?relation_first:mission_view==1?mission_faction:search));
}
