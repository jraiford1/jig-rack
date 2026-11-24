/* Jig Rack -- the hot corner jig: rules that run an action when the cursor rests in a screen corner.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * Each rule (Settings > Hot Corners) names a corner; where it counts (the corner of the whole workspace, of any
 * monitor, or of one monitor); an action; how long the cursor must rest there; and whether to show a countdown.
 * "The workspace's top-right" is the top-right corner of the monitor reaching furthest right (the highest one if
 * several tie), and likewise for the other corners, so the corners between monitors never count for it.
 * The cursor is polled with GetCursorPos every 100 ms (works over elevated windows, unlike a low-level mouse hook),
 * only while there is at least one rule. A click while in a corner (e.g. a maximized window's close button) cancels;
 * after an action or a cancel the cursor must leave the corner before that rule can fire again, so nothing fires
 * again on return from the lock screen. Corners are recomputed when the display layout changes and every 5 s.
 */
#include "jigrack.h"
#include "gdip.h"
#include <shellscalingapi.h>
#include <powrprof.h>
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "powrprof.lib")

#define POLL_MS     100
#define RECHECK_MS  5000        /* layout recheck backstop */
#define ZONE        3           /* px from the corner, both ways */
#define MAX_MONS    16

/* ---------------------------------------------------------------- monitors ------------------------------------ */

typedef struct { MonitorDesc *out; int n, max; } MonEnum;

static BOOL CALLBACK mon_enum(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc;
    MonEnum *e = (MonEnum *)lp;
    if (e->n >= e->max) return FALSE;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof mi;
    MonitorDesc *d = &e->out[e->n++];
    ZeroMemory(d, sizeof *d);
    d->r = *r;
    if (GetMonitorInfoW(m, (MONITORINFO *)&mi)) wcscpy_s(d->gdi, 32, mi.szDevice);
    return TRUE;
}

/* Active monitors with their rectangles; with `names`, also each one's device path and friendly name. */
static int monitors(MonitorDesc *out, int max, BOOL names)
{
    MonEnum e = { out, 0, max };
    EnumDisplayMonitors(NULL, NULL, mon_enum, (LPARAM)&e);
    if (!names) return e.n;
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np) {
        DISPLAYCONFIG_PATH_INFO *paths = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, np * sizeof *paths);
        DISPLAYCONFIG_MODE_INFO *modes = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (nm ? nm : 1) * sizeof *modes);
        if (paths && modes && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
                ZeroMemory(&src, sizeof src);
                src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                src.header.size = sizeof src;
                src.header.adapterId = paths[i].sourceInfo.adapterId;
                src.header.id = paths[i].sourceInfo.id;
                DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;
                ZeroMemory(&tgt, sizeof tgt);
                tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                tgt.header.size = sizeof tgt;
                tgt.header.adapterId = paths[i].targetInfo.adapterId;
                tgt.header.id = paths[i].targetInfo.id;
                if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
                    DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;
                for (int k = 0; k < e.n; k++) {
                    if (out[k].path[0] || _wcsicmp(out[k].gdi, src.viewGdiDeviceName) != 0) continue;
                    wcscpy_s(out[k].path, 256, tgt.monitorDevicePath);
                    BOOL internal = tgt.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                                    tgt.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                                    tgt.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED;
                    if (tgt.monitorFriendlyDeviceName[0]) wcscpy_s(out[k].name, 64, tgt.monitorFriendlyDeviceName);
                    else wcscpy_s(out[k].name, 64, internal ? L"Built-in display" : L"Display");
                    break;
                }
            }
        }
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        if (modes) HeapFree(GetProcessHeap(), 0, modes);
    }
    for (int k = 0; k < e.n; k++)
        if (!out[k].path[0]) {
            wcscpy_s(out[k].path, 256, out[k].gdi);
            wcscpy_s(out[k].name, 64, wcsncmp(out[k].gdi, L"\\\\.\\", 4) == 0 ? out[k].gdi + 4 : out[k].gdi);
        }
    return e.n;
}

int monitors_list(MonitorDesc *out, int max) { return monitors(out, max, TRUE); }

/* ---------------------------------------------------------------- rules ---------------------------------------- */

typedef struct {
    POINT pt[MAX_MONS];         /* the corner point(s) this rule watches */
    int n;
    ULONGLONG since;            /* when the cursor arrived; 0 = not in the corner */
    BOOL spent;                 /* fired or cancelled: wait for the cursor to leave */
    POINT at;                   /* the point it is resting at */
} RuleRun;

static RuleRun g_run[MAX_RULES];
static ULONGLONG g_layoutAt;
static BOOL g_dry;              /* --selftest: log instead of acting */
static HWND g_owner;

static BOOL is_right(int corner) { return corner == CORNER_TR || corner == CORNER_BR; }
static BOOL is_bottom(int corner) { return corner == CORNER_BL || corner == CORNER_BR; }

static POINT corner_of(const RECT *r, int corner)
{
    POINT p = { is_right(corner) ? r->right - 1 : r->left, is_bottom(corner) ? r->bottom - 1 : r->top };
    return p;
}

/* The workspace's corner: the monitor reaching furthest that way horizontally, then vertically on a tie. */
static int workspace_monitor(const MonitorDesc *m, int n, int corner)
{
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (best < 0) { best = i; continue; }
        LONG xi = is_right(corner) ? m[i].r.right : -m[i].r.left, xb = is_right(corner) ? m[best].r.right : -m[best].r.left;
        LONG yi = is_bottom(corner) ? m[i].r.bottom : -m[i].r.top, yb = is_bottom(corner) ? m[best].r.bottom : -m[best].r.top;
        if (xi > xb || (xi == xb && yi > yb)) best = i;
    }
    return best;
}

static void layout(void)
{
    BOOL names = FALSE;
    for (int i = 0; i < g_set.ruleCount; i++) if (g_set.rules[i].where == WHERE_MONITOR) names = TRUE;
    MonitorDesc m[MAX_MONS];
    int n = monitors(m, MAX_MONS, names);
    for (int i = 0; i < g_set.ruleCount; i++) {
        const Rule *r = &g_set.rules[i];
        RuleRun *u = &g_run[i];
        int oldN = u->n;
        POINT old = u->pt[0];
        u->n = 0;
        if (r->where == WHERE_WORKSPACE) {
            int b = workspace_monitor(m, n, r->corner);
            if (b >= 0) u->pt[u->n++] = corner_of(&m[b].r, r->corner);
        } else if (r->where == WHERE_ANY) {
            for (int k = 0; k < n; k++) u->pt[u->n++] = corner_of(&m[k].r, r->corner);
        } else {
            for (int k = 0; k < n; k++)
                if (_wcsicmp(m[k].path, r->monitor) == 0) { u->pt[u->n++] = corner_of(&m[k].r, r->corner); break; }
        }
        if (u->n != oldN || (u->n && (old.x != u->pt[0].x || old.y != u->pt[0].y))) {
            if (u->n) trace("hot corner %d at %ld,%ld%s", i + 1, u->pt[0].x, u->pt[0].y, u->n > 1 ? " and more" : "");
            else trace("hot corner %d: its monitor isn't connected", i + 1);
        }
    }
    g_layoutAt = GetTickCount64();
}

