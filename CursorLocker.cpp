// CursorLocker.cpp
// A small Win32 tray app that confines the mouse cursor to a chosen area.
//
// Modes:
//   * Lock to primary monitor
//   * Lock to a chosen monitor
//   * Lock to a custom region drawn by the user
//   * Lock to the currently active window (3s grace to focus the target,
//     after which the clip follows the window as it moves)
//
// Global hotkey: Ctrl+Alt+L toggles lock/unlock.
// A 250ms timer re-applies the clip because Windows releases ClipCursor
// on focus loss, UAC prompts, lock screen, resolution changes, etc.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "resource.h"
#include <windowsx.h>   // GET_X_LPARAM, GET_Y_LPARAM
#include <shellapi.h>
#include <vector>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

// ---------- IDs ----------
#define WM_TRAYICON      (WM_APP + 1)
#define HOTKEY_TOGGLE    1
#define TIMER_REASSERT   1
#define TIMER_FOCUS_GRAB 2

#define IDM_LOCK_PRIMARY  1001
#define IDM_LOCK_REGION   1002
#define IDM_LOCK_WINDOW   1003
#define IDM_TOGGLE        1004
#define IDM_UNLOCK        1005
#define IDM_EXIT          1006
#define IDM_AUTOSTART      1007
#define IDM_SAVE_REGION    1008
#define IDM_CLEAR_REGIONS  1009
#define IDM_MONITOR_BASE   2000  // 2000..2099 dynamic per-monitor entries
#define IDM_SAVED_BASE     3000  // 3000..3019 saved region entries

// ---------- Lock state ----------
enum class LockMode { None, Rect, Window };

static HWND        g_mainWnd       = nullptr;
static HWND        g_overlayWnd    = nullptr;
static NOTIFYICONDATAW g_nid       = {};
static LockMode    g_mode          = LockMode::None;
static bool        g_locked        = false;
static RECT        g_lockedRect    = {};
static HWND        g_trackedWnd    = nullptr;

// Low-level mouse hook — installed when locked, bypasses ClipCursor fights.
static HHOOK       g_mouseHook     = nullptr;

// Region picker state
static POINT       g_dragStart     = {};
static POINT       g_dragCurrent   = {};
static bool        g_dragging      = false;

// Monitor enumeration cache (for the tray submenu)
struct MonInfo { RECT bounds; std::wstring name; };
static std::vector<MonInfo> g_monitors;

// Saved custom regions (loaded from registry into this cache on each menu open)
struct SavedRegion { RECT rect; wchar_t name[64]; };
static std::vector<SavedRegion> g_savedRegions;

// ---------- Low-level mouse hook ----------
//
// WH_MOUSE_LL intercepts mouse input in the OS hook chain before any
// application (including the game) processes it. When the cursor would move
// outside g_lockedRect we clamp it with SetCursorPos() and swallow the
// original event. ClipCursor() is kept as a secondary layer.
static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp)
{
    if (nCode == HC_ACTION && g_locked && wp == WM_MOUSEMOVE)
    {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);

        // LLMHF_INJECTED: ignore events we generated ourselves to avoid loops.
        if (!(ms->flags & LLMHF_INJECTED))
        {
            const RECT& r = g_lockedRect;
            LONG x = ms->pt.x;
            LONG y = ms->pt.y;

            LONG cx = (x < r.left) ? r.left : (x >= r.right  ? r.right  - 1 : x);
            LONG cy = (y < r.top)  ? r.top  : (y >= r.bottom ? r.bottom - 1 : y);

            if (cx != x || cy != y)
            {
                SetCursorPos(cx, cy);
                return 1; // swallow the out-of-bounds event
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wp, lp);
}

static void InstallHook()
{
    if (!g_mouseHook)
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
}

static void RemoveHook()
{
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
}

// ---------- Helpers ----------
static void ApplyClip(const RECT& r) { ClipCursor(&r); }
static void ReleaseClip()             { ClipCursor(nullptr); }

static void SetTrayTooltip(const wchar_t* text)
{
    wcsncpy_s(g_nid.szTip, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void Unlock()
{
    g_locked = false;
    // Preserve g_mode and g_trackedWnd so ToggleLock can re-arm the previous mode.
    RemoveHook();
    ReleaseClip();
    SetTrayTooltip(L"Cursor Locker (idle)");
}

static void ReassertClip()
{
    if (!g_locked) return;

    if (g_mode == LockMode::Window)
    {
        RECT wr;
        if (!IsWindow(g_trackedWnd) || !GetWindowRect(g_trackedWnd, &wr))
        {
            Unlock();
            return;
        }
        g_lockedRect = wr; // keep hook rect in sync as window moves
        ApplyClip(wr);
    }
    else if (g_mode == LockMode::Rect)
    {
        ApplyClip(g_lockedRect);
    }

    InstallHook(); // no-op if already installed
}

static void LockToRect(const RECT& r, const wchar_t* label)
{
    g_mode = LockMode::Rect;
    g_lockedRect = r;
    g_trackedWnd = nullptr;
    g_locked = true;
    ReassertClip();

    wchar_t buf[128];
    swprintf_s(buf, L"Cursor Locker — locked (%ls)", label);
    SetTrayTooltip(buf);
}

static void LockToWindow(HWND hwnd)
{
    if (!IsWindow(hwnd)) return;
    g_mode = LockMode::Window;
    g_trackedWnd = hwnd;
    g_locked = true;
    ReassertClip();
    SetTrayTooltip(L"Cursor Locker — locked (window)");
}

static void ToggleLock()
{
    if (g_locked) { Unlock(); return; }

    // Re-arm the previous mode if we have one, otherwise default to primary.
    if (g_mode == LockMode::Rect && g_lockedRect.right > g_lockedRect.left)
    {
        g_locked = true;
        ReassertClip();
        SetTrayTooltip(L"Cursor Locker — locked (region)");
    }
    else if (g_mode == LockMode::Window && IsWindow(g_trackedWnd))
    {
        g_locked = true;
        ReassertClip();
        SetTrayTooltip(L"Cursor Locker — locked (window)");
    }
    else
    {
        RECT r;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
        // Use full primary monitor bounds, not work area, to match user expectation.
        HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi { sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi))
            LockToRect(mi.rcMonitor, L"primary monitor");
        else
            LockToRect(r, L"primary monitor");
    }
}

// ---------- Monitor enumeration ----------
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM)
{
    MONITORINFOEXW mi { };
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi))
    {
        MonInfo info;
        info.bounds = mi.rcMonitor;
        info.name = mi.szDevice;
        g_monitors.push_back(info);
    }
    return TRUE;
}

