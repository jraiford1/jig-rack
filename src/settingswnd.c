/* Jig Rack -- the Settings window: one tab per jig. Every change is saved and takes effect at once.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE. */
#define COBJMACROS
#include "jigrack.h"
#include "resource.h"
#include "gdip.h"
#include <uxtheme.h>
#include <stdlib.h>
#include <shlobj.h>
#include <shobjidl.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")

enum { PAGE_MOUSE, PAGE_CORNERS, PAGE_DOWNLOADS, PAGE_WORKSPACE, PAGES };

static HINSTANCE g_inst;
static HWND g_dlg, g_tab, g_pages[PAGES];
static DeviceList g_devs;
static BOOL g_searching, g_searched;
static MonitorDesc g_mons[16];
static int g_nmons;
static int g_rule = -1;                     /* selected hot corner rule */
static BOOL g_filling;                      /* filling controls: ignore their change notifications */

HWND settings_window(void) { return g_dlg; }

enum { APPLY_MOUSE = 1, APPLY_CORNERS = 2, APPLY_DOWNLOADS = 4 };
static void save_and_apply(int what)
{
    settings_save(&g_set);
    if (what & APPLY_MOUSE) mouse_apply();
    if (what & APPLY_CORNERS) hotcorner_apply();
    if (what & APPLY_DOWNLOADS) downloads_apply();
    tray_refresh();
}

/* ---------------------------------------------------------------- Mouse tab ----------------------------------- */

static void mouse_fill_list(HWND pg)
{
    HWND lb = GetDlgItem(pg, IDC_MOUSE_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (int i = 0; i < g_devs.count; i++) {
        const FoundDevice *d = &g_devs.d[i];
        wchar_t t[200];
        if (d->percent >= 0) swprintf_s(t, 200, L"%s \x2014 %d%%   (%s)", d->name, d->percent, d->via);
        else swprintf_s(t, 200, L"%s   (%s)", d->name, d->via);
        int idx = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(lb, LB_SETITEMDATA, idx, i);
        if (d->productId == g_set.productId && d->index == g_set.deviceIndex) sel = idx;
    }
    if (sel < 0 && g_set.productId) {                        /* the chosen mouse, asleep or unplugged */
        wchar_t t[200];
        swprintf_s(t, 200, L"%s   (not found right now)", g_set.deviceName[0] ? g_set.deviceName : L"Logitech mouse");
        sel = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(lb, LB_SETITEMDATA, sel, (LPARAM)-1);
    }
    SendMessageW(lb, LB_SETCURSEL, sel, 0);
}

static void mouse_enable(HWND pg)
{
    BOOL logi = g_set.mouseType == MOUSE_LOGITECH;
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_LISTLABEL), logi);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_LIST), logi);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_STATUS), logi);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_REFRESH), logi && !g_searching);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_METER), mouse_chosen(&g_set));
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_LOWLABEL), logi);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_LOW), logi);
    EnableWindow(GetDlgItem(pg, IDC_MOUSE_LOWNOTE), logi);
}

static void mouse_search_start(HWND pg)
{
    if (g_searching) return;
    g_searching = TRUE;
    SetDlgItemTextW(pg, IDC_MOUSE_STATUS, L"Looking for Logitech mice\x2026");
    mouse_enable(pg);
    mouse_search(g_dlg);
}

static void mouse_search_done(DeviceList *l)
{
    g_devs = *l;
    g_searching = FALSE;
    g_searched = TRUE;
    HWND pg = g_pages[PAGE_MOUSE];
    if (!pg) return;
    mouse_fill_list(pg);
    SetDlgItemTextW(pg, IDC_MOUSE_STATUS, g_devs.count
        ? L"Not listed? Move that mouse to wake it, then click Refresh."
        : L"No Logitech mouse found. Move the mouse to wake it (or plug in its receiver), then click Refresh.");
    mouse_enable(pg);
}

