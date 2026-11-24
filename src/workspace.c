/* Jig Rack -- the Workspace jig.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * One click (the tray menu's "Launch workspace") starts your standard apps and puts each window in place. Where each
 * app goes is kept per monitor setup: a saved set of monitors (how many, their sizes, where they sit, so landscape
 * and portrait differ), named like "2026-11-24-1". The launch uses the setup that matches the monitors connected
 * now. An app that's already open isn't started again; its window is moved. A started app's window is waited for
 * (up to a minute) and placed as soon as it shows, then once more a moment later, after the app has restored its
 * own last place and adapted to the monitor's scaling. */
#define COBJMACROS
#include "jigrack.h"
#include <dwmapi.h>
#include <appmodel.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

#define WAIT_MS 60000           /* how long to wait for a started app's window */
#define WAIT_PROMPT_MS 600000   /* ...while it shows a prompt (a password, say): up to 10 minutes after starting */
#define SETTLE_MS 1500          /* when to place it again */

const wchar_t *POSITION_LABELS[POSITIONS] = {
    L"Maximized", L"Left half", L"Right half", L"Top half", L"Bottom half",
    L"Left third", L"Middle third", L"Right third", L"Custom"
};

/* ---------------------------------------------------------------- positions ----------------------------------- */

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

/* Where a drop at p snaps on a monitor. Landscape: the top band is left half / maximized / right half, below it
   the thirds. Portrait: top half / maximized / bottom half from top to bottom. */
int workspace_snap(const RECT *mon, POINT p)
{
    int W = mon->right - mon->left, H = mon->bottom - mon->top;
    if (W <= 0 || H <= 0) return POS_MAXIMIZED;
    int fx = MulDiv(p.x - mon->left, 100, W), fy = MulDiv(p.y - mon->top, 100, H);   /* % across and down */
    if (W >= H) {
        if (fy < 30) return fx < 35 ? POS_LEFT : fx >= 65 ? POS_RIGHT : POS_MAXIMIZED;
        return fx < 33 ? POS_LEFT_THIRD : fx < 67 ? POS_MIDDLE_THIRD : POS_RIGHT_THIRD;
    }
    return fy < 30 ? POS_TOP : fy >= 70 ? POS_BOTTOM : POS_MAXIMIZED;
}

/* ---------------------------------------------------------------- monitor setups ------------------------------ */

int workspace_current_monitors(LayoutMon *out, int max)
{
    MonitorDesc m[MAX_LAYOUT_MONS];
    int n = monitors_list(m, max < MAX_LAYOUT_MONS ? max : MAX_LAYOUT_MONS);
    for (int k = 0; k < n; k++) {
        ZeroMemory(&out[k], sizeof out[k]);
        wcscpy_s(out[k].path, ARRAYSIZE(out[k].path), m[k].path);
        wcscpy_s(out[k].name, ARRAYSIZE(out[k].name), m[k].name);
        out[k].r = m[k].r;
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoW(MonitorFromRect(&m[k].r, MONITOR_DEFAULTTONEAREST), &mi);
        out[k].work = mi.rcWork;
    }
    return n;
}

/* Same monitors: as many, each in the same place at the same size (byPath: and the same device too). */
static BOOL same_monitors(const Layout *l, const LayoutMon *cur, int n, BOOL byPath)
{
    if (l->nmon != n) return FALSE;
    for (int i = 0; i < n; i++) {
        int k;
        for (k = 0; k < l->nmon; k++)
            if (EqualRect(&l->mons[k].r, &cur[i].r) && (!byPath || !_wcsicmp(l->mons[k].path, cur[i].path))) break;
        if (k == l->nmon) return FALSE;
    }
    return TRUE;
}

/* The saved setup for these monitors: the same devices in the same places, else any in the same places (a device
   path can change after a driver update). -1 if none. */
