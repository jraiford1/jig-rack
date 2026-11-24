/* Jig Rack -- the Workspace jig.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * One click (the tray menu's "Launch workspace") starts your standard apps and puts each window where the Workspace
 * tab says: on a chosen monitor, maximized, in a half or a third, or at a custom place. An app that's already open
 * isn't started again; its window is moved into place. Apps are started in order; each one's window is waited for
 * (up to a minute) and placed as soon as it shows, then placed once more a moment later, after the app has finished
 * restoring its own last position and adapting to the monitor's scaling. */
#include "jigrack.h"
#include <dwmapi.h>
#include <appmodel.h>
#include <shlwapi.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")

#define WAIT_MS 60000           /* how long to wait for a started app's window */
#define SETTLE_MS 1500          /* when to place it again */

const wchar_t *POSITION_LABELS[POSITIONS] = {
    L"Maximized", L"Left half", L"Right half", L"Top half", L"Bottom half",
    L"Left third", L"Middle third", L"Right third", L"Custom"
};

/* ---------------------------------------------------------------- where a window goes ------------------------- */

/* The rectangle for a position in a monitor's work area (custom: x, y, w, h in % of it). */
void workspace_target(const RECT *work, int pos, int x, int y, int w, int h, RECT *out)
{
    int W = work->right - work->left, H = work->bottom - work->top;
    RECT r = *work;
    switch (pos) {
    case POS_LEFT:         r.right = work->left + W / 2; break;
    case POS_RIGHT:        r.left = work->left + W / 2; break;
    case POS_TOP:          r.bottom = work->top + H / 2; break;
    case POS_BOTTOM:       r.top = work->top + H / 2; break;
    case POS_LEFT_THIRD:   r.right = work->left + W / 3; break;
    case POS_MIDDLE_THIRD: r.left = work->left + W / 3; r.right = work->left + 2 * W / 3; break;
    case POS_RIGHT_THIRD:  r.left = work->left + 2 * W / 3; break;
    case POS_CUSTOM:
        r.left = work->left + MulDiv(W, x, 100);
        r.top = work->top + MulDiv(H, y, 100);
        r.right = r.left + MulDiv(W, w, 100);
        r.bottom = r.top + MulDiv(H, h, 100);
        if (r.right > work->right) r.right = work->right;
        if (r.bottom > work->bottom) r.bottom = work->bottom;
        if (r.right - r.left < 100) r.right = r.left + 100;
        if (r.bottom - r.top < 100) r.bottom = r.top + 100;
        break;
    default: break;                                                   /* maximized: the whole work area */
    }
    *out = r;
}

/* The work area of the app's monitor; the main monitor if it's "" or isn't connected (then *missing). */
static void app_work_area(const App *a, RECT *work, BOOL *missing)
{
    MonitorDesc m[16];
    int n = monitors_list(m, 16);
    HMONITOR mon = MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    *missing = FALSE;
    if (a->monitor[0]) {
        int k;
        for (k = 0; k < n; k++) if (!_wcsicmp(m[k].path, a->monitor)) break;
        if (k < n) mon = MonitorFromRect(&m[k].r, MONITOR_DEFAULTTOPRIMARY);
        else *missing = TRUE;
    }
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    *work = mi.rcWork;
}

/* Put the window there: its visible frame on the target (windows have invisible resize borders around it). */
static BOOL place(HWND w, const App *a)
{
    RECT work, t;
    BOOL missing;
    app_work_area(a, &work, &missing);
    workspace_target(&work, a->pos, a->x, a->y, a->w, a->h, &t);
    RECT wr, fr;
    GetWindowRect(w, &wr);
    if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) fr = wr;
    if (IsZoomed(w) || IsIconic(w)) { fr = wr; }                       /* margins are only right when restored */
    RECT n = { t.left - (fr.left - wr.left), t.top - (fr.top - wr.top),
               t.right + (wr.right - fr.right), t.bottom + (wr.bottom - fr.bottom) };
    /* SetWindowPlacement moves a maximized or minimized window too; its coordinates are relative to the main
       monitor's work area. */
    MONITORINFO pm = { sizeof pm };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &pm);
    WINDOWPLACEMENT wp = { sizeof wp };
    GetWindowPlacement(w, &wp);
    wp.flags = 0;
    wp.rcNormalPosition = n;
    OffsetRect(&wp.rcNormalPosition, -pm.rcWork.left, -pm.rcWork.top);
    if (a->pos == POS_MAXIMIZED) {
        wp.rcNormalPosition = (RECT){ work.left + 40, work.top + 40, work.right - 40, work.bottom - 40 };
        OffsetRect(&wp.rcNormalPosition, -pm.rcWork.left, -pm.rcWork.top);
        if (IsZoomed(w)) {                                            /* maximized elsewhere: restore, then move */
            wp.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(w, &wp);
        }
        wp.showCmd = SW_SHOWMAXIMIZED;
    } else {
        wp.showCmd = SW_SHOWNOACTIVATE;
    }
    BOOL ok = SetWindowPlacement(w, &wp);
    DWORD err = ok ? 0 : GetLastError();
    if (ok && a->pos != POS_MAXIMIZED)
        ok = SetWindowPos(w, NULL, n.left, n.top, n.right - n.left, n.bottom - n.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    if (!ok && !err) err = GetLastError();
    if (!ok) trace("workspace: %ls: can't move its window (error %lu%s)", a->name, err,
                   err == ERROR_ACCESS_DENIED ? ", it runs as administrator" : "");
    if (missing) trace("workspace: %ls: its monitor (%ls) isn't connected; on the main one", a->name, a->monitorName);
    return ok;
}

