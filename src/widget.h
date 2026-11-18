#pragma once
#include <windows.h>
#include <string>
#include "controls.h"

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme);
void Destroy();
bool Alive();
void Refresh();
void SetLanguage(AppLang lang);
void SetLightTheme(bool light);
void SetUpdateAvailable(const std::wstring& version);
void SetBackground(bool on);
bool Reposition();
HWND LastClickedWnd();
RECT SlotScreenRect(HWND widgetWnd, int idx);

}  // namespace widget
