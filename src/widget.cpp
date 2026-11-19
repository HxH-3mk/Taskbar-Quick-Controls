#include "widget.h"
#include "controls.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

struct BarInstance {
    HWND taskbar = nullptr;
    HWND wnd = nullptr;
    HWND tip = nullptr;
    HWND rebar = nullptr;
    bool isPrimary = false;
    int hoverIdx = -1;
    int dragIdx = -1;
    int wheelAccum = 0;
    std::vector<RECT> slotRects;
    std::wstring tipText;
    int lastW = -1;
    int lastDpi = 0;
    int lastLeft = -1;
    SIZE size{};
    RECT lastTray{};
    RECT lastBar{};
    RECT lastWidgets{};
    RECT lastBattery{};
    RECT lastAdhan{};
};

HINSTANCE g_inst = nullptr;
HWND g_notify = nullptr;
UINT g_clickMsg = 0;
HWND g_lastClickedWnd = nullptr;
bool g_light = false;
bool g_background = false;
bool g_updateDot = false;
std::wstring g_updateVer;
AppLang g_lang = AppLang::English;
std::vector<std::unique_ptr<BarInstance>> g_bars;

const wchar_t* kClass = L"QuickControlsTaskbarWidget";

int DpiFor(HWND taskbar) {
    UINT d = taskbar ? GetDpiForWindow(taskbar) : 0;
    return d ? static_cast<int>(d) : 96;
}
int Px(HWND taskbar, int v) { return MulDiv(v, DpiFor(taskbar), 96); }

// Background UI Automation lookup for Windows 11 Widgets button on primary taskbar.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};
HWND g_wbRectBar = nullptr;
HWND g_wbTaskbar = nullptr;
HANDLE g_wbWake = nullptr;

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