static INT_PTR CALLBACK mouse_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        g_filling = TRUE;
        HWND cb = GetDlgItem(h, IDC_MOUSE_TYPE);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"None");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Logitech (receiver or cable)");
        SendMessageW(cb, CB_SETCURSEL, g_set.mouseType, 0);
        CheckDlgButton(h, IDC_MOUSE_METER, g_set.showMeter ? BST_CHECKED : BST_UNCHECKED);
        SendDlgItemMessageW(h, IDC_MOUSE_LOW, EM_LIMITTEXT, 2, 0);
        SetDlgItemInt(h, IDC_MOUSE_LOW, g_set.lowPercent, FALSE);
        mouse_fill_list(h);
        g_filling = FALSE;
        g_pages[PAGE_MOUSE] = h;
        mouse_enable(h);
        if (g_set.mouseType == MOUSE_LOGITECH) mouse_search_start(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (g_filling) break;
        switch (LOWORD(wp)) {
        case IDC_MOUSE_TYPE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                g_set.mouseType = (int)SendDlgItemMessageW(h, IDC_MOUSE_TYPE, CB_GETCURSEL, 0, 0);
                save_and_apply(APPLY_MOUSE);
                mouse_enable(h);
                if (g_set.mouseType == MOUSE_LOGITECH && !g_searched) mouse_search_start(h);
            }
            return TRUE;
        case IDC_MOUSE_LIST:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                int idx = (int)SendDlgItemMessageW(h, IDC_MOUSE_LIST, LB_GETCURSEL, 0, 0);
                int i = idx < 0 ? -1 : (int)SendDlgItemMessageW(h, IDC_MOUSE_LIST, LB_GETITEMDATA, idx, 0);
                if (i >= 0 && i < g_devs.count) {
                    g_set.productId = g_devs.d[i].productId;
                    g_set.deviceIndex = g_devs.d[i].index;
                    wcscpy_s(g_set.deviceName, 64, g_devs.d[i].name);
                    save_and_apply(APPLY_MOUSE);
                    mouse_enable(h);
                }
            }
            return TRUE;
        case IDC_MOUSE_REFRESH:
            mouse_refresh();
            mouse_search_start(h);
            return TRUE;
        case IDC_MOUSE_METER:
            g_set.showMeter = IsDlgButtonChecked(h, IDC_MOUSE_METER) == BST_CHECKED;
            save_and_apply(0);
            return TRUE;
        case IDC_MOUSE_LOW: {                                  /* 1-99; anything else shows the level in use again */
            BOOL ok;
            UINT v = GetDlgItemInt(h, IDC_MOUSE_LOW, &ok, FALSE);
            if (HIWORD(wp) == EN_CHANGE && ok && v >= 1 && v <= 99 && (int)v != g_set.lowPercent) {
                g_set.lowPercent = (int)v;
                save_and_apply(0);                             /* redraws the meter in its new colour */
            } else if (HIWORD(wp) == EN_KILLFOCUS && (!ok || v < 1 || v > 99)) {
                g_filling = TRUE;
                SetDlgItemInt(h, IDC_MOUSE_LOW, g_set.lowPercent, FALSE);
                g_filling = FALSE;
            }
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- Hot Corners tab ----------------------------- */

/* Let the rules list scroll sideways just as far as its widest rule (recomputed on every change, and when the DPI
   changes, so it never keeps the width of a longer description it no longer has). */
static void rules_fit(HWND lb)
{
    HDC dc = GetDC(lb);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(lb, WM_GETFONT, 0, 0));
    int widest = 0, n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        wchar_t t[700];
        SIZE sz;
        if (SendMessageW(lb, LB_GETTEXTLEN, i, 0) >= 700) continue;
        int len = (int)SendMessageW(lb, LB_GETTEXT, i, (LPARAM)t);
        if (len > 0 && GetTextExtentPoint32W(dc, t, len, &sz) && sz.cx > widest) widest = sz.cx;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(lb, dc);
    SendMessageW(lb, LB_SETHORIZONTALEXTENT, widest ? widest + 8 : 0, 0);
}

static void rules_fill(HWND pg)
{
    HWND lb = GetDlgItem(pg, IDC_RULES);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_set.ruleCount; i++) {
        wchar_t t[700];
        rule_describe(&g_set.rules[i], t, 700);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
    }
    rules_fit(lb);
    if (g_rule >= g_set.ruleCount) g_rule = g_set.ruleCount - 1;
    SendMessageW(lb, LB_SETCURSEL, g_rule, 0);
}

static void rule_relabel(HWND pg)
{
    if (g_rule < 0) return;
    HWND lb = GetDlgItem(pg, IDC_RULES);
    wchar_t t[700];
    rule_describe(&g_set.rules[g_rule], t, 700);
    SendMessageW(lb, LB_DELETESTRING, g_rule, 0);
    SendMessageW(lb, LB_INSERTSTRING, g_rule, (LPARAM)t);
    SendMessageW(lb, LB_SETCURSEL, g_rule, 0);
    rules_fit(lb);
}

