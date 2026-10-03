"""Exercise the selector's real window messages, scrolling, order and settings."""
from pathlib import Path
import subprocess,shutil
from test_mod_loader_native import ROOT,CASE,msvc
SOURCE=r"""
#include <cassert>
#include "mod_selector.cpp"
namespace mercmods {
void save_selection(const fs::path&,const Selection&){}
Launch prepare(const fs::path&,const fs::path&,const Selection&,bool){return {};}
void start(const Launch&){}
}
int main(){
 using namespace mercmods;State s;assert(s.art.init());
 for(int i=0;i<10000;++i)s.selection.mods.push_back({L"Mod "+std::to_wstring(i),{},false});
 WNDCLASSW wc{};wc.lpfnWndProc=proc;wc.hInstance=GetModuleHandleW(0);wc.lpszClassName=L"ModSelectorTest";assert(RegisterClassW(&wc));
 HWND w=CreateWindowW(wc.lpszClassName,L"test",WS_POPUP|WS_THICKFRAME,0,0,W,H,0,0,wc.hInstance,&s);assert(w);

 // Focus loss must not paint a standard frame over the client artwork.
 ShowWindow(w,SW_SHOWNOACTIVATE);UpdateWindow(w);
 auto border=[&](){
   HDC dc=GetWindowDC(w),copy=CreateCompatibleDC(dc);assert(dc&&copy);
   BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
   info.bmiHeader.biWidth=W;info.bmiHeader.biHeight=-H;
   info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
   DWORD *data=nullptr;HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,(void**)&data,0,0);assert(bitmap&&data);
   auto previous=SelectObject(copy,bitmap);assert(BitBlt(copy,0,0,W,H,dc,0,0,SRCCOPY));GdiFlush();
   std::vector<DWORD> pixels;
   for(int y=0;y<H;++y)for(int x=0;x<W;++x)
     if((x<6||x>=W-6||y<6||y>=H-6)&&!(x>W-40&&y<40))pixels.push_back(data[y*W+x]&0xFFFFFF);
   SelectObject(copy,previous);DeleteObject(bitmap);DeleteDC(copy);ReleaseDC(w,dc);return pixels;
 };
 auto original_border=border();
 for(BOOL active:{FALSE,TRUE,FALSE,TRUE}){
   assert(SendMessageW(w,WM_NCACTIVATE,active,0));
   SendMessageW(w,WM_NCPAINT,1,0);
   assert(border()==original_border);
 }
 ShowWindow(w,SW_HIDE);
 RECT client{},bounds{};GetClientRect(w,&client);GetWindowRect(w,&bounds);
 assert(client.right==bounds.right-bounds.left&&client.bottom==bounds.bottom-bounds.top);
 HRGN outline=CreateRectRgn(0,0,0,0);assert(GetWindowRgn(w,outline)!=ERROR);
 assert(!PtInRegion(outline,W-2,2));assert(PtInRegion(outline,W-2,50));DeleteObject(outline);
 POINT title{200,25};ClientToScreen(w,&title);assert(SendMessageW(w,WM_NCHITTEST,0,MAKELPARAM(title.x,title.y))==HTCAPTION);
 POINT edge{1,H/2};ClientToScreen(w,&edge);assert(SendMessageW(w,WM_NCHITTEST,0,MAKELPARAM(edge.x,edge.y))==HTLEFT);
 RECT sizing{0,0,700,H};SendMessageW(w,WM_SIZING,WMSZ_RIGHT,(LPARAM)&sizing);assert(sizing.bottom==MulDiv(700,H,W));
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(672,205));assert(s.selection.mods[0].enabled);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(672,243));assert(!s.selection.mods[0].enabled&&!s.selection.mods[1].enabled);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(672,243));assert(s.selection.mods[0].enabled);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(420,205));assert(s.selected==1&&s.selection.mods[1].name==L"Mod 0"&&s.selection.mods[1].enabled);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(543,283));assert(s.selected==0&&s.selection.mods[0].name==L"Mod 0");

 auto settle=[&](){
   for(int tick=0;tick<100&&s.scrolling;++tick){s.scroll_tick=GetTickCount64()-16;SendMessageW(w,WM_TIMER,scroll_timer,0);}
   assert(!s.scrolling&&s.scroll==s.scroll_target);
 };
 SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
 assert(s.scroll==0&&s.scroll_target==117&&s.scrolling);
 s.scroll_tick=GetTickCount64()-16;SendMessageW(w,WM_TIMER,scroll_timer,0);
 assert(s.scroll>0&&s.scroll<117);settle();
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(887,639));assert(s.scroll_target==s.scroll_limit());settle();
 // Grabbing the thumb preserves the exact fractional offset; dragging tracks the pointer.
 scroll_to(s,0,false);SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(887,100));assert(s.scroll==0&&s.drag);
 SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,100));assert(s.scroll==0);
 SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,357));assert(std::abs(s.scroll-s.scroll_limit()*.5)<.001);
 SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,660));assert(s.scroll==s.scroll_limit());
 SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,50));assert(s.scroll==0);
 SendMessageW(w,WM_LBUTTONUP,0,0);assert(!s.drag&&GetCapture()!=w);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(887,639));settle();assert(s.scroll==s.scroll_limit());
 // Wheel distance remains comfortable for a nearly full list and very large lists.
 for(int count:{0,4,6,7,10,24,100,10000}){
   s.selection.mods.resize(count);scroll_to(s,0,false);
   SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-30),0);
   assert(s.scroll_target==std::min(29.25,s.scroll_limit()));
   if(count>6){
     assert(s.scroll==0&&s.scrolling);
     s.scroll_tick=GetTickCount64()-16;SendMessageW(w,WM_TIMER,scroll_timer,0);
     double prior=s.scroll;assert(prior>0&&prior<s.scroll_target);
     SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,15),0);
     assert(s.scroll_target<prior);settle();assert(s.scroll<prior);
     scroll_to(s,0,false);
     for(int i=0;i<4;++i)SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-30),0);
     assert(s.scroll_target==std::min(117.0,s.scroll_limit()));settle();
     double prior_scroll=s.scroll;RectF grip=scroll_grip(s);
     int grab=(int)(grip.Y+5);SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(887,grab));
     assert(s.drag&&s.scroll==prior_scroll);
     SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,grab));assert(std::abs(s.scroll-prior_scroll)<.03);
     SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,900));assert(s.scroll==s.scroll_limit());
     SendMessageW(w,WM_MOUSEMOVE,0,MAKELPARAM(887,0));assert(s.scroll==0);
     SendMessageW(w,WM_LBUTTONUP,0,0);
   }else assert(!s.scrolling&&s.scroll==0);
 }

 // Exercise actual queued timer messages, not just deterministic animation steps.
 scroll_to(s,0,false);SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);
 ULONGLONG started=GetTickCount64();unsigned frames=0;double last_offset=0;
 while(s.scrolling&&GetTickCount64()-started<2000){
   MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
   if(s.scroll!=last_offset){assert(s.scroll>last_offset&&s.scroll<=117);last_offset=s.scroll;++frames;}
   Sleep(1);
 }
 assert(!s.scrolling&&s.scroll==117&&frames>=3);
 // Clicks use the painted fractional offset, and stop pending wheel animation.
 scroll_to(s,17.25,false);s.selection.mods[0].enabled=false;s.selection.mods[1].enabled=false;
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(672,240));
 assert(!s.selection.mods[0].enabled&&s.selection.mods[1].enabled);
 SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);assert(s.scrolling);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(672,240));assert(!s.scrolling&&!s.selection.mods[1].enabled&&s.scroll==17.25);
 // Animation is monotonic and time-based rather than dependent on timer frequency.
 State slow,fast;slow.selection.mods.resize(100);fast.selection.mods.resize(100);
 slow.scroll_target=fast.scroll_target=117;
 for(int i=0;i<10;++i)slow.advance_scroll(.016);
 for(int i=0;i<20;++i)fast.advance_scroll(.008);
 assert(std::abs(slow.scroll-fast.scroll)<.00001&&slow.scroll>0&&slow.scroll<117);
 double previous=slow.scroll;
 while(slow.advance_scroll(.016)){assert(slow.scroll>=previous&&slow.scroll<=117);previous=slow.scroll;}
 assert(slow.scroll==117);
 s.selected=9999;SendMessageW(w,WM_KEYDOWN,VK_DOWN,0);assert(s.selected==9999);
 SendMessageW(w,WM_KEYDOWN,VK_SPACE,0);assert(s.selection.mods[9999].enabled);
 SendMessageW(w,WM_LBUTTONDOWN,0,MAKELPARAM(310,810));assert(!s.selection.separate_saves);
 s.busy=true;SendMessageW(w,WM_CLOSE,0,0);assert(IsWindow(w));s.busy=false;
 s.selection.mods.resize(2);s.clamp();assert(s.scroll==0);s.selected=0;
 SendMessageW(w,WM_MOUSEWHEEL,MAKEWPARAM(0,-WHEEL_DELTA),0);assert(s.scroll==0);
 s.selection.mods[0].name=L"A very long mod name which must fit inside its row without overlapping load order controls";
 Bitmap image(620,640,PixelFormat32bppPARGB);{Graphics g(&image);scene(g,620,640,s);}
 const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};assert(image.Save(L"selector-small.png",&png,0)==Ok);
 State mock;assert(mock.art.init());
 mock.selection.mods={{L"Example mod C",{},true},{L"Example mod A",{},false},{L"Example mod D",{},false},{L"Example mod B",{},false}};mock.focus=3;
 Bitmap full(W,H,PixelFormat32bppPARGB);{Graphics g(&full);scene(g,W,H,mock);}assert(full.Save(L"selector-mockup.png",&png,0)==Ok);

 // The uppercase U aligns with the top of the separate-saves checkbox.
 int text_top=H;
 for(int y=770;y<835;++y)for(int x=350;x<370;++x){
   Color c;full.GetPixel(x,y,&c);
   if(c.GetR()>240&&c.GetG()>240&&c.GetB()>240)text_top=std::min(text_top,y);
 }
 assert(abs(text_top-(int)saves_control.Y)<=1);
 mock.focus=0;
 Bitmap selected(W,H,PixelFormat32bppPARGB);{Graphics g(&selected);scene(g,W,H,mock);}
 for(int y=172;y<250;++y){Color a,b;full.GetPixel(710,y,&a);selected.GetPixel(710,y,&b);assert(a.GetValue()!=b.GetValue());}
 Color a,b;full.GetPixel(710,251,&a);selected.GetPixel(710,251,&b);assert(a.GetValue()==b.GetValue());
 assert(selected.Save(L"selector-selected.png",&png,0)==Ok);
 mock.focus=3;
 // Preparing must change only the selected launch button's label.
 for(int active=0;active<2;++active){
   mock.busy=true;mock.preparing_vanilla=(active==0);
   Bitmap preparing(W,H,PixelFormat32bppPARGB);{Graphics g(&preparing);scene(g,W,H,mock);}
   unsigned changed[2]={};
   for(int button=0;button<2;++button)for(int y=675;y<755;++y)for(int x=(int)launch_buttons[button].X+5;x<(int)launch_buttons[button].GetRight()-5;++x){
     Color a,b;full.GetPixel(x,y,&a);preparing.GetPixel(x,y,&b);changed[button]+=a.GetValue()!=b.GetValue();
   }
   assert(changed[active]>0&&changed[1-active]==0);
   assert(preparing.Save(active?L"selector-preparing-modded.png":L"selector-preparing-vanilla.png",&png,0)==Ok);
 }
 mock.busy=false;
 assert(mercenaries_mod_preview(L"selector-full.png"));
 for(int i=4;i<24;++i)mock.selection.mods.push_back({L"Example mod "+std::to_wstring(i+1),{},false});
 const wchar_t *views[]={L"selector-scroll-top.png",L"selector-scroll-middle.png",L"selector-scroll-bottom.png"};
 for(int i=0;i<3;++i){
   mock.scroll=mock.scroll_target=i*9*row_height;RectF grip=scroll_grip(mock);
   assert(grip.Y>=scroll_track.Y+scroll_inset&&grip.GetBottom()<=scroll_track.GetBottom()-scroll_inset);
   Bitmap preview(W,H,PixelFormat32bppPARGB);{Graphics g(&preview);scene(g,W,H,mock);}
   Color rail,body;preview.GetPixel(874,639,&rail);assert(rail.GetR()==255&&rail.GetG()==255&&rail.GetB()==255);
   preview.GetPixel(887,(int)grip.Y+5,&body);assert(body.GetR()==73&&body.GetG()==87&&body.GetB()==98);
   assert(preview.Save(views[i],&png,0)==Ok);
 }


 // Fractional rows cannot paint over the header or launch controls.
 mock.scroll=mock.scroll_target=0;
 Bitmap unscrolled(W,H,PixelFormat32bppPARGB);{Graphics g(&unscrolled);scene(g,W,H,mock);}
 mock.scroll=mock.scroll_target=37.5;
 Bitmap fractional(W,H,PixelFormat32bppPARGB);{Graphics g(&fractional);scene(g,W,H,mock);}
 for(int y=88;y<172;++y)for(int x=42;x<859;++x){Color a,b;unscrolled.GetPixel(x,y,&a);fractional.GetPixel(x,y,&b);assert(a.GetValue()==b.GetValue());}
 for(int y=640;y<770;++y)for(int x=42;x<859;++x){Color a,b;unscrolled.GetPixel(x,y,&a);fractional.GetPixel(x,y,&b);assert(a.GetValue()==b.GetValue());}
 assert(fractional.Save(L"selector-fractional-scroll.png",&png,0)==Ok);
 s.selection.mods.clear();Bitmap empty(940,900,PixelFormat32bppPARGB);{Graphics g(&empty);scene(g,940,900,s);}assert(empty.Save(L"selector-empty.png",&png,0)==Ok);
 SendMessageW(w,WM_CLOSE,0,0);assert(!IsWindow(w));puts("PASS: 10,000 mods, enable/disable, reorder, wheel/drag, boundary navigation, saves toggle, busy close guard, small/empty layout");
}
"""
def main():
 CASE.mkdir(parents=True,exist_ok=True);cpp=CASE/'selector.cpp';cpp.write_text(SOURCE);env=msvc();exe=CASE/'selector.exe'
 res=ROOT/'build/mercenaries-perf20260930/mercenaries_launcher.dir/Release/launcher.res'
 subprocess.run([shutil.which('cl',path=env['PATH']),'/nologo','/std:c++17','/EHsc','/O2','/I'+str(ROOT/'ports/mercenaries/src'),str(cpp),str(res),'/Fo'+str(CASE/'selector.obj'),'/Fe'+str(exe),'/link','gdiplus.lib','ole32.lib','user32.lib','gdi32.lib','/STACK:8388608'],env=env,check=True)
 subprocess.run([str(exe)],cwd=CASE,check=True)
if __name__=='__main__':main()