RECT WidgetsButtonRect(HWND taskbar) {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = (g_wbRectBar && g_wbRectBar == taskbar) ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

void ApplyTipTheme(HWND tip) {
    if (!tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(tip, WM_THEMECHANGED, 0, 0);
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
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, static_cast<size_t>(W) * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), static_cast<int>(s.size()));
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = static_cast<int>((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return static_cast<BYTE>(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize(static_cast<size_t>(m.w) * m.h);
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
    Canvas(int w_, int h_) : w(w_), h(h_), px(static_cast<size_t>(w_) * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = (static_cast<DWORD>(oa) << 24) | (static_cast<DWORD>(orr) << 16) |
                        (static_cast<DWORD>(og) << 8) | static_cast<DWORD>(ob);
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

int SlotCount() {
    return static_cast<int>(controls::GetSlots().size());
}

void SlotToControl(int slotIdx, ControlKind& outKind, int& outMonIdx) {
    auto slots = controls::GetSlots();
    if (slotIdx < 0 || slotIdx >= static_cast<int>(slots.size())) {
        outKind = ControlKind::Volume;
        outMonIdx = -1;
        return;
    }
    outKind = slots[slotIdx].kind;
    outMonIdx = slots[slotIdx].monIdx;
}

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
    int total = SlotCount();
    if (idx < 0 || idx >= total) {
        std::wstring out;
        wchar_t buf[128]{};
        swprintf_s(buf, ar ? L"الصوت: \u200E%d%%" : L"Volume: %d%%", controls::GetValue(ControlKind::Volume));
        out = buf;
        if (controls::IsPerMonitorBrightness()) {
            int mc = controls::MonitorCount();
            for (int m = 0; m < mc; m++) {
                std::wstring lbl = controls::MonitorLabel(m, g_lang);
                swprintf_s(buf, ar ? L"\nالسطوع (%s): \u200E%d%%" : L"\nBrightness (%s): %d%%",
                           lbl.c_str(), controls::GetValue(ControlKind::Brightness, m));
                out += buf;
            }
        } else {
            swprintf_s(buf, ar ? L"\nالسطوع: \u200E%d%%" : L"\nBrightness: %d%%",
                       controls::GetValue(ControlKind::Brightness));
            out += buf;
        }
        swprintf_s(buf, ar ? L"\nصفار الشاشة: \u200E%d%%" : L"\nEye Warmth: %d%%",
                   controls::GetValue(ControlKind::Warmth));
        out += buf;
        if (g_updateDot && !g_updateVer.empty()) {
            out += ar ? (L"\nيتوفر تحديث: " + g_updateVer) : (L"\nUpdate available: " + g_updateVer);
        }
        return out;
    }

    ControlKind k = ControlKind::Volume;
    int monIdx = -1;
    SlotToControl(idx, k, monIdx);
    int val = controls::GetValue(k, monIdx);

    std::wstring label;
    if (k == ControlKind::Volume) {
        label = ar ? L"الصوت" : L"Volume";
    } else if (k == ControlKind::Warmth) {
        label = ar ? L"صفار الشاشة" : L"Eye Warmth";
    } else {
        label = ar ? L"السطوع" : L"Brightness";
        if (controls::IsPerMonitorBrightness() && monIdx >= 0) {
            label += L" (" + controls::MonitorLabel(monIdx, g_lang) + L")";
        }
    }

    wchar_t buf[96]{};
    swprintf_s(buf, ar ? L"%s: \u200E%d%%" : L"%s: %d%%", label.c_str(), val);
    std::wstring out = buf;
    if (g_updateDot && !g_updateVer.empty()) {
        out += ar ? (L"\nيتوفر تحديث: " + g_updateVer) : (L"\nUpdate available: " + g_updateVer);
    }
    return out;
}

void UpdateTooltip(BarInstance& b, int idx) {
    if (!b.tip) return;
    std::wstring tip = BuildTooltipForIndex(idx);
    if (tip != b.tipText) {
        b.tipText = tip;
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.hwnd = g_notify;
        ti.uId = reinterpret_cast<UINT_PTR>(b.wnd);
        ti.lpszText = const_cast<wchar_t*>(b.tipText.c_str());
        SendMessageW(b.tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

int HitTestScreen(const BarInstance& b) {
    if (!b.wnd) return -1;
    POINT pt{};
    RECT w{};
    if (!GetCursorPos(&pt) || !GetWindowRect(b.wnd, &w)) return -1;
    int x = pt.x - w.left;
    for (int i = 0; i < static_cast<int>(b.slotRects.size()); i++) {
        if (x >= b.slotRects[i].left && x < b.slotRects[i].right) return i;
    }
    return -1;
}

void PaintBar(BarInstance& b) {
    if (!b.wnd || !b.taskbar) return;
    RECT bar{};
    GetClientRect(b.taskbar, &bar);
    int H = bar.bottom - bar.top;
    if (H <= 0) return;

    auto px = [&](int v) { return Px(b.taskbar, v); };
    const int count = SlotCount();
    const int padX = px(4);
    const int slotW = px(28);
    const int gap = px(2);
    const int W = padX * 2 + slotW * count + gap * std::max(0, count - 1);

    Canvas cv(W, H);
    for (auto& p : cv.px) p = 0x01000000;

    if (g_background) {
        cv.FillRoundRect(0, px(4), W, H - px(4), px(6),
                         g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 }, 240);
    }

    if (controls::IsReorderMode()) {
        Rgb rc = g_light ? Rgb{ 0, 103, 192 } : Rgb{ 96, 205, 255 };
        int y0 = px(4), y1 = H - px(4) - 1;
        for (int x = 1; x < W - 1; x += 4) {
            cv.Blend(x, y0, rc, 200);
            cv.Blend(x + 1, y0, rc, 200);
            cv.Blend(x, y1, rc, 200);
            cv.Blend(x + 1, y1, rc, 200);
        }
        for (int y = y0; y <= y1; y += 4) {
            cv.Blend(1, y, rc, 200);
            cv.Blend(W - 2, y, rc, 200);
        }
    }

    HFONT icons = MakeFont(px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
    HFONT badgeFont = MakeFont(px(10), FW_BOLD, L"Segoe UI", L"Segoe UI");
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb warmColor = g_light ? Rgb{ 194, 102, 0 } : Rgb{ 255, 196, 86 };
    Rgb badgeCol = g_light ? Rgb{ 0, 103, 192 } : Rgb{ 96, 205, 255 };

    auto slots = controls::GetSlots();
    b.slotRects.resize(count);
    for (int i = 0; i < count; i++) {
        int sx0 = padX + i * (slotW + gap);
        int sx1 = sx0 + slotW;
        b.slotRects[i] = { sx0, 0, sx1, H };

        if (b.hoverIdx == i) {
            cv.FillRoundRect(sx0, px(4), sx1, H - px(4), px(4),
                             g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);
        }

        ControlKind k = slots[i].kind;
        int monIdx = slots[i].monIdx;

        Mask m = RenderMask(std::wstring(1, IconForKind(k)), icons, g_light);
        Rgb c = (k == ControlKind::Warmth && controls::GetValue(ControlKind::Warmth) > 0) ? warmColor : fg;
        int alpha = (k == ControlKind::Brightness && controls::GetValue(ControlKind::Brightness, monIdx) < 0) ? 165 : 255;
        cv.Draw(m, sx0 + (slotW - m.w) / 2, (H - m.h) / 2, c, alpha);

        if (k == ControlKind::Brightness && controls::IsPerMonitorBrightness() && monIdx >= 0 && controls::MonitorCount() > 1) {
            std::wstring num = std::to_wstring(monIdx + 1);
            Mask bm = RenderMask(num, badgeFont, g_light);
            cv.Draw(bm, sx1 - bm.w - px(2), H - bm.h - px(6), badgeCol, 245);
        }
    }

    DeleteObject(icons);
    DeleteObject(badgeFont);

    if (g_updateDot) {
        int r = std::max(2, px(3));
        int cx = W - px(5);
        int cy = px(7);
        Rgb dotCol{ 34, 197, 94 };
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r) cv.Blend(cx + dx, cy + dy, dotCol, 240);
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), static_cast<size_t>(W) * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    b.size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(b.wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    b.lastW = -1;
}

bool RepositionBar(BarInstance& b) {
    if (!b.wnd || !IsWindow(b.wnd) || !b.taskbar || !IsWindow(b.taskbar)) return false;
    RECT bar{};
    GetWindowRect(b.taskbar, &bar);
    if (bar.bottom - bar.top > bar.right - bar.left) return false;

    int dpi = DpiFor(b.taskbar);
    if (dpi != b.lastDpi) {
        b.lastDpi = dpi;
        PaintBar(b);
    }

    auto px = [&](int v) { return Px(b.taskbar, v); };
    bool rtl = (GetWindowLongW(b.taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) != 0;

    RECT tr{};
    int edge = rtl ? bar.left : bar.right;

    if (b.isPrimary) {
        HWND tray = FindWindowExW(b.taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        if (!tray) return true;
        GetWindowRect(tray, &tr);
        edge = rtl ? tr.right : tr.left;
    } else {
        bool foundClock = false;
        for (const wchar_t* cls : { L"TrayNotifyWnd", L"ClockButton", L"TrayClockWClass" }) {
            HWND clk = FindWindowExW(b.taskbar, nullptr, cls, nullptr);
            if (clk && IsWindowVisible(clk)) {
                GetWindowRect(clk, &tr);
                if (tr.right > tr.left) {
                    edge = rtl ? tr.right : tr.left;
                    foundClock = true;
                    break;
                }
            }
        }
        if (!foundClock) {
            bool isWin11 = FindWindowExW(b.taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr) != nullptr;
            int reserve = isWin11 ? px(80) : px(8);
            edge = rtl ? (bar.left + reserve) : (bar.right - reserve);
        }
    }

    auto stepPastSibling = [&](const wchar_t* cls, RECT& outRect) {
        outRect = {};
        HWND sib = FindWindowExW(b.taskbar, nullptr, cls, nullptr);
        if (sib && IsWindowVisible(sib)) {
            RECT r{};
            GetWindowRect(sib, &r);
            if (r.right > r.left && r.top < bar.bottom && r.bottom > bar.top) {
                outRect = r;
                if (rtl) { if (r.right > edge) edge = r.right; }
                else     { if (r.left < edge) edge = r.left; }
            }
        }
    };

    RECT br{}, ar{};
    stepPastSibling(L"WirelessBatteryWidget", br);
    stepPastSibling(L"AdhanTaskbarWidget", ar);

    if (b.isPrimary && !rtl) {
        for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
            HWND feeds = FindWindowExW(b.taskbar, nullptr, cls, nullptr);
            if (!feeds || !IsWindowVisible(feeds)) continue;
            RECT fr{};
            GetWindowRect(feeds, &fr);
            if (fr.left < edge && fr.right > fr.left) edge = fr.left;
        }
    }

    RECT wb = b.isPrimary ? WidgetsButtonRect(b.taskbar) : RECT{};
    if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
        (rtl ? wb.left < edge + b.size.cx + px(2) && wb.right > edge : wb.right > edge - b.size.cx - px(2) && wb.left < edge)) {
        edge = rtl ? wb.right : wb.left;
    }

    POINT p{ edge, bar.top };
    ScreenToClient(b.taskbar, &p);
    int left = p.x - b.size.cx - px(4);

    if (b.rebar && IsWindow(b.rebar)) {
        RECT rr{};
        GetWindowRect(b.rebar, &rr);
        POINT tl{ rtl ? rr.right : rr.left, rr.top };
        ScreenToClient(b.taskbar, &tl);
        if (tl.x + (rr.right - rr.left) > left && left > tl.x)
            MoveWindow(b.rebar, tl.x, tl.y, left - tl.x, rr.bottom - rr.top, TRUE);
    }

    bool covered = GetWindow(b.wnd, GW_HWNDPREV) != nullptr;
    if (!covered && EqualRect(&tr, &b.lastTray) && EqualRect(&bar, &b.lastBar) && EqualRect(&wb, &b.lastWidgets) &&
        EqualRect(&br, &b.lastBattery) && EqualRect(&ar, &b.lastAdhan) && b.lastW == b.size.cx && b.lastLeft == left)
        return true;

    b.lastTray = tr;
    b.lastWidgets = wb;
    b.lastBattery = br;
    b.lastAdhan = ar;
    b.lastBar = bar;
    b.lastW = b.size.cx;
    b.lastLeft = left;
    SetWindowPos(b.wnd, HWND_TOP, left, 0, b.size.cx, b.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* b = reinterpret_cast<BarInstance*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_MOUSEMOVE:
            if (b) {
                int idx = HitTestScreen(*b);
                if (b->dragIdx >= 0 && controls::IsReorderMode()) {
                    PostMessageW(g_notify, g_clickMsg, 2, 0);
                    if (idx >= 0 && idx != b->dragIdx) {
                        controls::MoveSlot(b->dragIdx, idx);
                        b->dragIdx = idx;
                        b->hoverIdx = idx;
                        UpdateTooltip(*b, idx);
                        for (auto& bar : g_bars) PaintBar(*bar);
                    }
                    return 0;
                }
                if (b->hoverIdx != idx) {
                    b->hoverIdx = idx;
                    TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                    TrackMouseEvent(&t);
                    UpdateTooltip(*b, idx);
                    PaintBar(*b);
                    RepositionBar(*b);
                }
            }
            return 0;
        case WM_MOUSELEAVE:
            if (b) {
                b->hoverIdx = -1;
                b->wheelAccum = 0;
                UpdateTooltip(*b, -1);
                PaintBar(*b);
                RepositionBar(*b);
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (b) {
                int idx = HitTestScreen(*b);
                if (idx < 0) idx = b->hoverIdx;
                if (idx >= 0 && idx < SlotCount()) {
                    b->wheelAccum += GET_WHEEL_DELTA_WPARAM(wp);
                    int steps = b->wheelAccum / WHEEL_DELTA;
                    if (steps != 0) {
                        b->wheelAccum -= steps * WHEEL_DELTA;
                        ControlKind k = ControlKind::Volume;
                        int monIdx = -1;
                        SlotToControl(idx, k, monIdx);
                        controls::Adjust(k, steps * 5, monIdx);
                        b->hoverIdx = idx;
                        UpdateTooltip(*b, idx);
                        for (auto& bar : g_bars) PaintBar(*bar);
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            if (b && controls::IsReorderMode()) {
                PostMessageW(g_notify, g_clickMsg, 2, 0);
                int idx = HitTestScreen(*b);
                if (idx >= 0) {
                    b->dragIdx = idx;
                    SetCapture(h);
                }
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (b) {
                if (b->dragIdx >= 0) {
                    ReleaseCapture();
                    b->dragIdx = -1;
                    return 0;
                }
                if (b->tip) SendMessageW(b->tip, TTM_POP, 0, 0);
                int idx = HitTestScreen(*b);
                if (idx >= 0 && idx < SlotCount()) {
                    g_lastClickedWnd = h;
                    PostMessageW(g_notify, g_clickMsg, 0, static_cast<LPARAM>(idx));
                }
            }
            return 0;
        case WM_RBUTTONUP:
            if (b && b->tip) SendMessageW(b->tip, TTM_POP, 0, 0);
            g_lastClickedWnd = h;
            PostMessageW(g_notify, g_clickMsg, 1, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void DestroyBar(BarInstance& b) {
    if (b.rebar && IsWindow(b.rebar) && b.taskbar && IsWindow(b.taskbar)) {
        RECT r{};
        GetClientRect(b.taskbar, &r);
        PostMessageW(b.taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    b.rebar = nullptr;
    if (b.tip) { DestroyWindow(b.tip); b.tip = nullptr; }
    if (b.wnd) { DestroyWindow(b.wnd); b.wnd = nullptr; }
}

std::unique_ptr<BarInstance> CreateBar(HWND taskbar, bool isPrimary) {
    if (!taskbar || !IsWindow(taskbar) || !IsWindowVisible(taskbar)) return nullptr;
    RECT tb{};
    GetWindowRect(taskbar, &tb);
    if (tb.bottom - tb.top > tb.right - tb.left) return nullptr;
    if (isPrimary && !FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr)) return nullptr;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
    }

    auto b = std::make_unique<BarInstance>();
    b->taskbar = taskbar;
    b->isPrimary = isPrimary;
    b->rebar = (isPrimary && !FindWindowExW(taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr))
                   ? FindWindowExW(taskbar, nullptr, L"ReBarWindow32", nullptr)
                   : nullptr;

    b->wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_NOINHERITLAYOUT, kClass, L"",
                             WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar, nullptr, g_inst, nullptr);
    if (!b->wnd) return nullptr;
    LONG_PTR ex = GetWindowLongPtrW(b->wnd, GWL_EXSTYLE);
    if (ex & WS_EX_LAYOUTRTL) SetWindowLongPtrW(b->wnd, GWL_EXSTYLE, ex & ~WS_EX_LAYOUTRTL);
    SetWindowLongPtrW(b->wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b.get()));

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);
    b->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             0, 0, 0, 0, g_notify, nullptr, g_inst, nullptr);
    if (b->tip) {
        ApplyTipTheme(b->tip);
        b->tipText = BuildTooltipForIndex(-1);
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_notify;
        ti.uId = reinterpret_cast<UINT_PTR>(b->wnd);
        ti.lpszText = const_cast<wchar_t*>(b->tipText.c_str());
        SendMessageW(b->tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(b->tip, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(b->tip, TTM_SETDELAYTIME, TTDT_INITIAL, 150);
    }

    if (isPrimary) WatchWidgetsButton(taskbar);
    b->lastDpi = DpiFor(taskbar);
    PaintBar(*b);
    b->lastW = -1;
    RepositionBar(*b);
    ShowWindow(b->wnd, SW_SHOWNOACTIVATE);
    return b;
}

struct EnumTaskbarsCtx {
    std::vector<std::pair<HWND, bool>> list;
};

BOOL CALLBACK EnumSecondaryProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<EnumTaskbarsCtx*>(lp);
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 && IsWindowVisible(hwnd)) {
        ctx->list.push_back({ hwnd, false });
    }
    return TRUE;
}

std::vector<std::pair<HWND, bool>> FindAllTaskbars() {
    EnumTaskbarsCtx ctx;
    HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (primary && IsWindowVisible(primary)) {
        ctx.list.push_back({ primary, true });
    }
    EnumWindows(EnumSecondaryProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.list;
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_inst = inst;
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;
    return Reposition();
}

void Destroy() {
    if (g_wbWake) WatchWidgetsButton(nullptr);
    for (auto& b : g_bars) DestroyBar(*b);
    g_bars.clear();
}

bool Alive() {
    if (g_bars.empty()) return false;
    for (const auto& b : g_bars) {
        if (b->wnd && IsWindow(b->wnd) && b->taskbar && IsWindow(b->taskbar)) return true;
    }
    return false;
}

void Refresh() {
    for (auto& b : g_bars) {
        if (b->wnd && IsWindow(b->wnd)) {
            UpdateTooltip(*b, b->hoverIdx);
            PaintBar(*b);
            RepositionBar(*b);
        }
    }
}

void ResolveSlot(int slotIdx, ControlKind& outKind, int& outMonIdx) {
    SlotToControl(slotIdx, outKind, outMonIdx);
}

void SetLanguage(AppLang lang) {
    g_lang = lang;
    for (auto& b : g_bars) UpdateTooltip(*b, b->hoverIdx);
}

HWND LastClickedWnd() { return g_lastClickedWnd; }

RECT SlotScreenRect(HWND widgetWnd, int slotIdx) {
    RECT r{};
    BarInstance* target = nullptr;
    for (const auto& b : g_bars) {
        if (b->wnd == widgetWnd) { target = b.get(); break; }
    }
    if (!target && !g_bars.empty()) target = g_bars[0].get();
    if (!target || !target->wnd || !IsWindow(target->wnd)) return r;
    if (slotIdx < 0 || slotIdx >= static_cast<int>(target->slotRects.size())) return r;

    RECT w{};
    GetWindowRect(target->wnd, &w);
    r.left = w.left + target->slotRects[slotIdx].left;
    r.right = w.left + target->slotRects[slotIdx].right;
    r.top = w.top;
    r.bottom = w.bottom;
    return r;
}

void SetLightTheme(bool light) {
    g_light = light;
    for (auto& b : g_bars) {
        ApplyTipTheme(b->tip);
        PaintBar(*b);
    }
}

void SetUpdateAvailable(const std::wstring& version) {
    g_updateDot = !version.empty();
    g_updateVer = version;
    for (auto& b : g_bars) {
        UpdateTooltip(*b, b->hoverIdx);
        PaintBar(*b);
    }
}

void SetBackground(bool on) {
    g_background = on;
    for (auto& b : g_bars) PaintBar(*b);
}

bool Reposition() {
    auto found = FindAllTaskbars();

    for (auto it = g_bars.begin(); it != g_bars.end();) {
        bool keep = false;
        for (const auto& f : found) {
            if (f.first == (*it)->taskbar && f.second == (*it)->isPrimary) {
                keep = true;
                break;
            }
        }
        if (!keep || !(*it)->wnd || !IsWindow((*it)->wnd)) {
            DestroyBar(**it);
            it = g_bars.erase(it);
        } else {
            ++it;
        }
    }

    for (const auto& f : found) {
        bool exists = false;
        for (const auto& b : g_bars) {
            if (b->taskbar == f.first) { exists = true; break; }
        }
        if (!exists) {
            auto nb = CreateBar(f.first, f.second);
            if (nb) g_bars.push_back(std::move(nb));
        }
    }

    for (auto it = g_bars.begin(); it != g_bars.end();) {
        if (!RepositionBar(**it)) {
            DestroyBar(**it);
            it = g_bars.erase(it);
        } else {
            ++it;
        }
    }

    return !g_bars.empty();
}

}  // namespace widget
