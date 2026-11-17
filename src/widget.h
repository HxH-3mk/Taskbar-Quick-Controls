#pragma once
#include <windows.h>
#include "controls.h"

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme);
void Destroy();
bool Alive();
void Refresh();
void SetLanguage(AppLang lang);
void SetLightTheme(bool light);
void SetBackground(bool on);
bool Reposition();
RECT SlotScreenRect(int idx);

}  // namespace widget
