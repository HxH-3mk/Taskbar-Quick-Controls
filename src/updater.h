#pragma once
#include <windows.h>
#include <string>

namespace updater {

struct UpdateInfo {
    bool available = false;
    std::wstring version;
    std::wstring url;
};

// Checks GitHub Releases in the background if at least `intervalDays` have passed since the last check
// (or immediately when `force == true`). Posts `notifyMsg` to `notifyWnd` on completion with
// `wParam == 1` if an update is available, or `0` otherwise.
void CheckAsync(HWND notifyWnd, UINT notifyMsg, const wchar_t* repoSlug,
                const wchar_t* regKey, int curMajor, int curMinor, int curPatch,
                int intervalDays = 3, bool force = false);

UpdateInfo LatestResult();

}  // namespace updater
