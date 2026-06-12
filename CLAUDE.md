# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

MouseLock is a minimal native Win32 application (~50 KB, no runtime deps) that confines the mouse cursor to a screen region using a dual-layer mechanism: a `WH_MOUSE_LL` low-level mouse hook (primary) and `ClipCursor()` (secondary fallback). The entire implementation lives in a single file: [CursorLocker.cpp](CursorLocker.cpp).

## Build

**PowerShell script (recommended):** Handles MSVC environment setup automatically.
```powershell
.\make.ps1           # build
.\make.ps1 clean     # delete build folder
.\make.ps1 rebuild   # clean then build
```

**CMake manually:**
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

**MSVC directly** (from a VS Developer Command Prompt):
```bat
cl /EHsc /std:c++17 /O2 CursorLocker.cpp /link user32.lib gdi32.lib shell32.lib /SUBSYSTEM:WINDOWS /ENTRY:wWinMain /MANIFEST:NO
mt -manifest CursorLocker.manifest -outputresource:CursorLocker.exe;1
```

**MinGW:**
```bash
g++ -std=c++17 -O2 -municode -mwindows CursorLocker.cpp -o CursorLocker.exe -luser32 -lgdi32 -lshell32
```

Requires: MSVC (VS 2019+ Build Tools) or MinGW-w64, CMake 3.15+. Windows-only — no cross-compilation path.

## Architecture

The app uses a **message-only window** (`HWND_MESSAGE`) as its main window — no visible main UI, just a tray icon and an overlay for region picking.

**Dual-layer confinement:**
- **Primary — `WH_MOUSE_LL` hook (`LowLevelMouseProc`):** intercepts `WM_MOUSEMOVE` in the OS hook chain before any application sees it. If the cursor would leave `g_lockedRect`, it clamps the position with `SetCursorPos()` and swallows the event (returns 1). `LLMHF_INJECTED` events are skipped to avoid feedback loops.
- **Secondary — `ClipCursor()`:** OS-level rect restriction kept as a fallback. Installed/removed via `ApplyClip()` / `ReleaseClip()`.
- The hook is installed by `InstallHook()` on lock and torn down by `RemoveHook()` on unlock.

**Lock state** is global:
- `g_locked` / `g_mode` (`LockMode` enum: None, Rect, Window)
- `g_lockedRect` — active clip rect used by both the hook and `ClipCursor()`
- `g_trackedWnd` — HWND for window-follow mode
- `g_mouseHook` — handle to the installed `WH_MOUSE_LL` hook (null when unlocked)
- `g_mode` and `g_trackedWnd` are **preserved on unlock** so `ToggleLock()` can re-arm the previous mode.

**Why the 250ms timer (`TIMER_REASSERT`):** Windows silently releases `ClipCursor()` on UAC elevation, screen saver activation, display config changes, and other system events. The timer calls `ReassertClip()` to re-apply both layers.

**Region picker overlay** (`OverlayProc`): A full-screen layered window (`WS_EX_LAYERED`) with color-key transparency — black pixels are transparent, everything else is ~40% opaque. A red 2px outline marks the drag selection. On mouse-up it commits the rect and calls `LockToRect()`.

**Tray menu** is rebuilt dynamically on each `WM_CONTEXTMENU` to enumerate connected monitors via `EnumDisplayMonitors`. The monitor submenu supports up to 100 monitors.

**Single-instance guard:** Named mutex `Local\CursorLocker_SingleInstance` checked in `wWinMain`.

**Entry point:** `wWinMain` (line ~471) — registers two window classes (main + overlay), sets up tray icon, registers `Ctrl+Alt+L` as a global hotkey, then runs the message loop.

Key functions to know:
- `LowLevelMouseProc` — primary confinement; clamps and swallows out-of-bounds mouse events
- `InstallHook()` / `RemoveHook()` — install/uninstall the `WH_MOUSE_LL` hook
- `ApplyClip()` / `ReleaseClip()` — thin wrappers around `ClipCursor()`
- `ReassertClip()` — re-applies both layers; called by the 250ms timer and on lock transitions
- `ToggleLock()` — unlocks if locked, otherwise re-arms the previous mode (or defaults to primary monitor)
- `LockToRect()` / `LockToWindow()` — state transitions with timer management
- `OverlayProc` — handles the drag-to-select region picker
