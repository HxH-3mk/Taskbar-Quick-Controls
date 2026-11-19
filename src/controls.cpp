#include "controls.h"

#include <endpointvolume.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

const wchar_t* kSettingsKey = L"Software\\TaskbarQuickControls";
const wchar_t* kDimClass = L"QuickControlsDimOverlay";

struct MonitorEntry {
    HMONITOR hMon = nullptr;
    RECT rc{};
    bool isPrimary = false;
    std::wstring deviceName;
    int brightness = 80;
    HWND dimWnd = nullptr;
};

int g_values[3] = { 50, 80, 0 };  // Unified Volume, Brightness (-90..100), Warmth
bool g_muted = false;
bool g_perMonitor = false;
bool g_reorderMode = false;
std::vector<MonitorEntry> g_monitors;
std::vector<std::wstring> g_savedOrder;
HWND g_unifiedDimWnd = nullptr;

bool ReadRegDword(const wchar_t* name, int& out) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) {
        out = static_cast<int>(v);
        return true;
    }
    return false;
}

void WriteRegDword(const wchar_t* name, int val) {
    DWORD v = static_cast<DWORD>(val);
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

std::wstring ReadRegString(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
        return {};
    std::wstring out(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS)
        return {};
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

void WriteRegString(const wchar_t* name, const std::wstring& val) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_SZ, val.c_str(),
                    static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
}

std::wstring SlotKey(const SlotDef& s) {
    if (s.kind == ControlKind::Volume) return L"vol";
    if (s.kind == ControlKind::Warmth) return L"warm";
    if (s.monIdx >= 0) return L"bri_" + std::to_wstring(s.monIdx);
    return L"bri";
}

void LoadSavedOrder() {
    g_savedOrder.clear();
    std::wstring raw = ReadRegString(L"IconOrder");
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t comma = raw.find(L',', pos);
        if (comma == std::wstring::npos) comma = raw.size();
        std::wstring tok = raw.substr(pos, comma - pos);
        if (!tok.empty()) g_savedOrder.push_back(tok);
        pos = comma + 1;
    }
}

void SaveSlotOrder(const std::vector<SlotDef>& slots) {
    g_savedOrder.clear();
    std::wstring joined;
    for (size_t i = 0; i < slots.size(); i++) {
        std::wstring k = SlotKey(slots[i]);
        g_savedOrder.push_back(k);
        if (i > 0) joined.push_back(L',');
        joined += k;
    }
    WriteRegString(L"IconOrder", joined);
}

const wchar_t* RegName(ControlKind kind) {
    switch (kind) {
        case ControlKind::Volume:     return L"SavedVolume";
        case ControlKind::Brightness: return L"SavedBrightness";
        case ControlKind::Warmth:     return L"SavedWarmth";
        default:                      return L"";
    }
}

std::wstring MonitorBrightnessRegName(int idx) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"SavedBrightness_%d", idx);
    return buf;
}

// ---- Volume (Core Audio) ----------------------------------------------------------------------

IAudioEndpointVolume* GetAudioEndpoint() {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) || !en) {
        return nullptr;
    }
    IMMDevice* dev = nullptr;
    IAudioEndpointVolume* ep = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
        dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ep));
        dev->Release();
    }
    en->Release();
    return ep;
}

bool ReadSystemVolume(int& outPct, bool& outMuted) {
    IAudioEndpointVolume* ep = GetAudioEndpoint();
    if (!ep) return false;
    float scalar = 0.0f;
    BOOL muted = FALSE;
    bool ok = SUCCEEDED(ep->GetMasterVolumeLevelScalar(&scalar));
    if (SUCCEEDED(ep->GetMute(&muted))) outMuted = (muted != FALSE);
    ep->Release();
    if (ok) outPct = std::clamp(static_cast<int>(std::llround(scalar * 100.0f)), 0, 100);
    return ok;
}

bool WriteSystemVolume(int pct) {
    IAudioEndpointVolume* ep = GetAudioEndpoint();
    if (!ep) return false;
    pct = std::clamp(pct, 0, 100);
    bool ok = SUCCEEDED(ep->SetMasterVolumeLevelScalar(static_cast<float>(pct) / 100.0f, nullptr));
    ep->SetMute(pct == 0 ? TRUE : FALSE, nullptr);
    g_muted = (pct == 0);
    ep->Release();
    return ok;
}

// ---- Brightness (DDC/CI per-HMONITOR + WMI fallback) ------------------------------------------

