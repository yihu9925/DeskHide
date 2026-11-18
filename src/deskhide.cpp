// DeskHide - double-click empty desktop to hide/show desktop icons
// Native Win32, no runtime dependencies, tiny memory footprint.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <cstdlib>
#include <cstdarg>
#include "resource.h"

// ---------------------------------------------------------------- constants
static const wchar_t* kClassName = L"DeskHideMainWindow";
static const wchar_t* kMutexName = L"Local\\DeskHide_SingleInstance_9F3A2C";
static const wchar_t* kRunKey    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue  = L"DeskHide";

static const UINT WM_TRAY_MSG   = WM_APP + 1;
static const UINT WM_TOGGLE_CMD = WM_APP + 2;
static const UINT TIMER_POLL    = 1;

// ---------------------------------------------------------------- state
static HINSTANCE g_inst = nullptr;
static HWND      g_wnd = nullptr;
static HHOOK     g_hook = nullptr;
static HWND      g_lv = nullptr;              // desktop SysListView32
static HWND      g_dv = nullptr;              // desktop SHELLDLL_DefView
static UINT      g_taskbarCreated = 0;
static bool      g_logOn = false;
static bool      g_trayAdded = false;
static bool      g_weHid = false;             // we hid the icons -> restore on exit
static bool      g_pollMode = false;
static bool      g_btnDown = false;
static bool      g_dblSeen = false;
static std::wstring g_logPath;

// previous left-button-down state
static DWORD g_prevTime = 0;
static POINT g_prevPt = {0, 0};
static bool  g_prevDesktop = false;

// ---------------------------------------------------------------- logging
static std::wstring ExePath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    buf[(n < (sizeof(buf) / sizeof(buf[0]))) ? n : 0] = 0;
    return std::wstring(buf);
}

static void InitLogPath(const wchar_t* overridePath) {
    if (overridePath && *overridePath) {   // diagnostics: caller chose a writable file
        g_logPath = overridePath;
        return;
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? (std::wstring(buf) + L"\\DeskHide")
                                               : (std::wstring(L"."));
    CreateDirectoryW(dir.c_str(), nullptr);
    g_logPath = dir + L"\\deskhide.log";
}

static void Log(const wchar_t* fmt, ...) {
    if (!g_logOn) return;
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(msg, 1024, fmt, ap);
    va_end(ap);
    msg[1023] = 0;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[64];
    swprintf(prefix, 64, L"%02d:%02d:%02d.%03d  ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    prefix[63] = 0;

    std::wstring line = std::wstring(prefix) + msg + L"\r\n";

    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }
}

// ---------------------------------------------------------------- desktop windows
struct FindCtx { HWND dv; HWND lv; };

static BOOL CALLBACK EnumChildFindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (_wcsicmp(cls, L"SHELLDLL_DefView") == 0) {
        c->dv = h;
        HWND lv = FindWindowExW(h, nullptr, L"SysListView32", nullptr);
        if (lv) { c->lv = lv; return FALSE; }
    }
    return TRUE;
}

static BOOL CALLBACK EnumTopFindProc(HWND h, LPARAM lp) {
    FindCtx* c = (FindCtx*)lp;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64)) return TRUE;
    if (_wcsicmp(cls, L"Progman") != 0 && _wcsicmp(cls, L"WorkerW") != 0) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    EnumChildWindows(h, EnumChildFindProc, lp);
    return c->lv ? FALSE : TRUE;
}

static void RefreshDesktopHandles(bool force) {
    if (!force && g_lv && IsWindow(g_lv)) {
        if (!g_dv || !IsWindow(g_dv)) g_dv = GetParent(g_lv);
        return;
    }
    FindCtx c = { nullptr, nullptr };
    EnumWindows(EnumTopFindProc, (LPARAM)&c);
    g_dv = c.dv;
    g_lv = c.lv;
    Log(L"refresh handles: dv=%p lv=%p", (void*)g_dv, (void*)g_lv);
}

