#ifndef NOMINMAX
#define NOMINMAX
#endif
#define UNICODE
#define _UNICODE
#include "setup_art.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <memory>
using namespace Gdiplus;

namespace {
ULONG_PTR token;
std::unique_ptr<Bitmap> art[8];
std::unique_ptr<PrivateFontCollection> fonts;
const float design_w = SETUP_DESIGN_WIDTH, design_h = SETUP_DESIGN_HEIGHT;

std::unique_ptr<Bitmap> load_png(HINSTANCE instance, int id) {
    HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return {};
    DWORD size = SizeofResource(instance, resource);
    HGLOBAL bytes = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!bytes) return {};
    void *destination = GlobalLock(bytes);
    if (!destination) { GlobalFree(bytes); return {}; }
    memcpy(destination, LockResource(LoadResource(instance, resource)), size);
    GlobalUnlock(bytes);
    IStream *stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(bytes, TRUE, &stream))) {
        GlobalFree(bytes); return {};
    }
    std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream));
    std::unique_ptr<Bitmap> result;
    // Clone before releasing the stream: GDI+ can lazily read its backing data.
    if (decoded && decoded->GetLastStatus() == Ok)
        result.reset(decoded->Clone(0, 0, decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppPARGB));
    decoded.reset();
    stream->Release();
    return result;
}
void image(Graphics &g, int n, float x, float y, float w, float h) {
    g.DrawImage(art[n].get(), RectF(x,y,w,h), 0.f,0.f,
                (float)art[n]->GetWidth(), (float)art[n]->GetHeight(), UnitPixel);
}
void text(Graphics &g, const wchar_t *value, RectF box, bool center = false, float condensed = 1.f) {
    // Vector outlines stay crisp at every DPI and avoid ClearType color fringes.
    FontFamily family(L"Agency FB", fonts.get()), fallback(L"Courier New");
    FontFamily *face = family.GetLastStatus() == Ok ? &family : &fallback;
    GraphicsPath path;
    path.AddString(value, -1, face, FontStyleRegular, 100.f,
                   PointF(0,0), StringFormat::GenericTypographic());
    RectF bounds;
    path.GetBounds(&bounds);
    if (bounds.Width <= 0 || bounds.Height <= 0) return;
    GraphicsPath cap;
    cap.AddString(L"M",1,face,FontStyleRegular,100.f,PointF(0,0),StringFormat::GenericTypographic());
    RectF cap_bounds;cap.GetBounds(&cap_bounds);
    float scale = std::min(box.Height / cap_bounds.Height, box.Width / (bounds.Width*condensed));
    float x = box.X + (center ? (box.Width-bounds.Width*scale*condensed)*.5f : 0.f);
    float y = box.Y + (box.Height-bounds.Height*scale)*.5f;
    Matrix transform(scale*condensed,0,0,scale,x-bounds.X*scale*condensed,y-bounds.Y*scale);
    path.Transform(&transform);
    SolidBrush white(Color(255,255,255,255));
    g.FillPath(&white,&path);
}
void scene(Graphics &g, int w, int h, const setup_view *view) {
    g.Clear(Color(255,0,0,0));
    float scale = std::min(w/design_w,h/design_h);
    float x=(w-design_w*scale)*.5f, y=(h-design_h*scale)*.5f;
    g.TranslateTransform(x,y);g.ScaleTransform(scale,scale);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // The outer mockup margin represents the desktop. The actual window is
    // the supplied frame, with artwork uniformly fitted inside its body.
    image(g,6,0,118,2962,1678);
    const RectF inner(10,128,2942,1656);
    float cover=std::max(inner.Width/art[0]->GetWidth(),inner.Height/art[0]->GetHeight());
    float bw=art[0]->GetWidth()*cover,bh=art[0]->GetHeight()*cover;
    GraphicsState clip=g.Save();g.SetClip(inner);
    image(g,0,inner.X+(inner.Width-bw)*.5f,inner.Y+(inner.Height-bh)*.5f,bw,bh);
    g.Restore(clip);
    image(g,5,0,0,2962,118);
    image(g,4,2546,1007,347,694);
    image(g,1,2587,1042,264,329);
    image(g,3,2569,1414,301,264);
    text(g,L"MERCENARIES RECOMPILED",RectF(36,24,2600,70));
    // Draw this panel as geometry to preserve crisp borders at every DPI.
    const PointF corners[]={PointF(84,1372),PointF(1458,1372),PointF(1542,1416),
                            PointF(1542,1690),PointF(84,1690)};
    SolidBrush track(Color(192,46,60,69));
    Pen white(Color(255,255,255,255),5);
    g.FillPolygon(&track,corners,5);g.DrawPolygon(&white,corners,5);
    g.DrawLine(&white,84.f,1460.f,1542.f,1460.f);
    text(g,view->headline,RectF(102,1391,1380,47));
    SolidBrush fill(Color(255,255,255,255));
    const RectF progress(462,1486,876,175);
    g.FillRectangle(&track,progress);
    const unsigned percent_value=std::min(view->progress,100u);
    if(percent_value)
        g.FillRectangle(&fill,RectF(progress.X,progress.Y,progress.Width*percent_value/100.f,progress.Height));
    g.DrawRectangle(&white,progress);
    wchar_t percent[16];
    if(view->busy || view->completed) swprintf_s(percent,L"%u%%",percent_value);
    else wcscpy_s(percent,L"---");
    text(g,percent,RectF(1365,1548,150,47),true);

}
}

