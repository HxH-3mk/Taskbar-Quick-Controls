#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <uxtheme.h>
#include <cstdio>
#include <string>

#include "controls.h"
#include "updater.h"
#include "widget.h"

namespace {

constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 4;
constexpr UINT_PTR TIMER_REPOSITION = 1;
constexpr UINT_PTR TIMER_UPDATE_CHECK = 2;

enum MenuId : UINT {
    ID_UPDATE_NOW = 90,
    ID_RESET_WARMTH = 100,
    ID_LANG_EN,
    ID_LANG_AR,
    ID_WIDGET_BACKGROUND,
    ID_AUTOSTART,
    ID_CHECK_UPDATES,
    ID_EXIT
};

const wchar_t* kAppName = L"Taskbar Quick Controls";
const wchar_t* kWndClass = L"QuickControlsWnd";
const wchar_t* kPopupClass = L"QuickControlsSliderPopup";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"TaskbarQuickControls";
const wchar_t* kSettingsKey = L"Software\\TaskbarQuickControls";

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HWND g_popup = nullptr;
UINT g_taskbarCreatedMsg = 0;
bool g_lightTaskbar = false;
bool g_lightApps = false;
bool g_widgetBackground = false;
AppLang g_lang = AppLang::English;
updater::UpdateInfo g_updateInfo;

bool ReadPersonalizeBool(const wchar_t* valueName, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     valueName, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        return v != 0;
    }
    return defaultVal;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2]{};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- Floating Slider Popup (for Laptop Touchpads & Clicks) ------------------------------------

struct SliderPopupState {
    ControlKind kind = ControlKind::Volume;
    HWND label = nullptr;
    HWND slider = nullptr;
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr;
    bool light = false;
};

std::wstring SliderLabelText(ControlKind kind, int val) {
    bool ar = (g_lang == AppLang::Arabic);
    const wchar_t* name = ar
        ? ((kind == ControlKind::Volume) ? L"الصوت" : (kind == ControlKind::Brightness) ? L"السطوع" : L"صفار الشاشة")
        : ((kind == ControlKind::Volume) ? L"Volume" : (kind == ControlKind::Brightness) ? L"Brightness" : L"Eye Warmth");
    wchar_t buf[64]{};
    swprintf_s(buf, ar ? L"%s: \u200E%d%%" : L"%s: %d%%", name, val);
    return buf;
}

