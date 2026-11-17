#include "widget.h"
#include "controls.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

HWND g_wnd, g_taskbar, g_tip, g_notify, g_rebar;
UINT g_clickMsg;
bool g_light, g_background;
AppLang g_lang = AppLang::English;
int g_hoverIdx = -1;  // -1 = none, 0 = Volume, 1 = Brightness, 2 = Warmth
int g_wheelAccum = 0;
RECT g_slotRects[3]{};
std::wstring g_tipText;
RECT g_lastTray{}, g_lastBar{}, g_lastWidgets{}, g_lastBattery{}, g_lastAdhan{};
int g_lastW = -1, g_lastDpi = 0, g_lastLeft = -1;
SIZE g_size{};

const wchar_t* kClass = L"QuickControlsTaskbarWidget";

int Dpi() {
    UINT d = g_taskbar ? GetDpiForWindow(g_taskbar) : 0;
    return d ? (int)d : 96;
}
int Px(int v) { return MulDiv(v, Dpi(), 96); }

// Background UI Automation lookup for Windows 11 Widgets button.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};
HWND g_wbRectBar;
HWND g_wbTaskbar;
HANDLE g_wbWake;

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ApplyTipTheme() {
    if (!g_tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(g_tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(g_tip, WM_THEMECHANGED, 0, 0);
    }
}

HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback) {
    for (const wchar_t* f : { face, fallback }) {
        HFONT h = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
    void FillRoundRect(int x0, int y0, int x1, int y1, int r, Rgb c, int a) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                int dx = x < x0 + r ? (x0 + r) - x : (x >= x1 - r ? x - (x1 - r - 1) : 0);
                int dy = y < y0 + r ? (y0 + r) - y : (y >= y1 - r ? y - (y1 - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) Blend(x, y, c, a);
            }
    }
};

wchar_t IconForKind(ControlKind kind) {
    switch (kind) {
        case ControlKind::Volume:
            return (controls::IsMuted() || controls::GetValue(ControlKind::Volume) == 0) ? 0xE74F : 0xE767;
        case ControlKind::Brightness:
            return 0xE706;
        case ControlKind::Warmth:
            return 0xE708;
        default:
            return 0xE706;
    }
}

std::wstring BuildTooltipForIndex(int idx) {
    bool ar = (g_lang == AppLang::Arabic);
    if (idx < 0 || idx >= 3) {
        wchar_t buf[128]{};
        if (ar) {
            swprintf_s(buf, L"الصوت: \u200E%d%%\nالسطوع: \u200E%d%%\nصفار الشاشة: \u200E%d%%",
                       controls::GetValue(ControlKind::Volume),
                       controls::GetValue(ControlKind::Brightness),
                       controls::GetValue(ControlKind::Warmth));
        } else {
            swprintf_s(buf, L"Volume: %d%%\nBrightness: %d%%\nEye Warmth: %d%%",
                       controls::GetValue(ControlKind::Volume),
                       controls::GetValue(ControlKind::Brightness),
                       controls::GetValue(ControlKind::Warmth));
        }
        return buf;
    }
    ControlKind k = static_cast<ControlKind>(idx);
    int val = controls::GetValue(k);
    const wchar_t* name = ar
        ? ((idx == 0) ? L"الصوت" : (idx == 1) ? L"السطوع" : L"صفار الشاشة")
        : ((idx == 0) ? L"Volume" : (idx == 1) ? L"Brightness" : L"Eye Warmth");
    wchar_t buf[64]{};
    swprintf_s(buf, ar ? L"%s: \u200E%d%%" : L"%s: %d%%", name, val);
    return buf;
}