/* ---------------------------------------------------------------- finding an app's window --------------------- */

static BOOL process_path(DWORD pid, wchar_t *out, DWORD cch)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return FALSE;
    BOOL ok = QueryFullProcessImageNameW(p, 0, out, &cch);
    CloseHandle(p);
    return ok;
}

/* A window a person would call "the app's window": visible, titled, not a tool window, not owned, on this desktop. */
static BOOL app_window(HWND w)
{
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || GetWindowTextLengthW(w) == 0) return FALSE;
    LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return FALSE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return FALSE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid != GetCurrentProcessId();
}

typedef struct { const App *a; HWND found; HWND *list; int n, max; } FindCtx;

static BOOL CALLBACK find_enum(HWND w, LPARAM lp)
{
    FindCtx *c = (FindCtx *)lp;
    if (!app_window(w)) return TRUE;
    if (c->list) { if (c->n < c->max) c->list[c->n++] = w; return TRUE; }
    DWORD pid = 0;
    wchar_t path[MAX_PATH];
    GetWindowThreadProcessId(w, &pid);
    if (!process_path(pid, path, MAX_PATH) || _wcsicmp(path, c->a->match)) return TRUE;
    if (c->a->title[0]) {
        wchar_t t[512];
        GetWindowTextW(w, t, 512);
        if (!StrStrIW(t, c->a->title)) return TRUE;
    }
    c->found = w;                                                     /* the top one, in z-order */
    return FALSE;
}

HWND workspace_find(const App *a)
{
    if (!a->match[0]) return NULL;
    FindCtx c = { a, NULL, NULL, 0, 0 };
    EnumWindows(find_enum, (LPARAM)&c);
    return c.found;
}

/* The open app windows, top first: for the Workspace tab's "Open app..." menu. */
int workspace_open_windows(HWND *out, int max)
{
    FindCtx c = { NULL, NULL, out, 0, max };
    EnumWindows(find_enum, (LPARAM)&c);
    return c.n;
}

