/* Jig Rack -- the Downloads jig.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * A Downloads button on the taskbar of every monitor (or just the main one), like the Downloads stack in a Mac's
 * Dock: click it and a panel pops up above it with the newest files in the Downloads folder. Click a file to open
 * it, drag it out to copy or move it, or right-click it for "Show in folder".
 *
 * Windows puts a window's taskbar button on the taskbar of the monitor the window is on, so each monitor gets one
 * small window of its own: invisible and click-through while it's just the button, and the panel itself when the button is clicked. All of them carry
 * their own AppUserModelID, so they group as "Downloads" with its own icon, apart from Jig Rack's other windows. */
#define COBJMACROS
#include "jigrack.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <propsys.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <shellscalingapi.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")

#define DL_APPID L"JonRaiford.JigRack.Downloads"
#define DL_DEFAULT_ICON L"%SystemRoot%\\system32\\imageres.dll,-184"     /* Windows' own Downloads folder icon */

/* The taskbar's AppUserModel properties (propkey.h), here so no GUIDs need defining. */
static const PROPERTYKEY PK_APPID = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 5 };
static const PROPERTYKEY PK_PREVENT_PINNING = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 9 };

/* ---------------------------------------------------------------- the folder and its newest items -------------- */

void downloads_folder(wchar_t *out, int cch)
{
    if (g_set.dlFolder[0]) { wcscpy_s(out, cch, g_set.dlFolder); return; }
    PWSTR p = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &p))) wcscpy_s(out, cch, p);
    CoTaskMemFree(p);
}