void UpdateTooltip(int idx) {
    if (!g_tip) return;
    std::wstring tip = BuildTooltipForIndex(idx);
    if (tip != g_tipText) {
        g_tipText = tip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = reinterpret_cast<UINT_PTR>(g_wnd);
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

int HitTest(int clientX) {
    for (int i = 0; i < 3; i++) {
        if (clientX >= g_slotRects[i].left && clientX < g_slotRects[i].right) return i;
    }
    return -1;
}

void Paint() {
    if (!g_wnd) return;
    RECT bar;
    GetClientRect(g_taskbar, &bar);
    int H = bar.bottom - bar.top;
    if (H <= 0) return;

    const int padX = Px(4);
    const int slotW = Px(28);
    const int gap = Px(2);
    const int W = padX * 2 + slotW * 3 + gap * 2;

    Canvas cv(W, H);
    for (auto& p : cv.px) p = 0x01000000;

    if (g_background) {
        cv.FillRoundRect(0, Px(4), W, H - Px(4), Px(6),
                         g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 }, 240);
    }

    HFONT icons = MakeFont(Px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb warmColor = g_light ? Rgb{ 194, 102, 0 } : Rgb{ 255, 196, 86 };

    for (int i = 0; i < 3; i++) {
        int sx0 = padX + i * (slotW + gap);
        int sx1 = sx0 + slotW;
        g_slotRects[i] = { sx0, 0, sx1, H };

        if (g_hoverIdx == i) {
            cv.FillRoundRect(sx0, Px(4), sx1, H - Px(4), Px(4),
                             g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);
        }

        ControlKind k = static_cast<ControlKind>(i);
        Mask m = RenderMask(std::wstring(1, IconForKind(k)), icons, g_light);
        Rgb c = (k == ControlKind::Warmth && controls::GetValue(ControlKind::Warmth) > 0) ? warmColor : fg;
        int alpha = (k == ControlKind::Brightness && controls::GetValue(ControlKind::Brightness) < 0) ? 165 : 255;
        cv.Draw(m, sx0 + (slotW - m.w) / 2, (H - m.h) / 2, c, alpha);
    }

    DeleteObject(icons);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    g_size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    g_lastW = -1;
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp);
            int idx = HitTest(x);
            if (g_hoverIdx != idx) {
                g_hoverIdx = idx;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                UpdateTooltip(idx);
                Paint();
                widget::Reposition();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hoverIdx = -1;
            g_wheelAccum = 0;
            UpdateTooltip(-1);
            Paint();
            widget::Reposition();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(h, &pt);
            int idx = HitTest(pt.x);
            if (idx < 0) idx = g_hoverIdx;
            if (idx >= 0 && idx < 3) {
                g_wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
                int steps = g_wheelAccum / WHEEL_DELTA;
                if (steps != 0) {
                    g_wheelAccum -= steps * WHEEL_DELTA;
                    controls::Adjust(static_cast<ControlKind>(idx), steps * 5);
                    g_hoverIdx = idx;
                    UpdateTooltip(idx);
                    Paint();
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
            int idx = HitTest(GET_X_LPARAM(lp));
            if (idx >= 0 && idx < 3) {
                PostMessageW(g_notify, g_clickMsg, 0, static_cast<LPARAM>(idx));
            }
            return 0;
        }
        case WM_RBUTTONUP:
            if (g_tip) SendMessageW(g_tip, TTM_POP, 0, 0);
            PostMessageW(g_notify, g_clickMsg, 1, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!g_taskbar || !FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { g_taskbar = nullptr; return false; }
    RECT tb;
    GetWindowRect(g_taskbar, &tb);
    if (tb.bottom - tb.top > tb.right - tb.left) { g_taskbar = nullptr; return false; }
    g_rebar = FindWindowExW(g_taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                  ? nullptr
                  : FindWindowExW(g_taskbar, nullptr, L"ReBarWindow32", nullptr);
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOINHERITLAYOUT, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1,
                            g_taskbar, nullptr, inst, nullptr);
    if (!g_wnd) { g_taskbar = nullptr; return false; }

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                            notifyWnd, nullptr, inst, nullptr);
    if (g_tip) {
        ApplyTipTheme();
        g_tipText = BuildTooltipForIndex(-1);
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = notifyWnd;
        ti.uId = reinterpret_cast<UINT_PTR>(g_wnd);
        ti.lpszText = const_cast<wchar_t*>(g_tipText.c_str());
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(g_tip, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
    }
    WatchWidgetsButton(g_taskbar);
    g_lastDpi = Dpi();
    Paint();
    g_lastW = -1;
    Reposition();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    return true;
}

void Destroy() {
    if (g_rebar && IsWindow(g_rebar) && g_taskbar && IsWindow(g_taskbar)) {
        RECT r;
        GetClientRect(g_taskbar, &r);
        PostMessageW(g_taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    g_rebar = nullptr;
    if (g_wbWake) WatchWidgetsButton(nullptr);
    if (g_tip) { DestroyWindow(g_tip); g_tip = nullptr; }
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    g_taskbar = nullptr;
    g_hoverIdx = -1;
}

bool Alive() { return g_wnd && IsWindow(g_wnd) && g_taskbar && IsWindow(g_taskbar); }

void Refresh() {
    if (Alive()) Paint();
}

void SetLanguage(AppLang lang) {
    g_lang = lang;
    UpdateTooltip(g_hoverIdx);
}

RECT SlotScreenRect(int idx) {
    RECT r{};
    if (!Alive() || idx < 0 || idx >= 3) return r;
    RECT w{};
    GetWindowRect(g_wnd, &w);
    r.left = w.left + g_slotRects[idx].left;
    r.right = w.left + g_slotRects[idx].right;
    r.top = w.top;
    r.bottom = w.bottom;
    return r;
}

void SetLightTheme(bool light) {
    g_light = light;
    ApplyTipTheme();
    Paint();
}

void SetBackground(bool on) {
    g_background = on;
    Paint();
}

bool Reposition() {
    if (!Alive()) return false;
    if (Dpi() != g_lastDpi) { g_lastDpi = Dpi(); Paint(); }
    HWND tray = FindWindowExW(g_taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (!tray) return true;
    RECT tr, bar;
    GetWindowRect(tray, &tr);
    GetWindowRect(g_taskbar, &bar);
    bool rtl = GetWindowLongW(g_taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int edge = rtl ? tr.right : tr.left;

    auto stepPastSibling = [&](const wchar_t* cls, RECT& outRect) {
        outRect = {};
        HWND sib = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
        if (sib && IsWindowVisible(sib)) {
            RECT r{};
            GetWindowRect(sib, &r);
            if (r.right > r.left && r.top < bar.bottom && r.bottom > bar.top) {
                outRect = r;
                if (rtl) {
                    if (r.right > edge) edge = r.right;
                } else {
                    if (r.left < edge) edge = r.left;
                }
            }
        }
    };

    RECT br{}, ar{};
    stepPastSibling(L"WirelessBatteryWidget", br);
    stepPastSibling(L"AdhanTaskbarWidget", ar);

    if (!rtl)
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(g_taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr;
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    RECT wb = WidgetsButtonRect();
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + g_size.cx + Px(2) && wb.right > edge : wb.right > edge - g_size.cx - Px(2) && wb.left < edge))
        edge = rtl ? wb.right : wb.left;
    POINT p{ edge, bar.top };
    ScreenToClient(g_taskbar, &p);
    int left = p.x - g_size.cx - Px(4);

    if (g_rebar && IsWindow(g_rebar)) {
        RECT rr;
        GetWindowRect(g_rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(g_taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(g_rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    bool covered = GetWindow(g_wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &g_lastTray) && EqualRect(&bar, &g_lastBar) && EqualRect(&wb, &g_lastWidgets) &&
        EqualRect(&br, &g_lastBattery) && EqualRect(&ar, &g_lastAdhan) && g_lastW == g_size.cx && g_lastLeft == left)
        return true;
    g_lastTray = tr;
    g_lastWidgets = wb;
    g_lastBattery = br;
    g_lastAdhan = ar;
    g_lastBar = bar;
    g_lastW = g_size.cx;
    g_lastLeft = left;
    SetWindowPos(g_wnd, HWND_TOP, left, 0, g_size.cx, g_size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace widget
