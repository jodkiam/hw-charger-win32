# HW Charger Win32 x86 Client

This is a native Win32/C++ client for 32-bit Windows 10 systems. It does not use
Flutter because Flutter Windows desktop builds do not provide an x86 target.

## Current Scope

Implemented:

- Scan for charger-style BLE names: `HWCDQBLE_NIUB`, `HWCDQ`, `HE315`, `NIUB`.
- Connect to the verified GATT service `0000ffe0-0000-1000-8000-00805f9b34fb`.
- Subscribe to the verified notify characteristic `0000ffe2-0000-1000-8000-00805f9b34fb`.
- Write the verified poll command `02 06 06` to `0000ffe3-0000-1000-8000-00805f9b34fb`.
- Decode verified `30 06` telemetry packets into voltage, current, frequency,
  temperature, and raw packet hex.
- If the expected `FFE0` service is not found, enumerate every visible GATT
  service and characteristic into the app log so firmware variants and Windows
  pairing problems can be diagnosed.

Not implemented yet:

- Password/authentication packets.
- Configuration writes.
- Start/stop charging or parameter changes.
- Firmware update.

Those commands are intentionally excluded because the available captures only
verify the read-only telemetry flow.

## Build Requirements

- A Windows development machine. This can be 64-bit Windows; the output target
  is still 32-bit x86.
- Visual Studio 2022 or Visual Studio Build Tools.
- Workload: `Desktop development with C++`.
- Windows 10 SDK with C++/WinRT headers.

The target 32-bit Windows 10 machine only needs the built EXE and a BLE adapter.
It does not need Visual Studio installed.

If `build-win32-x86.bat` prints `vswhere.exe was not found`, no Visual Studio
Build Tools installation was found. Build on a Windows development machine with
Visual Studio/Build Tools installed, then copy the generated EXE to the 32-bit
Windows 10 target machine.

Visual Studio/Build Tools 2022 does not run on 32-bit Windows. A 32-bit Windows
10 machine is only the target machine for this project, not the build machine.

## Build x86 EXE

### Method A: Local Windows Build

From a Developer Command Prompt or normal `cmd.exe`:

```bat
cd win32-hw-charger-client
build-win32-x86.bat
```

If you double-click `build-win32-x86.bat`, the window now stays open at the end
so you can read any Visual Studio/MSBuild error.

To build and start the app immediately:

```bat
build-win32-x86.bat --run
```

Output:

```text
win32-hw-charger-client\bin\Win32\Release\HWChargerWin32.exe
```

You can also open `HWChargerWin32.sln` in Visual Studio and build
`Release|Win32`.

If Visual Studio reports a missing SDK version, right-click the solution and use
`Retarget solution` to your installed Windows 10 SDK.

If you build with Visual Studio 2019, retarget the project platform toolset from
`v143` to `v142`, then build `Release|Win32`.

### Method B: GitHub Actions Cloud Build

If you do not have a 64-bit Windows development machine, use the repository
workflow:

1. Push this repository to GitHub.
2. Open the GitHub repository page.
3. Go to `Actions`.
4. Select `Build HW Charger Win32 x86`.
5. Click `Run workflow`.
6. Wait for the run to finish.
7. Download the `HWChargerWin32-x86` artifact.
8. Extract `HWChargerWin32-x86.zip`.
9. Copy `HWChargerWin32.exe` to the 32-bit Windows 10 target machine.

The workflow file is at:

```text
.github\workflows\build-hwcharger-win32.yml
```

## Use

1. Turn on Bluetooth in Windows 10.
2. Run `HWChargerWin32.exe`.
3. Click `Scan`.
4. Select a charger device.
5. Click `Connect`.
6. The app subscribes to notifications and polls `02 06 06` once per second.

The app logs all recognized telemetry and shows the raw packet hex so future
captures can be compared against the existing protocol notes.

## Password / Pairing Notes

There are two different password cases:

- Windows Bluetooth pairing PIN/password: pair the charger from Windows
  `Settings > Devices > Bluetooth & other devices` first, then run this app and
  connect again.
- Charger app password: this is an application-level BLE packet. It is not
  implemented yet because the available Android captures did not include the
  password-submit write. The client intentionally does not guess this packet.

If connection fails, check `app-log.txt` next to `HWChargerWin32.exe`. It now
lists the Windows pairing state plus every GATT service and characteristic that
Windows can see.

If the EXE has already been built, you can also double-click:

```bat
run-win32-x86.bat
```

`run-win32-x86.bat` waits for the app to exit and writes:

- `run-log.txt` next to the batch files.
- `startup-log.txt` next to `HWChargerWin32.exe` if the process starts.
- `app-log.txt` next to `HWChargerWin32.exe` with scan/connect/GATT details.
- `crash-log.txt` next to `HWChargerWin32.exe` if the app catches a startup
  exception or crash.

If the app window flashes and closes, run `run-win32-x86.bat` again and inspect
those three files.

## Local Protocol Test

The BLE layer requires Windows, but the protocol decoder can be checked on any
machine with a C++17 compiler:

```sh
clang++ -std=c++17 -Wall -Wextra -pedantic tests/protocol_tests.cpp src/protocol.cpp -o /tmp/hw_charger_protocol_tests
/tmp/hw_charger_protocol_tests
```