/* The program's description ("Google Chrome") from its version resource; "" if none. */
static void file_description(const wchar_t *path, wchar_t *out, int cch)
{
    out[0] = 0;
    DWORD h = 0, size = GetFileVersionInfoSizeW(path, &h);
    if (!size) return;
    void *buf = HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    struct { WORD lang, cp; } *tr;
    UINT len;
    if (GetFileVersionInfoW(path, 0, size, buf) && VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len) {
        wchar_t key[64];
        wchar_t *v;
        swprintf_s(key, 64, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
        if (VerQueryValueW(buf, key, (void **)&v, &len) && len > 1) wcsncpy_s(out, cch, v, _TRUNCATE);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

/* The monitor a window is mostly on, as a device path and name. */
static void window_monitor(HWND w, wchar_t *path, int cchPath, wchar_t *name, int cchName, RECT *work)
{
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(mon, (MONITORINFO *)&mi);
    *work = mi.rcWork;
    path[0] = name[0] = 0;
    MonitorDesc m[16];
    int n = monitors_list(m, 16);
    for (int k = 0; k < n; k++)
        if (!_wcsicmp(m[k].gdi, mi.szDevice)) { wcscpy_s(path, cchPath, m[k].path); wcscpy_s(name, cchName, m[k].name); }
}

/* The window's current place as a position: maximized, or custom in % of its monitor's work area. */
static void current_position(HWND w, App *a)
{
    RECT work;
    window_monitor(w, a->monitor, ARRAYSIZE(a->monitor), a->monitorName, ARRAYSIZE(a->monitorName), &work);
    if (IsZoomed(w)) { a->pos = POS_MAXIMIZED; return; }
    RECT fr;
    if (IsIconic(w)) {
        WINDOWPLACEMENT wp = { sizeof wp };
        GetWindowPlacement(w, &wp);
        fr = wp.rcNormalPosition;
    } else if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) {
        GetWindowRect(w, &fr);
    }
    int W = work.right - work.left, H = work.bottom - work.top;
    a->pos = POS_CUSTOM;
    a->x = MulDiv(fr.left - work.left, 100, W);
    a->y = MulDiv(fr.top - work.top, 100, H);
    a->w = MulDiv(fr.right - fr.left, 100, W);
    a->h = MulDiv(fr.bottom - fr.top, 100, H);
    if (a->x < 0) a->x = 0;
    if (a->y < 0) a->y = 0;
    if (a->w < 1) a->w = 1;
    if (a->h < 1) a->h = 1;
    if (a->w > 100) a->w = 100;
    if (a->h > 100) a->h = 100;
}

/* Fill an app from an open window: what to start (the program, or the Store app's ID), how to find it, its name,
   monitor and current place. */
void workspace_app_from_window(HWND w, App *a)
{
    ZeroMemory(a, sizeof *a);
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    process_path(pid, a->match, MAX_PATH);
    wcscpy_s(a->command, ARRAYSIZE(a->command), a->match);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t aumid[APPLICATION_USER_MODEL_ID_MAX_LENGTH];
        UINT32 len = ARRAYSIZE(aumid);
        if (GetApplicationUserModelId(p, &len, aumid) == ERROR_SUCCESS)   /* a Store app: started by its ID */
            swprintf_s(a->command, ARRAYSIZE(a->command), L"shell:AppsFolder\\%s", aumid);
        CloseHandle(p);
    }
    file_description(a->match, a->name, ARRAYSIZE(a->name));
    if (!a->name[0]) GetWindowTextW(w, a->name, ARRAYSIZE(a->name));
    current_position(w, a);
}

/* "Use current position": the app's open window's place now. FALSE if it isn't open. */
BOOL workspace_use_current(App *a)
{
    HWND w = workspace_find(a);
    if (!w) return FALSE;
    current_position(w, a);
    return TRUE;
}

/* Fill name and match from a program file chosen with Browse. */
void workspace_app_from_file(const wchar_t *path, App *a)
{
    wcscpy_s(a->command, ARRAYSIZE(a->command), path);
    wcscpy_s(a->match, MAX_PATH, path);
    file_description(path, a->name, ARRAYSIZE(a->name));
    if (!a->name[0]) {
        const wchar_t *f = wcsrchr(path, L'\\');
        wcscpy_s(a->name, ARRAYSIZE(a->name), f ? f + 1 : path);
    }
}

/* ---------------------------------------------------------------- launching ----------------------------------- */

enum { ST_IDLE, ST_WAITING, ST_SETTLING, ST_DONE, ST_FAILED };
static struct { int state; HWND w; ULONGLONG until; } g_run[MAX_APPS];
static int g_count;
static HWND g_owner;

static void finish_if_done(void)
{
    int done = 0, failed = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_run[i].state == ST_WAITING || g_run[i].state == ST_SETTLING) return;
        if (g_run[i].state == ST_FAILED) failed++; else done++;
    }
    KillTimer(g_owner, TIMER_WORKSPACE);
    trace("workspace: done, %d placed, %d failed", done, failed);
    if (failed) {
        wchar_t text[256] = L"", *sep = L"";
        for (int i = 0; i < g_count; i++) {
            if (g_run[i].state != ST_FAILED) continue;
            if (wcslen(text) + wcslen(g_set.apps[i].name) + 3 >= ARRAYSIZE(text)) break;
            wcscat_s(text, ARRAYSIZE(text), sep);
            wcscat_s(text, ARRAYSIZE(text), g_set.apps[i].name);
            sep = L", ";
        }
        wcscat_s(text, ARRAYSIZE(text), L" didn't open in time.");
        tray_notify(FALSE, L"Workspace", text);
    }
}

void workspace_launch(HWND owner)
{
    g_owner = owner;
    g_count = g_set.appCount;
    trace("workspace: launching %d app(s)", g_count);
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_count; i++) {
        const App *a = &g_set.apps[i];
        g_run[i].w = workspace_find(a);
        if (g_run[i].w) {
            trace("workspace: %ls is open: placing it", a->name);
            place(g_run[i].w, a);
            g_run[i].state = ST_SETTLING;
            g_run[i].until = now + SETTLE_MS;
            continue;
        }
        if (!a->command[0]) { g_run[i].state = ST_FAILED; continue; }
        SHELLEXECUTEINFOW sei = { sizeof sei };
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        sei.lpFile = a->command;
        sei.lpParameters = a->args[0] ? a->args : NULL;
        sei.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&sei);
        trace("workspace: starting %ls (%ls %ls): %s", a->name, a->command, a->args, ok ? "ok" : "failed");
        g_run[i].state = ok ? ST_WAITING : ST_FAILED;
        g_run[i].until = now + WAIT_MS;
    }
    SetTimer(owner, TIMER_WORKSPACE, 250, NULL);
    finish_if_done();
}

void workspace_tick(void)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_count; i++) {
        const App *a = &g_set.apps[i];
        if (g_run[i].state == ST_WAITING) {
            HWND w = workspace_find(a);
            if (w) {
                trace("workspace: %ls opened: placing it", a->name);
                place(w, a);
                g_run[i].w = w;
                g_run[i].state = ST_SETTLING;
                g_run[i].until = now + SETTLE_MS;
            } else if (now >= g_run[i].until) {
                trace("workspace: %ls: no window after %d s", a->name, WAIT_MS / 1000);
                g_run[i].state = ST_FAILED;
            }
        } else if (g_run[i].state == ST_SETTLING && now >= g_run[i].until) {
            /* again: the app may have restored its own last place, or rescaled for the monitor */
            HWND w = IsWindow(g_run[i].w) ? g_run[i].w : workspace_find(a);
            if (w) place(w, a);
            g_run[i].state = ST_DONE;
        }
    }
    finish_if_done();
}