int workspace_find_layout(const LayoutMon *cur, int n)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < g_set.layoutCount; i++)
            if (same_monitors(&g_set.layouts[i], cur, n, pass == 0)) return i;
    return -1;
}

int workspace_current_layout(void)
{
    LayoutMon cur[MAX_LAYOUT_MONS];
    int n = workspace_current_monitors(cur, MAX_LAYOUT_MONS);
    int i = workspace_find_layout(cur, n);
    static int logged = -2;
    if (i != logged) {                                                /* say which, when it changes */
        logged = i;
        for (int k = 0; k < n; k++)
            trace("workspace: monitor %d now at %ld,%ld,%ld,%ld", k + 1, cur[k].r.left, cur[k].r.top, cur[k].r.right, cur[k].r.bottom);
        trace("workspace: saved setup for these monitors: %ls", i >= 0 ? g_set.layouts[i].name : L"(none)");
    }
    return i;
}

/* The monitors now, named with today's date and the next free number ("2026-11-24-2"). */
void workspace_capture_layout(Layout *l)
{
    ZeroMemory(l, sizeof *l);
    l->nmon = workspace_current_monitors(l->mons, MAX_LAYOUT_MONS);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t day[16];
    swprintf_s(day, 16, L"%04d-%02d-%02d-", t.wYear, t.wMonth, t.wDay);
    int next = 1;
    for (int i = 0; i < g_set.layoutCount; i++) {
        if (wcsncmp(g_set.layouts[i].name, day, 11)) continue;
        int k = _wtoi(g_set.layouts[i].name + 11);
        if (k >= next) next = k + 1;
    }
    swprintf_s(l->name, ARRAYSIZE(l->name), L"%s%d", day, next);
}

Place *workspace_place_of(Layout *l, int appId)
{
    for (int i = 0; i < l->nplace; i++) if (l->places[i].app == appId) return &l->places[i];
    return NULL;
}

void workspace_set_place(Layout *l, const Place *p)
{
    Place *q = workspace_place_of(l, p->app);
    if (!q && l->nplace < MAX_APPS) q = &l->places[l->nplace++];
    if (q) *q = *p;
}

void workspace_unplace(Layout *l, int appId)
{
    for (int i = 0; i < l->nplace; i++)
        if (l->places[i].app == appId) {
            l->places[i] = l->places[--l->nplace];
            return;
        }
}

void workspace_remove_app(int index)
{
    if (index < 0 || index >= g_set.appCount) return;
    int id = g_set.apps[index].id;
    for (int i = 0; i < g_set.layoutCount; i++) workspace_unplace(&g_set.layouts[i], id);
    MoveMemory(&g_set.apps[index], &g_set.apps[index + 1], (g_set.appCount - index - 1) * sizeof(App));
    g_set.appCount--;
}

/* ---------------------------------------------------------------- placing a window ---------------------------- */

