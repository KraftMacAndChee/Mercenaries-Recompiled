/* Opt-in telemetry: existing completed-frame history, no GPU readbacks. */
#include "dev_overlay.h"
#include "dev_menu.h"
#include "free_cam.h"
#include "d3d8_internal.h"
#include "d3d8_frame_timing.h"
#include <stdio.h>
#include <string.h>
static int enabled;
static HDC dc;
static HBITMAP bitmap;
static HFONT font;
static void *pixels;
static ULONGLONG updated;
#define W 360
#define H 128
void recomp_dev_overlay_set(int value){enabled=value && recomp_dev_menu_allowed();updated=0;}
static void paint(void){
    if(!enabled)return;
    if(!dc){
        BITMAPINFO info={0};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=W;info.bmiHeader.biHeight=-H;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(NULL);bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
        if(!bitmap || !pixels){enabled=0;return;}SelectObject(dc,bitmap);
        font=CreateFontA(-15,0,0,0,FW_MEDIUM,0,0,0,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,"Consolas");
        SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
    }
    ULONGLONG now=GetTickCount64();
    if(!updated || now-updated>=100){
        uint32_t times[160];uint64_t serial;unsigned count=d3d8_GetFrameIntervalHistory(0,times,160,&serial);
        memset(pixels,0,W*H*4);
        double sum=0;unsigned recent=count<60?count:60;
        for(unsigned i=count-recent;i<count;++i)sum+=times[i];
        char text[128];snprintf(text,sizeof(text),"FPS %.1f   Frametime %.2f ms",sum?recent*1000000./sum:0,count?times[count-1]/1000.:0);
        SetTextColor(dc,RGB(230,244,246));TextOutA(dc,10,8,text,(int)strlen(text));
        snprintf(text,sizeof(text),"World %s  |  16.7 / 33.3 ms guides",recomp_freecam_time_scale()==0?"frozen":recomp_freecam_time_scale()<1?"20%":"100%");
        SetTextColor(dc,RGB(164,187,195));TextOutA(dc,10,28,text,(int)strlen(text));
        HPEN grid=CreatePen(PS_SOLID,1,RGB(65,79,86)),line=CreatePen(PS_SOLID,1,RGB(99,211,219));
        HGDIOBJ old=SelectObject(dc,grid);
        for(int y=80;y<=100;y+=20){MoveToEx(dc,10,y,NULL);LineTo(dc,350,y);}
        SelectObject(dc,line);
        for(unsigned i=0;i<count;++i){int y=120-(int)(times[i]*.0012);if(y<48)y=48;
            int x=10+(int)i*340/159;if(i==0)MoveToEx(dc,x,y,NULL);else LineTo(dc,x,y);}
        SelectObject(dc,old);DeleteObject(grid);DeleteObject(line);GdiFlush();
        /* GDI writes RGB only. Convert its black-backed antialiasing to
         * premultiplied alpha, with a one-pixel dark outline for readability.
         * Empty graph/text space remains fully transparent. */
        uint32_t *rgba=(uint32_t*)pixels;
        for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){
            unsigned i=y*W+x,c=rgba[i]&0xffffffu;
            unsigned a=c&255u;if(((c>>8)&255u)>a)a=(c>>8)&255u;if((c>>16)>a)a=c>>16;
            if(!a){
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                    int nx=(int)x+dx,ny=(int)y+dy;
                    if(nx>=0 && nx<W && ny>=0 && ny<H && (rgba[ny*W+nx]&0xffffffu))a=192;
                }
            }
            rgba[i]=c|(a<<24);
        }
        updated=now;
    }
    d3d8_CompositeHostOverlay(pixels,W,H,12,12,960,540);
}
void recomp_dev_overlay_init(void){d3d8_SetHostOverlayCallback(paint);}