static bool IsDesktopRoot(HWND root) {
    wchar_t cls[64];
    if (!root || !GetClassNameW(root, cls, 64)) return false;
    return _wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0;
}

static bool IsDesktopPoint(POINT pt) {
    HWND h = WindowFromPoint(pt);
    if (!h) return false;
    return IsDesktopRoot(GetAncestor(h, GA_ROOT));
}

// 0 = nothing selected, 1 = something selected, -1 = unknown
static int DesktopSelectionState() {
    RefreshDesktopHandles(false);
    if (!g_lv) return -1;
    DWORD_PTR result = 0;
    LRESULT ok = SendMessageTimeoutW(g_lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result);
    if (!ok) return -1;
    return (result == (DWORD_PTR)-1) ? 0 : 1;
}

static bool IconsVisible() {
    RefreshDesktopHandles(false);
    return g_lv && IsWindowVisible(g_lv);
}

static void NotifyBalloon(const wchar_t* text);

// Toggle through Explorer's own "show desktop icons" command so the state is
// native and survives Explorer restarts; fall back to ShowWindow if needed.
static bool ApplyIconsVisible(bool show) {
    RefreshDesktopHandles(true);
    if (!g_lv || !g_dv) { Log(L"apply: desktop windows not found"); return false; }

    bool vis = IsWindowVisible(g_lv) != FALSE;
    if (vis == show) { return true; }

    DWORD_PTR r = 0;
    SetLastError(0);
    LRESULT sendOk = SendMessageTimeoutW(g_dv, WM_COMMAND, 0x7402, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &r);
    DWORD sendErr = GetLastError();
    Log(L"apply: WM_COMMAND 0x7402 dv=%p ok=%d r=%p err=%lu",
        (void*)g_dv, (int)sendOk, (void*)r, (unsigned long)sendErr);

    // Access denied means UIPI dropped it: do not burn a second waiting.
    bool hopeless = (!sendOk && sendErr == ERROR_ACCESS_DENIED);
    for (int i = 0; !hopeless && i < 40; ++i) {
        if (!g_lv || !IsWindow(g_lv)) { RefreshDesktopHandles(true); }
        if (g_lv && IsWindow(g_lv) && ((IsWindowVisible(g_lv) != FALSE) == show)) {
            g_weHid = !show;
            Log(L"apply: native toggle ok, visible=%d", (int)show);
            return true;
        }
        Sleep(25);
    }

    // fallback: plain window visibility
    RefreshDesktopHandles(true);
    if (g_lv) {
        SetLastError(0);
        BOOL sw = ShowWindow(g_lv, show ? SW_SHOW : SW_HIDE);
        Log(L"apply: ShowWindow(%d)=%d err=%lu visible_now=%d",
            (int)show, (int)sw, (unsigned long)GetLastError(),
            (int)(IsWindowVisible(g_lv) != FALSE));
        if (((IsWindowVisible(g_lv) != FALSE) == show)) {
            g_weHid = !show;
            Log(L"apply: fallback ShowWindow ok, visible=%d", (int)show);
            return true;
        }
        Log(L"apply: failed, visible=%d", (int)(IsWindowVisible(g_lv) != FALSE));
    }
    return false;
}

static void ToggleDesktopIcons() {
    RefreshDesktopHandles(true);
    if (!g_lv) { Log(L"toggle: desktop listview not found"); return; }
    bool vis = IconsVisible();
    Log(L"toggle: visible before=%d", (int)vis);
    if (!ApplyIconsVisible(!vis))
        NotifyBalloon(L"无法切换桌面图标：权限不足。请以管理员身份运行 DeskHide。");
}

static void RestoreIconsIfWeHid() {
    if (!g_weHid) return;
    Log(L"exit: restoring icons");
    ApplyIconsVisible(true);
    g_weHid = false;
}

