#pragma once
#include <windows.h>
#include <string>
#include <vector>

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

struct SlotDef {
    ControlKind kind = ControlKind::Volume;
    int monIdx = -1;
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

bool IsReorderMode();
void SetReorderMode(bool on);
std::vector<SlotDef> GetSlots();
void MoveSlot(int fromIdx, int toIdx);

int GetValue(ControlKind kind, int monIdx = -1);
bool IsMuted();
void Adjust(ControlKind kind, int delta, int monIdx = -1);
void SetValue(ControlKind kind, int pct, int monIdx = -1);
void ReapplyDisplaySettings();

}  // namespace controls
