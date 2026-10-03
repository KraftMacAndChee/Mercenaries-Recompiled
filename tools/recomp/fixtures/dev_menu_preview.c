/* Render and exercise the real native panel without showing desktop windows. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#ifndef _MSC_VER
int strcat_s(char *,size_t,const char *);
#endif
#include "dev_missions.h"
#include "free_cam.h"
#include "dev_overlay.h"
static int preview_show(HWND,int);
static HWND preview_foreground,preview_focus;
static HWND preview_get_foreground(void){return preview_foreground;}
static BOOL preview_gui_info(DWORD thread,GUITHREADINFO *info){info->hwndFocus=preview_focus;return TRUE;}
#define GetForegroundWindow preview_get_foreground
#define GetGUIThreadInfo preview_gui_info
#define ShowWindow preview_show
#define SetForegroundWindow(h) 0
#define SetFocus(h) ((HWND)0)
#define GetPrivateProfileIntA(a,b,c,d) 1
#include "dev_menu.c"
#undef ShowWindow
static int preview_show(HWND h,int command){if(h==panel){SetWindowPos(h,HWND_BOTTOM,-32000,-32000,0,0,SWP_NOSIZE|SWP_NOACTIVATE);return ShowWindow(h,SW_SHOWNOACTIVATE);}return ShowWindow(h,command);}
static int freecam;
int recomp_controls_freecam_menu_key(unsigned key){return key=='W' || key==VK_SPACE;}
void recomp_freecam_set(int value){freecam=value;}
int recomp_freecam_enabled(void){return freecam;}
void recomp_dev_overlay_set(int value){(void)value;}
void xbox_InputSetOverlayCapture(BOOL value){(void)value;}
int recomp_dev_current_province(void){return 1;}
unsigned recomp_dev_mission_count(int p,unsigned f){return 4;}
unsigned recomp_dev_mission_number(int p,unsigned f,unsigned row){return row+1;}
int recomp_dev_request_mission(unsigned f,unsigned n){return 1;}
int recomp_dev_request_travel(unsigned p){return 1;}
static void capture(const char *path){
 RECT r;GetClientRect(panel,&r);int w=r.right,h=r.bottom;
 BITMAPINFO bi={0};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=w;bi.bmiHeader.biHeight=-h;bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
 void *pixels;HDC dc=CreateCompatibleDC(NULL);HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,NULL,0);SelectObject(dc,bitmap);
 SendMessageA(panel,WM_PRINT,(WPARAM)dc,PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
 BITMAPFILEHEADER head={0};head.bfType=0x4d42;head.bfOffBits=sizeof(head)+sizeof(bi.bmiHeader);head.bfSize=head.bfOffBits+w*h*4;
 FILE *f=fopen(path,"wb");assert(f);fwrite(&head,sizeof(head),1,f);fwrite(&bi.bmiHeader,sizeof(bi.bmiHeader),1,f);fwrite(pixels,w*h*4,1,f);fclose(f);DeleteDC(dc);DeleteObject(bitmap);
}
int main(int argc,char **argv){
 assert(argc==3);HWND window=CreateWindowA("STATIC","test owner",WS_POPUP,0,0,640,480,NULL,NULL,GetModuleHandleA(NULL),NULL);
 recomp_dev_menu_init(window,NULL);recomp_dev_menu_toggle();assert(panel);RECT bounds;GetWindowRect(panel,&bounds);assert(bounds.left==-32000);
 assert(SendMessageA(faction,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)"unknown")==CB_ERR);
 assert(SendMessageA(faction,CB_GETCOUNT,0,0)==7);
 SendMessageA(faction,CB_SELECTSTRING,(WPARAM)-1,(LPARAM)"civ");filter_changed();assert(shown_count==25);
 capture(argv[1]);switch_view(2);assert(shown_count==20);capture(argv[2]);
 assert(SendMessageA(faction,CB_GETCOUNT,0,0)==6);
 assert(SendMessageA(faction,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)"unknown")==CB_ERR);
 assert(SendMessageA(faction,CB_FINDSTRINGEXACT,(WPARAM)-1,(LPARAM)"civ")==CB_ERR);
 SendMessageA(faction,CB_SELECTSTRING,(WPARAM)-1,(LPARAM)"mafia");filter_changed();assert(shown_count==3);
 switch_view(0);assert(shown_count==13);switch_view(2);assert(shown_count==3);
 SendMessageA(faction,CB_SETCURSEL,0,0);filter_changed();
 SendMessageA(quantity,CB_SETCURSEL,2,0);SendMessageA(weapon,CB_SETCURSEL,8,0);request_spawn();unsigned request;
 assert(recomp_dev_take_spawn(&request) && (request&DEV_SPAWN_TROOP) && ((request>>DEV_SPAWN_COUNT_SHIFT)&15u)==7u && ((request>>DEV_SPAWN_WEAPON_SHIFT)&31u)==8u);assert(!recomp_dev_take_spawn(&request));
 recomp_dev_spawn_result(1,"test");switch_view(0);SendMessageA(crew_mode,CB_SETCURSEL,2,0);SendMessageA(crew_faction,CB_SETCURSEL,4,0);request_spawn();
 assert(recomp_dev_take_spawn(&request) && ((request>>DEV_SPAWN_CREW_SHIFT)&3)==2 && ((request>>DEV_SPAWN_FACTION_SHIFT)&7)==4);
 SendDlgItemMessageA(panel,ID_PASSIVE,BM_SETCHECK,BST_CHECKED,0);SendMessageA(panel,WM_COMMAND,ID_PASSIVE,0);assert(recomp_dev_battle_flags()==2);
 SendDlgItemMessageA(panel,ID_UNTARGETABLE,BM_SETCHECK,BST_CHECKED,0);SendMessageA(panel,WM_COMMAND,ID_UNTARGETABLE,0);assert(recomp_dev_battle_flags()==3);
 SendDlgItemMessageA(panel,ID_PASSIVE,BM_SETCHECK,BST_UNCHECKED,0);SendMessageA(panel,WM_COMMAND,ID_PASSIVE,0);assert(recomp_dev_battle_flags()==1);
 SendDlgItemMessageA(panel,ID_UNTARGETABLE,BM_SETCHECK,BST_UNCHECKED,0);SendMessageA(panel,WM_COMMAND,ID_UNTARGETABLE,0);assert(recomp_dev_battle_flags()==0);
 recomp_dev_spawn_result(1,"test");
 char caption[64];GetDlgItemTextA(panel,ID_BOIDS,caption,sizeof(caption));assert(!strcmp(caption,"Boids Simulation"));
 assert((GetWindowLongPtrA(GetDlgItem(panel,ID_BOIDS),GWL_STYLE)&0xFu)==BS_OWNERDRAW);
 assert(!recomp_dev_take_boids());SendMessageA(panel,WM_COMMAND,ID_BOIDS,0);assert(busy);
 SendMessageA(panel,WM_COMMAND,ID_BOIDS,0);assert(recomp_dev_take_boids());assert(!recomp_dev_take_boids());
 recomp_dev_spawn_result(1,"flock created");assert(!busy);
 SendMessageA(panel,WM_COMMAND,ID_BOIDS,0);assert(recomp_dev_take_boids());recomp_dev_spawn_result(0,"test failure");assert(!busy);
 preview_foreground=panel;preview_focus=list;freecam=0;assert(!recomp_dev_menu_camera_input(1));
 freecam=1;assert(recomp_dev_menu_camera_input(1) && recomp_dev_menu_camera_input(0));
 preview_focus=search;assert(!recomp_dev_menu_camera_input(1) && recomp_dev_menu_camera_input(0));
 preview_focus=faction;assert(!recomp_dev_menu_camera_input(1));
 preview_focus=spawn;assert(recomp_dev_menu_camera_input(1));
 preview_foreground=NULL;assert(!recomp_dev_menu_camera_input(0) && !recomp_dev_menu_camera_input(1));
 recomp_dev_spawn_result(1,"test");switch_view(3);
 assert(IsWindowVisible(relation_first) && !IsWindowVisible(list) && !IsWindowVisible(weapon));
 request_relation();assert(busy);unsigned relation=recomp_dev_take_relation();
 assert(relation==dev_relation_command(0,5,3) && !recomp_dev_take_relation());
 recomp_dev_relation_result(1,"test");assert(!busy && IsWindowEnabled(relation_apply));
 SendMessageA(relation_second,CB_SETCURSEL,0,0);request_relation();assert(!busy && !recomp_dev_take_relation());
 assert(!recomp_dev_request_relation(7,0,0) && !recomp_dev_request_relation(0,1,4));
 assert(recomp_dev_request_relation(0,1,0));assert(!recomp_dev_request_relation(2,3,1));
 assert(recomp_dev_take_relation()==dev_relation_command(0,1,0));
 SendMessageA(relation_second,CB_SETCURSEL,5,0);recomp_dev_relation_result(1,"Select two factions and a relationship, then Apply.");
 char relation_capture[MAX_PATH];snprintf(relation_capture,sizeof(relation_capture),"%s.relations.bmp",argv[2]);capture(relation_capture);
 DestroyWindow(panel);DestroyWindow(window);puts("PASS: real panel categories, troop/crew request encoding, atomic consume, and checkbox on/off");return 0;
}
