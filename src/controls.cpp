#include "controls.h"

#include <endpointvolume.h>
#include <highlevelmonitorconfigurationapi.h>
#include <mmdeviceapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

const wchar_t* kSettingsKey = L"Software\\TaskbarQuickControls";
const wchar_t* kDimClass = L"QuickControlsDimOverlay";

int g_values[3] = { 50, 80, 0 };  // Volume, Brightness (-90..100), Warmth
bool g_muted = false;
HWND g_dimWnd = nullptr;

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

const wchar_t* RegName(ControlKind kind) {
    switch (kind) {
        case ControlKind::Volume:     return L"SavedVolume";
        case ControlKind::Brightness: return L"SavedBrightness";
        case ControlKind::Warmth:     return L"SavedWarmth";
        default:                      return L"";
    }
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

// ---- Brightness (DDC/CI + WMI fallback) -------------------------------------------------------

struct DdcReadCtx {
    bool found = false;
    int pct = 80;
};

BOOL CALLBACK EnumDdcReadProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<DdcReadCtx*>(lp);
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) return TRUE;
    std::vector<PHYSICAL_MONITOR> mons(count);
    if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons.data())) {
        for (DWORD i = 0; i < count; i++) {
            DWORD minB = 0, curB = 0, maxB = 0;
            if (GetMonitorBrightness(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                ctx->pct = std::clamp(static_cast<int>(std::llround(100.0 * (curB - minB) / (maxB - minB))), 0, 100);
                ctx->found = true;
                break;
            }
        }
        DestroyPhysicalMonitors(count, mons.data());
    }
    return !ctx->found;
}

BOOL CALLBACK EnumDdcWriteProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* ctx = reinterpret_cast<DdcReadCtx*>(lp);
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &count) || count == 0) return TRUE;
    std::vector<PHYSICAL_MONITOR> mons(count);
    if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons.data())) {
        for (DWORD i = 0; i < count; i++) {
            DWORD minB = 0, curB = 0, maxB = 0;
            if (GetMonitorBrightness(mons[i].hPhysicalMonitor, &minB, &curB, &maxB) && maxB > minB) {
                DWORD target = minB + static_cast<DWORD>(std::llround((maxB - minB) * (ctx->pct / 100.0)));
                if (SetMonitorBrightness(mons[i].hPhysicalMonitor, target)) ctx->found = true;
            }
        }
        DestroyPhysicalMonitors(count, mons.data());
    }
    return TRUE;
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

bool ReadSystemBrightness(int& outPct) {
    if (ReadWmiBrightness(outPct)) return true;
    DdcReadCtx ctx{};
    EnumDisplayMonitors(nullptr, nullptr, EnumDdcReadProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) { outPct = ctx.pct; return true; }
    return false;
}

bool WriteSystemBrightness(int pct) {
    pct = std::clamp(pct, 0, 100);
    if (WriteWmiBrightness(pct)) return true;
    DdcReadCtx ctx{ false, pct };
    EnumDisplayMonitors(nullptr, nullptr, EnumDdcWriteProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
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

// ---- Multi-Monitor Dimming Overlay (for negative brightness -90..-1) --------------------------

LRESULT CALLBACK DimOverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, msg, wp, lp);
}

void ApplyDimOverlay(int brightnessPct) {
    if (brightnessPct >= 0) {
        if (g_dimWnd && IsWindow(g_dimWnd)) {
            ShowWindow(g_dimWnd, SW_HIDE);
        }
        return;
    }

    int dimPct = std::clamp(-brightnessPct, 1, 90);
    BYTE alpha = static_cast<BYTE>(MulDiv(dimPct, 255, 100));

    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = DimOverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kDimClass;
        reg = RegisterClassExW(&wc) != 0;
    }

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (!g_dimWnd || !IsWindow(g_dimWnd)) {
        g_dimWnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kDimClass, L"", WS_POPUP,
            vx, vy, vw, vh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    } else {
        SetWindowPos(g_dimWnd, HWND_TOPMOST, vx, vy, vw, vh, SWP_NOACTIVATE);
    }

    if (g_dimWnd) {
        SetLayeredWindowAttributes(g_dimWnd, 0, alpha, LWA_ALPHA);
        ShowWindow(g_dimWnd, SW_SHOWNA);
    }
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

void Init() {
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
        WriteSystemBrightness(std::max(0, g_values[1]));
        ApplyDimOverlay(g_values[1]);
    } else if (ReadSystemBrightness(v)) {
        g_values[1] = v;
        WriteRegDword(RegName(ControlKind::Brightness), v);
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
    // Restore neutral screen colors and remove dimming overlay on explicit exit.
    if (g_dimWnd && IsWindow(g_dimWnd)) {
        DestroyWindow(g_dimWnd);
        g_dimWnd = nullptr;
    }
    ApplyWarmthGamma(0);
}

int GetValue(ControlKind kind) {
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

void SetValue(ControlKind kind, int pct) {
    int idx = static_cast<int>(kind);
    int minVal = (kind == ControlKind::Brightness) ? -90 : 0;
    pct = std::clamp(pct, minVal, 100);
    g_values[idx] = pct;
    WriteRegDword(RegName(kind), pct);

    switch (kind) {
        case ControlKind::Volume:
            WriteSystemVolume(pct);
            break;
        case ControlKind::Brightness:
            WriteSystemBrightness(std::max(0, pct));
            ApplyDimOverlay(pct);
            break;
        case ControlKind::Warmth:
            ApplyWarmthGamma(pct);
            break;
        default:
            break;
    }
}

void Adjust(ControlKind kind, int delta) {
    int cur = GetValue(kind);
    SetValue(kind, cur + delta);
}

void ReapplyDisplaySettings() {
    ApplyWarmthGamma(g_values[static_cast<int>(ControlKind::Warmth)]);
    ApplyDimOverlay(g_values[static_cast<int>(ControlKind::Brightness)]);
}

}  // namespace controls
