@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [X] vcvars64 no disponible.
    exit /b 1
)

set WINSDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0
set "INCLUDE=%WINSDK%\Include\%SDKVER%\ucrt;%WINSDK%\Include\%SDKVER%\um;%WINSDK%\Include\%SDKVER%\shared;%WINSDK%\Include\%SDKVER%\winrt;%INCLUDE%"
set "LIB=%WINSDK%\Lib\%SDKVER%\ucrt\x64;%WINSDK%\Lib\%SDKVER%\um\x64;%LIB%"

if not exist bin mkdir bin
if not exist obj mkdir obj

echo [..] Recursos...
"%WINSDK%\bin\%SDKVER%\x64\rc.exe" /nologo /fo obj\app.res app.rc
if errorlevel 1 (
    echo [X] rc app.rc fallo.
    exit /b 1
)
"%WINSDK%\bin\%SDKVER%\x64\rc.exe" /nologo /fo obj\manifest.res manifest.rc
if errorlevel 1 (
    echo [X] rc manifest.rc fallo.
    exit /b 1
)

echo [..] Compilando...
cl /nologo /EHsc /O2 /std:c++17 /W3 /GS- ^
   /Foobj\ /I panel\src /I panel\imgui ^
   panel\src\identity.cpp ^
   panel\src\spoofer.cpp ^
   panel\src\cleaner.cpp ^
   panel\src\main.cpp ^
   panel\imgui\imgui.cpp ^
   panel\imgui\imgui_draw.cpp ^
   panel\imgui\imgui_tables.cpp ^
   panel\imgui\imgui_widgets.cpp ^
   panel\imgui\imgui_impl_dx11.cpp ^
   panel\imgui\imgui_impl_win32.cpp ^
   obj\app.res ^
   obj\manifest.res ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:bin\SPOOFER_BS.exe
if errorlevel 1 (
    echo [X] Compilacion fallida.
    exit /b 1
)

echo [OK] bin\SPOOFER_BS.exe
exit /b 0
