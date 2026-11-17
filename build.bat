@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>&1
if errorlevel 1 (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)

if not exist build mkdir build

rc /nologo /I res /fo build\app.res res\app.rc || exit /b 1

cl /nologo /std:c++17 /O2 /MT /utf-8 /EHsc /W4 ^
   /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 ^
   /Fo:build\ /Fe:QuickControls.exe ^
   src\main.cpp src\widget.cpp src\controls.cpp build\app.res ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib ole32.lib oleaut32.lib advapi32.lib dxva2.lib wbemuuid.lib dwmapi.lib uxtheme.lib || exit /b 1

echo Built QuickControls.exe
