@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  Building LAN Phone Camera DirectShow Driver (x64)
echo ===================================================

:: Locate and initialize MSVC x64 build environment if cl.exe is not already in PATH
where cl.exe >nul 2>&1
if %ERRORLEVEL% equ 0 goto :msvc_ready

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "!VSWHERE!" (
    for /f "delims=" %%i in ('"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat') do (
        if exist "%%i" set "VCVARS=%%i"
    )
)

if defined VCVARS goto :found_vcvars

if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"

if not defined VCVARS (
    echo [ERROR] Visual Studio C++ build tools vcvars64.bat not found.
    echo Please install Visual Studio or Visual Studio Build Tools with Desktop development with C++.
    exit /b 1
)

:found_vcvars
call "!VCVARS!"
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Failed to initialize MSVC x64 build environment.
    exit /b %ERRORLEVEL%
)

:msvc_ready

set VCAM_ROOT=%~dp0
set SRC_DIR=%VCAM_ROOT%src
set INC_DIR=%VCAM_ROOT%include
set BUILD_DIR=%VCAM_ROOT%build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo Compiling LanCamFilter.dll...
cl.exe /O2 /MT /EHsc /W3 /LD ^
    /I"%INC_DIR%" ^
    /Fo"%BUILD_DIR%\\" ^
    "%SRC_DIR%\lancam_filter.cpp" "%SRC_DIR%\dll_main.cpp" ^
    /Fe"%BUILD_DIR%\LanCamFilter.dll" ^
    /link /DEF:"%SRC_DIR%\lancam_filter.def" /SUBSYSTEM:WINDOWS ole32.lib oleaut32.lib user32.lib shell32.lib advapi32.lib winhttp.lib windowscodecs.lib strmiids.lib

if %ERRORLEVEL% neq 0 (
    echo Error compiling LanCamFilter.dll!
    exit /b %ERRORLEVEL%
)

echo Compiling LanCamMfSource.dll (Media Foundation virtual camera source)...
cl.exe /O2 /MT /EHsc /W3 /LD ^
    /I"%INC_DIR%" ^
    /Fo"%BUILD_DIR%\\" ^
    "%SRC_DIR%\lancam_mfsource.cpp" ^
    /Fe"%BUILD_DIR%\LanCamMfSource.dll" ^
    /link /DEF:"%SRC_DIR%\lancam_mfsource.def" /SUBSYSTEM:WINDOWS

if %ERRORLEVEL% neq 0 (
    echo Error compiling LanCamMfSource.dll!
    exit /b %ERRORLEVEL%
)

echo Compiling lancam-setup.exe...
cl.exe /O2 /MT /EHsc /W3 ^
    /I"%INC_DIR%" ^
    /Fo"%BUILD_DIR%\\" ^
    "%SRC_DIR%\lancam_setup.cpp" ^
    /Fe"%BUILD_DIR%\lancam-setup.exe" ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" user32.lib shell32.lib advapi32.lib ole32.lib ws2_32.lib mfplat.lib mf.lib mfuuid.lib

if %ERRORLEVEL% neq 0 (
    echo Error compiling lancam-setup.exe!
    exit /b %ERRORLEVEL%
)

echo Building the single-file Inno Setup installer...
set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Error: Inno Setup 6 was not found. Install it from https://jrsoftware.org/isdl.php and run this script again.
    exit /b 1
)

set APP_VERSION=1.0.0
if exist "%VCAM_ROOT%..\package.json" (
    for /f "usebackq delims=" %%V in (`powershell -NoProfile -Command "(Get-Content -Raw '%VCAM_ROOT%..\package.json' | ConvertFrom-Json).version"`) do set APP_VERSION=%%V
)
if defined LANCAM_VERSION set APP_VERSION=%LANCAM_VERSION%

set OUT_DIR=%BUILD_DIR%\installer
if exist "%VCAM_ROOT%..\package.json" set OUT_DIR=%VCAM_ROOT%..\dist
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
del /q "%OUT_DIR%\lan-camera-setupv*.exe" >nul 2>&1

"%ISCC%" /Qp /DAppVersion=%APP_VERSION% /DBuildDir="%BUILD_DIR%" /DOutDir="%OUT_DIR%" "%VCAM_ROOT%installer\lan-camera.iss"
if %ERRORLEVEL% neq 0 (
    echo Error building the Inno Setup installer!
    exit /b %ERRORLEVEL%
)
echo   - %OUT_DIR%\lan-camera-setupv%APP_VERSION%.exe

echo.
echo Build successful! Outputs:
echo   - %BUILD_DIR%\LanCamFilter.dll
echo   - %BUILD_DIR%\LanCamMfSource.dll
echo   - %BUILD_DIR%\lancam-setup.exe
echo.