static void rule_fill_editor(HWND pg)
{
    g_filling = TRUE;
    BOOL have = g_rule >= 0 && g_rule < g_set.ruleCount;
    static const int ids[] = { IDC_RULE_CORNER, IDC_RULE_WHERE, IDC_RULE_ACTION, IDC_RULE_COMMAND, IDC_RULE_CMDLABEL,
                               IDC_RULE_DELAY, IDC_RULE_COUNTDOWN, IDC_RULE_WARN, IDC_RULE_DEL, IDC_RULE_GROUP };
    for (int k = 0; k < (int)(sizeof ids / sizeof ids[0]); k++) EnableWindow(GetDlgItem(pg, ids[k]), have);
    EnableWindow(GetDlgItem(pg, IDC_RULE_ADD), g_set.ruleCount < MAX_RULES);
    HWND where = GetDlgItem(pg, IDC_RULE_WHERE);
    SendMessageW(where, CB_RESETCONTENT, 0, 0);
    if (have) {
        const Rule *r = &g_set.rules[g_rule];
        SendDlgItemMessageW(pg, IDC_RULE_CORNER, CB_SETCURSEL, r->corner, 0);
        int i = (int)SendMessageW(where, CB_ADDSTRING, 0, (LPARAM)L"The whole workspace (all monitors together)");
        SendMessageW(where, CB_SETITEMDATA, i, (LPARAM)-1);
        i = (int)SendMessageW(where, CB_ADDSTRING, 0, (LPARAM)L"Any monitor");
        SendMessageW(where, CB_SETITEMDATA, i, (LPARAM)-2);
        int sel = r->where == WHERE_ANY ? 1 : 0;
        for (int k = 0; k < g_nmons; k++) {
            wchar_t t[160];
            const wchar_t *gdi = wcsncmp(g_mons[k].gdi, L"\\\\.\\", 4) == 0 ? g_mons[k].gdi + 4 : g_mons[k].gdi;
            swprintf_s(t, 160, L"%s \x2014 %ld \x00d7 %ld (%s)", g_mons[k].name, g_mons[k].r.right - g_mons[k].r.left,
                       g_mons[k].r.bottom - g_mons[k].r.top, gdi);
            i = (int)SendMessageW(where, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(where, CB_SETITEMDATA, i, k);
            if (r->where == WHERE_MONITOR && _wcsicmp(g_mons[k].path, r->monitor) == 0) sel = i;
        }
        if (r->where == WHERE_MONITOR && sel < 2) {           /* its monitor isn't connected now */
            wchar_t t[160];
            swprintf_s(t, 160, L"%s (not connected)", r->monitorName[0] ? r->monitorName : L"A monitor");
            sel = (int)SendMessageW(where, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(where, CB_SETITEMDATA, sel, (LPARAM)-3);
        }
        SendMessageW(where, CB_SETCURSEL, sel, 0);
        SendDlgItemMessageW(pg, IDC_RULE_ACTION, CB_SETCURSEL, r->action, 0);
        SetDlgItemTextW(pg, IDC_RULE_COMMAND, r->command);
        wchar_t d[32];
        swprintf_s(d, 32, L"%g", r->delayMs / 1000.0);
        SetDlgItemTextW(pg, IDC_RULE_DELAY, d);
        CheckDlgButton(pg, IDC_RULE_COUNTDOWN, r->countdown ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(pg, IDC_RULE_WARN, r->warnLow ? BST_CHECKED : BST_UNCHECKED);
        EnableWindow(GetDlgItem(pg, IDC_RULE_WARN), r->countdown);   /* the warning is part of the countdown */
        EnableWindow(GetDlgItem(pg, IDC_RULE_COMMAND), r->action == ACT_RUN);
        EnableWindow(GetDlgItem(pg, IDC_RULE_CMDLABEL), r->action == ACT_RUN);
    } else {
        SendDlgItemMessageW(pg, IDC_RULE_CORNER, CB_SETCURSEL, (WPARAM)-1, 0);
        SendDlgItemMessageW(pg, IDC_RULE_ACTION, CB_SETCURSEL, (WPARAM)-1, 0);
        SetDlgItemTextW(pg, IDC_RULE_COMMAND, L"");
        SetDlgItemTextW(pg, IDC_RULE_DELAY, L"");
        CheckDlgButton(pg, IDC_RULE_COUNTDOWN, BST_UNCHECKED);
        CheckDlgButton(pg, IDC_RULE_WARN, BST_UNCHECKED);
    }
    g_filling = FALSE;
}

/* The delay box: whole or decimal seconds, 0.1 to 600. */
static BOOL read_delay(HWND pg, int *ms)
{
    wchar_t t[32], *end;
    GetDlgItemTextW(pg, IDC_RULE_DELAY, t, 32);
    double s = wcstod(t, &end);
    while (*end == L' ') end++;
    if (end == t || *end || s < 0.1 || s > 600) return FALSE;
    *ms = (int)(s * 1000 + 0.5);
    return TRUE;
}

static INT_PTR CALLBACK corners_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        g_filling = TRUE;
        for (int c = 0; c < CORNERS; c++) SendDlgItemMessageW(h, IDC_RULE_CORNER, CB_ADDSTRING, 0, (LPARAM)corner_label(c));
        for (int a = 0; a < ACTIONS; a++) SendDlgItemMessageW(h, IDC_RULE_ACTION, CB_ADDSTRING, 0, (LPARAM)action_label(a));
        SendDlgItemMessageW(h, IDC_RULE_COMMAND, EM_SETCUEBANNER, TRUE,
                            (LPARAM)L"e.g. notepad.exe, or \"C:\\Path\\app.exe\" --option");
        g_nmons = monitors_list(g_mons, 16);
        g_rule = g_set.ruleCount ? 0 : -1;
        g_filling = FALSE;
        rules_fill(h);
        rule_fill_editor(h);
        g_pages[PAGE_CORNERS] = h;
        return TRUE;
    }
    case WM_DPICHANGED_AFTERPARENT:                    /* the list's font changed size with the monitor */
        rules_fit(GetDlgItem(h, IDC_RULES));
        break;
    case WM_COMMAND: {
        if (g_filling) break;
        int id = LOWORD(wp), code = HIWORD(wp);
        Rule *r = g_rule >= 0 ? &g_set.rules[g_rule] : NULL;
        switch (id) {
        case IDC_RULES:
            if (code == LBN_SELCHANGE) {
                g_rule = (int)SendDlgItemMessageW(h, IDC_RULES, LB_GETCURSEL, 0, 0);
                rule_fill_editor(h);
            }
            return TRUE;
        case IDC_RULE_ADD: {
            if (g_set.ruleCount >= MAX_RULES) return TRUE;
            Rule *n = &g_set.rules[g_set.ruleCount];
            rule_defaults(n);
            for (int c = 0; c < CORNERS; c++) {                /* the first corner no rule uses yet */
                BOOL used = FALSE;
                for (int i = 0; i < g_set.ruleCount; i++) used |= g_set.rules[i].corner == c;
                if (!used) { n->corner = c; break; }
            }
            g_rule = g_set.ruleCount++;
            rules_fill(h);
            rule_fill_editor(h);
            save_and_apply(APPLY_CORNERS);
            SetFocus(GetDlgItem(h, IDC_RULE_CORNER));
            return TRUE;
        }
        case IDC_RULE_DEL:
            if (!r) return TRUE;
            for (int i = g_rule; i + 1 < g_set.ruleCount; i++) g_set.rules[i] = g_set.rules[i + 1];
            g_set.ruleCount--;
            if (g_rule >= g_set.ruleCount) g_rule = g_set.ruleCount - 1;
            rules_fill(h);
            rule_fill_editor(h);
            save_and_apply(APPLY_CORNERS);
            return TRUE;
        }
        if (!r) break;
        switch (id) {
        case IDC_RULE_CORNER:
            if (code != CBN_SELCHANGE) return TRUE;
            r->corner = (int)SendDlgItemMessageW(h, IDC_RULE_CORNER, CB_GETCURSEL, 0, 0);
            break;
        case IDC_RULE_WHERE: {
            if (code != CBN_SELCHANGE) return TRUE;
            int i = (int)SendDlgItemMessageW(h, IDC_RULE_WHERE, CB_GETCURSEL, 0, 0);
            int k = (int)SendDlgItemMessageW(h, IDC_RULE_WHERE, CB_GETITEMDATA, i, 0);
            if (k == -1) r->where = WHERE_WORKSPACE;
            else if (k == -2) r->where = WHERE_ANY;
            else if (k >= 0 && k < g_nmons) {
                r->where = WHERE_MONITOR;
                wcscpy_s(r->monitor, 256, g_mons[k].path);
                wcscpy_s(r->monitorName, 64, g_mons[k].name);
            }
            break;
        }
        case IDC_RULE_ACTION:
            if (code != CBN_SELCHANGE) return TRUE;
            r->action = (int)SendDlgItemMessageW(h, IDC_RULE_ACTION, CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(h, IDC_RULE_COMMAND), r->action == ACT_RUN);
            EnableWindow(GetDlgItem(h, IDC_RULE_CMDLABEL), r->action == ACT_RUN);
            if (r->action == ACT_RUN && !r->command[0]) SetFocus(GetDlgItem(h, IDC_RULE_COMMAND));
            break;
        case IDC_RULE_COMMAND:
            if (code != EN_CHANGE) return TRUE;
            GetDlgItemTextW(h, IDC_RULE_COMMAND, r->command, 512);
            break;
        case IDC_RULE_DELAY:
            if (code == EN_KILLFOCUS) {                        /* not a valid delay: show the one in use */
                int ms;
                if (!read_delay(h, &ms)) {
                    wchar_t d[32];
                    swprintf_s(d, 32, L"%g", r->delayMs / 1000.0);
                    g_filling = TRUE;
                    SetDlgItemTextW(h, IDC_RULE_DELAY, d);
                    g_filling = FALSE;
                }
                return TRUE;
            }
            if (code != EN_CHANGE || !read_delay(h, &r->delayMs)) return TRUE;
            break;
        case IDC_RULE_COUNTDOWN:
            r->countdown = IsDlgButtonChecked(h, IDC_RULE_COUNTDOWN) == BST_CHECKED;
            EnableWindow(GetDlgItem(h, IDC_RULE_WARN), r->countdown);
            break;
        case IDC_RULE_WARN:
            r->warnLow = IsDlgButtonChecked(h, IDC_RULE_WARN) == BST_CHECKED;
            break;
        default:
            return FALSE;
        }
        rule_relabel(h);
        save_and_apply(APPLY_CORNERS);
        return TRUE;
    }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- Workspace tab ------------------------------- */

static int g_app = -1;                      /* selected workspace app */

static void apps_fill(HWND pg)
{
    HWND lb = GetDlgItem(pg, IDC_WS_APPS);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_set.appCount; i++) {
        const App *a = &g_set.apps[i];
        wchar_t t[400], where[96];
        if (!a->monitor[0]) wcscpy_s(where, 96, L"main monitor");
        else swprintf_s(where, 96, L"%s", a->monitorName[0] ? a->monitorName : L"a monitor");
        if (a->pos == POS_CUSTOM)
            swprintf_s(t, 400, L"%s: %s, at %d%%, %d%% (%d%% \x00d7 %d%%)", a->name[0] ? a->name : L"(no program)", where,
                       a->x, a->y, a->w, a->h);
        else
            swprintf_s(t, 400, L"%s: %s, %s", a->name[0] ? a->name : L"(no program)", where, POSITION_LABELS[a->pos]);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
    }
    if (g_app >= g_set.appCount) g_app = g_set.appCount - 1;
    SendMessageW(lb, LB_SETCURSEL, g_app, 0);
    rules_fit(lb);                                             /* scroll sideways only as far as needed */
}

static void app_fill_editor(HWND pg)
{
    g_filling = TRUE;
    BOOL have = g_app >= 0 && g_app < g_set.appCount;
    static const int ids[] = { IDC_WS_DEL, IDC_WS_GROUP, IDC_WS_PROGRAM, IDC_WS_BROWSE, IDC_WS_ARGS, IDC_WS_TITLE,
                               IDC_WS_MONITOR, IDC_WS_POS, IDC_WS_CURRENT, IDC_WS_L1, IDC_WS_L2, IDC_WS_L3, IDC_WS_L4,
                               IDC_WS_L5 };
    for (int k = 0; k < (int)ARRAYSIZE(ids); k++) EnableWindow(GetDlgItem(pg, ids[k]), have);
    EnableWindow(GetDlgItem(pg, IDC_WS_ADD), g_set.appCount < MAX_APPS);
    EnableWindow(GetDlgItem(pg, IDC_WS_UP), have && g_app > 0);
    EnableWindow(GetDlgItem(pg, IDC_WS_DOWN), have && g_app < g_set.appCount - 1);
    HWND mon = GetDlgItem(pg, IDC_WS_MONITOR);
    SendMessageW(mon, CB_RESETCONTENT, 0, 0);
    const App *a = have ? &g_set.apps[g_app] : NULL;
    if (a) {
        SetDlgItemTextW(pg, IDC_WS_PROGRAM, a->command);
        SetDlgItemTextW(pg, IDC_WS_ARGS, a->args);
        SetDlgItemTextW(pg, IDC_WS_TITLE, a->title);
        int i = (int)SendMessageW(mon, CB_ADDSTRING, 0, (LPARAM)L"The main monitor");
        SendMessageW(mon, CB_SETITEMDATA, i, (LPARAM)-1);
        int sel = 0;
        for (int k = 0; k < g_nmons; k++) {
            wchar_t t[160];
            const wchar_t *gdi = wcsncmp(g_mons[k].gdi, L"\\\\.\\", 4) == 0 ? g_mons[k].gdi + 4 : g_mons[k].gdi;
            swprintf_s(t, 160, L"%s \x2014 %ld \x00d7 %ld (%s)", g_mons[k].name, g_mons[k].r.right - g_mons[k].r.left,
                       g_mons[k].r.bottom - g_mons[k].r.top, gdi);
            i = (int)SendMessageW(mon, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(mon, CB_SETITEMDATA, i, k);
            if (a->monitor[0] && _wcsicmp(g_mons[k].path, a->monitor) == 0) sel = i;
        }
        if (a->monitor[0] && sel == 0) {                       /* its monitor isn't connected now */
            wchar_t t[160];
            swprintf_s(t, 160, L"%s (not connected)", a->monitorName[0] ? a->monitorName : L"A monitor");
            sel = (int)SendMessageW(mon, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(mon, CB_SETITEMDATA, sel, (LPARAM)-3);
        }
        SendMessageW(mon, CB_SETCURSEL, sel, 0);
        SendDlgItemMessageW(pg, IDC_WS_POS, CB_SETCURSEL, a->pos, 0);
        SetDlgItemInt(pg, IDC_WS_X, a->x, FALSE);
        SetDlgItemInt(pg, IDC_WS_Y, a->y, FALSE);
        SetDlgItemInt(pg, IDC_WS_W, a->w, FALSE);
        SetDlgItemInt(pg, IDC_WS_H, a->h, FALSE);
    } else {
        static const int clear[] = { IDC_WS_PROGRAM, IDC_WS_ARGS, IDC_WS_TITLE, IDC_WS_X, IDC_WS_Y, IDC_WS_W, IDC_WS_H };
        for (int k = 0; k < (int)ARRAYSIZE(clear); k++) SetDlgItemTextW(pg, clear[k], L"");
        SendDlgItemMessageW(pg, IDC_WS_POS, CB_SETCURSEL, (WPARAM)-1, 0);
    }
    BOOL custom = a && a->pos == POS_CUSTOM;
    static const int cust[] = { IDC_WS_X, IDC_WS_Y, IDC_WS_W, IDC_WS_H, IDC_WS_L6, IDC_WS_L7, IDC_WS_L8, IDC_WS_L9, IDC_WS_L10 };
    for (int k = 0; k < (int)ARRAYSIZE(cust); k++) EnableWindow(GetDlgItem(pg, cust[k]), custom);
    g_filling = FALSE;
}

static void apps_changed(HWND pg)
{
    save_and_apply(0);
    apps_fill(pg);
    app_fill_editor(pg);
}

/* "+": a menu of the open app windows (adds one with its program, monitor and place), and "Program file...". */
static void app_add(HWND pg)
{
    if (g_set.appCount >= MAX_APPS) return;
    HWND wins[40];
    int n = workspace_open_windows(wins, 40);
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < n; i++) {
        wchar_t t[120];
        GetWindowTextW(wins[i], t, 100);
        if (wcslen(t) >= 99) wcscat_s(t, 120, L"\x2026");
        for (wchar_t *c = t; *c; c++) if (*c == L'&') *c = L'+';
        AppendMenuW(m, MF_STRING, 100 + i, t);
    }
    if (n) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1, L"Program &file\x2026");
    RECT r;
    GetWindowRect(GetDlgItem(pg, IDC_WS_ADD), &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTBUTTON, r.left, r.bottom, 0, pg, NULL);
    DestroyMenu(m);
    if (!cmd) return;
    App a;
    ZeroMemory(&a, sizeof a);
    a.pos = POS_MAXIMIZED;
    a.w = 50; a.h = 100;
    if (cmd >= 100 && cmd < 100 + n && IsWindow(wins[cmd - 100])) {
        workspace_app_from_window(wins[cmd - 100], &a);
    } else {
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW of = { sizeof of };
        of.hwndOwner = pg;
        of.lpstrFilter = L"Programs\0*.exe;*.lnk;*.bat;*.cmd\0All files\0*.*\0";
        of.lpstrFile = file;
        of.nMaxFile = MAX_PATH;
        of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
        if (!GetOpenFileNameW(&of)) return;
        workspace_app_from_file(file, &a);
    }
    g_set.apps[g_set.appCount++] = a;
    g_app = g_set.appCount - 1;
    apps_changed(pg);
}

static INT_PTR CALLBACK workspace_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        for (int k = 0; k < POSITIONS; k++) SendDlgItemMessageW(h, IDC_WS_POS, CB_ADDSTRING, 0, (LPARAM)POSITION_LABELS[k]);
        for (int id = IDC_WS_X; id <= IDC_WS_H; id++) SendDlgItemMessageW(h, id, EM_LIMITTEXT, 3, 0);
        if (!g_nmons) g_nmons = monitors_list(g_mons, 16);
        g_app = g_set.appCount ? 0 : -1;
        g_pages[PAGE_WORKSPACE] = h;
        apps_fill(h);
        app_fill_editor(h);
        return TRUE;
    case WM_COMMAND: {
        if (g_filling) break;
        int id = LOWORD(wp), code = HIWORD(wp);
        App *a = g_app >= 0 && g_app < g_set.appCount ? &g_set.apps[g_app] : NULL;
        switch (id) {
        case IDC_WS_APPS:
            if (code == LBN_SELCHANGE) {
                g_app = (int)SendDlgItemMessageW(h, IDC_WS_APPS, LB_GETCURSEL, 0, 0);
                app_fill_editor(h);
            }
            return TRUE;
        case IDC_WS_ADD:
            app_add(h);
            return TRUE;
        case IDC_WS_DEL:
            if (!a) return TRUE;
            MoveMemory(&g_set.apps[g_app], &g_set.apps[g_app + 1], (g_set.appCount - g_app - 1) * sizeof(App));
            g_set.appCount--;
            if (g_app >= g_set.appCount) g_app = g_set.appCount - 1;
            apps_changed(h);
            return TRUE;
        case IDC_WS_UP: case IDC_WS_DOWN: {
            int to = g_app + (id == IDC_WS_UP ? -1 : 1);
            if (!a || to < 0 || to >= g_set.appCount) return TRUE;
            App t = g_set.apps[to];
            g_set.apps[to] = g_set.apps[g_app];
            g_set.apps[g_app] = t;
            g_app = to;
            apps_changed(h);
            return TRUE;
        }
        case IDC_WS_BROWSE: {
            if (!a) return TRUE;
            wchar_t file[MAX_PATH] = L"";
            OPENFILENAMEW of = { sizeof of };
            of.hwndOwner = h;
            of.lpstrFilter = L"Programs\0*.exe;*.lnk;*.bat;*.cmd\0All files\0*.*\0";
            of.lpstrFile = file;
            of.nMaxFile = MAX_PATH;
            of.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (!GetOpenFileNameW(&of)) return TRUE;
            workspace_app_from_file(file, a);
            apps_changed(h);
            return TRUE;
        }
        case IDC_WS_PROGRAM: case IDC_WS_ARGS: case IDC_WS_TITLE:
            if (!a || code != EN_CHANGE) return TRUE;
            if (id == IDC_WS_PROGRAM) {
                GetDlgItemTextW(h, id, a->command, ARRAYSIZE(a->command));
                if (_wcsnicmp(a->command, L"shell:", 6))                   /* a program: its windows are its own */
                    wcscpy_s(a->match, MAX_PATH, a->command);
            } else if (id == IDC_WS_ARGS) GetDlgItemTextW(h, id, a->args, ARRAYSIZE(a->args));
            else GetDlgItemTextW(h, id, a->title, ARRAYSIZE(a->title));
            save_and_apply(0);
            return TRUE;
        case IDC_WS_MONITOR:
            if (!a || code != CBN_SELCHANGE) return TRUE;
            {
                int sel = (int)SendDlgItemMessageW(h, IDC_WS_MONITOR, CB_GETCURSEL, 0, 0);
                LRESULT k = SendDlgItemMessageW(h, IDC_WS_MONITOR, CB_GETITEMDATA, sel, 0);
                if (k == -1) { a->monitor[0] = a->monitorName[0] = 0; }
                else if (k >= 0 && k < g_nmons) {
                    wcscpy_s(a->monitor, ARRAYSIZE(a->monitor), g_mons[k].path);
                    wcscpy_s(a->monitorName, ARRAYSIZE(a->monitorName), g_mons[k].name);
                }
            }
            save_and_apply(0);
            apps_fill(h);
            return TRUE;
        case IDC_WS_POS:
            if (!a || code != CBN_SELCHANGE) return TRUE;
            a->pos = (int)SendDlgItemMessageW(h, IDC_WS_POS, CB_GETCURSEL, 0, 0);
            apps_changed(h);
            return TRUE;
        case IDC_WS_CURRENT:
            if (!a) return TRUE;
            if (!workspace_use_current(a))
                MessageBoxW(h, L"Open the app first, put its window where you want it, then click this again.",
                            L"Use current position", MB_OK | MB_ICONINFORMATION);
            else apps_changed(h);
            return TRUE;
        case IDC_WS_X: case IDC_WS_Y: case IDC_WS_W: case IDC_WS_H: {
            if (!a) return TRUE;
            int *v = id == IDC_WS_X ? &a->x : id == IDC_WS_Y ? &a->y : id == IDC_WS_W ? &a->w : &a->h;
            BOOL ok;
            UINT n = GetDlgItemInt(h, id, &ok, FALSE);
            if (code == EN_CHANGE && ok && n <= 100 && (int)n != *v) {
                *v = (int)n;
                save_and_apply(0);
                apps_fill(h);
            } else if (code == EN_KILLFOCUS && (!ok || n > 100)) {
                g_filling = TRUE;
                SetDlgItemInt(h, id, *v, FALSE);
                g_filling = FALSE;
            }
            return TRUE;
        }
        }
        break;
    }
    }
    return FALSE;
}

