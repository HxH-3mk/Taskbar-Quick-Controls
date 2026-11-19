#pragma once
#include <windows.h>
#include <string>

enum class ControlKind : int {
    Volume = 0,
    Brightness = 1,
    Warmth = 2,
    Count = 3
};

enum class AppLang : int {
    English = 0,
    Arabic = 1
};

namespace controls {

void Init();
void Shutdown();

AppLang LoadLanguage();
void SaveLanguage(AppLang lang);

bool IsPerMonitorBrightness();
void SetPerMonitorBrightness(bool on);
int MonitorCount();
std::wstring MonitorLabel(int monIdx, AppLang lang);

int GetValue(ControlKind kind, int monIdx = -1);
bool IsMuted();
void Adjust(ControlKind kind, int delta, int monIdx = -1);
void SetValue(ControlKind kind, int pct, int monIdx = -1);
void ReapplyDisplaySettings();

}  // namespace controls