bool ReadDdcMonitorBrightness(HMONITOR hMon, int& outPct) {
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) return false;
    std::vector<PHYSICAL_MONITOR> mons(count);
    bool ok = false;
    if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons.data())) {
        for (DWORD i = 0; i < count; i++) {
            DWORD minB = 0, curB = 0, maxB = 0;
            if (GetMonitorBrightness(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                outPct = std::clamp(static_cast<int>(std::llround(100.0 * (curB - minB) / (maxB - minB))), 0, 100);
                ok = true;
                break;
            }
        }
        DestroyPhysicalMonitors(count, mons.data());
    }
    return ok;
}

bool WriteDdcMonitorBrightness(HMONITOR hMon, int pct) {
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) return false;
    std::vector<PHYSICAL_MONITOR> mons(count);
    bool ok = false;
    if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons.data())) {
        for (DWORD i = 0; i < count; i++) {
            DWORD minB = 0, curB = 0, maxB = 0;
            if (GetMonitorBrightness(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                DWORD target = minB + static_cast<DWORD>(std::llround((maxB - minB) * (pct / 100.0)));
                if (SetMonitorBrightness(mons[i].hPhysicalMonitor, target)) ok = true;
            }
        }
        DestroyPhysicalMonitors(count, mons.data());
    }
    return ok;
}

IWbemServices* ConnectWmi() {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void**>(&loc))) || !loc) {
        return nullptr;
    }
    IWbemServices* svc = nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    if (ns) {
        if (SUCCEEDED(loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc)) && svc) {
            CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                              RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        }
        SysFreeString(ns);
    }
    loc->Release();
    return svc;
}

bool ReadWmiBrightness(int& outPct) {
    IWbemServices* svc = ConnectWmi();
    if (!svc) return false;
    bool ok = false;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT CurrentBrightness FROM WmiMonitorBrightness");
    IEnumWbemClassObject* en = nullptr;
    if (lang && query && SUCCEEDED(svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)) && en) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        if (SUCCEEDED(en->Next(WBEM_INFINITE, 1, &obj, &ret)) && ret == 1 && obj) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
                if (v.vt == VT_UI1) { outPct = std::clamp(static_cast<int>(v.bVal), 0, 100); ok = true; }
                else if (v.vt == VT_I4) { outPct = std::clamp(static_cast<int>(v.lVal), 0, 100); ok = true; }
            }
            VariantClear(&v);
            obj->Release();
        }
        en->Release();
    }
    if (lang) SysFreeString(lang);
    if (query) SysFreeString(query);
    svc->Release();
    return ok;
}

bool WriteWmiBrightness(int pct) {
    IWbemServices* svc = ConnectWmi();
    if (!svc) return false;
    bool ok = false;
    BSTR clsName = SysAllocString(L"WmiMonitorBrightnessMethods");
    BSTR methName = SysAllocString(L"WmiSetBrightness");
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM WmiMonitorBrightnessMethods");
    IWbemClassObject* cls = nullptr;
    IEnumWbemClassObject* en = nullptr;
    if (clsName && methName && lang && query &&
        SUCCEEDED(svc->GetObject(clsName, 0, nullptr, &cls, nullptr)) && cls &&
        SUCCEEDED(svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en)) && en) {
        IWbemClassObject* inSig = nullptr;
        if (SUCCEEDED(cls->GetMethod(methName, 0, &inSig, nullptr)) && inSig) {
            IWbemClassObject* inst = nullptr;
            ULONG ret = 0;
            while (SUCCEEDED(en->Next(WBEM_INFINITE, 1, &inst, &ret)) && ret == 1 && inst) {
                VARIANT path;
                VariantInit(&path);
                if (SUCCEEDED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR) {
                    IWbemClassObject* inParams = nullptr;
                    if (SUCCEEDED(inSig->SpawnInstance(0, &inParams)) && inParams) {
                        VARIANT vTimeout, vBright;
                        VariantInit(&vTimeout);
                        VariantInit(&vBright);
                        vTimeout.vt = VT_I4;
                        vTimeout.lVal = 1;
                        vBright.vt = VT_UI1;
                        vBright.bVal = static_cast<BYTE>(std::clamp(pct, 0, 100));
                        inParams->Put(L"Timeout", 0, &vTimeout, 0);
                        inParams->Put(L"Brightness", 0, &vBright, 0);
                        if (SUCCEEDED(svc->ExecMethod(path.bstrVal, methName, 0, nullptr, inParams, nullptr, nullptr))) {
                            ok = true;
                        }
                        inParams->Release();
                    }
                }
                VariantClear(&path);
                inst->Release();
            }
            inSig->Release();
        }
    }
    if (en) en->Release();
    if (cls) cls->Release();
    if (clsName) SysFreeString(clsName);
    if (methName) SysFreeString(methName);
    if (lang) SysFreeString(lang);
    if (query) SysFreeString(query);
    svc->Release();
    return ok;
}