/* ---------------------------------------------------------------- Downloads tab ------------------------------- */

static const int SORT_ORDER[DL_SORTS] = { DL_SORT_NAME, DL_SORT_KIND, DL_SORT_ADDED, DL_SORT_MODIFIED };  /* as on a Mac */

static void dl_fill(HWND pg)
{
    g_filling = TRUE;
    CheckDlgButton(pg, IDC_DL_BUTTON, g_set.dlButton ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(pg, IDC_DL_ALLMON, g_set.dlAllMonitors ? BST_CHECKED : BST_UNCHECKED);
    wchar_t folder[MAX_PATH];
    downloads_folder(folder, MAX_PATH);
    SetDlgItemTextW(pg, IDC_DL_FOLDER, folder);
    SetDlgItemInt(pg, IDC_DL_COUNT, g_set.dlCount, FALSE);
    for (int k = 0; k < DL_SORTS; k++)
        if (SORT_ORDER[k] == g_set.dlSort) SendDlgItemMessageW(pg, IDC_DL_SORT, CB_SETCURSEL, k, 0);
    HICON old = (HICON)SendDlgItemMessageW(pg, IDC_DL_ICON, STM_SETICON,
                                           (WPARAM)downloads_load_icon(GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(pg))), 0);
    if (old) DestroyIcon(old);
    static const int deps[] = { IDC_DL_ALLMON, IDC_DL_ICONLABEL, IDC_DL_ICON, IDC_DL_CHANGEICON, IDC_DL_DEFICON,
                                IDC_DL_FOLDERLABEL, IDC_DL_FOLDER, IDC_DL_BROWSE, IDC_DL_DEFFOLDER, IDC_DL_COUNTLABEL,
                                IDC_DL_COUNT, IDC_DL_COUNTNOTE, IDC_DL_SORT };
    for (int k = 0; k < (int)ARRAYSIZE(deps); k++) EnableWindow(GetDlgItem(pg, deps[k]), g_set.dlButton);
    EnableWindow(GetDlgItem(pg, IDC_DL_DEFICON), g_set.dlButton && g_set.dlIcon[0]);
    EnableWindow(GetDlgItem(pg, IDC_DL_DEFFOLDER), g_set.dlButton && g_set.dlFolder[0]);
    g_filling = FALSE;
}