/* Put the window on the target: its visible frame there (windows have invisible resize borders around it). */
static BOOL place(HWND w, const App *a, const RECT *work, const Place *p)
{
    RECT t;
    workspace_target(work, p->pos, p->x, p->y, p->w, p->h, &t);
    RECT wr, fr;
    GetWindowRect(w, &wr);
    if (IsZoomed(w) || IsIconic(w) || FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr)))
        fr = wr;                                                      /* margins are only right when restored */
    RECT n = { t.left - (fr.left - wr.left), t.top - (fr.top - wr.top),
               t.right + (wr.right - fr.right), t.bottom + (wr.bottom - fr.bottom) };
    /* An app that isn't per-monitor DPI aware (HCL Notes) is sized with the scaling of the monitor its top-left
       corner lands on: let its invisible border reach onto the monitor to the left or above and it's scaled for
       that one (1890 px asked became 3188). So it's sized with its corner on its own monitor (shifted right and
       down just enough), then only moved the few pixels into place, which doesn't rescale it. */
    RECT sized = n;
    BOOL shift = FALSE;
    if (GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(w)) != DPI_AWARENESS_PER_MONITOR_AWARE) {
        MONITORINFO mm = { sizeof mm };
        GetMonitorInfoW(MonitorFromRect(&t, MONITOR_DEFAULTTONEAREST), &mm);
        int dx = mm.rcMonitor.left > n.left ? mm.rcMonitor.left - n.left : 0;
        int dy = mm.rcMonitor.top > n.top ? mm.rcMonitor.top - n.top : 0;
        OffsetRect(&sized, dx, dy);
        shift = dx || dy;
    }
    /* SetWindowPlacement moves a maximized or minimized window too; its coordinates are relative to the main
       monitor's work area. */
    MONITORINFO pm = { sizeof pm };
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &pm);
    WINDOWPLACEMENT wp = { sizeof wp };
    GetWindowPlacement(w, &wp);
    wp.flags = 0;
    wp.rcNormalPosition = sized;
    if (p->pos == POS_MAXIMIZED) {
        wp.rcNormalPosition = (RECT){ work->left + 40, work->top + 40, work->right - 40, work->bottom - 40 };
        OffsetRect(&wp.rcNormalPosition, -pm.rcWork.left, -pm.rcWork.top);
        if (IsZoomed(w)) {                                            /* maximized elsewhere: restore, then move */
            wp.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(w, &wp);
        }
        wp.showCmd = SW_SHOWMAXIMIZED;
    } else {
        OffsetRect(&wp.rcNormalPosition, -pm.rcWork.left, -pm.rcWork.top);
        wp.showCmd = SW_SHOWNOACTIVATE;
    }
    BOOL ok = SetWindowPlacement(w, &wp);
    DWORD err = ok ? 0 : GetLastError();
    if (ok && p->pos != POS_MAXIMIZED) {
        UINT f = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
        ok = SetWindowPos(w, NULL, sized.left, sized.top, sized.right - sized.left, sized.bottom - sized.top, f);
        if (ok && shift) ok = SetWindowPos(w, NULL, n.left, n.top, 0, 0, f | SWP_NOSIZE);   /* in order: same queue */
    }
    if (!ok && !err) err = GetLastError();
    if (!ok) trace("workspace: %ls: can't move its window (error %lu%s)", a->name, err,
                   err == ERROR_ACCESS_DENIED ? ", it runs as administrator" : "");
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

/* A main window, not a prompt: resizable or maximizable, and not a dialog box. (A login prompt comes from the
   same program as the main window, before it.) */
static BOOL main_window(HWND w)
{
    wchar_t cls[32];
    GetClassNameW(w, cls, 32);
    LONG_PTR st = GetWindowLongPtrW(w, GWL_STYLE);
    return wcscmp(cls, L"#32770") && (st & (WS_THICKFRAME | WS_MAXIMIZEBOX));
}

/* Every process's parent, for "started by what we launched" (a launcher such as HCL Notes' notes.exe starts the
   program whose window it is). */
typedef struct { DWORD pid[1024], parent[1024]; int n; } ProcTree;

static void proc_tree(ProcTree *t)
{
    t->n = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W e = { sizeof e };
    for (BOOL ok = Process32FirstW(s, &e); ok && t->n < 1024; ok = Process32NextW(s, &e)) {
        t->pid[t->n] = e.th32ProcessID;
        t->parent[t->n++] = e.th32ParentProcessID;
    }
    CloseHandle(s);
}

static BOOL descends(const ProcTree *t, DWORD pid, DWORD root)
{
    for (int depth = 0; depth < 8 && pid; depth++) {
        if (pid == root) return TRUE;
        int k;
        for (k = 0; k < t->n && t->pid[k] != pid; k++) {}
        if (k == t->n) return FALSE;
        pid = t->parent[k];
    }
    return FALSE;
}