// ---- Warmth / Eye Protection (Gamma Ramp) -----------------------------------------------------

struct GammaCtx {
    WORD ramp[3][256];
};

BOOL CALLBACK EnumGammaProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<GammaCtx*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        HDC dc = CreateDCW(L"DISPLAY", mi.szDevice, nullptr, nullptr);
        if (dc) {
            SetDeviceGammaRamp(dc, ctx->ramp);
            DeleteDC(dc);
        }
    }
    return TRUE;
}

void ApplyWarmthGamma(int pct) {
    double t = std::clamp(pct, 0, 100) / 100.0;
    double rScale = 1.0;
    double gScale = 1.0 - 0.24 * t;
    double bScale = 1.0 - 0.62 * t;

    GammaCtx ctx{};
    for (int i = 0; i < 256; i++) {
        double base = i * 257.0;
        ctx.ramp[0][i] = static_cast<WORD>(std::clamp(static_cast<int>(std::llround(base * rScale)), 0, 65535));
        ctx.ramp[1][i] = static_cast<WORD>(std::clamp(static_cast<int>(std::llround(base * gScale)), 0, 65535));
        ctx.ramp[2][i] = static_cast<WORD>(std::clamp(static_cast<int>(std::llround(base * bScale)), 0, 65535));
    }
    EnumDisplayMonitors(nullptr, nullptr, EnumGammaProc, reinterpret_cast<LPARAM>(&ctx));
}

// ---- Dimming Overlay (per-monitor or unified) -------------------------------------------------

LRESULT CALLBACK DimOverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

void EnsureDimClass() {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DimOverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kDimClass;
        reg = RegisterClassExW(&wc) != 0;
    }
}

void ApplyOverlayToRect(HWND& wnd, const RECT& rc, int brightnessPct) {
    if (brightnessPct >= 0) {
        if (wnd && IsWindow(wnd)) ShowWindow(wnd, SW_HIDE);
        return;
    }
    int dimPct = std::clamp(-brightnessPct, 1, 90);
    BYTE alpha = static_cast<BYTE>(MulDiv(dimPct, 255, 100));
    EnsureDimClass();

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (!wnd || !IsWindow(wnd)) {
        wnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kDimClass, L"", WS_POPUP,
            rc.left, rc.top, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    } else {
        SetWindowPos(wnd, HWND_TOPMOST, rc.left, rc.top, w, h, SWP_NOACTIVATE);
    }

    if (wnd) {
        SetLayeredWindowAttributes(wnd, 0, alpha, LWA_ALPHA);
        ShowWindow(wnd, SW_SHOWNA);
    }
}

void HideAllDimOverlays() {
    if (g_unifiedDimWnd && IsWindow(g_unifiedDimWnd)) ShowWindow(g_unifiedDimWnd, SW_HIDE);
    for (auto& m : g_monitors) {
        if (m.dimWnd && IsWindow(m.dimWnd)) ShowWindow(m.dimWnd, SW_HIDE);
    }
}

// ---- Monitor Enumeration ----------------------------------------------------------------------

BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<MonitorEntry>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorEntry e;
        e.hMon = hMon;
        e.rc = mi.rcMonitor;
        e.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        e.deviceName = mi.szDevice;
        out->push_back(std::move(e));
    }
    return TRUE;
}

void RefreshMonitors() {
    std::vector<MonitorEntry> next;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, reinterpret_cast<LPARAM>(&next));
    std::sort(next.begin(), next.end(), [](const MonitorEntry& a, const MonitorEntry& b) {
        if (a.isPrimary != b.isPrimary) return a.isPrimary > b.isPrimary;
        if (a.rc.left != b.rc.left) return a.rc.left < b.rc.left;
        return a.rc.top < b.rc.top;
    });

    for (size_t i = 0; i < next.size(); i++) {
        // Preserve existing dimWnd & brightness if same deviceName, else load from Registry.
        bool matched = false;
        for (auto& old : g_monitors) {
            if (old.deviceName == next[i].deviceName) {
                next[i].dimWnd = old.dimWnd;
                next[i].brightness = old.brightness;
                old.dimWnd = nullptr;
                matched = true;
                break;
            }
        }
        if (!matched) {
            int saved = g_values[1];
            if (ReadRegDword(MonitorBrightnessRegName(static_cast<int>(i)).c_str(), saved)) {
                next[i].brightness = std::clamp(saved, -90, 100);
            } else {
                int hw = 80;
                if (ReadDdcMonitorBrightness(next[i].hMon, hw)) next[i].brightness = hw;
                else if (next[i].isPrimary && ReadWmiBrightness(hw)) next[i].brightness = hw;
                else next[i].brightness = g_values[1];
            }
        }
    }

    for (auto& old : g_monitors) {
        if (old.dimWnd && IsWindow(old.dimWnd)) DestroyWindow(old.dimWnd);
    }
    g_monitors = std::move(next);
}