static void dl_pick_icon(HWND pg)
{
    wchar_t path[MAX_PATH];
    int index;
    downloads_icon_location(path, MAX_PATH, &index);
    if (index < 0) index = 0;                          /* PickIconDlg counts icons; a resource ID won't do */
    if (!PickIconDlg(pg, path, MAX_PATH, &index)) return;
    wchar_t full[MAX_PATH];
    ExpandEnvironmentStringsW(path, full, MAX_PATH);
    swprintf_s(g_set.dlIcon, ARRAYSIZE(g_set.dlIcon), L"%s,%d", full, index);
    save_and_apply(APPLY_DOWNLOADS);
    dl_fill(pg);
}

static void dl_pick_folder(HWND pg)
{
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d))) return;
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(d, &opts);
    IFileOpenDialog_SetOptions(d, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(d, L"Folder for the Downloads button");
    wchar_t now[MAX_PATH];
    downloads_folder(now, MAX_PATH);
    IShellItem *start = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(now, NULL, &IID_IShellItem, (void **)&start))) {
        IFileOpenDialog_SetFolder(d, start);
        IShellItem_Release(start);
    }
    IShellItem *it = NULL;
    if (SUCCEEDED(IFileOpenDialog_Show(d, pg)) && SUCCEEDED(IFileOpenDialog_GetResult(d, &it))) {
        PWSTR p = NULL;
        if (SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            PWSTR dl = NULL;
            BOOL isDefault = SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &dl)) && !_wcsicmp(dl, p);
            CoTaskMemFree(dl);
            wcscpy_s(g_set.dlFolder, MAX_PATH, isDefault ? L"" : p);
            CoTaskMemFree(p);
            save_and_apply(APPLY_DOWNLOADS);          /* the jump list names the folder */
            dl_fill(pg);
        }
        IShellItem_Release(it);
    }
    IFileOpenDialog_Release(d);
}