typedef struct {
    const App *a;
    DWORD root;                 /* the process we started, or 0 */
    const ProcTree *tree;
    HWND found, prompt;         /* its main window; a prompt of it */
    wchar_t foundPath[MAX_PATH];
    HWND *list; int n, max;     /* workspace_open_windows */
} FindCtx;

static BOOL CALLBACK find_enum(HWND w, LPARAM lp)
{
    FindCtx *c = (FindCtx *)lp;
    if (!app_window(w)) return TRUE;
    if (c->list) { if (c->n < c->max) c->list[c->n++] = w; return TRUE; }
    DWORD pid = 0;
    wchar_t path[MAX_PATH] = L"";
    GetWindowThreadProcessId(w, &pid);
    process_path(pid, path, MAX_PATH);
    BOOL ours = (c->a->match[0] && !_wcsicmp(path, c->a->match)) || (c->root && c->tree && descends(c->tree, pid, c->root));
    if (!ours) return TRUE;
    if (!main_window(w)) {
        if (!c->prompt) c->prompt = w;
        return TRUE;
    }
    if (c->a->title[0]) {
        wchar_t t[512];
        GetWindowTextW(w, t, 512);
        if (!StrStrIW(t, c->a->title)) return TRUE;
    }
    c->found = w;                                                     /* the top one, in z-order */
    wcscpy_s(c->foundPath, MAX_PATH, path);
    return FALSE;
}

static HWND find_window(const App *a, DWORD root, HWND *prompt, wchar_t *path)
{
    ProcTree *tree = NULL;
    if (root) {
        tree = HeapAlloc(GetProcessHeap(), 0, sizeof *tree);
        if (tree) proc_tree(tree);
    }
    FindCtx c = { a, root, tree, NULL, NULL, L"", NULL, 0, 0 };
    EnumWindows(find_enum, (LPARAM)&c);
    if (tree) HeapFree(GetProcessHeap(), 0, tree);
    if (prompt) *prompt = c.prompt;
    if (path) wcscpy_s(path, MAX_PATH, c.foundPath);
    return c.found;
}

HWND workspace_find(const App *a)
{
    return a->match[0] ? find_window(a, 0, NULL, NULL) : NULL;
}

int workspace_open_windows(HWND *out, int max)
{
    FindCtx c = { 0 };
    c.list = out;
    c.max = max;
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

/* Where an open window is, as a place on a setup's monitors: maximized, or custom in % of the work area. FALSE if
   it's on a monitor the setup doesn't have. */
BOOL workspace_place_from_window(HWND w, const Layout *l, Place *p)
{
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
    int k;
    for (k = 0; k < l->nmon; k++) if (EqualRect(&l->mons[k].r, &mi.rcMonitor)) break;
    if (k == l->nmon) return FALSE;
    p->mon = k;
    if (IsZoomed(w)) { p->pos = POS_MAXIMIZED; return TRUE; }
    RECT fr, work = mi.rcWork;
    if (IsIconic(w)) {
        WINDOWPLACEMENT wp = { sizeof wp };
        GetWindowPlacement(w, &wp);
        fr = wp.rcNormalPosition;
    } else if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr))) {
        GetWindowRect(w, &fr);
    }
    int W = work.right - work.left, H = work.bottom - work.top;
    p->pos = POS_CUSTOM;
    p->x = max(0, min(100, MulDiv(fr.left - work.left, 100, W)));
    p->y = max(0, min(100, MulDiv(fr.top - work.top, 100, H)));
    p->w = max(1, min(100, MulDiv(fr.right - fr.left, 100, W)));
    p->h = max(1, min(100, MulDiv(fr.bottom - fr.top, 100, H)));
    return TRUE;
}

/* Fill an app from an open window: what to start (the program, or the Store app's ID), how to find it, its name. */
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
}

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