static BOOL in_zone(POINT p, POINT c, int corner)
{
    BOOL inx = is_right(corner) ? p.x >= c.x - (ZONE - 1) && p.x <= c.x : p.x >= c.x && p.x <= c.x + (ZONE - 1);
    BOOL iny = is_bottom(corner) ? p.y <= c.y && p.y >= c.y - (ZONE - 1) : p.y >= c.y && p.y <= c.y + (ZONE - 1);
    return inx && iny;
}

/* One poll for one rule. ok = cursor position known (it isn't while the lock screen is up); button = a mouse
   button went down. TRUE = run the action now. */
static BOOL rule_step(int i, POINT p, BOOL ok, BOOL button, ULONGLONG now)
{
    if (!ok) return FALSE;                                   /* lock screen etc.: keep state as it is */
    RuleRun *u = &g_run[i];
    BOOL in = FALSE;
    for (int k = 0; k < u->n && !in; k++)
        if (in_zone(p, u->pt[k], g_set.rules[i].corner)) { in = TRUE; u->at = u->pt[k]; }
    if (!in) { u->since = 0; u->spent = FALSE; return FALSE; }
    if (u->spent) return FALSE;
    if (button) { u->spent = TRUE; u->since = 0; trace("hot corner %d: cancelled by a click", i + 1); return FALSE; }
    if (!u->since) { u->since = now; return FALSE; }
    if (now - u->since < (ULONGLONG)g_set.rules[i].delayMs) return FALSE;
    u->spent = TRUE;
    u->since = 0;
    return TRUE;
}

/* ---------------------------------------------------------------- countdown ----------------------------------- */
/* While the cursor rests in a corner, a big white number with a black outline counts down beside the corner,
   inside the screen; with a rule's "warn if the mouse battery is low" and a low mouse, a red line under it (over
   it, at a bottom corner) says so. A layered, click-through, never-activated topmost window, so it can't take a
   click or the focus. Shown from 0.5 s in (a quick trip to a close button doesn't flash it); redrawn only when what
   it shows changes. */
static HWND g_cd;
static int g_cdShown;           /* number on screen; 0 = none */
static int g_cdWarn;            /* battery % in the warning on screen; -1 = none */
static POINT g_cdAt;
static int g_cdCorner;

static void countdown_create(HINSTANCE inst)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"JigRackCountdown";
    RegisterClassW(&wc);
    g_cd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_cdWarn = -1;
}

/* The overlay's size in pixels for a number (n > 0) and/or a warning (warn >= 0) at this DPI. */
static void overlay_size(int n, int warn, UINT dpi, int *w, int *h)
{
    int size = MulDiv(76, dpi, 96), line = MulDiv(34, dpi, 96);
    *w = warn >= 0 ? MulDiv(330, dpi, 96) : size;
    *h = (n > 0 ? size : 0) + (warn >= 0 ? line : 0);
}

static void outlined_text(GpGraphics *g, const wchar_t *text, GpFontFamily *fam, REAL em, const GpRectF *box,
                          int align, ARGB fill, REAL pen)
{
    GpStringFormat *fmt = NULL;
    GdipCreateStringFormat(0x1000 /* NoClip */, 0, &fmt);
    GdipSetStringFormatAlign(fmt, align);
    GdipSetStringFormatLineAlign(fmt, 1);
    GpPath *tp = NULL;
    GdipCreatePath(0, &tp);
    GdipAddPathString(tp, text, -1, fam, 1 /* bold */, em, box, fmt);
    GpPen *outline = NULL;
    GdipCreatePen1(ARGBV(255, 0, 0, 0), pen, 0, &outline);
    GdipSetPenLineJoin(outline, 2 /* round */);
    GpBrush *brush = NULL;
    GdipCreateSolidFill(fill, &brush);
    GdipDrawPath(g, outline, tp);
    GdipFillPath(g, brush, tp);
    GdipDeleteBrush(brush);
    GdipDeletePen(outline);
    GdipDeletePath(tp);
    GdipDeleteStringFormat(fmt);
}

/* Draw the overlay into g (w x h, cleared): the number on the corner's side, the warning line on the inner side. */
static void overlay_draw(GpGraphics *g, int w, int h, int n, int warn, int corner, UINT dpi)
{
    int size = MulDiv(76, dpi, 96), line = MulDiv(34, dpi, 96);
    GdipSetSmoothingMode(g, 4);
    GdipSetPixelOffsetMode(g, 4);
    GpFontFamily *fam = NULL;
    GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
    if (!fam) GdipCreateFontFamilyFromName(L"Arial", NULL, &fam);
    BOOL right = is_right(corner), bottom = is_bottom(corner);
    REAL numTop = (REAL)(bottom && warn >= 0 ? h - size : 0);
    if (n > 0) {
        wchar_t text[8];
        swprintf_s(text, 8, L"%d", n);
        GpRectF box = { (REAL)(right ? w - size : 0), numTop - size * 0.06f, (REAL)size, (REAL)size };
        outlined_text(g, text, fam, size * (n >= 10 ? 0.62f : 0.86f), &box, 1, ARGBV(255, 255, 255, 255), size * 0.11f);
    }
    if (warn >= 0) {
        wchar_t text[64];
        swprintf_s(text, 64, L"Mouse battery %d%% \x2014 charge it", warn);
        REAL top = n > 0 ? (bottom ? numTop - line : (REAL)size) : 0;
        GpRectF box = { 0, top, (REAL)w, (REAL)line };
        REAL em = (REAL)MulDiv(21, dpi, 96);
        outlined_text(g, text, fam, em, &box, right ? 2 : 0, ARGBV(255, 255, 92, 80), em * 0.2f);
    }
    if (fam) GdipDeleteFontFamily(fam);
}