static BOOL skip_name(const WIN32_FIND_DATAW *fd)
{
    static const wchar_t *partial[] = { L".crdownload", L".part", L".partial", L".download", L".tmp", L".opdownload" };
    if (fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return TRUE;
    if (!wcscmp(fd->cFileName, L".") || !wcscmp(fd->cFileName, L"..")) return TRUE;
    if (!_wcsicmp(fd->cFileName, L"desktop.ini") || !wcsncmp(fd->cFileName, L"~$", 2)) return TRUE;
    const wchar_t *ext = PathFindExtensionW(fd->cFileName);
    for (int i = 0; i < (int)ARRAYSIZE(partial); i++) if (!_wcsicmp(ext, partial[i])) return TRUE;  /* still downloading */
    return FALSE;
}

/* a comes before b in this sort? */
static BOOL before(const DlItem *a, const DlItem *b, int sort)
{
    if (sort == DL_SORT_NAME) return StrCmpLogicalW(a->name, b->name) < 0;
    if (sort == DL_SORT_KIND) {                                           /* folders, then by type, then by name */
        if (a->dir != b->dir) return a->dir;
        int c = _wcsicmp(PathFindExtensionW(a->name), PathFindExtensionW(b->name));
        return c ? c < 0 : StrCmpLogicalW(a->name, b->name) < 0;
    }
    const FILETIME *x = sort == DL_SORT_MODIFIED ? &a->modified : &a->added, *y = sort == DL_SORT_MODIFIED ? &b->modified : &b->added;
    return CompareFileTime(x, y) > 0;                                     /* newest first */
}

/* The first max items of the folder in this sort order (files and folders; never hidden or partial downloads). */
int downloads_collect(const wchar_t *folder, int sort, DlItem *out, int max)
{
    wchar_t pattern[MAX_PATH + 4];
    swprintf_s(pattern, ARRAYSIZE(pattern), L"%s\\*", folder);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (skip_name(&fd)) continue;
        DlItem it;
        wcscpy_s(it.name, MAX_PATH, fd.cFileName);
        it.added = fd.ftCreationTime;
        it.modified = fd.ftLastWriteTime;
        it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        int at = n;                                                       /* insertion into the top max */
        while (at > 0 && before(&it, &out[at - 1], sort)) at--;
        if (at >= max) continue;
        int last = n < max ? n : max - 1;
        MoveMemory(&out[at + 1], &out[at], (last - at) * sizeof *out);
        out[at] = it;
        if (n < max) n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return n;
}

/* "Just now", "5 min ago", "3 hr ago", "Yesterday", "4 days ago", "Nov 3", "Nov 3, 2025". */
void downloads_when(FILETIME ft, FILETIME now, wchar_t *out, int cch)
{
    ULARGE_INTEGER a = { ft.dwLowDateTime, ft.dwHighDateTime }, b = { now.dwLowDateTime, now.dwHighDateTime };
    LONGLONG secs = b.QuadPart > a.QuadPart ? (LONGLONG)((b.QuadPart - a.QuadPart) / 10000000ULL) : 0;
    if (secs < 60) { wcscpy_s(out, cch, L"Just now"); return; }
    if (secs < 3600) { swprintf_s(out, cch, L"%d min ago", (int)(secs / 60)); return; }
    FILETIME lf, ln;
    SYSTEMTIME t, n;
    FileTimeToLocalFileTime(&ft, &lf);
    FileTimeToLocalFileTime(&now, &ln);
    FileTimeToSystemTime(&lf, &t);
    FileTimeToSystemTime(&ln, &n);
    /* whole calendar days between the two local dates */
    SYSTEMTIME d0 = { t.wYear, t.wMonth, 0, t.wDay }, d1 = { n.wYear, n.wMonth, 0, n.wDay };
    FILETIME f0, f1;
    SystemTimeToFileTime(&d0, &f0);
    SystemTimeToFileTime(&d1, &f1);
    ULARGE_INTEGER u0 = { f0.dwLowDateTime, f0.dwHighDateTime }, u1 = { f1.dwLowDateTime, f1.dwHighDateTime };
    int days = u1.QuadPart > u0.QuadPart ? (int)((u1.QuadPart - u0.QuadPart) / (86400ULL * 10000000ULL)) : 0;
    static const wchar_t *mon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    if (days == 0) swprintf_s(out, cch, L"%d hr ago", (int)(secs / 3600));
    else if (days == 1) wcscpy_s(out, cch, L"Yesterday");
    else if (days < 7) swprintf_s(out, cch, L"%d days ago", days);
    else if (t.wYear == n.wYear) swprintf_s(out, cch, L"%s %d", mon[t.wMonth - 1], t.wDay);
    else swprintf_s(out, cch, L"%s %d, %d", mon[t.wMonth - 1], t.wDay, t.wYear);
}

static void size_text(ULONGLONG b, wchar_t *out, int cch)
{
    if (b < 1000) swprintf_s(out, cch, L"%llu bytes", b);
    else if (b < 1000000) swprintf_s(out, cch, L"%.0f KB", b / 1e3);
    else if (b < 1000000000) swprintf_s(out, cch, L"%.1f MB", b / 1e6);
    else swprintf_s(out, cch, L"%.1f GB", b / 1e9);
}

/* ---------------------------------------------------------------- the icon ------------------------------------ */

/* The icon in use: the one chosen on the Downloads tab, or Windows' Downloads icon. path is expanded. */
void downloads_icon_location(wchar_t *path, int cch, int *index)
{
    wchar_t raw[MAX_PATH + 16];
    wcscpy_s(raw, ARRAYSIZE(raw), g_set.dlIcon[0] ? g_set.dlIcon : DL_DEFAULT_ICON);
    *index = 0;
    wchar_t *comma = wcsrchr(raw, L',');
    if (comma) { *index = _wtoi(comma + 1); *comma = 0; }
    ExpandEnvironmentStringsW(raw, path, cch);
}

HICON downloads_load_icon(int size)
{
    wchar_t path[MAX_PATH];
    int index;
    downloads_icon_location(path, MAX_PATH, &index);
    HICON big = NULL;
    if (SHDefExtractIconW(path, index, 0, &big, NULL, MAKELONG(size, size)) != S_OK || !big) {
        ExpandEnvironmentStringsW(L"%SystemRoot%\\system32\\imageres.dll", path, MAX_PATH);   /* the chosen file is gone */
        SHDefExtractIconW(path, -184, 0, &big, NULL, MAKELONG(size, size));
    }
    return big;
}

/* ---------------------------------------------------------------- the buttons and the panel -------------------- */

enum { ID_LIST = 10 };
#define DL_SHOWN_MAX 100

typedef struct {
    HWND hwnd;
    HMONITOR mon;
    HICON big, little;
} Button;

static HINSTANCE g_inst;
static Button g_buttons[16];
static int g_nbuttons;
static HWND g_open;                     /* the button whose panel is showing; NULL = none */
static HWND g_list, g_title, g_empty;
static HFONT g_font, g_titleFont;
static DlItem g_items[DL_SHOWN_MAX];
static int g_nitems;
static wchar_t g_folder[MAX_PATH];
static BOOL g_dragging;
static HWND g_closedBy;                 /* the button whose panel just closed by losing the focus, and when */
static ULONGLONG g_closedAt;
static HWND g_previous;                 /* the window that had the focus before the panel opened */

static void set_window_props(HWND w, BOOL set)
{
    IPropertyStore *ps = NULL;
    if (FAILED(SHGetPropertyStoreForWindow(w, &IID_IPropertyStore, (void **)&ps))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (set) { v.vt = VT_BOOL; v.boolVal = VARIANT_TRUE; }
    IPropertyStore_SetValue(ps, &PK_PREVENT_PINNING, &v);
    if (set) { v.vt = VT_LPWSTR; v.pwszVal = (LPWSTR)DL_APPID; }
    IPropertyStore_SetValue(ps, &PK_APPID, &v);
    IPropertyStore_Release(ps);
}

/* The 1-pixel, invisible, click-through button window, in the bottom-right corner of its monitor's work area. */
static void collapse_place(HWND w, HMONITOR mon)
{
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    SetWindowPos(w, NULL, mi.rcWork.right - 1, mi.rcWork.bottom - 1, 1, 1,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static Button *button_of(HWND w)
{
    for (int i = 0; i < g_nbuttons; i++) if (g_buttons[i].hwnd == w) return &g_buttons[i];
    return NULL;
}

static void panel_close(void)
{
    HWND w = g_open;
    if (!w) return;
    g_open = NULL;
    if (g_list) DestroyWindow(g_list);
    if (g_title) DestroyWindow(g_title);
    if (g_empty) DestroyWindow(g_empty);
    g_list = g_title = g_empty = NULL;
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    g_font = g_titleFont = NULL;
    Button *b = button_of(w);
    if (b) collapse_place(w, b->mon);
}

static void item_path(int i, wchar_t *out, int cch)
{
    swprintf_s(out, cch, L"%s\\%s", g_folder, g_items[i].name);
}

static void fill_list(UINT dpi, int width)
{
    SHFILEINFOW sfi;
    HIMAGELIST sys = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    ListView_SetImageList(g_list, sys, LVSIL_NORMAL);
    ListView_SetView(g_list, LV_VIEW_TILE);
    LVTILEVIEWINFO tvi = { sizeof tvi };
    tvi.dwMask = LVTVIM_TILESIZE | LVTVIM_COLUMNS;
    tvi.dwFlags = LVTVIF_FIXEDWIDTH;
    tvi.sizeTile.cx = width - MulDiv(12, dpi, 96) - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - MulDiv(4, dpi, 96);
    tvi.cLines = 1;
    ListView_SetTileViewInfo(g_list, &tvi);
    LVCOLUMNW col = { LVCF_WIDTH, 0, 200 };
    ListView_InsertColumn(g_list, 0, &col);
    ListView_InsertColumn(g_list, 1, &col);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    for (int i = 0; i < g_nitems; i++) {
        wchar_t path[MAX_PATH * 2], when[48], size[32], detail[96];
        item_path(i, path, ARRAYSIZE(path));
        SHGetFileInfoW(path, 0, &sfi, sizeof sfi, SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
        LVITEMW it = { LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM };
        it.iItem = i;
        it.pszText = g_items[i].name;
        it.iImage = sfi.iIcon;
        it.lParam = i;
        ListView_InsertItem(g_list, &it);
        downloads_when(g_set.dlSort == DL_SORT_MODIFIED ? g_items[i].modified : g_items[i].added, now, when, 48);
        if (g_items[i].dir) wcscpy_s(size, 32, L"Folder");
        else size_text(g_items[i].size, size, 32);
        swprintf_s(detail, 96, L"%s \x00b7 %s", size, when);
        ListView_SetItemText(g_list, i, 1, detail);
        UINT cols[1] = { 1 };
        int fmts[1] = { LVCFMT_LEFT };
        LVTILEINFO ti = { sizeof ti, i, 1, cols, fmts };
        ListView_SetTileInfo(g_list, &ti);
    }
}

static void panel_open(HWND w)
{
    Button *b = button_of(w);
    if (!b) return;
    if (g_open && g_open != w) panel_close();
    g_open = w;
    downloads_folder(g_folder, MAX_PATH);
    int want = g_set.dlCount < 1 ? 1 : g_set.dlCount > DL_SHOWN_MAX ? DL_SHOWN_MAX : g_set.dlCount;
    g_nitems = downloads_collect(g_folder, g_set.dlSort, g_items, want);

    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(b->mon, &mi);
    UINT dpi = 96, dx, dy;
    if (GetDpiForMonitor(b->mon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK) dpi = dx;
    int margin = MulDiv(12, dpi, 96), pad = MulDiv(12, dpi, 96);
    int width = MulDiv(380, dpi, 96), titleH = MulDiv(40, dpi, 96), footH = MulDiv(10, dpi, 96);   /* just a margin: the folder is in the right-click menu */
    int maxH = (mi.rcWork.bottom - mi.rcWork.top) * 3 / 4;
    int listH = MulDiv(96, dpi, 96);                                  /* for now; fitted to the rows below */

    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -MulDiv(15, dpi, 72);
    lf.lfWeight = FW_SEMIBOLD;
    g_titleFont = CreateFontIndirectW(&lf);


    const wchar_t *folderName = PathFindFileNameW(g_folder);
    g_title = CreateWindowExW(0, L"STATIC", folderName && *folderName ? folderName : L"Downloads",
                              WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                              pad, 0, width - 2 * pad, titleH, w, NULL, g_inst, NULL);
    SendMessageW(g_title, WM_SETFONT, (WPARAM)g_titleFont, FALSE);
    if (g_nitems) {
        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_SINGLESEL |
                                 LVS_SHAREIMAGELISTS | LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS,
                                 0, titleH, width, listH, w, (HMENU)(INT_PTR)ID_LIST, g_inst, NULL);
        SetWindowTheme(g_list, L"Explorer", NULL);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_DOUBLEBUFFER | LVS_EX_ONECLICKACTIVATE | LVS_EX_TRACKSELECT);
        ListView_SetHoverTime(g_list, 1);                             /* select as the mouse moves, not after 400 ms */
        SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, FALSE);
        fill_list(dpi, width);
        /* Fit the list to its rows (the tile height depends on the fonts and icons), up to 3/4 of the screen. */
        RECT ir;
        POINT p0, p1;
        int pitch = MulDiv(48, dpi, 96);
        if (g_nitems > 1 && ListView_GetItemPosition(g_list, 0, &p0) && ListView_GetItemPosition(g_list, 1, &p1) &&
            p1.y > p0.y) pitch = p1.y - p0.y;
        else if (ListView_GetItemRect(g_list, 0, &ir, LVIR_BOUNDS)) pitch = ir.bottom - ir.top;
        listH = g_nitems * pitch + MulDiv(4, dpi, 96);
        if (titleH + listH + footH > maxH) listH = maxH - titleH - footH;
        MoveWindow(g_list, pad / 2, titleH, width - pad, listH, FALSE);
    } else {
        g_empty = CreateWindowExW(0, L"STATIC", L"Nothing here yet.", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                  0, titleH, width, listH, w, NULL, g_inst, NULL);
        SendMessageW(g_empty, WM_SETFONT, (WPARAM)g_font, FALSE);
    }

    /* Over the button that was clicked: centred on the cursor when it's on this monitor's taskbar. */
    int height = titleH + listH + footH;
    POINT pt;
    GetCursorPos(&pt);
    BOOL onMon = PtInRect(&mi.rcMonitor, pt);
    int x = onMon ? pt.x - width / 2 : mi.rcWork.left + margin;
    if (x < mi.rcWork.left + margin) x = mi.rcWork.left + margin;
    if (x + width > mi.rcWork.right - margin) x = mi.rcWork.right - margin - width;
    int y = mi.rcWork.top > mi.rcMonitor.top && mi.rcWork.bottom == mi.rcMonitor.bottom
            ? mi.rcWork.top + margin                                  /* taskbar at the top */
            : mi.rcWork.bottom - margin - height;
    SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) & ~(WS_EX_LAYERED | WS_EX_TRANSPARENT));
    SetWindowPos(w, HWND_TOP, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    DWORD round = 2;                                                  /* DWMWCP_ROUND */
    DwmSetWindowAttribute(w, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    SetForegroundWindow(w);
    SetFocus(g_list ? g_list : w);
    trace("downloads: panel on monitor %p, %d item(s) from %ls", (void *)b->mon, g_nitems, g_folder);
}

static void open_item(int i, BOOL showInFolder)
{
    if (i < 0 || i >= g_nitems) return;
    wchar_t path[MAX_PATH * 2];
    item_path(i, path, ARRAYSIZE(path));
    panel_close();
    if (showInFolder) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
        if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, NULL, 0); ILFree(pidl); }
    } else {
        ShellExecuteW(NULL, NULL, path, NULL, g_folder, SW_SHOWNORMAL);
    }
}