// -------------------------------------------------- desktop access check
// 1 = we may talk to Explorer's desktop, 0 = access denied, -1 = no desktop yet
static int DesktopAccessProbe() {
    RefreshDesktopHandles(true);
    // WM_NULL is whitelisted through UIPI, so probe with a real ListView message
    // that has no side effects: ask for the first selected item.
    if (!g_lv) return -1;
    DWORD_PTR r = 0;
    SetLastError(0);
    LRESULT ok = SendMessageTimeoutW(g_lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED,
                                     SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &r);
    if (ok) return 1;
    return (GetLastError() == ERROR_ACCESS_DENIED) ? 0 : -1;
}

// ---------------------------------------------------------------- autostart
static bool IsAutostart() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool exists = (RegQueryValueExW(k, kRunValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS)
                  && type == REG_SZ;
    RegCloseKey(k);
    return exists;
}

static void SetAutostart(bool on) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ,
                       (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        Log(L"autostart: enabled -> %ls", cmd.c_str());
    } else {
        RegDeleteValueW(k, kRunValue);
        Log(L"autostart: disabled");
    }
    RegCloseKey(k);
}

// ---------------------------------------------------------------- tray
static void AddTrayIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_MSG;
    nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(nid.szTip, L"DeskHide - 双击桌面空白处隐藏/显示图标", 128);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_trayAdded = true;
        Log(L"tray: added");
    } else {
        Log(L"tray: add failed");
    }
}

static void RemoveTrayIcon() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