static void countdown_show(int n, int warn, POINT at, int corner)
{
    if (n == g_cdShown && warn == g_cdWarn &&
        ((!n && warn < 0) || (at.x == g_cdAt.x && at.y == g_cdAt.y && corner == g_cdCorner))) return;
    g_cdShown = n;
    g_cdWarn = warn;
    g_cdAt = at;
    g_cdCorner = corner;
    if (!g_cd) return;
    if (!n && warn < 0) { ShowWindow(g_cd, SW_HIDE); return; }

    UINT dpi = 96, dx = 96, dy = 96;
    HMONITOR mon = MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    if (GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK) dpi = dx;
    int w, h, margin = MulDiv(14, dpi, 96);
    overlay_size(n, warn, dpi, &w, &h);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) { DeleteDC(mem); ReleaseDC(NULL, screen); return; }
    HGDIOBJ old = SelectObject(mem, dib);

    /* GDI+ draws straight into the DIB as premultiplied ARGB, which is what UpdateLayeredWindow wants. */
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppPARGB, (BYTE *)bits, &bmp);
    GdipGetImageGraphicsContext(bmp, &g);
    GdipGraphicsClear(g, 0);
    overlay_draw(g, w, h, n, warn, corner, dpi);
    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);

    POINT dst = { is_right(corner) ? at.x - margin - w + 1 : at.x + margin,
                  is_bottom(corner) ? at.y - margin - h + 1 : at.y + margin }, src = { 0, 0 };
    SIZE sz = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_cd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_cd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

/* --preview: the overlay as PNGs, over a dark and a light background, at the system DPI. */
void hotcorner_write_previews(const wchar_t *dir)
{
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    struct { const wchar_t *name; int n, warn, corner; ARGB bg; } s[] = {
        { L"countdown-3",                  3, -1, CORNER_TR, ARGBV(255, 40, 70, 120) },
        { L"countdown-3-warning-top-right", 3, 12, CORNER_TR, ARGBV(255, 40, 70, 120) },
        { L"countdown-2-warning-bottom-left", 2, 18, CORNER_BL, ARGBV(255, 236, 236, 236) },
    };
    UINT dpi = GetDpiForSystem();
    for (int i = 0; i < (int)(sizeof s / sizeof s[0]); i++) {
        int w, h;
        overlay_size(s[i].n, s[i].warn, dpi, &w, &h);
        GpBitmap *bmp = NULL;
        GpGraphics *g = NULL;
        GdipCreateBitmapFromScan0(w, h, 0, PixelFormat32bppARGB, NULL, &bmp);
        GdipGetImageGraphicsContext(bmp, &g);
        GdipGraphicsClear(g, s[i].bg);
        overlay_draw(g, w, h, s[i].n, s[i].warn, s[i].corner, dpi);
        GdipDeleteGraphics(g);
        wchar_t path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%s\\%s.png", dir, s[i].name);
        GdipSaveImageToFile(bmp, path, &png, NULL);
        GdipDisposeImage(bmp);
    }
}

/* A rule's dwell in progress, from 0.5 s in until it fires? remain = ms left. */
static BOOL rule_dwelling(int i, ULONGLONG now, ULONGLONG *remain)
{
    const RuleRun *u = &g_run[i];
    const Rule *r = &g_set.rules[i];
    if (!u->since || u->spent) return FALSE;
    ULONGLONG el = now - u->since;
    if (el < 500 || el >= (ULONGLONG)r->delayMs) return FALSE;
    *remain = r->delayMs - el;
    return TRUE;
}

/* ---------------------------------------------------------------- actions ------------------------------------- */

static void press(WORD a, WORD b)
{
    INPUT in[4];
    ZeroMemory(in, sizeof in);
    for (int k = 0; k < 4; k++) in[k].type = INPUT_KEYBOARD;
    in[0].ki.wVk = a;
    in[1].ki.wVk = b;
    in[2].ki.wVk = b; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = a; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

/* "C:\Program Files\App\app.exe" --flag, or notepad.exe file.txt, or a document or URL. */
static void run_command(const wchar_t *cmd)
{
    wchar_t file[512];
    const wchar_t *s = cmd, *args;
    while (*s == L' ') s++;
    if (*s == L'"') {
        const wchar_t *e = wcschr(++s, L'"');
        size_t n = e ? (size_t)(e - s) : wcslen(s);
        wcsncpy_s(file, 512, s, n < 511 ? n : 511);
        args = e ? e + 1 : L"";
    } else {
        const wchar_t *e = wcschr(s, L' ');
        size_t n = e ? (size_t)(e - s) : wcslen(s);
        wcsncpy_s(file, 512, s, n < 511 ? n : 511);
        args = e ? e : L"";
    }
    while (*args == L' ') args++;
    if (!file[0]) return;
    INT_PTR r = (INT_PTR)ShellExecuteW(NULL, NULL, file, *args ? args : NULL, NULL, SW_SHOWNORMAL);
    if (r <= 32) trace("hot corner: could not run \"%ls\" (%d)", cmd, (int)r);
}

static void fire(int i)
{
    const Rule *r = &g_set.rules[i];
    wchar_t what[700];
    rule_describe(r, what, 700);
    trace("hot corner %d: %ls", i + 1, what);
    if (g_dry) return;
    switch (r->action) {
    case ACT_LOCK:         LockWorkStation(); break;
    case ACT_SLEEP:        SetSuspendState(FALSE, FALSE, FALSE); break;
    case ACT_SCREENS_OFF:  PostMessageW(g_owner, WM_SYSCOMMAND, SC_MONITORPOWER, 2); break;
    case ACT_SHOW_DESKTOP: press(VK_LWIN, 'D'); break;
    case ACT_TASK_VIEW:    press(VK_LWIN, VK_TAB); break;
    case ACT_RUN:          run_command(r->command); break;
    }
}

/* ---------------------------------------------------------------- driving it ---------------------------------- */

/* What rule i's dwell shows now: the countdown number (0 = none) and, with it, the battery warning (-1 = none).
   The warning is part of the countdown: without the countdown there's nothing. TRUE if shown; remain = ms left. */
static BOOL rule_overlay(int i, ULONGLONG now, int *n, int *warn, ULONGLONG *remain)
{
    *n = 0;
    *warn = -1;
    if (!rule_dwelling(i, now, remain)) return FALSE;
    if (!g_set.rules[i].countdown) return FALSE;
    *n = (int)((*remain + 999) / 1000);
    if (g_set.rules[i].warnLow && mouse_battery_low()) *warn = g_battery.percent;
    return TRUE;
}

void hotcorner_tick(void)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_layoutAt >= RECHECK_MS) layout();
    POINT p;
    BOOL ok = GetCursorPos(&p);
    BOOL button = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8001) != 0;
    int show = 0, warn = -1, corner = 0;
    ULONGLONG best = (ULONGLONG)-1;
    POINT at = { 0, 0 };
    for (int i = 0; i < g_set.ruleCount; i++) {
        if (rule_step(i, p, ok, button, now)) {
            countdown_show(0, -1, at, 0);
            fire(i);
            now = GetTickCount64();
        } else if (ok) {
            ULONGLONG remain;
            int n, w;
            if (rule_overlay(i, now, &n, &w, &remain) && remain < best) {
                best = remain; show = n; warn = w; at = g_run[i].at; corner = g_set.rules[i].corner;
            }
        }
    }
    countdown_show(show, warn, at, corner);
}