static void RefreshMonitorList()
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, 0);
}

// ---------- Region picker overlay ----------
//
// A layered, click-through-disabled, topmost window covering the entire
// virtual screen. The user drags a rectangle; on mouse-up we lock to it.
static const wchar_t* kOverlayClass = L"CursorLockerOverlay";

static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
    {
        SetCapture(hwnd);
        g_dragging = true;
        POINT p { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ClientToScreen(hwnd, &p);
        g_dragStart = g_dragCurrent = p;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            POINT p { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            g_dragCurrent = p;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
    {
        ReleaseCapture();
        g_dragging = false;
        RECT sel;
        sel.left   = (g_dragStart.x < g_dragCurrent.x) ? g_dragStart.x : g_dragCurrent.x;
        sel.top    = (g_dragStart.y < g_dragCurrent.y) ? g_dragStart.y : g_dragCurrent.y;
        sel.right  = (g_dragStart.x > g_dragCurrent.x) ? g_dragStart.x : g_dragCurrent.x;
        sel.bottom = (g_dragStart.y > g_dragCurrent.y) ? g_dragStart.y : g_dragCurrent.y;

        ShowWindow(hwnd, SW_HIDE);
        if (sel.right - sel.left > 4 && sel.bottom - sel.top > 4)
            LockToRect(sel, L"region");
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { ShowWindow(hwnd, SW_HIDE); g_dragging = false; }
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);

        // Fill with the dim color (the layered alpha makes it translucent).
        RECT cr; GetClientRect(hwnd, &cr);
        HBRUSH dim = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(dc, &cr, dim);
        DeleteObject(dim);

        if (g_dragging)
        {
            // Convert screen-space drag points back to client space for drawing.
            POINT a = g_dragStart, b = g_dragCurrent;
            ScreenToClient(hwnd, &a);
            ScreenToClient(hwnd, &b);
            RECT sel;
            sel.left   = (a.x < b.x) ? a.x : b.x;
            sel.top    = (a.y < b.y) ? a.y : b.y;
            sel.right  = (a.x > b.x) ? a.x : b.x;
            sel.bottom = (a.y > b.y) ? a.y : b.y;

            // "Punch out" the selected area to fully transparent.
            HBRUSH clearBrush = CreateSolidBrush(RGB(0, 0, 0)); // matches color key
            FillRect(dc, &sel, clearBrush);
            DeleteObject(clearBrush);

            // Outline.
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 80, 80));
            HGDIOBJ oldPen = SelectObject(dc, pen);
            HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, sel.left, sel.top, sel.right, sel.bottom);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(pen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // we handle painting
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void RegisterOverlayClass(HINSTANCE hInst)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = OverlayProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_CROSS);
    wc.lpszClassName = kOverlayClass;
    RegisterClassW(&wc);
}