static void NotifyBalloon(const wchar_t* text) {
    if (!g_trayAdded || !g_wnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(nid.szInfoTitle, L"DeskHide", 64);
    lstrcpynW(nid.szInfo, text, 256);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ShowTrayMenu() {
    RefreshDesktopHandles(false);
    bool hidden = !IconsVisible();

    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, hidden ? L"显示桌面图标" : L"隐藏桌面图标");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetMenuDefaultItem(m, IDM_TOGGLE, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------------------------------------------------------- input
// Returns true when this click completed a double-click on the bare desktop.
static bool HandleClick(POINT pt, DWORD msgTime) {
    if (!IsDesktopPoint(pt)) {
        g_prevDesktop = false;
        return false;
    }

    int sel = DesktopSelectionState();   // state BEFORE this click is processed
    bool noSel = (sel == 0);

    int halfW = GetSystemMetrics(SM_CXDOUBLECLK) / 2;
    int halfH = GetSystemMetrics(SM_CYDOUBLECLK) / 2;
    // Two quick clicks on the desktop while nothing is selected.  A click that
    // lands on an icon leaves it selected, so its follow-up click fails noSel.
    bool isDouble = g_prevDesktop && noSel
                 && (DWORD)(msgTime - g_prevTime) <= GetDoubleClickTime()
                 && abs(pt.x - g_prevPt.x) <= halfW
                 && abs(pt.y - g_prevPt.y) <= halfH;

    if (isDouble) {
        Log(L"double-click at (%ld,%ld) desktop, no selection -> toggle", pt.x, pt.y);
        g_prevDesktop = false;
        return true;
    }

    Log(L"click at (%ld,%ld) desktop sel=%d", pt.x, pt.y, sel);
    g_prevDesktop = true;
    g_prevTime = msgTime;
    g_prevPt = pt;
    return false;
}

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
        if (HandleClick(ms->pt, ms->time)) {
            // Never touch Explorer from inside a global hook: post instead.
            PostMessageW(g_wnd, WM_TOGGLE_CMD, 0, 0);
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

static void PollTick() {
    bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down && !g_btnDown) {
        POINT pt;
        GetCursorPos(&pt);
        if (HandleClick(pt, GetTickCount())) {
            PostMessageW(g_wnd, WM_TOGGLE_CMD, 0, 0);
        }
    }
    g_btnDown = down;
}

// ---------------------------------------------------------------- window proc
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_taskbarCreated != 0 && msg == g_taskbarCreated) {
        Log(L"explorer restarted");
        g_trayAdded = false;
        RefreshDesktopHandles(true);
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDM_TOGGLE:    ToggleDesktopIcons(); return 0;
        case IDM_AUTOSTART: SetAutostart(!IsAutostart()); return 0;
        case IDM_EXIT:      DestroyWindow(h); return 0;
        }
        return 0;

    case WM_TRAY_MSG:
        switch (LOWORD(l)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            return 0;
        case WM_LBUTTONDBLCLK:
            g_dblSeen = true;
            return 0;
        case WM_LBUTTONUP:
            if (g_dblSeen) { g_dblSeen = false; }
            else           { ToggleDesktopIcons(); }
            return 0;
        }
        return 0;

    case WM_TOGGLE_CMD:
        ToggleDesktopIcons();
        return 0;

    case WM_TIMER:
        if (w == TIMER_POLL && g_pollMode) PollTick();
        return 0;

    case WM_QUERYENDSESSION:
        // Log off / shut down: give the icons back before Explorer goes away.
        RestoreIconsIfWeHid();
        return TRUE;

    case WM_ENDSESSION:
        if (w) RestoreIconsIfWeHid();
        return 0;

    case WM_CLOSE:
        RestoreIconsIfWeHid();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

// ---------------------------------------------------------------- entry
static bool HasArg(LPWSTR* argv, int argc, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return true;
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    const wchar_t* logFileArg = nullptr;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--logfile") == 0) { logFileArg = argv[i + 1]; break; }
    }
    InitLogPath(logFileArg);

    bool optLog     = HasArg(argv, argc, L"--log");
    bool optToggle  = HasArg(argv, argc, L"--toggle");
    bool optHide    = HasArg(argv, argc, L"--hide");
    bool optShow    = HasArg(argv, argc, L"--show") || HasArg(argv, argc, L"--restore");
    bool optAuto    = HasArg(argv, argc, L"--autostart");
    bool optNoAuto  = HasArg(argv, argc, L"--no-autostart");
    bool optExit    = HasArg(argv, argc, L"--exit");

    g_logOn = optLog;
    Log(L"----- start: %ls", GetCommandLineW());

    bool plainRun = !optToggle && !optHide && !optShow && !optAuto && !optNoAuto && !optExit;
    if (plainRun && DesktopAccessProbe() == 0) {
        // A sandbox or filtered token dropped us below Explorer's integrity
        // level; Explorer would silently ignore every message we send it.
        Log(L"startup: desktop access denied");
        MessageBoxW(nullptr,
            L"DeskHide 无法访问桌面（权限不足）。\n\n"
            L"请直接双击 DeskHide.exe 启动，或以管理员身份运行。",
            L"DeskHide", MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        if (argv) LocalFree(argv);
        return 0;
    }

    if (optExit) {
        HWND w = FindWindowW(kClassName, nullptr);
        if (w) PostMessageW(w, WM_CLOSE, 0, 0);
        if (argv) LocalFree(argv);
        return 0;
    }

    if (optToggle || optHide || optShow) {
        if (optToggle) ToggleDesktopIcons();
        else if (optHide) ApplyIconsVisible(false);
        else ApplyIconsVisible(true);
        if (argv) LocalFree(argv);
        return 0;
    }

    if (optAuto || optNoAuto) {
        SetAutostart(optAuto);
        if (argv) LocalFree(argv);
        return 0;
    }

    // single instance
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (argv) LocalFree(argv);
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    if (!RegisterClassExW(&wc)) {
        if (argv) LocalFree(argv);
        return 1;
    }

    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"DeskHide",
                            WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, inst, nullptr);
    if (!g_wnd) {
        if (argv) LocalFree(argv);
        return 1;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    RefreshDesktopHandles(true);
    AddTrayIcon();

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, inst, 0);
    if (!g_hook) {
        Log(L"hook failed, falling back to polling");
        g_pollMode = true;
        SetTimer(g_wnd, TIMER_POLL, 10, nullptr);
    } else {
        Log(L"hook installed");
    }

    if (argv) LocalFree(argv);
    argv = nullptr;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return 0;
}