void hotcorner_apply(void)
{
    for (int i = 0; i < MAX_RULES; i++) {
        g_run[i].since = 0;
        g_run[i].spent = TRUE;                               /* don't fire if the cursor is already there */
        g_run[i].n = 0;
    }
    POINT none = { 0, 0 };
    countdown_show(0, -1, none, 0);
    layout();
    if (!g_owner) return;
    if (g_set.ruleCount) SetTimer(g_owner, TIMER_CORNER, POLL_MS, NULL);
    else KillTimer(g_owner, TIMER_CORNER);
}

void hotcorner_layout_changed(void) { layout(); }

void hotcorner_init(HINSTANCE inst, HWND owner)
{
    g_owner = owner;
    countdown_create(inst);
}

/* --demo-countdown: 5..1 at the first rule's corner (or the workspace's top-right), without any action. */
void hotcorner_demo_countdown(void)
{
    if (!g_set.ruleCount) settings_defaults(&g_set);
    layout();
    POINT at = g_run[0].n ? g_run[0].pt[0] : (POINT){ 0, 0 };
    for (int n = 5; n >= 1; n--) {
        countdown_show(n, -1, at, g_set.rules[0].corner);
        ULONGLONG until = GetTickCount64() + 1000;
        MSG dm;
        while (GetTickCount64() < until) {
            while (PeekMessageW(&dm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&dm);
            Sleep(20);
        }
    }
    countdown_show(0, -1, at, 0);
}

/* ---------------------------------------------------------------- self-test ----------------------------------- */
/* --selftest: the rules on scripted cursor timelines against the real monitor layout, and a settings file round
   trip. Never acts. Results go to the log. */

static int g_fails;
#define EXPECT(cond, what) do { BOOL ok_ = (cond); trace("selftest %s: %s", ok_ ? "ok  " : "FAIL", what); \
                                if (!ok_) g_fails++; } while (0)

static void reset_runs(void)
{
    layout();
    for (int i = 0; i < MAX_RULES; i++) { g_run[i].since = 0; g_run[i].spent = FALSE; }
}

/* Rest at p for ms (polls every 100 ms, from t0 to t0 + ms inclusive). Did rule i fire? */
static BOOL dwell(int i, POINT p, ULONGLONG t0, int ms)
{
    BOOL fired = FALSE;
    for (int k = 0; k <= ms; k += 100) fired |= rule_step(i, p, TRUE, FALSE, t0 + k);
    return fired;
}

static void test_default_rule(const MonitorDesc *m, int n)
{
    settings_defaults(&g_set);                               /* top-right of the workspace, lock, 5 s */
    reset_runs();
    POINT c = g_run[0].pt[0], away = { c.x - 400, c.y + 400 }, nearIn = { c.x - 2, c.y + 2 };
    ULONGLONG t = 1000000;
    EXPECT(!rule_step(0, away, TRUE, FALSE, t), "away from the corner: nothing");
    BOOL fired = FALSE;
    for (int ms = 0; ms < 4900; ms += 100) fired |= rule_step(0, c, TRUE, FALSE, t + ms);
    EXPECT(!fired, "4.9 s in the corner: no lock yet");
    EXPECT(rule_step(0, c, TRUE, FALSE, t + 5000), "5 s in the corner: lock");
    EXPECT(!rule_step(0, c, TRUE, FALSE, t + 5100), "still there after the lock: no second lock");
    EXPECT(!rule_step(0, c, FALSE, FALSE, t + 9000), "lock screen up (no cursor): no change");
    EXPECT(!dwell(0, c, t + 10000, 20000), "back from the lock, cursor still in the corner 20 s: no relock");
    EXPECT(!rule_step(0, away, TRUE, FALSE, t + 31000), "leave the corner");
    rule_step(0, nearIn, TRUE, FALSE, t + 32000);
    EXPECT(rule_step(0, nearIn, TRUE, FALSE, t + 37000), "re-enter 2 px from the corner, 5 s: lock again");
    rule_step(0, away, TRUE, FALSE, t + 38000);
    rule_step(0, c, TRUE, FALSE, t + 39000);
    rule_step(0, c, TRUE, TRUE, t + 40000);
    EXPECT(!rule_step(0, c, TRUE, FALSE, t + 46000), "click in the corner (close button): cancelled");
    rule_step(0, away, TRUE, FALSE, t + 47000);
    POINT inner = { 0, 0 };
    BOOL haveInner = FALSE;
    for (int k = 0; k < n && !haveInner; k++) {
        POINT q = corner_of(&m[k].r, CORNER_TR);
        if (q.x != c.x || q.y != c.y) { inner = q; haveInner = TRUE; }
    }
    if (haveInner) EXPECT(!dwell(0, inner, t + 48000, 10000), "another monitor's top-right corner, 10 s: nothing");
    else trace("selftest skip: only one monitor");
    rule_step(0, c, TRUE, FALSE, t + 60000);
    rule_step(0, away, TRUE, FALSE, t + 62000);
    EXPECT(!rule_step(0, c, TRUE, FALSE, t + 64000), "dwell restarts after leaving (2 s + 2 s): no lock");
}