static void drag_item(HWND w, int i)
{
    wchar_t path[MAX_PATH * 2];
    item_path(i, path, ARRAYSIZE(path));
    IShellItem *si = NULL;
    if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&si))) return;
    IDataObject *obj = NULL;
    if (SUCCEEDED(IShellItem_BindToHandler(si, NULL, &BHID_DataObject, &IID_IDataObject, (void **)&obj))) {
        DWORD effect = 0;
        g_dragging = TRUE;
        SHDoDragDrop(w, obj, NULL, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
        g_dragging = FALSE;
        IDataObject_Release(obj);
        trace("downloads: dragged %ls (effect %lu)", g_items[i].name, effect);
    }
    IShellItem_Release(si);
    if (GetForegroundWindow() != w) panel_close();
}

/* Is the cursor on the taskbar of this button's monitor (in the monitor, outside its work area)? */
static BOOL from_taskbar(HWND w)
{
    Button *b = button_of(w);
    MONITORINFO mi = { sizeof mi };
    POINT pt;
    if (!b || !GetMonitorInfoW(b->mon, &mi) || !GetCursorPos(&pt)) return FALSE;
    return PtInRect(&mi.rcMonitor, pt) && !PtInRect(&mi.rcWork, pt);
}

/* Closed from the keyboard (Esc): the focus goes back to the window that had it, not to an invisible button. */
static void panel_dismiss(void)
{
    HWND w = g_open;
    panel_close();
    HWND to = g_previous && IsWindow(g_previous) && IsWindowVisible(g_previous) ? g_previous : NULL;
    /* Usually Windows doesn't say (the taskbar had the focus): the next real window down, as Windows would pick. */
    for (HWND n = w ? GetWindow(w, GW_HWNDNEXT) : NULL; !to && n; n = GetWindow(n, GW_HWNDNEXT)) {
        wchar_t cls[32];
        DWORD cloaked = 0;
        LONG_PTR ex = GetWindowLongPtrW(n, GWL_EXSTYLE);
        GetClassNameW(n, cls, 32);
        DwmGetWindowAttribute(n, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
        if (IsWindowVisible(n) && !IsIconic(n) && !cloaked && !(ex & WS_EX_TOOLWINDOW) &&
            (!GetWindow(n, GW_OWNER) || (ex & WS_EX_APPWINDOW)) && wcscmp(cls, L"JigRackDownloads") &&
            wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"Shell_SecondaryTrayWnd") && GetWindowTextLengthW(n) > 0)
            to = n;
    }
    if (to) SetForegroundWindow(to);
}

