#define UNICODE
#define _UNICODE
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "mod_loader.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <objidl.h>
#include <gdiplus.h>
#include <thread>
#include <windowsx.h>
using namespace Gdiplus;
namespace mercmods
{
namespace
{
constexpr int W = 940, H = 887, finished = WM_APP + 41;
constexpr float top = 164, rows_top = 172, row_height = 78, bottom = 640;
const RectF list_box(42, 88, 816, bottom - 88);
const RectF launch_buttons[] = {RectF(120, 666, 300, 99), RectF(520, 666, 300, 99)};
const RectF saves_control(284, 782, 390, 50);
const RectF scroll_track(873, list_box.Y, 32, list_box.Height);
constexpr float scroll_inset = 3, min_scroll_grip_height = 32;
constexpr UINT_PTR scroll_timer = 1;
constexpr double wheel_step = row_height * 1.5, scroll_response_seconds = .055;
struct Art
{
    ULONG_PTR token = 0;
    std::unique_ptr<Bitmap> background, up, down, button;
    std::unique_ptr<PrivateFontCollection> fonts;
    std::unique_ptr<Bitmap> load(int id)
    {
        HMODULE module = GetModuleHandleW(nullptr);
        auto r = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!r)
            return {};
        auto n = SizeofResource(module, r);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, n);
        if (!memory)
            return {};
        auto p = GlobalLock(memory);
        if (!p)
        {
            GlobalFree(memory);
            return {};
        }
        memcpy(p, LockResource(LoadResource(module, r)), n);
        GlobalUnlock(memory);
        IStream *stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)))
        {
            GlobalFree(memory);
            return {};
        }
        std::unique_ptr<Bitmap> original(Bitmap::FromStream(stream));
        std::unique_ptr<Bitmap> copy;
        if (original && original->GetLastStatus() == Ok)
            copy.reset(original->Clone(0, 0, original->GetWidth(), original->GetHeight(),
                                       PixelFormat32bppPARGB));
        original.reset();
        stream->Release();
        return copy;
    }
    bool init()
    {
        GdiplusStartupInput input;
        if (GdiplusStartup(&token, &input, nullptr) != Ok)
            return false;
        background = load(301);
        up = load(302);
        down = load(303);
        button = load(304);
        fonts.reset(new PrivateFontCollection);
        auto m = GetModuleHandleW(nullptr);
        auto r = FindResourceW(m, MAKEINTRESOURCEW(209), RT_RCDATA);
        return background && up && down && button && r &&
               fonts->AddMemoryFont(LockResource(LoadResource(m, r)), SizeofResource(m, r)) == Ok;
    }
    ~Art()
    {
        background.reset();
        up.reset();
        down.reset();
        button.reset();
        fonts.reset();
        if (token)
            GdiplusShutdown(token);
    }
};
struct State
{
    HWND window = nullptr;
    fs::path install, game;
    Selection selection;
    Art art;
    size_t selected = 0;
    double scroll = 0, scroll_target = 0;
    ULONGLONG scroll_tick = 0;
    bool scrolling = false;
    bool busy = false, launched = false, drag = false;
    bool preparing_vanilla = false;
    float drag_offset = 0;
    int focus = 0;
    std::wstring status;
    std::thread worker;
    std::string error;
    double scroll_limit() const
    {
        return std::max(0.0, selection.mods.size() * (double)row_height - (bottom - rows_top));
    }
    void clamp()
    {
        const double limit = scroll_limit();
        scroll = std::clamp(scroll, 0.0, limit);
        scroll_target = std::clamp(scroll_target, 0.0, limit);
    }
    bool advance_scroll(double seconds)
    {
        clamp();
        scroll += (scroll_target - scroll) * (1 - std::exp(-seconds / scroll_response_seconds));
        if (std::abs(scroll_target - scroll) < .1)
            scroll = scroll_target;
        return scroll != scroll_target;
    }
};
void scroll_to(State &s, double position, bool smooth)
{
    s.scroll_target = std::clamp(position, 0.0, s.scroll_limit());
    if (smooth && s.scroll != s.scroll_target)
    {
        if (!s.scrolling)
        {
            s.scroll_tick = GetTickCount64();
            s.scrolling = SetTimer(s.window, scroll_timer, 16, nullptr) != 0;
        }
        if (!s.scrolling)
            s.scroll = s.scroll_target;
    }
    else
    {
        s.scroll = s.scroll_target;
        s.scrolling = false;
        KillTimer(s.window, scroll_timer);
    }
    InvalidateRect(s.window, nullptr, FALSE);
}
void reveal_selected(State &s)
{
    const double first = s.selected * (double)row_height;
    const double last = first + row_height - (bottom - rows_top);
    scroll_to(s, std::clamp(s.scroll_target, std::max(0.0, last), first), true);
}
RectF scroll_grip(const State &s)
{
    const double content = std::max((double)(bottom - rows_top), s.selection.mods.size() * (double)row_height);
    const float track = scroll_track.Height - 2 * scroll_inset;
    const float height = std::max(min_scroll_grip_height, (float)(track * (bottom - rows_top) / content));
    const double limit = s.scroll_limit();
    const float position = limit ? (float)(std::clamp(s.scroll, 0.0, limit) / limit) : 0.f;
    return RectF(scroll_track.X + scroll_inset, scroll_track.Y + scroll_inset + (track - height) * position,
                 scroll_track.Width - 2 * scroll_inset, height);
}
void label(Graphics &g, Art &art, const std::wstring &text, RectF box,
           bool center = false, bool ellipsis = false, bool align_cap_top = false)
{
    if (text.empty())
        return;
    FontFamily family(L"Agency FB", art.fonts.get());
    GraphicsPath path, cap;
    cap.AddString(L"M", 1, &family, FontStyleRegular, 100.f,
                  PointF(0, 0), StringFormat::GenericTypographic());
    RectF bounds, cap_bounds;
    cap.GetBounds(&cap_bounds);
    std::wstring display = text;
    size_t length = text.size();
    for (;;)
    {
        path.Reset();
        path.AddString(display.c_str(), -1, &family, FontStyleRegular, 100.f,
                       PointF(0, 0), StringFormat::GenericTypographic());
        path.GetBounds(&bounds);
        if (!ellipsis || !length || bounds.Width * box.Height <= box.Width * cap_bounds.Height)
            break;
        --length;
        if (length && text[length] >= 0xDC00 && text[length] <= 0xDFFF)
            --length;
        display = text.substr(0, length) + L"\x2026";
    }
    if (bounds.Width <= 0 || bounds.Height <= 0 || cap_bounds.Height <= 0)
        return;
    // Match the main launcher's cap-height sizing and vector text rendering.
    float scale = std::min(box.Height / cap_bounds.Height, box.Width / bounds.Width);
    float x = box.X + (center ? (box.Width - bounds.Width * scale) * .5f : 0.f);
    float y = align_cap_top ? box.Y + (bounds.Y - cap_bounds.Y) * scale
                            : box.Y + (box.Height - bounds.Height * scale) * .5f;
    Matrix transform(scale, 0, 0, scale, x - bounds.X * scale, y - bounds.Y * scale);
    path.Transform(&transform);
    SolidBrush ink(Color::White);
    g.FillPath(&ink, &path);
}
void checkbox(Graphics &g, float x, float y, bool on, float size = 48)
{
    Pen p(Color::White, 2);
    g.DrawRectangle(&p, RectF(x, y, size, size));
    if (on)
    {
        g.DrawLine(&p, x, y, x + size, y + size);
        g.DrawLine(&p, x + size, y, x, y + size);
    }
}
void image(Graphics &g, Bitmap *b, RectF r)
{
    g.DrawImage(b, r, 0, 0, (REAL)b->GetWidth(), (REAL)b->GetHeight(), UnitPixel);
}
void scene(Graphics &g, int width, int height, State &s)
{
    g.Clear(Color::Transparent);
    g.ScaleTransform(width / (float)W, height / (float)H);
    const PointF outline[] = {{0, 0}, {W - W * 50.f / 1492, 0},
                              {W, H * 50.f / 1408}, {W, H}, {0, H}};
    GraphicsPath frame;
    frame.AddPolygon(outline, 5);
    g.SetClip(&frame);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    image(g, s.art.background.get(), RectF(0, 0, W, H));
    label(g, s.art, L"MERCENARIES RECOMPILED MOD SELECTOR", RectF(10, 13, 840, 31));
    Pen close(Color(255, 150, 38, 38), 4);
    g.DrawRectangle(&close, RectF(875, 13, 30, 30));
    g.DrawLine(&close, 878, 16, 902, 40);
    g.DrawLine(&close, 902, 16, 878, 40);
    SolidBrush panel(Color(180, 46, 60, 69));
    g.FillRectangle(&panel, RectF(20, 74, 900, 772));
    Pen white(Color::White, 2);
    g.DrawRectangle(&white, RectF(20, 74, 900, 772));
    g.DrawRectangle(&white, list_box);
    g.DrawLine(&white, list_box.X, top, list_box.GetRight(), top);
    label(g, s.art, L"Mod name", RectF(74, 104, 300, 40));
    label(g, s.art, L"Load order", RectF(385, 104, 177, 40));
    PointF order_arrow[] = {{570, 115}, {602, 115}, {586, 143}};
    SolidBrush ink(Color::White);
    g.FillPolygon(&ink, order_arrow, 3);
    label(g, s.art, L"ON", RectF(650, 104, 65, 40));
    auto count = s.selection.mods.size();
    s.clamp();
    const auto row_clip = g.Save();
    g.SetClip(RectF(44, rows_top, 812, bottom - rows_top), CombineModeIntersect);
    const size_t first = (size_t)(s.scroll / row_height);
    for (size_t index = first; index < count; ++index)
    {
        const float y = rows_top + (float)(index * (double)row_height - s.scroll);
        if (y >= bottom)
            break;
        auto &mod = s.selection.mods[index];
        if (index == s.selected && s.focus == 0)
        {
            SolidBrush selected(Color(35, 132, 157, 173));
            g.FillRectangle(&selected, RectF(44, y, 812, row_height));
        }
        label(g, s.art, mod.name, RectF(74, y + 16, 305, 40), false, true);
        image(g, s.art.down.get(), RectF(399, y + 20, 44, 44));
        label(g, s.art, std::to_wstring(index + 1), RectF(449, y + 18, 66, 44), true);
        image(g, s.art.up.get(), RectF(521, y + 20, 44, 44));
        checkbox(g, 640, y + 8, mod.enabled, 64);
    }
    g.Restore(row_clip);
    if (!count)
        label(g, s.art, L"No mods found in the mods folder.", RectF(60, 270, 770, 28), true);
    if (s.scroll_limit() > 0)
    {
        g.FillRectangle(&ink, scroll_track);
        const RectF thumb = scroll_grip(s);
        SolidBrush grip(Color(255, 73, 87, 98));
        g.FillRectangle(&grip, thumb);
        g.DrawLine(&white, thumb.X, thumb.Y + thumb.Height * .5f - min_scroll_grip_height / 6,
                   thumb.GetRight(), thumb.Y + thumb.Height * .5f - min_scroll_grip_height / 6);
        g.DrawLine(&white, thumb.X, thumb.Y + thumb.Height * .5f + min_scroll_grip_height / 6,
                   thumb.GetRight(), thumb.Y + thumb.Height * .5f + min_scroll_grip_height / 6);
    }
    label(g, s.art, s.status, RectF(45, 644, 848, 15), true);
    for (int i = 0; i < 2; ++i)
    {
        image(g, s.art.button.get(), launch_buttons[i]);
        label(g, s.art,
              s.busy && (i == 0) == s.preparing_vanilla ? L"PREPARING..."
                  : i ? L"LAUNCH MODDED" : L"LAUNCH VANILLA",
              RectF(launch_buttons[i].X + 9, launch_buttons[i].Y + 29, 282, 40), true);
        if (s.focus == i + 1)
        {
            Pen p(Color(255, 255, 230, 140), 2);
            g.DrawRectangle(&p, launch_buttons[i]);
        }
    }
    checkbox(g, saves_control.X, saves_control.Y, s.selection.separate_saves);
    label(g, s.art, L"Use separate saves", RectF(350, 782, 340, 40), false, false, true);
}
void launch(State &s, bool vanilla)
{
    if (s.busy)
        return;
    s.busy = true;
    s.preparing_vanilla = vanilla;
    s.status = L"Preparing selected files and checking the mod cache...";
    InvalidateRect(s.window, nullptr, FALSE);
    s.worker = std::thread(
        [&s, vanilla]()
        {
            try
            {
                save_selection(s.install, s.selection);
                auto l = prepare(s.install, s.game, s.selection, vanilla);
                start(l);
            }
            catch (const std::exception &e)
            {
                s.error = e.what();
            }
            PostMessageW(s.window, finished, 0, 0);
        });
}
void move(State &s, size_t index, int direction)
{
    if ((direction < 0 && !index) || (direction > 0 && index + 1 >= s.selection.mods.size()))
        return;
    size_t next = direction < 0 ? index - 1 : index + 1;
    std::swap(s.selection.mods[index], s.selection.mods[next]);
    s.selected = next;
    reveal_selected(s);
}
LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    auto s = (State *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (msg == WM_NCCREATE)
    {
        s = (State *)((CREATESTRUCTW *)lp)->lpCreateParams;
        s->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)s);
    }
    if (!s)
        return DefWindowProcW(window, msg, wp, lp);
    RECT rect;
    GetClientRect(window, &rect);
    float x = rect.right ? GET_X_LPARAM(lp) * (float)W / rect.right : 0,
          y = rect.bottom ? GET_Y_LPARAM(lp) * (float)H / rect.bottom : 0;
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        if (rect.right && rect.bottom)
        {
            Bitmap buffer(rect.right, rect.bottom, PixelFormat32bppPARGB);
            {
                Graphics g(&buffer);
                scene(g, rect.right, rect.bottom, *s);
            }
            Graphics out(dc);
            out.DrawImage(&buffer, 0, 0);
        }
        EndPaint(window, &ps);
        return 0;
    }
    case WM_NCACTIVATE:
        // Preserve activation handling without painting a standard frame over the artwork.
        return DefWindowProcW(window, msg, wp, -1);
    case WM_NCPAINT:
        return 0;
    case WM_NCCALCSIZE:
        // The artwork supplies the frame; the whole window is client-painted.
        return 0;
    case WM_SIZE:
    {
        POINT outline[] = {{0, 0}, {rect.right - MulDiv(rect.right, 50, 1492), 0},
                           {rect.right, MulDiv(rect.bottom, 50, 1408)},
                           {rect.right, rect.bottom}, {0, rect.bottom}};
        HRGN region = CreatePolygonRgn(outline, 5, WINDING);
        if (region && !SetWindowRgn(window, region, TRUE))
            DeleteObject(region);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_SIZING:
    {
        auto bounds = (RECT *)lp;
        int width = bounds->right - bounds->left;
        int height = bounds->bottom - bounds->top;
        if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM)
            bounds->right = bounds->left + MulDiv(height, W, H);
        else if (wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)
            bounds->top = bounds->bottom - MulDiv(width, H, W);
        else
            bounds->bottom = bounds->top + MulDiv(width, H, W);
        return TRUE;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize = {680, MulDiv(680, H, W)};
        return 0;
    case WM_NCHITTEST:
    {
        auto result = DefWindowProcW(window, msg, wp, lp);
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(window, &p);
        if (result != HTCLIENT)
            return result;
        const int edge = 6;
        bool left = p.x < edge, right = p.x >= rect.right - edge;
        bool upper = p.y < edge, lower = p.y >= rect.bottom - edge;
        if (upper)
            return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
        if (lower)
            return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
        if (left || right)
            return left ? HTLEFT : HTRIGHT;
        if (p.y < rect.bottom * 55 / H && p.x < rect.right * 865 / W)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_CLOSE:
        if (!s->busy)
            DestroyWindow(window);
        return 0;
    case WM_KEYDOWN:
        if (s->busy)
            return 0;
        if (wp == VK_ESCAPE)
        {
            DestroyWindow(window);
            return 0;
        }
        if (wp == VK_TAB)
        {
            s->focus = (s->focus + 1) % 4;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_RETURN)
        {
            if (s->focus == 1 || s->focus == 2)
                launch(*s, s->focus == 1);
            else if (s->focus == 3)
                s->selection.separate_saves = !s->selection.separate_saves;
            else if (!s->selection.mods.empty())
                s->selection.mods[s->selected].enabled = !s->selection.mods[s->selected].enabled;
        }
        if (wp == VK_SPACE)
        {
            if (s->focus == 3)
                s->selection.separate_saves = !s->selection.separate_saves;
            else if (s->focus == 0 && !s->selection.mods.empty())
                s->selection.mods[s->selected].enabled = !s->selection.mods[s->selected].enabled;
        }
        if ((wp == VK_UP || wp == VK_DOWN) && !s->selection.mods.empty())
        {
            int direction = wp == VK_UP ? -1 : 1;
            if (GetKeyState(VK_CONTROL) < 0)
                move(*s, s->selected, direction);
            else
            {
                if (direction < 0 && s->selected)
                    --s->selected;
                if (direction > 0 && s->selected + 1 < s->selection.mods.size())
                    ++s->selected;
                reveal_selected(*s);
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == scroll_timer && s->scrolling)
        {
            const auto now = GetTickCount64();
            const double seconds = std::min(.05, (now - s->scroll_tick) / 1000.0);
            s->scroll_tick = now;
            if (!s->advance_scroll(seconds))
            {
                KillTimer(window, scroll_timer);
                s->scrolling = false;
            }
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY:
        KillTimer(window, scroll_timer);
        return 0;
    case WM_MOUSEWHEEL:
        if (!s->busy && !s->drag)
        {
            const double step = -GET_WHEEL_DELTA_WPARAM(wp) * wheel_step / WHEEL_DELTA;
            // Reverse immediately, without first completing queued motion in the other direction.
            const double origin = step * (s->scroll_target - s->scroll) < 0 ? s->scroll : s->scroll_target;
            scroll_to(*s, origin + step, true);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (s->busy)
            return 0;
        // Stop pending motion so clicks act on the currently painted rows.
        scroll_to(*s, s->scroll, false);
        if (x >= 865 && y < 55)
        {
            DestroyWindow(window);
            return 0;
        }
        for (int i = 0; i < 2; ++i)
        {
            if (launch_buttons[i].Contains(x, y))
            {
                launch(*s, i == 0);
                return 0;
            }
        }
        if (saves_control.Contains(x, y))
        {
            s->selection.separate_saves = !s->selection.separate_saves;
            s->focus = 3;
        }
        else if (scroll_track.Contains(x, y) && s->scroll_limit() > 0)
        {
            const RectF thumb = scroll_grip(*s);
            if (thumb.Contains(x, y))
            {
                s->drag_offset = y - thumb.Y;
                s->drag = true;
                SetCapture(window);
            }
            else
            {
                const float travel = scroll_track.Height - 2 * scroll_inset - thumb.Height;
                scroll_to(*s, (y - scroll_track.Y - scroll_inset - thumb.Height * .5f) / travel * s->scroll_limit(), true);
            }
        }
        else if (y >= rows_top && y < bottom && x >= list_box.X && x <= list_box.GetRight())
        {
            size_t index = (size_t)((s->scroll + y - rows_top) / row_height);
            if (index < s->selection.mods.size())
            {
                s->selected = index;
                s->focus = 0;
                if (x >= 399 && x <= 443)
                    move(*s, index, 1);
                else if (x >= 521 && x <= 565)
                    move(*s, index, -1);
                else if (x >= 640 && x <= 704)
                    s->selection.mods[index].enabled = !s->selection.mods[index].enabled;
            }
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (s->drag && s->scroll_limit() > 0)
        {
            const float travel = scroll_track.Height - 2 * scroll_inset - scroll_grip(*s).Height;
            const double fraction = (y - s->drag_offset - scroll_track.Y - scroll_inset) / (double)travel;
            scroll_to(*s, fraction * s->scroll_limit(), false);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s->drag)
        {
            s->drag = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        s->drag = false;
        return 0;
    case finished:
        if (s->worker.joinable())
            s->worker.join();
        s->busy = false;
        if (!s->error.empty())
        {
            MessageBoxA(window, s->error.c_str(), "Could not launch selected mods",
                        MB_OK | MB_ICONERROR);
            s->status = L"Launch failed. Your original game files were not changed.";
            s->error.clear();
            InvalidateRect(window, nullptr, FALSE);
        }
        else
        {
            s->launched = true;
            DestroyWindow(window);
        }
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
} // namespace
BOOL selector(HWND owner, const fs::path &installation, const fs::path &game, Selection selection)
{
    State s;
    s.install = installation;
    s.game = game;
    s.selection = std::move(selection);
    if (!s.art.init())
        throw std::runtime_error("The mod selector artwork could not be loaded.");
    auto instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MercenariesModSelector";
    RegisterClassW(&wc);
    RECT desktop;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    int height = std::min(900, (int)(desktop.bottom - desktop.top - 40)),
        width = MulDiv(height, W, H);
    HWND w = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName,
                             L"Mercenaries Recompiled Mod Selector", WS_POPUP | WS_THICKFRAME,
                             desktop.left + (desktop.right - desktop.left - width) / 2,
                             desktop.top + (desktop.bottom - desktop.top - height) / 2, width,
                             height, owner, nullptr, instance, &s);
    if (!w)
        throw std::runtime_error("Cannot create mod selector window.");
    if (owner)
        EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    UpdateWindow(w);
    MSG message;
    while (IsWindow(w) && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (s.worker.joinable())
        s.worker.join();
    if (owner)
        EnableWindow(owner, TRUE);
    return s.launched;
}
} // namespace mercmods
extern "C" BOOL mercenaries_mod_preview(const wchar_t *output)
{
    mercmods::State s;
    if (!s.art.init())
        return FALSE;
    for (int i = 0; i < 24; i++)
        s.selection.mods.push_back({i == 0   ? L"JE3"
                                    : i == 1 ? L"Camera by Lis"
                                             : L"Example mod " + std::to_wstring(i + 1),
                                    {},
                                    i < 2});
    Bitmap bitmap(mercmods::W, mercmods::H, PixelFormat32bppPARGB);
    {
        Graphics g(&bitmap);
        mercmods::scene(g, mercmods::W, mercmods::H, s);
    }
    const CLSID png = {
        0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    return bitmap.Save(output, &png, nullptr) == Ok;
}
