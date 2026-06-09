@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "EXE=%SCRIPT_DIR%bin\Win32\Release\HWChargerWin32.exe"
if not exist "%EXE%" if exist "%SCRIPT_DIR%HWChargerWin32.exe" set "EXE=%SCRIPT_DIR%HWChargerWin32.exe"
set "LOG=%SCRIPT_DIR%run-log.txt"
for %%I in ("%EXE%") do set "EXE_DIR=%%~dpI"
set "STARTUP_LOG=%EXE_DIR%startup-log.txt"
set "CRASH_LOG=%EXE_DIR%crash-log.txt"
set "PAUSE_ON_EXIT=1"
set "EXIT_CODE=0"

if /I "%~1"=="--no-pause" set "PAUSE_ON_EXIT=0"

echo HWChargerWin32 run log>"%LOG%"
echo Date: %DATE% %TIME%>>"%LOG%"
echo EXE: %EXE%>>"%LOG%"
echo.>>"%LOG%"

if not exist "%EXE%" (
  echo HWChargerWin32.exe was not found.
  echo Build it first:
  echo build-win32-x86.bat
  echo ERROR: EXE not found.>>"%LOG%"
  set "EXIT_CODE=1"
  goto done
)

echo Starting:
echo %EXE%
echo Starting EXE and waiting for it to exit...>>"%LOG%"
start /wait "" "%EXE%"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
echo HWChargerWin32.exe exited with code %EXIT_CODE%.
echo Exit code: %EXIT_CODE%>>"%LOG%"

if exist "%STARTUP_LOG%" (
  echo Startup log:
  echo %STARTUP_LOG%
  echo Startup log: %STARTUP_LOG%>>"%LOG%"
) else (
  echo startup-log.txt was not created.
  echo startup-log.txt was not created.>>"%LOG%"
)

if exist "%CRASH_LOG%" (
  echo Crash log:
  echo %CRASH_LOG%
  echo Crash log: %CRASH_LOG%>>"%LOG%"
) else (
  echo crash-log.txt was not created.
  echo crash-log.txt was not created.>>"%LOG%"
)

echo.
echo Run log:
echo %LOG%

:done
echo.
if "%PAUSE_ON_EXIT%"=="1" pause
exit /b %EXIT_CODE%
