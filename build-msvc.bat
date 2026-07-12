@echo off
REM FPS Overlay MSVC Build Script
REM Builds the project using Visual Studio Build Tools

setlocal

REM Find vcvars64.bat
set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Users\ROG\Documents\Codex\2026-06-16\https-github-com-aneeskhan47-fps-overlay\work\msvc\setup_x64.bat" (
    set "VCVARS=C:\Users\ROG\Documents\Codex\2026-06-16\https-github-com-aneeskhan47-fps-overlay\work\msvc\setup_x64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
)

if "%VCVARS%"=="" (
    echo ERROR: Could not find Visual Studio vcvars64.bat
    echo Please install Visual Studio Build Tools with C++ workload
    exit /b 1
)

echo Setting up Visual Studio environment...
call "%VCVARS%"

echo.
echo Building FPS Overlay (Release x64)...
echo.

if not exist "build\overlay" mkdir "build\overlay"

REM Build using MSBuild
where msbuild >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    msbuild FPSOverlay.vcxproj /p:Configuration=Release /p:Platform=x64 /m /verbosity:minimal
) else (
    echo MSBuild not found; using the portable MSVC toolchain directly...
    if not exist "build\obj\Release" mkdir "build\obj\Release"
    rc /nologo /fo "build\obj\Release\resource.res" "src\resource.rc"
    if errorlevel 1 exit /b 1
    cl /nologo /c /std:c++20 /utf-8 /O2 /GL /Gy /Oi /sdl /EHsc /MD /W4 /permissive- /Zi /Fd"build\overlay\overlay_compile.pdb" ^
        /DNDEBUG /D_WINDOWS ^
        /I"libs\imgui" /I"libs\imgui\backends" /I"libs\lhwm" ^
        /Fo"build\obj\Release\\" ^
        "src\main.cpp" ^
        "features\FeatureRegistry.cpp" ^
        "features\LaptopPowerFeature.cpp" ^
        "libs\imgui\imgui.cpp" ^
        "libs\imgui\imgui_draw.cpp" ^
        "libs\imgui\imgui_tables.cpp" ^
        "libs\imgui\imgui_widgets.cpp" ^
        "libs\imgui\backends\imgui_impl_dx11.cpp" ^
        "libs\imgui\backends\imgui_impl_win32.cpp"
    if errorlevel 1 exit /b 1
    link /nologo /OUT:"build\overlay\overlay.exe" /SUBSYSTEM:WINDOWS ^
        /OPT:REF /OPT:ICF /LTCG /MANIFEST:NO /DEBUG:FULL /IGNORE:4099 /PDB:"build\overlay\overlay.pdb" ^
        "build\obj\Release\main.obj" ^
        "build\obj\Release\FeatureRegistry.obj" ^
        "build\obj\Release\LaptopPowerFeature.obj" ^
        "build\obj\Release\imgui.obj" ^
        "build\obj\Release\imgui_draw.obj" ^
        "build\obj\Release\imgui_tables.obj" ^
        "build\obj\Release\imgui_widgets.obj" ^
        "build\obj\Release\imgui_impl_dx11.obj" ^
        "build\obj\Release\imgui_impl_win32.obj" ^
        "build\obj\Release\resource.res" ^
        d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib gdi32.lib advapi32.lib ^
        ole32.lib oleaut32.lib wbemuuid.lib psapi.lib version.lib windowscodecs.lib ^
        setupapi.lib pdh.lib PowrProf.lib iphlpapi.lib taskschd.lib ws2_32.lib ^
        "libs\lhwm\lhwm-cpp-wrapper.lib" "libs\lhwm\mscoree.lib"
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo BUILD FAILED!
    exit /b 1
)

REM Create a versioned copy from the single source of truth in src\version.h.
powershell -NoProfile -Command ^
    "$h=Get-Content -Raw 'src\version.h';" ^
    "$a=[regex]::Match($h,'#define VER_MAJOR ([0-9]+)').Groups[1].Value;" ^
    "$b=[regex]::Match($h,'#define VER_MINOR ([0-9]+)').Groups[1].Value;" ^
    "$c=[regex]::Match($h,'#define VER_PATCH ([0-9]+)').Groups[1].Value;" ^
    "$s=[regex]::Match($h,'#define VER_BUILD_STAMP \"([0-9-]+)\"').Groups[1].Value;" ^
    "$name='overlay_v{0}.{1}.{2}_{3}.exe' -f $a,$b,$c,$s;" ^
    "Copy-Item -LiteralPath 'build\overlay\overlay.exe' -Destination (Join-Path 'build\overlay' $name) -Force;" ^
    "Write-Host ('Versioned output: build\overlay\' + $name)"
if errorlevel 1 exit /b 1

echo.
echo Build successful!
echo.

REM Copy required DLLs to build\overlay
echo Copying required DLLs...
if exist "libs\lhwm\lhwm-wrapper.dll" (
    copy /Y "libs\lhwm\lhwm-wrapper.dll" "build\overlay\" >nul
    echo   - lhwm-wrapper.dll copied
)
if exist "libs\lhwm\LibreHardwareMonitorLib.dll" (
    copy /Y "libs\lhwm\LibreHardwareMonitorLib.dll" "build\overlay\" >nul
    echo   - LibreHardwareMonitorLib.dll copied
)

REM Clean up intermediate files (obj folder)
echo Cleaning up intermediate files...
if exist "build\obj" (
    rmdir /S /Q "build\obj" >nul 2>&1
    echo   - obj folder removed
)

echo.
echo ========================================
echo   Build complete!
echo   Output: build\overlay\overlay.exe
echo ========================================
echo.
echo Required files in build\overlay:
echo   - overlay.exe
echo   - lhwm-wrapper.dll (for LHWM support)
echo   - LibreHardwareMonitorLib.dll (for LHWM support)
echo.
echo Run as Administrator for full functionality.

endlocal