static void test_more_rules(const MonitorDesc *m, int n)
{
    ULONGLONG t = 5000000;
    wchar_t what[160];

    /* bottom-left of the workspace, 1.5 s */
    settings_defaults(&g_set);
    g_set.rules[0].corner = CORNER_BL;
    g_set.rules[0].action = ACT_SHOW_DESKTOP;
    g_set.rules[0].delayMs = 1500;
    reset_runs();
    POINT bl = g_run[0].pt[0], tr = corner_of(&m[workspace_monitor(m, n, CORNER_TR)].r, CORNER_TR);
    EXPECT(!dwell(0, tr, t, 3000), "bottom-left rule: the top-right corner does nothing");
    rule_step(0, (POINT){ bl.x + 300, bl.y - 300 }, TRUE, FALSE, t + 4000);
    EXPECT(!dwell(0, bl, t + 5000, 1400), "bottom-left rule: 1.4 s, not yet");
    EXPECT(rule_step(0, bl, TRUE, FALSE, t + 6500), "bottom-left rule: fires at 1.5 s");
    POINT blIn = { bl.x + 2, bl.y - 2 }, blOut = { bl.x + 3, bl.y };
    rule_step(0, (POINT){ bl.x + 300, bl.y - 300 }, TRUE, FALSE, t + 7000);
    EXPECT(dwell(0, blIn, t + 8000, 1500), "bottom-left rule: 2 px inside the zone fires");
    rule_step(0, (POINT){ bl.x + 300, bl.y - 300 }, TRUE, FALSE, t + 10000);
    EXPECT(!dwell(0, blOut, t + 11000, 3000), "bottom-left rule: 3 px away is outside the zone");

    /* two rules on one corner: 2 s and 5 s */
    settings_defaults(&g_set);
    g_set.ruleCount = 2;
    rule_defaults(&g_set.rules[1]);
    g_set.rules[0].action = ACT_SHOW_DESKTOP;
    g_set.rules[0].delayMs = 2000;
    reset_runs();
    POINT c = g_run[0].pt[0];
    int when[2] = { -1, -1 }, times[2] = { 0, 0 };
    for (int ms = 0; ms <= 8000; ms += 100)
        for (int i = 0; i < 2; i++)
            if (rule_step(i, c, TRUE, FALSE, t + 20000 + ms)) { if (when[i] < 0) when[i] = ms; times[i]++; }
    swprintf_s(what, 160, L"two rules on the top-right: fired at %d ms and %d ms, %d and %d time(s)", when[0], when[1],
               times[0], times[1]);
    char w8[200];
    WideCharToMultiByte(CP_UTF8, 0, what, -1, w8, sizeof w8, NULL, NULL);
    EXPECT(when[0] == 2000 && when[1] == 5000 && times[0] == 1 && times[1] == 1, w8);

    /* any monitor's top-left */
    settings_defaults(&g_set);
    g_set.rules[0].corner = CORNER_TL;
    g_set.rules[0].where = WHERE_ANY;
    g_set.rules[0].delayMs = 1000;
    reset_runs();
    int hits = 0;
    ULONGLONG tt = t + 40000;
    for (int k = 0; k < n; k++) {
        POINT q = corner_of(&m[k].r, CORNER_TL);
        rule_step(0, (POINT){ q.x + 300, q.y + 300 }, TRUE, FALSE, tt);
        if (dwell(0, q, tt + 1000, 1000)) hits++;
        tt += 5000;
    }
    sprintf_s(w8, sizeof w8, "any monitor's top-left: fired on %d of %d monitors", hits, n);
    EXPECT(hits == n, w8);

    /* one monitor's top-left */
    MonitorDesc named[MAX_MONS];
    int nn = monitors(named, MAX_MONS, TRUE);
    settings_defaults(&g_set);
    g_set.rules[0].corner = CORNER_TL;
    g_set.rules[0].where = WHERE_MONITOR;
    g_set.rules[0].delayMs = 1000;
    wcscpy_s(g_set.rules[0].monitor, 256, named[0].path);
    wcscpy_s(g_set.rules[0].monitorName, 64, named[0].name);
    reset_runs();
    POINT mine = corner_of(&named[0].r, CORNER_TL);
    tt = t + 100000;
    EXPECT(g_run[0].n == 1 && dwell(0, mine, tt, 1000), "one monitor's top-left: fires there");
    if (nn > 1) {
        POINT other = corner_of(&named[1].r, CORNER_TL);
        rule_step(0, (POINT){ other.x + 300, other.y + 300 }, TRUE, FALSE, tt + 3000);
        EXPECT(!dwell(0, other, tt + 4000, 3000), "one monitor's top-left: another monitor's does nothing");
    }
    wcscpy_s(g_set.rules[0].monitor, 256, L"\\\\?\\DISPLAY#NOTCONNECTED#0");
    reset_runs();
    EXPECT(g_run[0].n == 0, "a monitor that isn't connected: the rule watches nothing");
    for (int k = 0; k < nn; k++) {
        char line[300];
        sprintf_s(line, sizeof line, "monitor %d: %ls (%ls) %ld,%ld-%ld,%ld", k + 1, named[k].name, named[k].gdi,
                  named[k].r.left, named[k].r.top, named[k].r.right, named[k].r.bottom);
        trace("%s", line);
    }
}

static void test_low_battery(void)
{
    Battery saved = g_battery;
    settings_defaults(&g_set);
    g_set.mouseType = MOUSE_LOGITECH;
    g_set.productId = 0xC547;
    g_set.deviceIndex = 1;
    g_set.lowPercent = 20;
    g_set.rules[0].warnLow = 1;
    ZeroMemory(&g_battery, sizeof g_battery);
    g_battery.ok = 1;
    g_battery.percent = 15;
    EXPECT(mouse_battery_low(), "low battery: 15% with the level at 20%: low");
    g_battery.percent = 20;
    EXPECT(mouse_battery_low(), "low battery: 20%, at the level: low");
    g_battery.percent = 21;
    EXPECT(!mouse_battery_low(), "low battery: 21%: not low");
    g_battery.percent = 15;
    g_battery.charging = 1;
    EXPECT(!mouse_battery_low(), "low battery: 15% but charging: no warning");
    g_battery.charging = 0;
    g_battery.stale = 1;
    EXPECT(mouse_battery_low(), "low battery: 15%, mouse asleep (last reading): low");
    g_battery.ok = 0;
    EXPECT(!mouse_battery_low(), "low battery: no reading: no warning");
    g_battery.ok = 1;
    g_battery.stale = 0;
    g_set.mouseType = MOUSE_NONE;
    EXPECT(!mouse_battery_low(), "low battery: no mouse chosen: no warning");
    g_set.mouseType = MOUSE_LOGITECH;

    reset_runs();
    POINT c = g_run[0].pt[0];
    ULONGLONG t = 9000000, rem;
    int n, w;
    rule_step(0, c, TRUE, FALSE, t);
    rule_step(0, c, TRUE, FALSE, t + 1200);
    BOOL any = rule_overlay(0, t + 1200, &n, &w, &rem);
    EXPECT(any && n == 4 && w == 15, "1.2 s into a 5 s dwell with a low mouse: countdown 4 and the 15% warning");
    EXPECT(!rule_overlay(0, t + 300, &n, &w, &rem), "0.3 s into the dwell: nothing shown yet");
    g_set.rules[0].countdown = 0;
    any = rule_overlay(0, t + 1200, &n, &w, &rem);
    EXPECT(!any && n == 0 && w == -1, "countdown off: no warning either (it's part of the countdown)");
    g_set.rules[0].warnLow = 0;
    g_set.rules[0].countdown = 1;
    any = rule_overlay(0, t + 1200, &n, &w, &rem);
    EXPECT(any && n == 4 && w == -1, "warning off: the countdown alone");
    g_set.rules[0].warnLow = 1;
    g_battery.percent = 60;
    any = rule_overlay(0, t + 1200, &n, &w, &rem);
    EXPECT(any && n == 4 && w == -1, "warning on but the mouse at 60%: the countdown alone");
    g_battery = saved;
}