/* The app's icon as a 32-bit premultiplied bitmap (works for programs and Store apps alike). */
HBITMAP workspace_app_icon(const App *a, int size)
{
    const wchar_t *src = a->command[0] ? a->command : a->match;
    IShellItemImageFactory *f = NULL;
    HBITMAP bmp = NULL;
    if (src[0] && SUCCEEDED(SHCreateItemFromParsingName(src, NULL, &IID_IShellItemImageFactory, (void **)&f))) {
        SIZE sz = { size, size };
        IShellItemImageFactory_GetImage(f, sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp);
        IShellItemImageFactory_Release(f);
    }
    return bmp;
}

/* ---------------------------------------------------------------- launching ----------------------------------- */

enum { ST_IDLE, ST_WAITING, ST_SETTLING, ST_CHECK, ST_DONE, ST_FAILED, ST_UNPLACED };
static struct { int state; HWND w; ULONGLONG until, started; DWORD pid; Place place; RECT work; } g_run[MAX_APPS];
static int g_count;
static HWND g_owner;

static void finish_if_done(void)
{
    int done = 0, failed = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_run[i].state == ST_WAITING || g_run[i].state == ST_SETTLING || g_run[i].state == ST_CHECK) return;
        if (g_run[i].state == ST_FAILED) failed++; else done++;
    }
    KillTimer(g_owner, TIMER_WORKSPACE);
    trace("workspace: done, %d placed or started, %d failed", done, failed);
    if (failed) {
        wchar_t text[256] = L"", *sep = L"";
        for (int i = 0; i < g_count; i++) {
            if (g_run[i].state != ST_FAILED) continue;
            if (wcslen(text) + wcslen(g_set.apps[i].name) + 24 >= ARRAYSIZE(text)) break;
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
    LayoutMon cur[MAX_LAYOUT_MONS];
    int ncur = workspace_current_monitors(cur, MAX_LAYOUT_MONS);
    int li = workspace_find_layout(cur, ncur);
    Layout *l = li >= 0 ? &g_set.layouts[li] : NULL;
    trace("workspace: launching %d app(s) on setup %ls", g_count, l ? l->name : L"(none saved for these monitors)");
    if (!l) tray_notify(FALSE, L"Workspace", L"These monitors aren't a saved setup yet, so the apps are started but not "
                                              L"placed. Add them on Settings > Workspace.");
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_count; i++) {
        const App *a = &g_set.apps[i];
        const Place *p = l ? workspace_place_of(l, a->id) : NULL;
        g_run[i].state = ST_UNPLACED;
        g_run[i].pid = 0;
        if (p) {
            g_run[i].place = *p;
            /* the setup's monitor, as it is now (same device, else same place) */
            const LayoutMon *m = &l->mons[p->mon];
            int k;
            for (k = 0; k < ncur; k++) if (!_wcsicmp(cur[k].path, m->path)) break;
            if (k == ncur) for (k = 0; k < ncur; k++) if (EqualRect(&cur[k].r, &m->r)) break;
            g_run[i].work = k < ncur ? cur[k].work : m->work;
        }
        g_run[i].w = workspace_find(a);
        if (g_run[i].w) {
            if (!p) { trace("workspace: %ls is open (no place on this setup)", a->name); continue; }
            trace("workspace: %ls is open: placing it", a->name);
            place(g_run[i].w, a, &g_run[i].work, &g_run[i].place);
            g_run[i].state = ST_SETTLING;
            g_run[i].until = now + SETTLE_MS;
            continue;
        }
        if (!a->command[0]) { g_run[i].state = ST_FAILED; continue; }
        SHELLEXECUTEINFOW sei = { sizeof sei };
        sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC | SEE_MASK_NOCLOSEPROCESS;
        sei.lpFile = a->command;
        sei.lpParameters = a->args[0] ? a->args : NULL;
        sei.nShow = SW_SHOWNORMAL;
        BOOL ok = ShellExecuteExW(&sei);
        if (ok && sei.hProcess) {                                     /* its windows may come from a child of it */
            g_run[i].pid = GetProcessId(sei.hProcess);
            CloseHandle(sei.hProcess);
        }
        trace("workspace: starting %ls (%ls %ls): %s, process %lu", a->name, a->command, a->args, ok ? "ok" : "failed",
              g_run[i].pid);
        if (!ok) { g_run[i].state = ST_FAILED; continue; }
        g_run[i].started = now;
        if (p) { g_run[i].state = ST_WAITING; g_run[i].until = now + WAIT_MS; }
    }
    SetTimer(owner, TIMER_WORKSPACE, 250, NULL);
    finish_if_done();
}