static INT_PTR CALLBACK downloads_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        for (int k = 0; k < DL_SORTS; k++)
            SendDlgItemMessageW(h, IDC_DL_SORT, CB_ADDSTRING, 0, (LPARAM)DL_SORT_LABELS[SORT_ORDER[k]]);
        SendDlgItemMessageW(h, IDC_DL_COUNT, EM_LIMITTEXT, 3, 0);
        g_pages[PAGE_DOWNLOADS] = h;
        dl_fill(h);
        return TRUE;
    case WM_COMMAND:
        if (g_filling) break;
        switch (LOWORD(wp)) {
        case IDC_DL_BUTTON:
            g_set.dlButton = IsDlgButtonChecked(h, IDC_DL_BUTTON) == BST_CHECKED;
            save_and_apply(APPLY_DOWNLOADS);
            dl_fill(h);
            return TRUE;
        case IDC_DL_ALLMON:
            g_set.dlAllMonitors = IsDlgButtonChecked(h, IDC_DL_ALLMON) == BST_CHECKED;
            save_and_apply(APPLY_DOWNLOADS);
            return TRUE;
        case IDC_DL_CHANGEICON:
            dl_pick_icon(h);
            return TRUE;
        case IDC_DL_DEFICON:
            g_set.dlIcon[0] = 0;
            save_and_apply(APPLY_DOWNLOADS);
            dl_fill(h);
            return TRUE;
        case IDC_DL_BROWSE:
            dl_pick_folder(h);
            return TRUE;
        case IDC_DL_DEFFOLDER:
            g_set.dlFolder[0] = 0;
            save_and_apply(APPLY_DOWNLOADS);
            dl_fill(h);
            return TRUE;
        case IDC_DL_SORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int k = (int)SendDlgItemMessageW(h, IDC_DL_SORT, CB_GETCURSEL, 0, 0);
                if (k >= 0 && k < DL_SORTS) {
                    g_set.dlSort = SORT_ORDER[k];
                    save_and_apply(0);
                    downloads_jumplist();                  /* the tick moves */
                }
            }
            return TRUE;
        case IDC_DL_COUNT: {                               /* 1-100; anything else shows the number in use again */
            BOOL ok;
            UINT v = GetDlgItemInt(h, IDC_DL_COUNT, &ok, FALSE);
            if (HIWORD(wp) == EN_CHANGE && ok && v >= 1 && v <= 100 && (int)v != g_set.dlCount) {
                g_set.dlCount = (int)v;
                save_and_apply(0);
            } else if (HIWORD(wp) == EN_KILLFOCUS && (!ok || v < 1 || v > 100)) {
                g_filling = TRUE;
                SetDlgItemInt(h, IDC_DL_COUNT, g_set.dlCount, FALSE);
                g_filling = FALSE;
            }
            return TRUE;
        }
        }
        break;
    case WM_DESTROY: {
        HICON old = (HICON)SendDlgItemMessageW(h, IDC_DL_ICON, STM_SETICON, 0, 0);
        if (old) DestroyIcon(old);
        break;
    }
    }
    return FALSE;
}

