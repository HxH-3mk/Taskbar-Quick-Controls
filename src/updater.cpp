#include "updater.h"

#include <winhttp.h>
#include <cstdio>
#include <string>

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
updater::UpdateInfo g_latest;
bool g_running = false;

struct CheckParams {
    HWND notifyWnd;
    UINT notifyMsg;
    std::wstring repoSlug;
    std::wstring regKey;
    int curMajor, curMinor, curPatch;
    int intervalDays;
    bool force;
};

ULONGLONG NowSeconds() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER ui{ ft.dwLowDateTime, ft.dwHighDateTime };
    return ui.QuadPart / 10000000ULL;
}

ULONGLONG ReadRegQword(const wchar_t* key, const wchar_t* name) {
    ULONGLONG v = 0;
    DWORD sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_QWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v;
    return 0;
}

void WriteRegQword(const wchar_t* key, const wchar_t* name, ULONGLONG val) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_QWORD, &val, sizeof(val));
}

bool ExtractJsonField(const std::string& json, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

bool ParseSemver(const std::string& tag, int& maj, int& min, int& pat) {
    const char* s = tag.c_str();
    if (*s == 'v' || *s == 'V') s++;
    return sscanf_s(s, "%d.%d.%d", &maj, &min, &pat) >= 2;
}

bool IsNewer(int maj, int min, int pat, int curMaj, int curMin, int curPat) {
    if (maj != curMaj) return maj > curMaj;
    if (min != curMin) return min > curMin;
    return pat > curPat;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), ws.data(), n);
    return ws;
}

DWORD WINAPI CheckThread(LPVOID lp) {
    auto* p = static_cast<CheckParams*>(lp);
    bool foundUpdate = false;

    ULONGLONG now = NowSeconds();
    ULONGLONG last = ReadRegQword(p->regKey.c_str(), L"LastUpdateCheck");
    ULONGLONG minInterval = static_cast<ULONGLONG>(p->intervalDays) * 86400ULL;

    if (p->force || last == 0 || (now > last && (now - last) >= minInterval)) {
        HINTERNET hSess = WinHttpOpen(L"HxH3mk-Updater/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSess) {
            WinHttpSetTimeouts(hSess, 4000, 4000, 4000, 6000);
            HINTERNET hConn = WinHttpConnect(hSess, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
            if (hConn) {
                std::wstring path = L"/repos/" + p->repoSlug + L"/releases/latest";
                HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
                if (hReq) {
                    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        WinHttpReceiveResponse(hReq, nullptr)) {
                        DWORD status = 0, ssz = sizeof(status);
                        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &ssz, WINHTTP_NO_HEADER_INDEX);
                        if (status == 200) {
                            std::string body;
                            DWORD avail = 0;
                            while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                                std::string chunk(avail, '\0');
                                DWORD read = 0;
                                if (WinHttpReadData(hReq, chunk.data(), avail, &read) && read > 0) {
                                    chunk.resize(read);
                                    body += chunk;
                                    if (body.size() > 128 * 1024) break;
                                } else break;
                            }
                            WriteRegQword(p->regKey.c_str(), L"LastUpdateCheck", now);
                            std::string tag, htmlUrl;
                            if (ExtractJsonField(body, "tag_name", tag) && ExtractJsonField(body, "html_url", htmlUrl)) {
                                int maj = 0, min = 0, pat = 0;
                                if (ParseSemver(tag, maj, min, pat) && IsNewer(maj, min, pat, p->curMajor, p->curMinor, p->curPatch)) {
                                    AcquireSRWLockExclusive(&g_lock);
                                    g_latest.available = true;
                                    g_latest.version = Utf8ToWide(tag);
                                    g_latest.url = Utf8ToWide(htmlUrl);
                                    ReleaseSRWLockExclusive(&g_lock);
                                    foundUpdate = true;
                                }
                            }
                        }
                    }
                    WinHttpCloseHandle(hReq);
                }
                WinHttpCloseHandle(hConn);
            }
            WinHttpCloseHandle(hSess);
        }
    }

    if (p->notifyWnd && IsWindow(p->notifyWnd)) {
        PostMessageW(p->notifyWnd, p->notifyMsg, foundUpdate ? 1 : 0, p->force ? 1 : 0);
    }

    AcquireSRWLockExclusive(&g_lock);
    g_running = false;
    ReleaseSRWLockExclusive(&g_lock);
    delete p;
    return 0;
}

}  // namespace

namespace updater {

void CheckAsync(HWND notifyWnd, UINT notifyMsg, const wchar_t* repoSlug,
                const wchar_t* regKey, int curMajor, int curMinor, int curPatch,
                int intervalDays, bool force) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_running) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    g_running = true;
    ReleaseSRWLockExclusive(&g_lock);

    auto* p = new CheckParams{
        notifyWnd, notifyMsg, repoSlug, regKey, curMajor, curMinor, curPatch, intervalDays, force
    };
    HANDLE t = CreateThread(nullptr, 0, CheckThread, p, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        AcquireSRWLockExclusive(&g_lock);
        g_running = false;
        ReleaseSRWLockExclusive(&g_lock);
        delete p;
    }
}

UpdateInfo LatestResult() {
    AcquireSRWLockShared(&g_lock);
    UpdateInfo copy = g_latest;
    ReleaseSRWLockShared(&g_lock);
    return copy;
}

}  // namespace updater
