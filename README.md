# MouseLock

A minimal native Win32 application (~50 KB, no runtime dependencies) that confines the mouse cursor to a screen region using `ClipCursor()`.

## Features

- Lock the cursor to a specific monitor
- Lock the cursor to a custom drawn region
- Lock the cursor to the currently active window
- System tray icon with context menu
- Global hotkey `Ctrl+Alt+L` to toggle lock on/off
- Automatically re-asserts the lock every 250ms to survive UAC prompts, screen savers, and display config changes
- Single-instance enforcement

## Requirements

- Windows 10 or later
- MSVC (VS 2019+) or MinGW-w64
- CMake 3.15+

## Building

The recommended way to build is with the included PowerShell script, which sets up the MSVC environment automatically:

```powershell
.\make.ps1          # build (default)
.\make.ps1 clean    # delete build output
.\make.ps1 rebuild  # clean then build
```

The output executable is `build\CursorLocker.exe`.

### Manual CMake build

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Manual MSVC build

From a VS Developer Command Prompt:

```bat
cl /EHsc /std:c++17 /O2 CursorLocker.cpp /link user32.lib gdi32.lib shell32.lib /SUBSYSTEM:WINDOWS /ENTRY:wWinMain /MANIFEST:NO
mt -manifest CursorLocker.manifest -outputresource:CursorLocker.exe;1
```

### MinGW build

```bash
g++ -std=c++17 -O2 -municode -mwindows CursorLocker.cpp -o CursorLocker.exe -luser32 -lgdi32 -lshell32
```

## Usage

1. Run `CursorLocker.exe` — a tray icon appears in the system tray
2. Right-click the tray icon to open the menu
3. Choose a lock mode:
   - **Lock to primary monitor** — confines cursor to the primary display
   - **Lock to monitor** — choose a specific monitor from the submenu
   - **Lock to custom region...** — drag to draw a rectangle on screen
   - **Lock to active window (3s)** — switch to the target window within 3 seconds
4. Press `Ctrl+Alt+L` at any time to toggle the lock on/off

## Project Structure

```
CursorLocker.cpp       # Entire application (single file)
CursorLocker.manifest  # UAC/DPI manifest embedded into the exe
CMakeLists.txt         # CMake build definition
make.ps1               # PowerShell build script
```