void settings_window_refresh(void)
{
    if (g_pages[PAGE_DOWNLOADS]) dl_fill(g_pages[PAGE_DOWNLOADS]);
}

/* ---------------------------------------------------------------- the window ---------------------------------- */

static void show_page(int i)
{
    for (int k = 0; k < PAGES; k++) if (g_pages[k]) ShowWindow(g_pages[k], k == i ? SW_SHOW : SW_HIDE);
}

static void layout_pages(void)
{
    RECT r;
    GetWindowRect(g_tab, &r);
    MapWindowPoints(NULL, g_dlg, (POINT *)&r, 2);
    TabCtrl_AdjustRect(g_tab, FALSE, &r);
    for (int k = 0; k < PAGES; k++)
        if (g_pages[k]) SetWindowPos(g_pages[k], HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE);
}

static INT_PTR CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        g_dlg = h;
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                     GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        g_tab = GetDlgItem(h, IDC_TABS);
        static const wchar_t *names[PAGES] = { L"Mouse", L"Hot Corners", L"Downloads", L"Workspace" };
        static const int ids[PAGES] = { IDD_PAGE_MOUSE, IDD_PAGE_CORNERS, IDD_PAGE_DOWNLOADS, IDD_PAGE_WORKSPACE };
        static const DLGPROC procs[PAGES] = { mouse_proc, corners_proc, downloads_proc, workspace_proc };
        for (int k = 0; k < PAGES; k++) {
            TCITEMW ti = {0};
            ti.mask = TCIF_TEXT;
            ti.pszText = (LPWSTR)names[k];
            TabCtrl_InsertItem(g_tab, k, &ti);
        }
        for (int k = 0; k < PAGES; k++) {
            HWND pg = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(ids[k]), h, procs[k], 0);
            g_pages[k] = pg;
            if (pg) EnableThemeDialogTexture(pg, ETDT_ENABLETAB);
        }
        layout_pages();
        show_page(0);
        return TRUE;
    }
    case WM_SIZE:
        if (g_tab) layout_pages();
        return FALSE;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == IDC_TABS && n->code == TCN_SELCHANGE) show_page(TabCtrl_GetCurSel(g_tab));
        return FALSE;
    }
    case WM_DEVICES: {
        DeviceList *l = (DeviceList *)lp;
        mouse_search_done(l);
        HeapFree(GetProcessHeap(), 0, l);
        return TRUE;
    }
    case WM_DISPLAYCHANGE:
        if (g_pages[PAGE_CORNERS]) {
            g_nmons = monitors_list(g_mons, 16);
            rule_fill_editor(g_pages[PAGE_CORNERS]);
        }
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            SetFocus(GetDlgItem(h, IDOK));                     /* let an edit box finish (EN_KILLFOCUS) first */
            DestroyWindow(h);
            return TRUE;
        }
        return FALSE;
    case WM_DESTROY: {
        HICON big = (HICON)SendMessageW(h, WM_GETICON, ICON_BIG, 0), little = (HICON)SendMessageW(h, WM_GETICON, ICON_SMALL, 0);
        if (big) DestroyIcon(big);
        if (little) DestroyIcon(little);
        g_dlg = g_tab = NULL;
        for (int k = 0; k < PAGES; k++) g_pages[k] = NULL;
        g_searched = g_searching = FALSE;
        return FALSE;
    }
    }
    return FALSE;
}

