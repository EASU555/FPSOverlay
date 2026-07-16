@echo off
setlocal

set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Users\ROG\Documents\Codex\2026-06-16\https-github-com-aneeskhan47-fps-overlay\work\msvc\setup_x64.bat" (
    set "VCVARS=C:\Users\ROG\Documents\Codex\2026-06-16\https-github-com-aneeskhan47-fps-overlay\work\msvc\setup_x64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

if "%VCVARS%"=="" (
    echo ERROR: Could not find an x64 MSVC toolchain.
    exit /b 1
)

call "%VCVARS%"
if not exist "build\obj\QA" mkdir "build\obj\QA"
if not exist "build\qa" mkdir "build\qa"

set "QA_OUTPUT="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "$h=Get-Content -Raw 'src\version.h'; $a=[regex]::Match($h,'#define VER_MAJOR ([0-9]+)').Groups[1].Value; $b=[regex]::Match($h,'#define VER_MINOR ([0-9]+)').Groups[1].Value; $c=[regex]::Match($h,'#define VER_PATCH ([0-9]+)').Groups[1].Value; Write-Output ('overlay_uiqa_v{0}.{1}.{2}.exe' -f $a,$b,$c)"`) do set "QA_OUTPUT=%%I"
if "%QA_OUTPUT%"=="" (
    echo ERROR: Could not determine the QA output name from src\version.h.
    exit /b 1
)

rc /nologo /d FPSOVERLAY_UI_QA /fo "build\obj\QA\resource.res" "src\resource.rc"
if errorlevel 1 exit /b 1

cl /nologo /c /std:c++20 /utf-8 /O2 /GL /Gy /Oi /sdl /EHsc /MD /W4 /permissive- /Zi /Fd"build\qa\overlay_uiqa_compile.pdb" ^
    /DNDEBUG /D_WINDOWS /DFPSOVERLAY_UI_QA ^
    /I"libs\imgui" /I"libs\imgui\backends" /I"libs\lhwm" ^
    /Fo"build\obj\QA\\" ^
    "src\main.cpp" ^
    "features\FeatureRegistry.cpp" ^
    "features\GameSessionReportFeature.cpp" ^
    "features\LaptopPowerFeature.cpp" ^
    "libs\imgui\imgui.cpp" ^
    "libs\imgui\imgui_draw.cpp" ^
    "libs\imgui\imgui_tables.cpp" ^
    "libs\imgui\imgui_widgets.cpp" ^
    "libs\imgui\backends\imgui_impl_dx11.cpp" ^
    "libs\imgui\backends\imgui_impl_win32.cpp"
if errorlevel 1 exit /b 1

link /nologo /OUT:"build\qa\%QA_OUTPUT%" /SUBSYSTEM:WINDOWS ^
    /OPT:REF /OPT:ICF /LTCG /MANIFEST:NO /DEBUG:FULL /IGNORE:4099 /PDB:"build\qa\overlay_uiqa.pdb" ^
    "build\obj\QA\main.obj" ^
    "build\obj\QA\FeatureRegistry.obj" ^
    "build\obj\QA\GameSessionReportFeature.obj" ^
    "build\obj\QA\LaptopPowerFeature.obj" ^
    "build\obj\QA\imgui.obj" ^
    "build\obj\QA\imgui_draw.obj" ^
    "build\obj\QA\imgui_tables.obj" ^
    "build\obj\QA\imgui_widgets.obj" ^
    "build\obj\QA\imgui_impl_dx11.obj" ^
    "build\obj\QA\imgui_impl_win32.obj" ^
    "build\obj\QA\resource.res" ^
    d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib gdi32.lib advapi32.lib ^
    ole32.lib oleaut32.lib wbemuuid.lib psapi.lib version.lib windowscodecs.lib ^
    setupapi.lib pdh.lib PowrProf.lib iphlpapi.lib taskschd.lib ws2_32.lib ^
    "libs\lhwm\lhwm-cpp-wrapper.lib" "libs\lhwm\mscoree.lib"
if errorlevel 1 exit /b 1

copy /Y "libs\lhwm\lhwm-wrapper.dll" "build\qa\" >nul
copy /Y "libs\lhwm\LibreHardwareMonitorLib.dll" "build\qa\" >nul
echo QA build complete: build\qa\%QA_OUTPUT%

endlocal
