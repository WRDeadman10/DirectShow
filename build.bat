@echo off
setlocal enabledelayedexpansion

echo ===================================================
echo  Building LAN Phone Camera DirectShow Driver (x64)
echo ===================================================

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

set VCAM_ROOT=%~dp0
set SRC_DIR=%VCAM_ROOT%src
set INC_DIR=%VCAM_ROOT%include
set BUILD_DIR=%VCAM_ROOT%build

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo Compiling LanCamFilter.dll...
cl.exe /O2 /MD /EHsc /W3 /LD ^
    /I"%INC_DIR%" ^
    "%SRC_DIR%\lancam_filter.cpp" "%SRC_DIR%\dll_main.cpp" ^
    /Fe"%BUILD_DIR%\LanCamFilter.dll" ^
    /link /DEF:"%SRC_DIR%\lancam_filter.def" /SUBSYSTEM:WINDOWS ole32.lib oleaut32.lib user32.lib shell32.lib advapi32.lib winhttp.lib windowscodecs.lib strmiids.lib

if %ERRORLEVEL% neq 0 (
    echo Error compiling LanCamFilter.dll!
    exit /b %ERRORLEVEL%
)

echo Compiling lancam-setup.exe...
cl.exe /O2 /MD /EHsc /W3 ^
    /I"%INC_DIR%" ^
    "%SRC_DIR%\lancam_setup.cpp" ^
    /Fe"%BUILD_DIR%\lancam-setup.exe" ^
    /link /SUBSYSTEM:WINDOWS user32.lib shell32.lib advapi32.lib ole32.lib

if %ERRORLEVEL% neq 0 (
    echo Error compiling lancam-setup.exe!
    exit /b %ERRORLEVEL%
)

echo.
echo Build successful! Outputs:
echo   - %BUILD_DIR%\LanCamFilter.dll
echo   - %BUILD_DIR%\lancam-setup.exe
echo.
