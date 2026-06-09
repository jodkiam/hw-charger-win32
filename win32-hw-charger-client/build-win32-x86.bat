@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SLN=%SCRIPT_DIR%HWChargerWin32.sln"
set "EXE=%SCRIPT_DIR%bin\Win32\Release\HWChargerWin32.exe"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "RUN_AFTER_BUILD=0"
set "PAUSE_ON_EXIT=1"
set "EXIT_CODE=0"

if /I "%~1"=="--run" set "RUN_AFTER_BUILD=1"
if /I "%~1"=="--no-pause" set "PAUSE_ON_EXIT=0"

if not exist "%VSWHERE%" (
  echo vswhere.exe was not found.
  echo Visual Studio 2022 or Build Tools 2022 is not installed on this machine.
  echo Build this project on a 64-bit Windows development machine with:
  echo   Visual Studio 2022 or Build Tools 2022
  echo   Desktop development with C++
  echo   Windows 10 SDK
  echo Then copy bin\Win32\Release\HWChargerWin32.exe to the 32-bit Windows 10 target machine.
  set "EXIT_CODE=1"
  goto done
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
  set "MSBUILD=%%I"
)

if not defined MSBUILD (
  echo MSBuild was not found.
  echo Install Visual Studio 2022 or Build Tools 2022 on a 64-bit Windows development machine.
  echo Select the Desktop development with C++ workload and Windows 10 SDK.
  set "EXIT_CODE=1"
  goto done
)

echo Building Release Win32...
"%MSBUILD%" "%SLN%" /m /p:Configuration=Release /p:Platform=Win32
if errorlevel 1 goto build_failed

if not exist "%EXE%" (
  echo Build finished, but the EXE was not found:
  echo %EXE%
  set "EXIT_CODE=1"
  goto done
)

echo.
echo Output:
echo %EXE%

if "%RUN_AFTER_BUILD%"=="1" (
  echo.
  echo Starting HWChargerWin32.exe...
  call "%SCRIPT_DIR%run-win32-x86.bat" --no-pause
  set "EXIT_CODE=%ERRORLEVEL%"
)

goto done

:build_failed
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Build failed with exit code %EXIT_CODE%.

:done
echo.
if "%PAUSE_ON_EXIT%"=="1" pause
exit /b %EXIT_CODE%