static LRESULT CALLBACK button_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            /* Only a click on this monitor's taskbar opens it: Windows also hands the focus to these windows when
               another one closes. And clicking the button while its panel is open first takes the focus away
               (closing it), then activates the button again: that's a "close", not a reopen. */
            if (!g_open && from_taskbar(w) && !(g_closedBy == w && GetTickCount64() - g_closedAt < 500)) {
                g_previous = (HWND)lp;
                panel_open(w);
            }
        } else if (g_open == w && !g_dragging) {
            panel_close();
            g_closedBy = w;
            g_closedAt = GetTickCount64();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { panel_close(); return 0; }   /* the taskbar's "click again to hide" */
        if ((wp & 0xFFF0) == SC_RESTORE) { if (!g_open) panel_open(w); return 0; }
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) { panel_dismiss(); return 0; }
        if (LOWORD(wp) == IDOK && g_list && GetFocus() == g_list) {      /* Enter on a file */
            open_item(ListView_GetNextItem(g_list, -1, LVNI_SELECTED), FALSE);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom != ID_LIST) break;
        if (n->code == LVN_ITEMACTIVATE) open_item(((NMITEMACTIVATE *)lp)->iItem, FALSE);
        else if (n->code == LVN_BEGINDRAG) drag_item(w, ((NMLISTVIEW *)lp)->iItem);
        else if (n->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_ESCAPE) panel_dismiss();
        else if (n->code == NM_RCLICK) {
            int i = ((NMITEMACTIVATE *)lp)->iItem;
            if (i < 0) break;
            POINT pt;
            GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"&Open");
            AppendMenuW(m, MF_STRING, 2, L"Show in &folder");
            SetMenuDefaultItem(m, 1, FALSE);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, w, NULL);
            DestroyMenu(m);
            if (cmd) open_item(i, cmd == 2);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { panel_dismiss(); return 0; }
        break;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        SetTextColor((HDC)wp, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        panel_close();
        return 0;                                                     /* the button stays */
    case WM_DESTROY:
        set_window_props(w, FALSE);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

static BOOL CALLBACK add_monitor(HMONITOR mon, HDC dc, LPRECT r, LPARAM lp)
{
    (void)dc; (void)r; (void)lp;
    if (g_nbuttons >= (int)ARRAYSIZE(g_buttons)) return FALSE;
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(mon, &mi);
    if (!g_set.dlAllMonitors && !(mi.dwFlags & MONITORINFOF_PRIMARY)) return TRUE;
    Button *b = &g_buttons[g_nbuttons];
    ZeroMemory(b, sizeof *b);
    b->mon = mon;
    /* An app window, so it gets a taskbar button (Windows 11 ignores ITaskbarList tabs for tool windows).
       WS_MINIMIZEBOX lets the taskbar "minimize" it when the button is clicked again, which closes the panel. */
    b->hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT, L"JigRackDownloads", L"\x200B", /* an invisible title: the taskbar shows just the icon (it skips untitled windows) */
                              WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, mi.rcWork.right - 1, mi.rcWork.bottom - 1, 1, 1,
                              NULL, NULL, g_inst, NULL);
    if (!b->hwnd) return TRUE;
    set_window_props(b->hwnd, TRUE);
    BOOL on = TRUE;
    DwmSetWindowAttribute(b->hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof on);   /* hover shows the icon */
    DwmSetWindowAttribute(b->hwnd, DWMWA_DISALLOW_PEEK, &on, sizeof on);
    UINT dx = 96, dy;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    b->big = downloads_load_icon(GetSystemMetricsForDpi(SM_CXICON, dx));
    b->little = downloads_load_icon(GetSystemMetricsForDpi(SM_CXSMICON, dx));
    SendMessageW(b->hwnd, WM_SETICON, ICON_BIG, (LPARAM)b->big);
    SendMessageW(b->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)b->little);
    collapse_place(b->hwnd, mon);
    g_nbuttons++;
    return TRUE;
}