static void test_settings_file(void)
{
    wchar_t tmp[MAX_PATH], old[MAX_PATH];
    wcscpy_s(old, MAX_PATH, settings_file());
    GetTempPathW(MAX_PATH, tmp);
    wcscat_s(tmp, MAX_PATH, L"jigrack-selftest.ini");
    DeleteFileW(tmp);
    settings_use_file(tmp);
    Settings a, b;
    settings_defaults(&a);
    a.mouseType = MOUSE_LOGITECH;
    a.productId = 0xC547;
    a.deviceIndex = 2;
    wcscpy_s(a.deviceName, 64, L"G502 X LIGHTSPEED \x2014 \x00e9\x00e8");
    a.showMeter = 0;
    a.lowPercent = 35;
    a.dlButton = 1;
    a.dlAllMonitors = 0;
    wcscpy_s(a.dlIcon, ARRAYSIZE(a.dlIcon), L"C:\\Windows\\System32\\shell32.dll,45");
    wcscpy_s(a.dlFolder, MAX_PATH, L"D:\\T\x00e9l\x00e9chargements");
    a.dlCount = 35;
    a.dlSort = DL_SORT_KIND;
    a.appCount = 2;
    wcscpy_s(a.apps[0].name, 64, L"Outlook");
    wcscpy_s(a.apps[0].command, ARRAYSIZE(a.apps[0].command), L"shell:AppsFolder\\Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows");
    wcscpy_s(a.apps[0].match, MAX_PATH, L"C:\\Program Files\\WindowsApps\\Microsoft.OutlookForWindows\\olk.exe");
    a.apps[0].id = 3;
    wcscpy_s(a.apps[1].name, 64, L"Google Chrome");
    wcscpy_s(a.apps[1].command, ARRAYSIZE(a.apps[1].command), L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");
    wcscpy_s(a.apps[1].match, MAX_PATH, a.apps[1].command);
    wcscpy_s(a.apps[1].args, 256, L"--profile-directory=\"Profile 1\"");
    wcscpy_s(a.apps[1].title, 64, L"LabWare 9");
    a.apps[1].id = 7;
    a.nextAppId = 8;
    a.layoutCount = 2;
    wcscpy_s(a.layouts[0].name, 64, L"2026-11-24-1");
    a.layouts[0].nmon = 2;
    wcscpy_s(a.layouts[0].mons[0].path, 256, L"\\\\?\\DISPLAY#DELA1C4#5&1a2b3c&0&UID4357#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
    wcscpy_s(a.layouts[0].mons[0].name, 64, L"DELL U3223QE");
    SetRect(&a.layouts[0].mons[0].r, 0, -3240, 5760, 0);
    a.layouts[0].mons[0].work = a.layouts[0].mons[0].r;
    wcscpy_s(a.layouts[0].mons[1].name, 64, L"Built-in display");
    SetRect(&a.layouts[0].mons[1].r, 0, 0, 3840, 2400);
    SetRect(&a.layouts[0].mons[1].work, 0, 0, 3840, 2292);
    a.layouts[0].nplace = 2;
    a.layouts[0].places[0] = (Place){ 3, 0, POS_CUSTOM, 5, 0, 95, 50 };
    a.layouts[0].places[1] = (Place){ 7, 1, POS_RIGHT_THIRD, 0, 0, 50, 100 };
    wcscpy_s(a.layouts[1].name, 64, L"Caf\x00e9");
    a.layouts[1].nmon = 1;
    SetRect(&a.layouts[1].mons[0].r, 0, 0, 1920, 1080);
    a.layouts[1].mons[0].work = a.layouts[1].mons[0].r;
    a.ruleCount = 3;
    rule_defaults(&a.rules[1]);
    rule_defaults(&a.rules[2]);
    a.rules[1].corner = CORNER_BL; a.rules[1].where = WHERE_ANY; a.rules[1].action = ACT_RUN; a.rules[1].delayMs = 1500;
    a.rules[1].warnLow = 1;
    wcscpy_s(a.rules[1].command, 512, L"\"C:\\Program Files\\Notepad++\\notepad++.exe\" -n10 file.txt");
    a.rules[2].corner = CORNER_TL; a.rules[2].where = WHERE_MONITOR; a.rules[2].action = ACT_SCREENS_OFF;
    a.rules[2].delayMs = 2250; a.rules[2].countdown = 0;
    wcscpy_s(a.rules[2].monitor, 256, L"\\\\?\\DISPLAY#GSM5BBF#5&2f1c2c1b&0&UID4352#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}");
    wcscpy_s(a.rules[2].monitorName, 64, L"\x00c9cran LG");
    BOOL saved = settings_save(&a);
    settings_load(&b);
    BOOL same = saved && a.mouseType == b.mouseType && a.productId == b.productId && a.deviceIndex == b.deviceIndex &&
                wcscmp(a.deviceName, b.deviceName) == 0 && a.showMeter == b.showMeter && a.ruleCount == b.ruleCount &&
                a.lowPercent == b.lowPercent && a.dlButton == b.dlButton && a.dlAllMonitors == b.dlAllMonitors &&
                !wcscmp(a.dlIcon, b.dlIcon) && !wcscmp(a.dlFolder, b.dlFolder) && a.dlCount == b.dlCount &&
                a.dlSort == b.dlSort && a.appCount == b.appCount &&
                memcmp(a.apps, b.apps, a.appCount * sizeof(App)) == 0 && a.nextAppId == b.nextAppId &&
                a.layoutCount == b.layoutCount && memcmp(a.layouts, b.layouts, a.layoutCount * sizeof(Layout)) == 0 &&
                memcmp(a.rules, b.rules, sizeof a.rules) == 0;
    EXPECT(same, "settings file: saved and read back the same (3 rules, non-ASCII names)");
    a.ruleCount = 1;
    settings_save(&a);
    settings_load(&b);
    EXPECT(b.ruleCount == 1 && memcmp(&a.rules[0], &b.rules[0], sizeof(Rule)) == 0, "settings file: removing rules");
    DeleteFileW(tmp);
    settings_load(&b);
    EXPECT(b.mouseType == MOUSE_NONE && b.ruleCount == 1 && b.rules[0].corner == CORNER_TR &&
           b.rules[0].action == ACT_LOCK && b.rules[0].delayMs == 5000 && !b.rules[0].warnLow && b.lowPercent == 20 &&
           !b.dlButton && b.dlAllMonitors && b.dlCount == 20 && b.dlSort == DL_SORT_ADDED && !b.dlIcon[0] && !b.dlFolder[0],
           "no settings file: the defaults");
    settings_use_file(old);
}

/* Eject media: the menu text, and that the list never offers the Windows drive or an internal disk. Never ejects. */
static void test_media(void)
{
    Drive d;
    ZeroMemory(&d, sizeof d);
    wcscpy_s(d.letters, 80, L"E:");
    wcscpy_s(d.volume, 64, L"BACKUP");
    wcscpy_s(d.product, 100, L"Samsung PSSD T7");
    d.bytes = 1000204886016ULL;
    media_label(&d);
    EXPECT(wcscmp(d.label, L"Eject &E: BACKUP \x2014 Samsung PSSD T7 (1.0 TB)") == 0, "eject menu text: letter, label, product, size");
    ZeroMemory(&d, sizeof d);
    wcscpy_s(d.letters, 80, L"F:, G:");
    wcscpy_s(d.product, 100, L"R&D Stick");
    d.bytes = 31914983424ULL;
    media_label(&d);
    EXPECT(wcscmp(d.label, L"Eject &F:, G: \x2014 R&&D Stick (32 GB)") == 0, "eject menu text: two letters, '&' doubled");
    Drive list[MAX_DRIVES];
    int n = media_list(list, MAX_DRIVES);
    wchar_t windir[MAX_PATH] = L"C";
    GetWindowsDirectoryW(windir, MAX_PATH);
    BOOL sys = FALSE;
    for (int i = 0; i < n; i++) sys |= (list[i].letterMask >> (towupper(windir[0]) - L'A')) & 1;
    char what[96];
    sprintf_s(what, sizeof what, "eject list (%d drive(s) now): never the Windows drive", n);
    EXPECT(n >= 0 && !sys, what);
}

/* A local time as a FILETIME (UTC), for the Downloads tests. */
static FILETIME local_ft(WORD y, WORD mo, WORD d, WORD h, WORD mi, WORD sec)
{
    SYSTEMTIME st = { y, mo, 0, d, h, mi, sec, 0 };
    FILETIME lf, ft;
    SystemTimeToFileTime(&st, &lf);
    LocalFileTimeToFileTime(&lf, &ft);
    return ft;
}

static FILETIME ft_ago(FILETIME now, ULONGLONG secs)
{
    ULARGE_INTEGER u = { now.dwLowDateTime, now.dwHighDateTime };
    u.QuadPart -= secs * 10000000ULL;
    FILETIME f = { u.LowPart, u.HighPart };
    return f;
}

static void make_item(const wchar_t *dir, const wchar_t *name, BOOL folder, FILETIME created, FILETIME written, DWORD attrs)
{
    wchar_t path[MAX_PATH];
    swprintf_s(path, MAX_PATH, L"%s\\%s", dir, name);
    if (folder) CreateDirectoryW(path, NULL);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, folder ? OPEN_EXISTING : CREATE_ALWAYS,
                           folder ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w;
    if (!folder) WriteFile(h, "jig", 3, &w, NULL);
    SetFileTime(h, &created, NULL, &written);
    CloseHandle(h);
    if (attrs) SetFileAttributesW(path, attrs);
}

/* Downloads: which items the panel shows, in each order, and how it says when. Uses a scratch folder. */
static void test_downloads(void)
{
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    wcscat_s(dir, MAX_PATH, L"jigrack-selftest-downloads");
    SHFILEOPSTRUCTW del = { NULL, FO_DELETE, NULL, NULL, FOF_NO_UI };
    wchar_t dz[MAX_PATH + 2] = {0};
    wcscpy_s(dz, MAX_PATH, dir);
    del.pFrom = dz;
    SHFileOperationW(&del);
    CreateDirectoryW(dir, NULL);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    make_item(dir, L"a.txt", FALSE, ft_ago(now, 3 * 86400), ft_ago(now, 60), 0);
    make_item(dir, L"b.zip", FALSE, ft_ago(now, 3600), ft_ago(now, 7200), 0);
    make_item(dir, L"c", TRUE, ft_ago(now, 2 * 86400), ft_ago(now, 2 * 86400), 0);
    make_item(dir, L"z.zip.crdownload", FALSE, now, now, 0);           /* still downloading */
    make_item(dir, L"hidden.txt", FALSE, now, now, FILE_ATTRIBUTE_HIDDEN);
    make_item(dir, L"desktop.ini", FALSE, now, now, 0);
    DlItem it[8];
    struct { int sort; const wchar_t *want; const char *what; } c[] = {
        { DL_SORT_ADDED, L"b.zip c a.txt", "downloads: date added, newest first; partial, hidden and desktop.ini left out" },
        { DL_SORT_MODIFIED, L"a.txt b.zip c", "downloads: date modified, newest first" },
        { DL_SORT_NAME, L"a.txt b.zip c", "downloads: name" },
        { DL_SORT_KIND, L"c a.txt b.zip", "downloads: kind (folders, then by type)" },
    };
    for (int k = 0; k < (int)ARRAYSIZE(c); k++) {
        int n = downloads_collect(dir, c[k].sort, it, 8);
        wchar_t got[200] = L"";
        for (int i = 0; i < n; i++) { if (i) wcscat_s(got, 200, L" "); wcscat_s(got, 200, it[i].name); }
        EXPECT(wcscmp(got, c[k].want) == 0, c[k].what);
        if (wcscmp(got, c[k].want)) trace("  got: %ls", got);
    }
    int n = downloads_collect(dir, DL_SORT_ADDED, it, 2);
    EXPECT(n == 2 && !wcscmp(it[0].name, L"b.zip") && !wcscmp(it[1].name, L"c") && it[1].dir,
           "downloads: only the newest 2 when it shows 2");
    SHFileOperationW(&del);

    FILETIME t = local_ft(2026, 11, 21, 15, 0, 0);
    struct { FILETIME when; const wchar_t *want; } w[] = {
        { ft_ago(t, 30), L"Just now" },
        { ft_ago(t, 5 * 60), L"5 min ago" },
        { ft_ago(t, 2 * 3600 + 59), L"2 hr ago" },
        { local_ft(2026, 11, 20, 23, 30, 0), L"Yesterday" },
        { local_ft(2026, 11, 17, 9, 0, 0), L"4 days ago" },
        { local_ft(2026, 11, 3, 9, 0, 0), L"Nov 3" },
        { local_ft(2025, 3, 2, 9, 0, 0), L"Mar 2, 2025" },
    };
    BOOL ok = TRUE;
    for (int k = 0; k < (int)ARRAYSIZE(w); k++) {
        wchar_t got[48];
        downloads_when(w[k].when, t, got, 48);
        if (wcscmp(got, w[k].want)) { ok = FALSE; trace("  when: got \"%ls\", want \"%ls\"", got, w[k].want); }
    }
    EXPECT(ok, "downloads: when (just now, minutes, hours, yesterday, days, a date, another year)");
    EXPECT(downloads_command_id(L"sort-kind") == DL_CMD_SORT_KIND && downloads_command_id(L"open") == DL_CMD_OPEN &&
           downloads_command_id(L"rm -rf") == -1, "downloads: jump list commands");
}

/* Workspace: where each position puts a window, in a work area that doesn't start at 0,0. */
static void test_workspace(void)
{
    RECT work = { 100, -3000, 5860, 240 }, r;               /* 5760 x 3240, above the main monitor */
    struct { int pos, x, y, w, h; RECT want; const char *what; } c[] = {
        { POS_MAXIMIZED, 0, 0, 0, 0, { 100, -3000, 5860, 240 }, "workspace: maximized is the whole work area" },
        { POS_LEFT, 0, 0, 0, 0, { 100, -3000, 2980, 240 }, "workspace: left half" },
        { POS_RIGHT, 0, 0, 0, 0, { 2980, -3000, 5860, 240 }, "workspace: right half" },
        { POS_TOP, 0, 0, 0, 0, { 100, -3000, 5860, -1380 }, "workspace: top half" },
        { POS_BOTTOM, 0, 0, 0, 0, { 100, -1380, 5860, 240 }, "workspace: bottom half" },
        { POS_LEFT_THIRD, 0, 0, 0, 0, { 100, -3000, 2020, 240 }, "workspace: left third" },
        { POS_MIDDLE_THIRD, 0, 0, 0, 0, { 2020, -3000, 3940, 240 }, "workspace: middle third" },
        { POS_RIGHT_THIRD, 0, 0, 0, 0, { 3940, -3000, 5860, 240 }, "workspace: right third" },
        { POS_CUSTOM, 25, 10, 50, 80, { 1540, -2676, 4420, -84 }, "workspace: custom 25%, 10%, 50% x 80%" },
        { POS_CUSTOM, 90, 90, 50, 50, { 5284, -84, 5860, 240 }, "workspace: custom past the edge is cut to the work area" },
    };
    for (int k = 0; k < (int)ARRAYSIZE(c); k++) {
        workspace_target(&work, c[k].pos, c[k].x, c[k].y, c[k].w, c[k].h, &r);
        BOOL ok = EqualRect(&r, &c[k].want);
        EXPECT(ok, c[k].what);
        if (!ok) trace("  got %ld,%ld,%ld,%ld", r.left, r.top, r.right, r.bottom);
    }

    /* dropping an app on a monitor in the picture */
    RECT land = { 0, -3240, 5760, 0 }, port = { 5760, -4164, 8595, 876 };
    struct { const RECT *m; int fx, fy, want; } d[] = {
        { &land, 10, 10, POS_LEFT }, { &land, 50, 10, POS_MAXIMIZED }, { &land, 90, 10, POS_RIGHT },
        { &land, 10, 60, POS_LEFT_THIRD }, { &land, 50, 60, POS_MIDDLE_THIRD }, { &land, 90, 60, POS_RIGHT_THIRD },
        { &port, 50, 10, POS_TOP }, { &port, 50, 50, POS_MAXIMIZED }, { &port, 50, 90, POS_BOTTOM },
    };
    BOOL ok = TRUE;
    for (int k = 0; k < (int)ARRAYSIZE(d); k++) {
        POINT p = { d[k].m->left + MulDiv(d[k].m->right - d[k].m->left, d[k].fx, 100),
                    d[k].m->top + MulDiv(d[k].m->bottom - d[k].m->top, d[k].fy, 100) };
        int got = workspace_snap(d[k].m, p);
        if (got != d[k].want) { ok = FALSE; trace("  drop at %d%%, %d%%: %ls, want %ls", d[k].fx, d[k].fy, POSITION_LABELS[got], POSITION_LABELS[d[k].want]); }
    }
    EXPECT(ok, "workspace: drops snap (landscape: halves and maximized on top, thirds below; portrait: top, maximized, bottom)");

    /* which saved monitor setup matches the monitors connected */
    ZeroMemory(g_set.layouts, sizeof g_set.layouts);
    g_set.layoutCount = 2;
    g_set.layouts[0].nmon = 2;
    wcscpy_s(g_set.layouts[0].mons[0].path, 256, L"A");
    SetRect(&g_set.layouts[0].mons[0].r, 0, 0, 3840, 2400);
    wcscpy_s(g_set.layouts[0].mons[1].path, 256, L"B");
    SetRect(&g_set.layouts[0].mons[1].r, 0, -3240, 5760, 0);
    g_set.layouts[1].nmon = 1;
    SetRect(&g_set.layouts[1].mons[0].r, 0, 0, 3840, 2400);
    LayoutMon cur[2];
    ZeroMemory(cur, sizeof cur);
    wcscpy_s(cur[0].path, 256, L"B");
    cur[0].r = g_set.layouts[0].mons[1].r;
    wcscpy_s(cur[1].path, 256, L"A");
    cur[1].r = g_set.layouts[0].mons[0].r;
    EXPECT(workspace_find_layout(cur, 2) == 0, "workspace: the setup with the same monitors (in any order) matches");
    wcscpy_s(cur[0].path, 256, L"B2");
    EXPECT(workspace_find_layout(cur, 2) == 0, "workspace: same places and sizes but a new device path still match");
    EXPECT(workspace_find_layout(&cur[1], 1) == 1, "workspace: just the laptop matches the one-monitor setup");
    SetRect(&cur[0].r, 0, -5760, 3240, 0);                            /* the big one turned to portrait */
    EXPECT(workspace_find_layout(cur, 2) == -1, "workspace: a rotated monitor is a different setup");
}

int hotcorner_selftest(void)
{
    Settings saved = g_set;
    g_dry = TRUE;
    g_fails = 0;
    MonitorDesc m[MAX_MONS];
    int n = monitors(m, MAX_MONS, FALSE);
    test_default_rule(m, n);
    test_more_rules(m, n);
    test_low_battery();
    test_settings_file();
    test_media();
    test_downloads();
    test_workspace();
    trace("selftest done: %d failure(s)", g_fails);
    g_set = saved;
    return g_fails;
}