void ApplyMonitorHardwareAndOverlay(MonitorEntry& m, int pct) {
    int hw = std::max(0, pct);
    bool wroteDdc = WriteDdcMonitorBrightness(m.hMon, hw);
    if (!wroteDdc && m.isPrimary) {
        WriteWmiBrightness(hw);
    }
    ApplyOverlayToRect(m.dimWnd, m.rc, pct);
}

void ApplyUnifiedBrightness(int pct) {
    int hw = std::max(0, pct);
    if (!WriteWmiBrightness(hw)) {
        for (auto& m : g_monitors) WriteDdcMonitorBrightness(m.hMon, hw);
    } else {
        for (auto& m : g_monitors) {
            if (!m.isPrimary) WriteDdcMonitorBrightness(m.hMon, hw);
        }
    }
    for (auto& m : g_monitors) {
        if (m.dimWnd && IsWindow(m.dimWnd)) ShowWindow(m.dimWnd, SW_HIDE);
    }
    RECT virt{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };
    ApplyOverlayToRect(g_unifiedDimWnd, virt, pct);
}

}  // namespace

namespace controls {

AppLang LoadLanguage() {
    int v = 0;
    if (ReadRegDword(L"Language", v)) {
        return (v == static_cast<int>(AppLang::Arabic)) ? AppLang::Arabic : AppLang::English;
    }
    return (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ARABIC) ? AppLang::Arabic : AppLang::English;
}

void SaveLanguage(AppLang lang) {
    WriteRegDword(L"Language", static_cast<int>(lang));
}

bool IsPerMonitorBrightness() { return g_perMonitor; }

void SetPerMonitorBrightness(bool on) {
    g_perMonitor = on;
    WriteRegDword(L"PerMonitorBrightness", on ? 1 : 0);
    RefreshMonitors();
    HideAllDimOverlays();
    ReapplyDisplaySettings();
}

int MonitorCount() {
    return g_monitors.empty() ? 1 : static_cast<int>(g_monitors.size());
}

std::wstring MonitorLabel(int monIdx, AppLang lang) {
    bool ar = (lang == AppLang::Arabic);
    wchar_t buf[64]{};
    swprintf_s(buf, ar ? L"الشاشة %d" : L"Display %d", monIdx + 1);
    return buf;
}

bool IsReorderMode() { return g_reorderMode; }
void SetReorderMode(bool on) { g_reorderMode = on; }

std::vector<SlotDef> GetSlots() {
    std::vector<SlotDef> active;
    active.push_back({ ControlKind::Volume, -1 });
    if (g_perMonitor) {
        int mc = MonitorCount();
        for (int i = 0; i < mc; i++) active.push_back({ ControlKind::Brightness, i });
    } else {
        active.push_back({ ControlKind::Brightness, -1 });
    }
    active.push_back({ ControlKind::Warmth, -1 });

    if (g_savedOrder.empty()) return active;

    std::vector<SlotDef> ordered;
    std::vector<bool> used(active.size(), false);
    for (const auto& key : g_savedOrder) {
        for (size_t i = 0; i < active.size(); i++) {
            if (!used[i] && SlotKey(active[i]) == key) {
                ordered.push_back(active[i]);
                used[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < active.size(); i++) {
        if (!used[i]) ordered.push_back(active[i]);
    }
    return ordered;
}

void MoveSlot(int fromIdx, int toIdx) {
    auto slots = GetSlots();
    int n = static_cast<int>(slots.size());
    if (fromIdx < 0 || fromIdx >= n || toIdx < 0 || toIdx >= n || fromIdx == toIdx) return;
    SlotDef item = slots[fromIdx];
    slots.erase(slots.begin() + fromIdx);
    slots.insert(slots.begin() + toIdx, item);
    SaveSlotOrder(slots);
}

void Init() {
    LoadSavedOrder();
    int pm = 0;
    if (ReadRegDword(L"PerMonitorBrightness", pm)) g_perMonitor = (pm != 0);

    int v = 0;
    if (ReadRegDword(RegName(ControlKind::Volume), v)) {
        g_values[0] = std::clamp(v, 0, 100);
        WriteSystemVolume(g_values[0]);
    } else if (ReadSystemVolume(v, g_muted)) {
        g_values[0] = v;
        WriteRegDword(RegName(ControlKind::Volume), v);
    }

    if (ReadRegDword(RegName(ControlKind::Brightness), v)) {
        g_values[1] = std::clamp(v, -90, 100);
    } else {
        int hw = 80;
        if (ReadWmiBrightness(hw)) g_values[1] = hw;
        WriteRegDword(RegName(ControlKind::Brightness), g_values[1]);
    }

    RefreshMonitors();
    if (g_perMonitor) {
        for (auto& m : g_monitors) ApplyMonitorHardwareAndOverlay(m, m.brightness);
    } else {
        ApplyUnifiedBrightness(g_values[1]);
    }

    if (ReadRegDword(RegName(ControlKind::Warmth), v)) {
        g_values[2] = std::clamp(v, 0, 100);
    } else {
        g_values[2] = 0;
        WriteRegDword(RegName(ControlKind::Warmth), 0);
    }
    ApplyWarmthGamma(g_values[2]);
}

void Shutdown() {
    if (g_unifiedDimWnd && IsWindow(g_unifiedDimWnd)) {
        DestroyWindow(g_unifiedDimWnd);
        g_unifiedDimWnd = nullptr;
    }
    for (auto& m : g_monitors) {
        if (m.dimWnd && IsWindow(m.dimWnd)) {
            DestroyWindow(m.dimWnd);
            m.dimWnd = nullptr;
        }
    }
    ApplyWarmthGamma(0);
}

int GetValue(ControlKind kind, int monIdx) {
    if (kind == ControlKind::Brightness && g_perMonitor && monIdx >= 0 && monIdx < static_cast<int>(g_monitors.size())) {
        return g_monitors[monIdx].brightness;
    }
    int idx = static_cast<int>(kind);
    if (kind == ControlKind::Volume) {
        int sysV = g_values[0];
        bool muted = false;
        if (ReadSystemVolume(sysV, muted)) {
            g_muted = muted;
            if (sysV != g_values[0]) {
                g_values[0] = sysV;
                WriteRegDword(RegName(ControlKind::Volume), sysV);
            }
        }
    }
    return g_values[idx];
}

bool IsMuted() { return g_muted; }

void SetValue(ControlKind kind, int pct, int monIdx) {
    int minVal = (kind == ControlKind::Brightness) ? -90 : 0;
    pct = std::clamp(pct, minVal, 100);

    if (kind == ControlKind::Brightness && g_perMonitor && monIdx >= 0 && monIdx < static_cast<int>(g_monitors.size())) {
        g_monitors[monIdx].brightness = pct;
        WriteRegDword(MonitorBrightnessRegName(monIdx).c_str(), pct);
        ApplyMonitorHardwareAndOverlay(g_monitors[monIdx], pct);
        return;
    }

    int idx = static_cast<int>(kind);
    g_values[idx] = pct;
    WriteRegDword(RegName(kind), pct);

    switch (kind) {
        case ControlKind::Volume:
            WriteSystemVolume(pct);
            break;
        case ControlKind::Brightness:
            ApplyUnifiedBrightness(pct);
            break;
        case ControlKind::Warmth:
            ApplyWarmthGamma(pct);
            break;
        default:
            break;
    }
}

void Adjust(ControlKind kind, int delta, int monIdx) {
    int cur = GetValue(kind, monIdx);
    SetValue(kind, cur + delta, monIdx);
}

void ReapplyDisplaySettings() {
    RefreshMonitors();
    ApplyWarmthGamma(g_values[static_cast<int>(ControlKind::Warmth)]);
    if (g_perMonitor) {
        if (g_unifiedDimWnd && IsWindow(g_unifiedDimWnd)) ShowWindow(g_unifiedDimWnd, SW_HIDE);
        for (auto& m : g_monitors) ApplyMonitorHardwareAndOverlay(m, m.brightness);
    } else {
        ApplyUnifiedBrightness(g_values[1]);
    }
}

}  // namespace controls