static void buttons_destroy(void)
{
    panel_close();
    for (int i = 0; i < g_nbuttons; i++) {
        DestroyWindow(g_buttons[i].hwnd);
        if (g_buttons[i].big) DestroyIcon(g_buttons[i].big);
        if (g_buttons[i].little) DestroyIcon(g_buttons[i].little);
    }
    g_nbuttons = 0;
}

void downloads_init(HINSTANCE inst)
{
    g_inst = inst;
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = button_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"JigRackDownloads";
    RegisterClassW(&wc);
}

/* (Re)make the buttons for the current settings and monitors: at start, after a settings change, when the
   monitors change and when Explorer restarts. */
void downloads_apply(void)
{
    buttons_destroy();
    if (!g_set.dlButton) { downloads_jumplist(); return; }
    EnumDisplayMonitors(NULL, NULL, add_monitor, 0);
    trace("downloads: %d taskbar button(s)", g_nbuttons);
    downloads_jumplist();
}

BOOL downloads_is_message(MSG *m)
{
    return g_open && IsDialogMessageW(g_open, m);
}

/* ---------------------------------------------------------------- the right-click menu (jump list) ------------ */
/* Right-clicking a taskbar button shows its jump list, so that's where the Mac stack's menu goes: Sort by (the
   current one ticked), Open Downloads and Settings. Each entry runs "JigRack.exe --downloads <command>", which
   hands the command to the running copy (downloads_command) and exits. */

