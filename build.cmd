@echo off
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
pushd "%~dp0"
if not exist build mkdir build
ml64 /nologo /c /Fobuild\proxy.obj src\proxy.asm
if errorlevel 1 exit /b 1
for %%F in (src\patch.cpp src\overlay.cpp) do (
    cl /nologo /c /O2 /MT /std:c++17 /utf-8 /W4 /WX /EHsc /Ithird_party\imgui /Fobuild\%%~nF.obj %%F
    if errorlevel 1 exit /b 1
)
for %%F in (third_party\imgui\imgui.cpp third_party\imgui\imgui_draw.cpp third_party\imgui\imgui_tables.cpp third_party\imgui\imgui_widgets.cpp third_party\imgui\backends\imgui_impl_win32.cpp third_party\imgui\backends\imgui_impl_dx11.cpp) do (
    cl /nologo /c /O2 /MT /std:c++17 /utf-8 /W3 /EHsc /Ithird_party\imgui /Fobuild\%%~nF.obj %%F
    if errorlevel 1 exit /b 1
)
link /nologo /DLL /OUT:build\winmm.dll build\proxy.obj build\patch.obj build\overlay.obj build\imgui.obj build\imgui_draw.obj build\imgui_tables.obj build\imgui_widgets.obj build\imgui_impl_win32.obj build\imgui_impl_dx11.obj d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib dwmapi.lib /DEF:src\proxy.def /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /INCREMENTAL:NO
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