BOOL setup_art_init(HINSTANCE instance) {
    GdiplusStartupInput input;
    if(GdiplusStartup(&token,&input,nullptr)!=Ok) return FALSE;
    for(int i=0;i<8;++i) {
        art[i]=load_png(instance,201+i);
        if(!art[i]) {setup_art_shutdown();return FALSE;}
    }
    HRSRC font_resource=FindResourceW(instance,MAKEINTRESOURCEW(209),RT_RCDATA);
    fonts.reset(new PrivateFontCollection);
    if(!font_resource || fonts->AddMemoryFont(
            LockResource(LoadResource(instance,font_resource)),
            SizeofResource(instance,font_resource))!=Ok) {
        setup_art_shutdown();return FALSE;
    }
    return TRUE;
}
void setup_art_shutdown(void) {
    for(auto &a:art)a.reset();
    fonts.reset();
    if(token) {GdiplusShutdown(token);token=0;}
}
RECT setup_art_rect(int w,int h,BOOL close_button) {
    float s=std::min(w/design_w,h/design_h);
    float x=(w-design_w*s)*.5f,y=(h-design_h*s)*.5f;
    RectF r=close_button?RectF(2808,18,84,84):RectF(122,1510,314,128);
    return RECT{(LONG)(x+r.X*s+.5f),(LONG)(y+r.Y*s+.5f),
                (LONG)(x+(r.X+r.Width)*s+.5f),(LONG)(y+(r.Y+r.Height)*s+.5f)};
}
void setup_art_paint(HDC dc,int w,int h,const setup_view *view) {
    Bitmap buffer(w,h,PixelFormat32bppPARGB);
    {Graphics g(&buffer);scene(g,w,h,view);}
    Graphics out(dc);out.DrawImage(&buffer,0,0);
}
void setup_art_button(HDC dc,int w,int h,const setup_view *view,BOOL close_button,UINT state) {
    Graphics g(dc);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    if(close_button) {
        image(g,view->busy?7:2,0,0,(float)w,(float)h);
    } else {
        SolidBrush background(Color(255,46,60,69));
        if(state & ODS_SELECTED) background.SetColor(Color(255,66,85,96));
        g.FillRectangle(&background,0,0,w,h);
        Pen border(Color(255,255,255,255),std::max(1.f,w*5.f/314));
        g.DrawRectangle(&border,RectF(1,1,(float)w-2,(float)h-2));
        text(g,view->button,RectF(w*.06f,h*.22f,w*.88f,h*.55f),true);
    }
    if((state&ODS_FOCUS) && !(state&ODS_NOFOCUSRECT)) {
        Pen focus(Color(255,255,230,140),1);focus.SetDashStyle(DashStyleDot);
        g.DrawRectangle(&focus,RectF(4,4,(float)w-9,(float)h-9));
    }
}
BOOL setup_art_export(const wchar_t *path,int w,int h,const setup_view *view) {
    Bitmap buffer(w,h,PixelFormat32bppPARGB);
    {Graphics g(&buffer);scene(g,w,h,view);}
    {Graphics g(&buffer);HDC dc=g.GetHDC();
     for(int i=0;i<2;++i) {
         RECT r=setup_art_rect(w,h,i);
         int saved=SaveDC(dc);SetViewportOrgEx(dc,r.left,r.top,nullptr);
         setup_art_button(dc,r.right-r.left,r.bottom-r.top,view,i,0);RestoreDC(dc,saved);
     }
     g.ReleaseHDC(dc);}
    const CLSID png={0x557cf406,0x1a04,0x11d3,{0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
    return buffer.Save(path,&png,nullptr)==Ok;
}