void workspace_tick(void)
{
    ULONGLONG now = GetTickCount64();
    for (int i = 0; i < g_count && i < g_set.appCount; i++) {
        const App *a = &g_set.apps[i];
        if (g_run[i].state == ST_WAITING) {
            HWND prompt = NULL;
            wchar_t path[MAX_PATH];
            HWND w = find_window(a, g_run[i].pid, &prompt, path);
            if (w) {
                trace("workspace: %ls opened: placing it", a->name);
                if (_wcsicmp(path, a->match)) {                       /* a launcher: remember the real program */
                    trace("workspace: %ls: its window belongs to %ls; matching that from now on", a->name, path);
                    wcscpy_s(g_set.apps[i].match, MAX_PATH, path);
                    settings_save(&g_set);
                }
                place(w, a, &g_run[i].work, &g_run[i].place);
                g_run[i].w = w;
                g_run[i].state = ST_SETTLING;
                g_run[i].until = now + SETTLE_MS;
            } else if (prompt && g_run[i].until < now + WAIT_MS && now < g_run[i].started + WAIT_PROMPT_MS) {
                if (g_run[i].until <= now + WAIT_MS - 1000) trace("workspace: %ls: waiting while it asks something", a->name);
                g_run[i].until = now + WAIT_MS;                       /* a prompt (a password): keep waiting */
            } else if (now >= g_run[i].until) {
                trace("workspace: %ls: no main window in time", a->name);
                g_run[i].state = ST_FAILED;
            }
        } else if (g_run[i].state == ST_SETTLING && now >= g_run[i].until) {
            /* again: the app may have restored its own last place or rescaled for the monitor, or a different main
               window (after a splash screen) may have taken over */
            HWND w = find_window(a, g_run[i].pid, NULL, NULL);
            if (w && w != g_run[i].w && IsWindow(g_run[i].w)) w = g_run[i].w;
            if (!w && IsWindow(g_run[i].w)) w = g_run[i].w;
            if (w) place(w, a, &g_run[i].work, &g_run[i].place);
            g_run[i].w = w;
            g_run[i].state = w ? ST_CHECK : ST_DONE;
            g_run[i].until = now + 700;
        } else if (g_run[i].state == ST_CHECK && now >= g_run[i].until) {
            /* did it take? an app can refuse a size (a minimum width wider than the spot, say) */
            RECT t, fr;
            const Place *p = &g_run[i].place;
            workspace_target(&g_run[i].work, p->pos, p->x, p->y, p->w, p->h, &t);
            HWND w = g_run[i].w;
            if (IsWindow(w) && !IsZoomed(w) && p->pos != POS_MAXIMIZED &&
                SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof fr)) &&
                (abs(fr.left - t.left) > 16 || abs(fr.top - t.top) > 16 || abs(fr.right - t.right) > 16 || abs(fr.bottom - t.bottom) > 16))
                trace("workspace: %ls is at %ld,%ld,%ld,%ld (%ld x %ld), not %ld,%ld,%ld,%ld (%ld x %ld): it won't go there or "
                      "that small", a->name, fr.left, fr.top, fr.right, fr.bottom, fr.right - fr.left, fr.bottom - fr.top,
                      t.left, t.top, t.right, t.bottom, t.right - t.left, t.bottom - t.top);
            g_run[i].state = ST_DONE;
        }
    }
    finish_if_done();
}