LRESULT CALLBACK SliderPopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<SliderPopupState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                DestroyWindow(h);
            }
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_HSCROLL:
            if (st && reinterpret_cast<HWND>(lp) == st->slider) {
                int pos = static_cast<int>(SendMessageW(st->slider, TBM_GETPOS, 0, 0));
                controls::SetValue(st->kind, pos);
                SetWindowTextW(st->label, SliderLabelText(st->kind, pos).c_str());
                widget::Refresh();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (st) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                int step = (delta > 0) ? 5 : -5;
                controls::Adjust(st->kind, step);
                int val = controls::GetValue(st->kind);
                SendMessageW(st->slider, TBM_SETPOS, TRUE, val);
                SetWindowTextW(st->label, SliderLabelText(st->kind, val).c_str());
                widget::Refresh();
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (st) {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, st->light ? RGB(20, 20, 20) : RGB(242, 242, 242));
                return reinterpret_cast<LRESULT>(st->bgBrush);
            }
            break;
        case WM_ERASEBKGND:
            if (st) {
                RECT rc{};
                GetClientRect(h, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, st->bgBrush);
                return 1;
            }
            break;
        case WM_DESTROY:
            if (st) {
                if (st->font) DeleteObject(st->font);
                if (st->bgBrush) DeleteObject(st->bgBrush);
                delete st;
                SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            }
            g_popup = nullptr;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ShowSliderPopup(int idx, RECT slotRect) {
    if (g_popup && IsWindow(g_popup)) {
        DestroyWindow(g_popup);
        g_popup = nullptr;
    }
    if (idx < 0 || idx >= 3) return;

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = SliderPopupProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kPopupClass;
        reg = RegisterClassExW(&wc) != 0;
    }

    UINT dpi = GetDpiForSystem();
    auto s = [&](int v) { return MulDiv(v, static_cast<int>(dpi ? dpi : 96), 96); };

    auto* st = new SliderPopupState();
    st->kind = static_cast<ControlKind>(idx);
    st->light = g_lightTaskbar;
    st->bgBrush = CreateSolidBrush(st->light ? RGB(245, 245, 245) : RGB(36, 36, 36));
    st->font = CreateFontW(-s(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int W = s(220);
    int H = s(82);

    POINT anchor{ (slotRect.left + slotRect.right) / 2, slotRect.top };
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    int x = anchor.x - W / 2;
    int y = slotRect.top - H - s(8);
    if (y < mi.rcWork.top) y = slotRect.bottom + s(8);
    if (x < mi.rcWork.left + s(6)) x = mi.rcWork.left + s(6);
    if (x + W > mi.rcWork.right - s(6)) x = mi.rcWork.right - W - s(6);

    g_popup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPopupClass, L"",
        WS_POPUP | WS_BORDER,
        x, y, W, H, g_hwnd, nullptr, g_inst, st);
    if (!g_popup) {
        DeleteObject(st->font);
        DeleteObject(st->bgBrush);
        delete st;
        return;
    }

    BOOL dark = st->light ? FALSE : TRUE;
    DwmSetWindowAttribute(g_popup, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    DWORD cornerPref = 2 /*DWMWCP_ROUND*/;
    DwmSetWindowAttribute(g_popup, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &cornerPref, sizeof(cornerPref));

    int curVal = controls::GetValue(st->kind);
    st->label = CreateWindowExW(0, L"STATIC", SliderLabelText(st->kind, curVal).c_str(),
                                WS_CHILD | WS_VISIBLE | SS_CENTER,
                                s(12), s(12), W - s(24), s(20), g_popup, nullptr, g_inst, nullptr);
    SendMessageW(st->label, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    st->slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH,
                                 s(12), s(38), W - s(24), s(30), g_popup, nullptr, g_inst, nullptr);
    SetWindowTheme(st->slider, st->light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
    int minRange = (st->kind == ControlKind::Brightness) ? -90 : 0;
    SendMessageW(st->slider, TBM_SETRANGEMIN, FALSE, minRange);
    SendMessageW(st->slider, TBM_SETRANGEMAX, TRUE, 100);
    SendMessageW(st->slider, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(st->slider, TBM_SETPOS, TRUE, curVal);

    ShowWindow(g_popup, SW_SHOW);
    SetForegroundWindow(g_popup);
    SetFocus(st->slider);
}

void EnsureWidget() {
    if (!widget::Alive()) {
        widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    } else if (!widget::Reposition()) {
        widget::Destroy();
    }
}

void ShowMenu() {
    if (g_popup && IsWindow(g_popup)) DestroyWindow(g_popup);
    bool ar = (g_lang == AppLang::Arabic);
    HMENU m = CreatePopupMenu();
    if (!m) return;

    if (g_updateInfo.available) {
        std::wstring updTxt = ar
            ? (L"تحديث جديد متاح (" + g_updateInfo.version + L") - تحميل...")
            : (L"Update available (" + g_updateInfo.version + L") - Download...");
        AppendMenuW(m, MF_STRING, ID_UPDATE_NOW, updTxt.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(m, MF_STRING, ID_RESET_WARMTH, ar ? L"إعادة ضبط صفار الشاشة (0%)" : L"Reset Eye Warmth (0%)");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    HMENU langMenu = CreatePopupMenu();
    AppendMenuW(langMenu, MF_STRING | (!ar ? MF_CHECKED : 0), ID_LANG_EN, L"English");
    AppendMenuW(langMenu, MF_STRING | (ar ? MF_CHECKED : 0), ID_LANG_AR, L"العربية");
    AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(langMenu), ar ? L"اللغة / Language" : L"Language / اللغة");

    AppendMenuW(m, MF_STRING | (g_widgetBackground ? MF_CHECKED : 0), ID_WIDGET_BACKGROUND,
                ar ? L"خلفية خلف النص (للشريط الشفاف)" : L"Background behind taskbar icons");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART,
                ar ? L"التشغيل مع ويندوز" : L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_CHECK_UPDATES, ar ? L"التحقق من التحديثات..." : L"Check for updates...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, ar ? L"خروج" : L"Exit");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case ID_UPDATE_NOW:
            if (!g_updateInfo.url.empty()) {
                ShellExecuteW(nullptr, L"open", g_updateInfo.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            break;
        case ID_CHECK_UPDATES:
            updater::CheckAsync(g_hwnd, WM_UPDATE_RESULT, L"HxH-3mk/Taskbar-Quick-Controls",
                                kSettingsKey, 1, 1, 0, 3, true);
            break;
        case ID_RESET_WARMTH:
            controls::SetValue(ControlKind::Warmth, 0);
            widget::Refresh();
            break;
        case ID_LANG_EN:
            g_lang = AppLang::English;
            controls::SaveLanguage(g_lang);
            widget::SetLanguage(g_lang);
            break;
        case ID_LANG_AR:
            g_lang = AppLang::Arabic;
            controls::SaveLanguage(g_lang);
            widget::SetLanguage(g_lang);
            break;
        case ID_WIDGET_BACKGROUND:
            g_widgetBackground = !g_widgetBackground;
            SaveBoolSetting(L"WidgetBackground", g_widgetBackground);
            widget::SetBackground(g_widgetBackground);
            break;
        case ID_AUTOSTART:
            SetAutostart(!AutostartEnabled());
            break;
        case ID_EXIT:
            DestroyWindow(g_hwnd);
            break;
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {
        widget::Destroy();
        EnsureWidget();
        return 0;
    }
    switch (msg) {
        case WM_WIDGET_CLICK:
            if (wp == 1) {
                ShowMenu();
            } else if (wp == 0) {
                int idx = static_cast<int>(lp);
                ShowSliderPopup(idx, widget::SlotScreenRect(widget::LastClickedWnd(), idx));
            }
            return 0;
        case WM_UPDATE_RESULT:
            if (wp == 1) {
                g_updateInfo = updater::LatestResult();
                widget::SetUpdateAvailable(g_updateInfo.version);
                if (lp == 1 && !g_updateInfo.url.empty()) {
                    bool ar = (g_lang == AppLang::Arabic);
                    std::wstring msgText = ar
                        ? (L"يتوفر إصدار جديد (" + g_updateInfo.version + L") على GitHub.\nهل تريد فتح صفحة التحميل؟")
                        : (L"A new version (" + g_updateInfo.version + L") is available on GitHub.\nOpen download page?");
                    if (MessageBoxW(nullptr, msgText.c_str(), L"Taskbar Quick Controls",
                                    MB_YESNO | MB_ICONINFORMATION | (ar ? (MB_RTLREADING | MB_RIGHT) : 0)) == IDYES) {
                        ShellExecuteW(nullptr, L"open", g_updateInfo.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
            } else if (lp == 1) {
                bool ar = (g_lang == AppLang::Arabic);
                MessageBoxW(nullptr, ar ? L"أنت تستخدم أحدث إصدار." : L"You are using the latest version.",
                            L"Taskbar Quick Controls", MB_OK | MB_ICONINFORMATION | (ar ? (MB_RTLREADING | MB_RIGHT) : 0));
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_REPOSITION) EnsureWidget();
            else if (wp == TIMER_UPDATE_CHECK) {
                updater::CheckAsync(g_hwnd, WM_UPDATE_RESULT, L"HxH-3mk/Taskbar-Quick-Controls",
                                    kSettingsKey, 1, 1, 0, 3, false);
            }
            return 0;
        case WM_DISPLAYCHANGE:
            controls::ReapplyDisplaySettings();
            EnsureWidget();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) {
                controls::ReapplyDisplaySettings();
                EnsureWidget();
            }
            return TRUE;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet")) {
                bool lightBar = ReadPersonalizeBool(L"SystemUsesLightTheme", false);
                bool lightApps = ReadPersonalizeBool(L"AppsUseLightTheme", false);
                g_lightApps = lightApps;
                if (lightBar != g_lightTaskbar) {
                    g_lightTaskbar = lightBar;
                    ApplyMenuTheme(lightBar);
                    widget::SetLightTheme(lightBar);
                }
            }
            return 0;
        case WM_ENDSESSION:
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_REPOSITION);
            if (g_popup && IsWindow(g_popup)) DestroyWindow(g_popup);
            widget::Destroy();
            controls::Shutdown();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {
        if (HWND w = FindWindowW(kWndClass, nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && FindWindowW(kWndClass, nullptr); i++) Sleep(100);
        }
        return 0;
    }

    g_inst = inst;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TaskbarQuickControls");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (!LoadBoolSetting(L"AutostartInitDone", false)) {
        SetAutostart(true);
        SaveBoolSetting(L"AutostartInitDone", true);
    }

    controls::Init();
    g_lang = controls::LoadLanguage();
    widget::SetLanguage(g_lang);

    g_lightTaskbar = ReadPersonalizeBool(L"SystemUsesLightTheme", false);
    g_lightApps = ReadPersonalizeBool(L"AppsUseLightTheme", false);
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWndClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) {
        controls::Shutdown();
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    g_widgetBackground = LoadBoolSetting(L"WidgetBackground", false);
    widget::SetBackground(g_widgetBackground);

    EnsureWidget();
    SetTimer(g_hwnd, TIMER_REPOSITION, 1000, nullptr);
    SetTimer(g_hwnd, TIMER_UPDATE_CHECK, 24 * 60 * 60 * 1000, nullptr);
    updater::CheckAsync(g_hwnd, WM_UPDATE_RESULT, L"HxH-3mk/Taskbar-Quick-Controls",
                        kSettingsKey, 1, 1, 0, 3, false);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
