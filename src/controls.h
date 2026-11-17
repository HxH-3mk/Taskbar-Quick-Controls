#pragma once
#include <windows.h>

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

int GetValue(ControlKind kind);
bool IsMuted();
void Adjust(ControlKind kind, int delta);
void SetValue(ControlKind kind, int pct);
void ReapplyDisplaySettings();

}  // namespace controls