void settings_window_open(HINSTANCE inst)
{
    if (g_dlg) {
        if (IsIconic(g_dlg)) ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    g_inst = inst;
    HWND h = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, main_proc, 0);
    if (!h) { trace("settings window: could not open (%lu)", GetLastError()); return; }
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

void settings_window_open_page(HINSTANCE inst, int page)
{
    settings_window_open(inst);
    if (!g_tab || page < 0 || page >= PAGES) return;
    TabCtrl_SetCurSel(g_tab, page);
    show_page(page);
}

/* --snap-settings <folder>: open the window off-screen, wait for the mouse search, and save each tab as a PNG. */
static void pump(DWORD ms)
{
    ULONGLONG until = GetTickCount64() + ms;
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (!g_dlg || !IsDialogMessageW(g_dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
        }
        Sleep(15);
    }
}

int settings_window_snapshots(HINSTANCE inst, const wchar_t *dir)
{
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    g_inst = inst;
    HWND h = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), NULL, main_proc, 0);
    if (!h) return 1;
    RECT vs = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    SetWindowPos(h, NULL, vs.left - 3000, vs.top - 3000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    for (int i = 0; i < 300 && g_searching; i++) pump(100);   /* up to 30 s for the mouse search */
    pump(300);
    static const wchar_t *names[PAGES] = { L"settings-mouse", L"settings-hot-corners", L"settings-downloads",
                                           L"settings-workspace" };
    for (int k = 0; k < PAGES; k++) {
        TabCtrl_SetCurSel(g_tab, k);
        show_page(k);
        pump(300);
        RECT r;
        GetWindowRect(h, &r);
        int w = r.right - r.left, ht = r.bottom - r.top;
        HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, ht);
        HGDIOBJ old = SelectObject(mem, bmp);
        PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT */);
        SelectObject(mem, old);
        GpBitmap *gb = NULL;
        GdipCreateBitmapFromHBITMAP(bmp, NULL, &gb);
        wchar_t path[MAX_PATH];
        swprintf_s(path, MAX_PATH, L"%s\\%s.png", dir, names[k]);
        if (gb) { GdipSaveImageToFile(gb, path, &png, NULL); GdipDisposeImage(gb); }
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
    DestroyWindow(h);
    pump(100);
    return 0;
}