const wchar_t *DL_SORT_LABELS[DL_SORTS] = { L"Date Added", L"Date Modified", L"Name", L"Kind" };
static const wchar_t *COMMANDS[DL_CMDS] = { L"sort-added", L"sort-modified", L"sort-name", L"sort-kind", L"open", L"settings" };

int downloads_command_id(const wchar_t *name)
{
    for (int i = 0; i < DL_CMDS; i++) if (!wcscmp(name, COMMANDS[i])) return i;
    return -1;
}
static const PROPERTYKEY PK_TITLE = { { 0xF29F85E0, 0x4FF9, 0x1068, { 0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9 } }, 2 };

static IShellLinkW *jump_link(const wchar_t *title, const wchar_t *command, const wchar_t *iconPath, int iconIndex)
{
    IShellLinkW *l = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&l))) return NULL;
    wchar_t exe[MAX_PATH], args[64];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    swprintf_s(args, 64, L"--downloads %s", command);
    IShellLinkW_SetPath(l, exe);
    IShellLinkW_SetArguments(l, args);
    IShellLinkW_SetDescription(l, title);
    if (iconPath) IShellLinkW_SetIconLocation(l, iconPath, iconIndex);
    IPropertyStore *ps = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(l, &IID_IPropertyStore, (void **)&ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_LPWSTR;
        v.pwszVal = (LPWSTR)title;
        IPropertyStore_SetValue(ps, &PK_TITLE, &v);
        IPropertyStore_Commit(ps);
        IPropertyStore_Release(ps);
    }
    return l;
}

