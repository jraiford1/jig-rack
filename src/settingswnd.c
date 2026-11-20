/* Jig Rack -- the Settings window: one tab per jig. Every change is saved and takes effect at once.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE. */
#include "jigrack.h"
#include "resource.h"
#include "gdip.h"
#include <uxtheme.h>
#include <stdlib.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

enum { PAGE_MOUSE, PAGE_CORNERS, PAGES };

static HINSTANCE g_inst;
static HWND g_dlg, g_tab, g_pages[PAGES];
static DeviceList g_devs;
static BOOL g_searching, g_searched;
static MonitorDesc g_mons[16];
static int g_nmons;
static int g_rule = -1;                     /* selected hot corner rule */
static BOOL g_filling;                      /* filling controls: ignore their change notifications */

HWND settings_window(void) { return g_dlg; }

enum { APPLY_MOUSE = 1, APPLY_CORNERS = 2 };
static void save_and_apply(int what)
{
    settings_save(&g_set);
    if (what & APPLY_MOUSE) mouse_apply();
    if (what & APPLY_CORNERS) hotcorner_apply();
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

static void rules_fill(HWND pg)
{
    HWND lb = GetDlgItem(pg, IDC_RULES);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    HDC dc = GetDC(lb);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(lb, WM_GETFONT, 0, 0));
    int widest = 0;
    for (int i = 0; i < g_set.ruleCount; i++) {
        wchar_t t[700];
        rule_describe(&g_set.rules[i], t, 700);
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
        SIZE sz;
        if (GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz) && sz.cx > widest) widest = sz.cx;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(lb, dc);
    SendMessageW(lb, LB_SETHORIZONTALEXTENT, widest + 8, 0);   /* scroll sideways to read a long rule */
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
    HDC dc = GetDC(lb);
    HGDIOBJ oldFont = SelectObject(dc, (HFONT)SendMessageW(lb, WM_GETFONT, 0, 0));
    SIZE sz;
    if (GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz) && sz.cx + 8 > (int)SendMessageW(lb, LB_GETHORIZONTALEXTENT, 0, 0))
        SendMessageW(lb, LB_SETHORIZONTALEXTENT, sz.cx + 8, 0);
    SelectObject(dc, oldFont);
    ReleaseDC(lb, dc);
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
        static const wchar_t *names[PAGES] = { L"Mouse", L"Hot Corners" };
        static const int ids[PAGES] = { IDD_PAGE_MOUSE, IDD_PAGE_CORNERS };
        static const DLGPROC procs[PAGES] = { mouse_proc, corners_proc };
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
    static const wchar_t *names[PAGES] = { L"settings-mouse", L"settings-hot-corners" };
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