static void ShowRegionPicker(HINSTANCE hInst)
{
    int x  = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cx = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int cy = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (!g_overlayWnd)
    {
        g_overlayWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kOverlayClass, L"Pick region",
            WS_POPUP,
            x, y, cx, cy,
            nullptr, nullptr, hInst, nullptr);
    }
    else
    {
        SetWindowPos(g_overlayWnd, HWND_TOPMOST, x, y, cx, cy, SWP_NOACTIVATE);
    }

    // Color key: pure black (RGB 0,0,0) becomes fully transparent — used for
    // the "hole" inside the selection rectangle. Everything else gets ~40%
    // opacity so the user can still see what's underneath.
    SetLayeredWindowAttributes(g_overlayWnd, RGB(0, 0, 0), 110, LWA_COLORKEY | LWA_ALPHA);

    ShowWindow(g_overlayWnd, SW_SHOW);
    SetForegroundWindow(g_overlayWnd);
    SetFocus(g_overlayWnd);
}

// ---------- Autostart ----------
static const wchar_t* kRunKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunVal = L"CursorLocker";

static bool IsAutoStartEnabled()
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return false;
    DWORD size = 0;
    bool found = RegQueryValueExW(hk, kRunVal, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(hk);
    return found;
}

static void SetAutoStart(bool enable)
{
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS)
        return;
    if (enable)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t val[MAX_PATH + 3];
        swprintf_s(val, L"\"%ls\"", exe);
        RegSetValueExW(hk, kRunVal, 0, REG_SZ,
                       (const BYTE*)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    }
    else
    {
        RegDeleteValueW(hk, kRunVal);
    }
    RegCloseKey(hk);
}

// ---------- Saved regions ----------
static const wchar_t* kRegionsKey  = L"SOFTWARE\\CursorLocker\\Regions";
static const int      kMaxSavedRegions = 20;

static std::vector<SavedRegion> LoadSavedRegions()
{
    std::vector<SavedRegion> result;
    HKEY hk;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegionsKey, 0, KEY_QUERY_VALUE, &hk) != ERROR_SUCCESS)
        return result;

    for (int i = 0; i < kMaxSavedRegions; ++i)
    {
        wchar_t valName[8];
        swprintf_s(valName, L"%d", i);
        wchar_t data[64] = {};
        DWORD size = sizeof(data);
        if (RegQueryValueExW(hk, valName, nullptr, nullptr, (LPBYTE)data, &size) != ERROR_SUCCESS)
            break;

        LONG l, t, r, b;
        if (swscanf_s(data, L"%ld,%ld,%ld,%ld", &l, &t, &r, &b) != 4) continue;

        SavedRegion sr;
        sr.rect = { l, t, r, b };
        swprintf_s(sr.name, L"%ldx%ld at (%ld,%ld)", r - l, b - t, l, t);
        result.push_back(sr);
    }
    RegCloseKey(hk);
    return result;
}

