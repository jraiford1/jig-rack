/*
 * Jig Rack -- a rack of small, purpose-built Windows tools ("jigs") in one notification-area app.
 *   - Mouse battery jig (mouse.c): battery level and charging state of a Logitech wireless mouse, without G HUB.
 *   - Hot corner jig (hotcorner.c): rest the cursor in a screen corner to lock the PC, sleep, show the desktop...
 * Right-click the tray icon for Settings (settingswnd.c), saved in %APPDATA%\Jig Rack\settings.ini (settings.c).
 *
 * Copyright (C) 2026 Jon Raiford
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. It is distributed WITHOUT ANY WARRANTY; see the LICENSE file for details.
 *
 * Build: build.cmd (needs the Visual Studio C++ tools). Libraries come in via #pragma comment(lib, ...).
 */
#include "jigrack.h"
#include "resource.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")

#define ID_SETTINGS 1
#define ID_EXIT     2

/* ---------------------------------------------------------------- log ----------------------------------------- */
/* What the app saw (settings, receiver, device, events, state changes, corners, actions), next to the exe.
   Recreated at each start; only changes are written, so it stays tiny. Long lines are cut, never fatal. */
HANDLE g_log = INVALID_HANDLE_VALUE;

void trace(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, sizeof line, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(line + n, sizeof line - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    n += m >= 0 ? m : (int)strlen(line + n);
    line[n++] = '\r'; line[n++] = '\n';
    DWORD w;
    WriteFile(g_log, line, n, &w, NULL);
}

static void open_log(const wchar_t *path)
{
    g_log = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* ---------------------------------------------------------------- tray icon ------------------------------------ */

static HINSTANCE g_inst;
static NOTIFYICONDATAW g_nid;
static HICON g_icon;
static UINT g_taskbarCreated;
static Battery g_shown = { 0, 0, 0, 0, 0, L"reading\x2026" };   /* the last reading from the monitor thread */

/* The battery meter when a mouse is chosen and "show the meter" is on; the Jig Rack icon otherwise. */
void tray_refresh(void)
{
    if (!g_nid.cbSize) return;
    BOOL chosen = mouse_chosen(&g_set);
    HICON icon = chosen && g_set.showMeter ? icon_meter(&g_shown) : icon_app(0);
    wchar_t tip[128];
    const Battery *b = &g_shown;
    if (!chosen) wcscpy_s(tip, 128, APP_NAME);
    else if (b->ok && b->stale)
        swprintf_s(tip, 128, L"%s\nMouse battery %s%d%% (last reading; %s)", APP_NAME, b->approx ? L"~" : L"", b->percent, b->state);
    else if (b->ok)
        swprintf_s(tip, 128, L"%s\nMouse battery %s%d%% - %s", APP_NAME, b->approx ? L"~" : L"", b->percent, b->state);
    else
        swprintf_s(tip, 128, L"%s\nMouse battery: %s", APP_NAME, b->state);
    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (g_icon) DestroyIcon(g_icon);
    g_icon = icon;
}

static void add_tray(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    if (!g_icon) g_icon = icon_app(0);
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    tray_refresh();
}

static void tray_menu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"&Settings\x2026");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"E&xit");
    SetMenuDefaultItem(m, ID_SETTINGS, FALSE);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == ID_SETTINGS) settings_window_open(g_inst);
    else if (cmd == ID_EXIT) DestroyWindow(hwnd);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated) { add_tray(hwnd); return 0; }  /* Explorer restarted */
    switch (msg) {
    case WM_CREATE:
        add_tray(hwnd);
        hotcorner_init(g_inst, hwnd);
        hotcorner_apply();
        mouse_start(hwnd);
        return 0;
    case WM_RESULT: {
        Battery *b = (Battery *)lp;
        g_shown = *b;
        HeapFree(GetProcessHeap(), 0, b);
        tray_refresh();
        return 0;
    }
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK: settings_window_open(g_inst); break;
        case WM_RBUTTONUP: case WM_CONTEXTMENU: tray_menu(hwnd); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_CORNER) hotcorner_tick();
        return 0;
    case WM_DISPLAYCHANGE:                                  /* monitor added, removed or rearranged */
        hotcorner_layout_changed();
        if (settings_window()) PostMessageW(settings_window(), WM_DISPLAYCHANGE, wp, lp);
        break;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_CORNER);
        if (settings_window()) DestroyWindow(settings_window());
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) DestroyIcon(g_icon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------------------------------------------------------------- start ---------------------------------------- */

static const wchar_t *arg_after(int argc, LPWSTR *argv, const wchar_t *flag)
{
    for (int i = 1; i + 1 < argc; i++) if (wcscmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

static BOOL has_arg(int argc, LPWSTR *argv, const wchar_t *flag)
{
    for (int i = 1; i < argc; i++) if (wcscmp(argv[i], flag) == 0) return TRUE;
    return FALSE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show_cmd)
{
    (void)prev; (void)cmd; (void)show_cmd;
    g_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   /* real sizes, not scaled */
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_TAB_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *v;
    if (argv && (v = arg_after(argc, argv, L"--settings")) != NULL) settings_use_file(v);

    /* Diagnostics: each runs alongside the tray app and exits. */
    if (argv && (v = arg_after(argc, argv, L"--selftest")) != NULL) {
        open_log(v);
        int fails = hotcorner_selftest();
        CloseHandle(g_log);
        return fails ? 2 : 0;
    }
    if (argv && (v = arg_after(argc, argv, L"--devices")) != NULL) {
        open_log(v);
        DeviceList l;
        trace("found %d Logitech mouse/mice", mouse_list_devices(&l));
        trace("found %d Bluetooth device(s) reporting a battery level", bt_list_devices(&l));
        CloseHandle(g_log);
        return 0;
    }
    if (argv && has_arg(argc, argv, L"--exit")) {          /* close the running copy cleanly (for upgrades) */
        HWND w = FindWindowW(L"JigRackTrayWindow", NULL);
        if (!w) return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(w, WM_CLOSE, 0, 0);
        DWORD r = p ? WaitForSingleObject(p, 10000) : WAIT_FAILED;
        if (p) CloseHandle(p);
        return r == WAIT_OBJECT_0 ? 0 : 1;
    }
    if (!icons_startup()) return 1;
    if (argv && has_arg(argc, argv, L"--demo-countdown")) {
        settings_load(&g_set);
        hotcorner_init(inst, NULL);
        hotcorner_demo_countdown();
        return 0;
    }
    if (argv && (v = arg_after(argc, argv, L"--preview")) != NULL) { icons_write_previews(v); return 0; }
    if (argv && (v = arg_after(argc, argv, L"--write-ico")) != NULL) return icons_write_ico(v) ? 0 : 1;
    if (argv && (v = arg_after(argc, argv, L"--snap-settings")) != NULL) {
        wchar_t log[MAX_PATH];
        swprintf_s(log, MAX_PATH, L"%s\\snap.log", v);
        open_log(log);
        settings_load(&g_set);
        int r = settings_window_snapshots(inst, v);
        CloseHandle(g_log);
        return r;
    }

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\JigRack");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    wchar_t logpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, logpath, MAX_PATH);
    while (len && logpath[len - 1] != L'\\') len--;
    logpath[len] = 0;
    wcscat_s(logpath, MAX_PATH, L"jigrack.log");
    open_log(logpath);
    trace("Jig Rack started");
    settings_load(&g_set);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"JigRackTrayWindow";
    RegisterClassW(&wc);
    /* A hidden top-level tool window, not a message-only one: only top-level windows receive the TaskbarCreated
       broadcast, which is how the icon comes back after Explorer restarts. Never shown, not on the taskbar. */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, APP_NAME, WS_POPUP, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        HWND dlg = settings_window();
        if (dlg && IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