/* A separator line in the jump list's tasks. */
static IShellLinkW *jump_separator(void)
{
    static const PROPERTYKEY PK_SEPARATOR = { { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 6 };
    IShellLinkW *l = NULL;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&l))) return NULL;
    IPropertyStore *ps = NULL;
    if (SUCCEEDED(IShellLinkW_QueryInterface(l, &IID_IPropertyStore, (void **)&ps))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        IPropertyStore_SetValue(ps, &PK_SEPARATOR, &v);
        IPropertyStore_Commit(ps);
        IPropertyStore_Release(ps);
    }
    return l;
}

static void collect(IObjectCollection *c, IShellLinkW *l)
{
    if (!l) return;
    IObjectCollection_AddObject(c, (IUnknown *)l);
    IShellLinkW_Release(l);
}

void downloads_jumplist(void)
{
    ICustomDestinationList *dl = NULL;
    if (FAILED(CoCreateInstance(&CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER, &IID_ICustomDestinationList, (void **)&dl)))
        return;
    ICustomDestinationList_SetAppID(dl, DL_APPID);
    if (!g_set.dlButton) {
        ICustomDestinationList_DeleteList(dl, DL_APPID);
        ICustomDestinationList_Release(dl);
        return;
    }
    UINT slots = 0;
    IObjectArray *removed = NULL;
    if (SUCCEEDED(ICustomDestinationList_BeginList(dl, &slots, &IID_IObjectArray, (void **)&removed))) {
        wchar_t iconPath[MAX_PATH], exe[MAX_PATH];
        int iconIndex;
        downloads_icon_location(iconPath, MAX_PATH, &iconIndex);
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        /* All in the Tasks section: a custom category ("Sort by") is refused (E_ACCESSDENIED) when Windows' "show
           recently opened items" is off, and tasks always show. Sort first, a separator, then the folder. */
        IObjectCollection *tasks = NULL;
        CoCreateInstance(&CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC_SERVER, &IID_IObjectCollection, (void **)&tasks);
        HRESULT hr = E_FAIL;
        if (tasks) {
            static const int order[DL_SORTS] = { DL_SORT_NAME, DL_SORT_KIND, DL_SORT_ADDED, DL_SORT_MODIFIED };  /* as on a Mac */
            for (int k = 0; k < DL_SORTS; k++) {
                int i = order[k];
                wchar_t title[64];
                swprintf_s(title, 64, L"%sSort by %s", i == g_set.dlSort ? L"\x2713 " : L"\x2003 ", DL_SORT_LABELS[i]);
                collect(tasks, jump_link(title, COMMANDS[DL_CMD_SORT_ADDED + i], NULL, 0));
            }
            collect(tasks, jump_separator());
            wchar_t open[MAX_PATH + 8], folder[MAX_PATH];
            downloads_folder(folder, MAX_PATH);
            const wchar_t *name = PathFindFileNameW(folder);
            swprintf_s(open, ARRAYSIZE(open), L"Open %s", name && *name ? name : L"Downloads");
            collect(tasks, jump_link(open, COMMANDS[DL_CMD_OPEN], iconPath, iconIndex));
            collect(tasks, jump_link(L"Downloads settings\x2026", COMMANDS[DL_CMD_SETTINGS], exe, 0));
            hr = ICustomDestinationList_AddUserTasks(dl, (IObjectArray *)tasks);
            IObjectCollection_Release(tasks);
        }
        HRESULT commit = ICustomDestinationList_CommitList(dl);
        trace("downloads: jump list (sort %ls): tasks 0x%08lx, commit 0x%08lx", DL_SORT_LABELS[g_set.dlSort], hr, commit);
        if (removed) IObjectArray_Release(removed);
    }
    ICustomDestinationList_Release(dl);
}

void downloads_command(int cmd)
{
    if (cmd < 0 || cmd >= DL_CMDS) return;
    trace("downloads: command %ls", COMMANDS[cmd]);
    if (cmd <= DL_CMD_SORT_KIND) {
        g_set.dlSort = cmd - DL_CMD_SORT_ADDED;
        settings_save(&g_set);
        downloads_jumplist();
        settings_window_refresh();
    } else if (cmd == DL_CMD_OPEN) {
        wchar_t folder[MAX_PATH];
        downloads_folder(folder, MAX_PATH);
        ShellExecuteW(NULL, NULL, folder, NULL, NULL, SW_SHOWNORMAL);
    } else if (cmd == DL_CMD_SETTINGS) {
        settings_window_open_page(g_inst, 2);
    }
}
