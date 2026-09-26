@echo off
setlocal enabledelayedexpansion

echo ============================================
echo   errer build
echo ============================================
echo.

rem ---------------------------------------------------------------
rem [1/4] Visual Studio environment
rem ---------------------------------------------------------------
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"

echo [1/4] Setting up the Visual Studio environment...

if not exist "%VCVARS%" (
    echo.
    echo [ERROR] couldnt set environment with vcvarsall.bat
    echo.
    echo   vcvarsall.bat is the script Visual Studio ships to set up the
    echo   C++ compiler environment. I went looking for it here:
    echo       %VCVARS%
    echo   and it is not there.
    echo.
    echo   how to fix it:
    echo     - install Visual Studio 2026 Community, and
    echo     - during setup select the "Desktop development with C++" workload,
    echo     - if you installed it somewhere else, edit the VCVARS line at the
    echo       top of build.bat so it points at your vcvarsall.bat,
    echo     - sanity check: open "Developer Command Prompt for VS 2026" and
    echo       type cl - if that works, so should this script.
    echo.
    exit /b 1
)

call "%VCVARS%" x64 >nul
if errorlevel 1 (
    echo.
    echo [ERROR] couldnt set environment with vcvarsall.bat
    echo.
    echo   vcvarsall.bat ran but gave up while setting up the x64
    echo   environment. usually that means the C++ workload or the
    echo   Windows SDK is missing or broken inside Visual Studio.
    echo.
    echo   how to fix it:
    echo     - open Visual Studio Installer, and
    echo     - make sure the "Desktop development with C++" workload is
    echo       checked, then hit "Repair", and try this again.
    echo.
    exit /b 1
)

where cl.exe >nul 2>nul
if errorlevel 1 (set "MISSING_CL=1") else (set "MISSING_CL=0")
where rc.exe >nul 2>nul
if errorlevel 1 (set "MISSING_RC=1") else (set "MISSING_RC=0")

if "%MISSING_CL%"=="1" if "%MISSING_RC%"=="1" goto :no_cl_rc
if "%MISSING_CL%"=="1" goto :no_cl
if "%MISSING_RC%"=="1" goto :no_rc
goto :env_ok

:no_cl_rc
echo.
echo [ERROR] please install visual studio first (missing cl.exe and rc.exe)
echo.
echo   I need cl.exe to compile the C++ and rc.exe to turn errer.rc into
echo   a resource file, and I cannot see either one anywhere.
echo.
echo   how to fix it:
echo     - install Visual Studio 2026 Community, and
echo     - during setup tick "Desktop development with C++",
echo     - close this window and run build.bat again from a fresh cmd.exe.
echo.
exit /b 1

:no_cl
echo.
echo [ERROR] i dont know how you did this but cl.exe is missing but rc.exe is not.... i cant explain how to fix it
echo.
echo   seriously. how. cl.exe and rc.exe live in the same Visual Studio
echo   install, so if rc.exe is findable then cl.exe should be too.
echo   my only guesses:
echo     - something deleted or quarantined cl.exe, or
echo     - your PATH is a mess and hides it, or
echo     - the C++ workload is half-installed.
echo   troubleshooting:
echo     - reinstall the "Desktop development with C++" workload,
echo     - or run "where cl.exe" to see what is going on.
echo.
exit /b 1

:no_rc
echo.
echo [ERROR] i dont know how you did this but rc.exe is missing but cl.exe is not.... i cant explain how to fix it
echo.
echo   seriously. how. cl.exe and rc.exe live in the same Visual Studio
echo   install, so if cl.exe is findable then rc.exe should be too.
echo   my only guesses:
echo     - something deleted or quarantined rc.exe, or
echo     - your PATH is a mess and hides it, or
echo     - the C++ workload is half-installed.
echo   troubleshooting:
echo     - reinstall the "Desktop development with C++" workload,
echo     - or run "where rc.exe" to see what is going on.
echo.
exit /b 1

:env_ok
echo         OK: cl.exe and rc.exe ready
echo.

rem ---------------------------------------------------------------
rem [2/4] Check that every required file exists
rem ---------------------------------------------------------------
echo [2/4] Checking project files...
if not exist "src" (
    echo.
    echo [ERROR] uhhh the src folder is not there... try git cloning this again?
    echo.
    echo   I expect a "src" folder right next to build.bat, holding main.cpp
    echo   and the payload sources, and it is just... gone.
    echo.
    echo   how to fix it:
    echo     - make sure you got the WHOLE project, not just build.bat,
    echo     - if you copied it, copy the entire errer folder,
    echo     - or git clone the repo again into a fresh folder and run
    echo       build.bat from there.
    echo.
    exit /b 1
)
set "missing=0"
for %%F in (src\main.cpp src\entry.cpp src\FakeExplorer.cpp src\Cascade.cpp src\Payload1\payload1.cpp src\Payload2\payload2.cpp src\Payload3\payload3.cpp src\Payload4\payload4.cpp src\Payload5\payload5.cpp src\Payload6\payload6.cpp src\Payload7\payload7.cpp errer.rc Icon.ico Resources\Icon.ico Resources\badapple.mp3 debug\entry1.cpp debug\entry2.cpp debug\entry3.cpp debug\entry4.cpp debug\entry5.cpp debug\entry6.cpp debug\entry7.cpp) do (
    if not exist "%%F" (
        echo [ERROR] Missing file: %%F
        set "missing=1"
    )
)
if "%missing%"=="1" (
    echo.
    echo   What you might be missing or did wrong:
    echo     - A source file was deleted or moved. Restore it from git or a backup.
    echo     - errer.rc requires Icon.ico and Resources\badapple.mp3,
    echo       the exe icon and the embedded audio. If either is missing the
    echo       resource compiler will fail.
    echo     - The project layout must stay: everything inside this errer
    echo       folder, sources under src\..., entries under debug\...
    echo.
    exit /b 1
)
echo         OK: all source files present
echo.

rem ---------------------------------------------------------------
rem [3/4] Build errer.exe
rem ---------------------------------------------------------------
echo [3/4] Building errer.exe...
echo.
rc errer.rc
if errorlevel 1 goto :unexpected

cl /EHsc /Fe:errer.exe /D_WIN32_WINNT=0x0601 /D_WIN32_IE=0x0A00 src\main.cpp src\entry.cpp src\FakeExplorer.cpp src\Cascade.cpp src\Payload1\payload1.cpp src\Payload2\payload2.cpp src\Payload3\payload3.cpp src\Payload4\payload4.cpp src\Payload5\payload5.cpp src\Payload6\payload6.cpp src\Payload7\payload7.cpp errer.res /link /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" user32.lib shell32.lib ole32.lib oleaut32.lib advapi32.lib gdi32.lib
if errorlevel 1 goto :unexpected
echo         OK: errer.exe built
echo.

echo ============================================
echo   Build finished OK.
echo     - errer.exe
echo ============================================
echo.
echo   Tip: run build_debug.bat if you want the
echo   debug\errer_debug1 .. 3.exe variants.
echo.
echo   congrats on building (:
pause
endlocal
exit /b 0

:unexpected
echo.
echo [ERROR] how cute i got a unexpected fail.... please try re running build.bat sorry for the incovenience
echo.
pause
exit /b 1
