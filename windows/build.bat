@echo off
setlocal enabledelayedexpansion
rem Сборка ygg_control.exe (панель управления Yggdrasil) и запуск.
rem   build.bat        - собрать, если исходники новее exe, и открыть панель
rem   build.bat force  - пересобрать в любом случае

set "ROOT=%~dp0"
set "EXE=%ROOT%ygg_control.exe"
pushd "%ROOT%src"

set "NEED_BUILD=1"
if exist "%EXE%" if /i not "%~1"=="force" (
    set "NEED_BUILD=0"
    for /f %%a in ('powershell -NoProfile -Command "$e=(Get-Item '%EXE%').LastWriteTime; [bool](Get-ChildItem '*.cpp','*.h','*.rc','*.png','*.ico' | Where-Object { $_.LastWriteTime -gt $e })"') do set "SRC_NEWER=%%a"
    if /i "!SRC_NEWER!"=="True" set "NEED_BUILD=1"
)
if "%NEED_BUILD%"=="0" goto RUN

echo Сборка ygg_control.exe...
set "LIBS_GCC=-ld2d1 -ldwrite -ldwmapi -lole32 -loleaut32 -luser32 -lgdi32 -lshell32 -ladvapi32 -lwindowscodecs -luuid -lcomdlg32 -lws2_32 -liphlpapi -lwinhttp"
set "LIBS_CL=d2d1.lib dwrite.lib dwmapi.lib ole32.lib oleaut32.lib user32.lib gdi32.lib shell32.lib advapi32.lib windowscodecs.lib comdlg32.lib ws2_32.lib iphlpapi.lib winhttp.lib"

where g++ >nul 2>&1
if not errorlevel 1 (
    echo Собираю через MinGW g++...
    set "RESOBJ="
    where windres >nul 2>&1
    if not errorlevel 1 (
        windres app.rc -O coff -o app.res
        if not errorlevel 1 set "RESOBJ=app.res"
    )
    g++ -std=c++17 -O2 -municode -mwindows -static -o "%EXE%" ygg_control.cpp !RESOBJ! %LIBS_GCC%
    set "RES=!errorlevel!"
    if exist app.res del app.res
    if "!RES!"=="0" goto OK
    goto FAIL
)

where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLPATH=%%i"
        if defined VSINSTALLPATH call "!VSINSTALLPATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    )
)
where cl >nul 2>&1
if errorlevel 1 goto NOCOMPILER

echo Собираю через MSVC...
set "RESOBJ="
rc /nologo /fo app.res app.rc >nul && set "RESOBJ=app.res"
cl /nologo /EHsc /std:c++17 /utf-8 /O2 ygg_control.cpp !RESOBJ! /Fe:"%EXE%" /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS_CL%
set "RES=!errorlevel!"
if exist app.res del app.res
if exist ygg_control.obj del ygg_control.obj
if "!RES!"=="0" goto OK
goto FAIL

:NOCOMPILER
echo Не найден ни g++, ни cl, ни Visual Studio с компонентом C++.
echo   1. MinGW-w64 с winlibs.com - проще всего, добавь его bin в PATH
echo   2. Visual Studio Community с компонентом "Desktop development with C++"
popd
pause
exit /b 1

:FAIL
echo.
echo Ошибка сборки, текст выше.
popd
pause
exit /b 1

:OK
echo Готово: %EXE%

:RUN
popd
start "" "%EXE%"
exit /b 0