static void SaveCurrentRegion(const RECT& r)
{
    auto existing = LoadSavedRegions();
    for (const auto& sr : existing)
        if (EqualRect(&sr.rect, &r)) return; // already saved

    if ((int)existing.size() >= kMaxSavedRegions) return;

    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegionsKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;

    wchar_t valName[8];
    swprintf_s(valName, L"%d", (int)existing.size());
    wchar_t data[64];
    swprintf_s(data, L"%ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
    RegSetValueExW(hk, valName, 0, REG_SZ,
                   (const BYTE*)data, (DWORD)((wcslen(data) + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}

static void ClearAllRegions()
{
    RegDeleteTreeW(HKEY_CURRENT_USER, kRegionsKey);
    g_savedRegions.clear();
}

// ---------- Tray menu ----------
static void ShowTrayMenu(HWND owner)
{
    POINT pt; GetCursorPos(&pt);
    RefreshMonitorList();
    g_savedRegions = LoadSavedRegions();

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_LOCK_PRIMARY, L"Lock to primary monitor");

    HMENU monSub = CreatePopupMenu();
    for (size_t i = 0; i < g_monitors.size() && i < 100; ++i)
    {
        wchar_t buf[256];
        const RECT& b = g_monitors[i].bounds;
        swprintf_s(buf, L"%ls  (%ldx%ld)",
                   g_monitors[i].name.c_str(),
                   b.right - b.left, b.bottom - b.top);
        AppendMenuW(monSub, MF_STRING, IDM_MONITOR_BASE + i, buf);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)monSub, L"Lock to monitor");

    AppendMenuW(menu, MF_STRING, IDM_LOCK_REGION, L"Lock to custom region...");

    HMENU savedSub = CreatePopupMenu();
    if (g_savedRegions.empty())
    {
        AppendMenuW(savedSub, MF_STRING | MF_GRAYED, 0, L"(no saved regions)");
    }
    else
    {
        for (size_t i = 0; i < g_savedRegions.size(); ++i)
            AppendMenuW(savedSub, MF_STRING, IDM_SAVED_BASE + (UINT)i, g_savedRegions[i].name);
        AppendMenuW(savedSub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(savedSub, MF_STRING, IDM_CLEAR_REGIONS, L"Clear all");
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)savedSub, L"Saved regions");

    AppendMenuW(menu, MF_STRING, IDM_LOCK_WINDOW, L"Lock to active window (3s)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (g_mode == LockMode::Rect && g_lockedRect.right > g_lockedRect.left)
    {
        AppendMenuW(menu, MF_STRING, IDM_SAVE_REGION, L"Save current region");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Toggle lock\tCtrl+Alt+L");
    AppendMenuW(menu, MF_STRING, IDM_UNLOCK, L"Unlock");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    UINT autostartFlags = MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(menu, autostartFlags, IDM_AUTOSTART, L"Start on login");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT,   L"Exit");

    // Required so the menu dismisses correctly when clicking elsewhere.
    SetForegroundWindow(owner);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(menu);
}

// ---------- Main window proc ----------
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_REASSERT, 250, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_REASSERT) ReassertClip();
        else if (wp == TIMER_FOCUS_GRAB)
        {
            KillTimer(hwnd, TIMER_FOCUS_GRAB);
            HWND target = GetForegroundWindow();
            if (target && target != hwnd) LockToWindow(target);
        }
        return 0;

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE) ToggleLock();
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            ToggleLock();
        return 0;

    case WM_COMMAND:
    {
        WORD id = LOWORD(wp);
        if (id == IDM_LOCK_PRIMARY)
        {
            HMONITOR hm = MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi { sizeof(mi) };
            if (GetMonitorInfoW(hm, &mi)) LockToRect(mi.rcMonitor, L"primary monitor");
        }
        else if (id == IDM_LOCK_REGION)
        {
            ShowRegionPicker((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        }
        else if (id == IDM_LOCK_WINDOW)
        {
            SetTrayTooltip(L"Focus target window — locking in 3s…");
            SetTimer(hwnd, TIMER_FOCUS_GRAB, 3000, nullptr);
        }
        else if (id == IDM_TOGGLE)        ToggleLock();
        else if (id == IDM_UNLOCK)        Unlock();
        else if (id == IDM_AUTOSTART)     SetAutoStart(!IsAutoStartEnabled());
        else if (id == IDM_SAVE_REGION)   SaveCurrentRegion(g_lockedRect);
        else if (id == IDM_CLEAR_REGIONS) ClearAllRegions();
        else if (id == IDM_EXIT)          DestroyWindow(hwnd);
        else if (id >= IDM_MONITOR_BASE && id < IDM_MONITOR_BASE + (int)g_monitors.size())
        {
            const auto& m = g_monitors[id - IDM_MONITOR_BASE];
            LockToRect(m.bounds, m.name.c_str());
        }
        else if (id >= IDM_SAVED_BASE && id < IDM_SAVED_BASE + (int)g_savedRegions.size())
        {
            const auto& sr = g_savedRegions[id - IDM_SAVED_BASE];
            LockToRect(sr.rect, sr.name);
        }
        return 0;
    }

    case WM_DISPLAYCHANGE:
        // Resolution / monitor topology changed: re-apply if locked.
        ReassertClip();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REASSERT);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        RemoveHook();
        ReleaseClip();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------- Entry point ----------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // Single-instance guard.
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\CursorLocker_SingleInstance");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // Make us per-monitor DPI aware so coordinates match what the user sees
    // on mixed-DPI multi-monitor setups.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Register the hidden main window class.
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"CursorLockerMain";
    RegisterClassW(&wc);

    g_mainWnd = CreateWindowExW(0, L"CursorLockerMain", L"CursorLocker",
                                0, 0, 0, 0, 0,
                                HWND_MESSAGE, // message-only window
                                nullptr, hInst, nullptr);

    RegisterOverlayClass(hInst);

    // Tray icon.
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_mainWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    wcscpy_s(g_nid.szTip, L"Cursor Locker (idle)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Global hotkey: Ctrl+Alt+L.
    RegisterHotKey(g_mainWnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_ALT, 'L');

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
