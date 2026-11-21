/* Jig Rack -- settings: %APPDATA%\Jig Rack\settings.ini, written by the Settings window.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * The file is UTF-16 (so monitor and device names keep any character) and read and written with the
 * Get/WritePrivateProfile functions. With no file, the defaults apply: no mouse chosen (the tray shows the Jig Rack
 * icon) and one hot corner rule, the top-right of the workspace locks the PC after 5 s with a countdown.
 */
#include "jigrack.h"
#include <shlobj.h>
#include <stdlib.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

Settings g_set;
static wchar_t g_file[MAX_PATH];

static const wchar_t *CORNER_KEYS[CORNERS] = { L"top-left", L"top-right", L"bottom-left", L"bottom-right" };
static const wchar_t *CORNER_NAMES[CORNERS] = { L"Top-left", L"Top-right", L"Bottom-left", L"Bottom-right" };
static const wchar_t *WHERE_KEYS[] = { L"workspace", L"any", L"monitor" };
static const wchar_t *ACTION_KEYS[ACTIONS] = { L"lock", L"sleep", L"screens-off", L"show-desktop", L"task-view", L"run" };
static const wchar_t *ACTION_NAMES[ACTIONS] = { L"Lock the PC", L"Sleep", L"Turn off the screens", L"Show the desktop",
                                                 L"Task View", L"Run a command" };
static const wchar_t *MOUSE_KEYS[MOUSE_TYPES] = { L"none", L"logitech" };

BOOL mouse_chosen(const Settings *s)
{
    return s->mouseType == MOUSE_LOGITECH && s->productId;
}

/* Worth a warning: a mouse is chosen, it isn't charging, and its last reading (even while it sleeps) is low. */
BOOL mouse_battery_low(void)
{
    const Battery *b = &g_battery;
    return mouse_chosen(&g_set) && b->ok && !b->charging && b->percent <= g_set.lowPercent;
}

const wchar_t *corner_label(int c) { return c >= 0 && c < CORNERS ? CORNER_NAMES[c] : L"?"; }
const wchar_t *action_label(int a) { return a >= 0 && a < ACTIONS ? ACTION_NAMES[a] : L"?"; }

void settings_use_file(const wchar_t *path) { wcsncpy_s(g_file, MAX_PATH, path, _TRUNCATE); }

const wchar_t *settings_file(void)
{
    if (!g_file[0]) {
        PWSTR appdata = NULL;
        if (SHGetKnownFolderPath(&FOLDERID_RoamingAppData, 0, NULL, &appdata) == S_OK) {
            swprintf_s(g_file, MAX_PATH, L"%s\\Jig Rack\\settings.ini", appdata);
            CoTaskMemFree(appdata);
        }
    }
    return g_file;
}

void rule_defaults(Rule *r)
{
    ZeroMemory(r, sizeof *r);
    r->corner = CORNER_TR;
    r->where = WHERE_WORKSPACE;
    r->action = ACT_LOCK;
    r->delayMs = 5000;
    r->countdown = 1;
}

void settings_defaults(Settings *s)
{
    ZeroMemory(s, sizeof *s);
    s->mouseType = MOUSE_NONE;
    s->showMeter = 1;
    s->lowPercent = 20;
    s->ruleCount = 1;
    rule_defaults(&s->rules[0]);
}

static int find_key(const wchar_t *v, const wchar_t *const *keys, int n, int dflt)
{
    for (int i = 0; i < n; i++) if (_wcsicmp(v, keys[i]) == 0) return i;
    return dflt;
}

static void get_str(const wchar_t *sec, const wchar_t *key, wchar_t *out, int cch)
{
    GetPrivateProfileStringW(sec, key, L"", out, cch, g_file);
}

static int get_int(const wchar_t *sec, const wchar_t *key, int dflt)
{
    return (int)GetPrivateProfileIntW(sec, key, dflt, g_file);
}

void settings_load(Settings *s)
{
    settings_defaults(s);
    settings_file();
    if (GetFileAttributesW(g_file) == INVALID_FILE_ATTRIBUTES) {
        DWORD v = 1, n = sizeof v;                          /* 1.0 kept its on/off switch in the registry */
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\JigRack", L"HotCornerLock", RRF_RT_REG_DWORD, NULL, &v, &n) ==
                ERROR_SUCCESS && v == 0)
            s->ruleCount = 0;
        trace("settings: none yet (%ls), using the defaults", g_file);
        return;
    }
    wchar_t v[512];
    get_str(L"Mouse", L"Type", v, 64);
    s->mouseType = find_key(v, MOUSE_KEYS, MOUSE_TYPES, MOUSE_NONE);
    get_str(L"Mouse", L"Product", v, 64);
    s->productId = (WORD)wcstoul(v, NULL, 16);
    s->deviceIndex = get_int(L"Mouse", L"Index", 1);
    get_str(L"Mouse", L"Name", s->deviceName, 64);
    s->showMeter = get_int(L"Mouse", L"ShowMeter", 1) != 0;
    s->lowPercent = get_int(L"Mouse", L"Low", 20);
    if (s->lowPercent < 1) s->lowPercent = 1;
    if (s->lowPercent > 99) s->lowPercent = 99;

    int n = get_int(L"HotCorners", L"Count", 0);
    if (n < 0) n = 0;
    if (n > MAX_RULES) n = MAX_RULES;
    s->ruleCount = n;
    for (int i = 0; i < n; i++) {
        Rule *r = &s->rules[i];
        wchar_t sec[32];
        swprintf_s(sec, 32, L"HotCorner%d", i + 1);
        rule_defaults(r);
        get_str(sec, L"Corner", v, 64);  r->corner = find_key(v, CORNER_KEYS, CORNERS, CORNER_TR);
        get_str(sec, L"Where", v, 64);   r->where = find_key(v, WHERE_KEYS, 3, WHERE_WORKSPACE);
        get_str(sec, L"Action", v, 64);  r->action = find_key(v, ACTION_KEYS, ACTIONS, ACT_LOCK);
        get_str(sec, L"Delay", v, 64);
        if (v[0]) {
            double secs = wcstod(v, NULL);
            if (secs < 0.1) secs = 0.1;
            if (secs > 600) secs = 600;
            r->delayMs = (int)(secs * 1000 + 0.5);
        }
        r->countdown = get_int(sec, L"Countdown", 1) != 0;
        r->warnLow = get_int(sec, L"WarnLow", 0) != 0;
        get_str(sec, L"Monitor", r->monitor, 256);
        get_str(sec, L"MonitorName", r->monitorName, 64);
        get_str(sec, L"Command", r->command, 512);
    }
    trace("settings: loaded %ls (mouse %ls %04X/%d, %d hot corner rule(s))", g_file, MOUSE_KEYS[s->mouseType],
          s->productId, s->deviceIndex, s->ruleCount);
}

static BOOL put(const wchar_t *sec, const wchar_t *key, const wchar_t *v)
{
    return WritePrivateProfileStringW(sec, key, v, g_file);
}

static BOOL put_int(const wchar_t *sec, const wchar_t *key, int v)
{
    wchar_t b[16];
    swprintf_s(b, 16, L"%d", v);
    return put(sec, key, b);
}

BOOL settings_save(const Settings *s)
{
    settings_file();
    if (GetFileAttributesW(g_file) == INVALID_FILE_ATTRIBUTES) {
        /* Create it as UTF-16 (byte-order mark first), so WritePrivateProfileString keeps writing Unicode. */
        wchar_t dir[MAX_PATH];
        wcscpy_s(dir, MAX_PATH, g_file);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) { *slash = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
        HANDLE h = CreateFileW(g_file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            static const wchar_t head[] = L"\xFEFF; Jig Rack settings. The Settings window (right-click the tray icon) "
                                          L"writes this file.\r\n";
            DWORD w;
            WriteFile(h, head, (DWORD)(wcslen(head) * sizeof(wchar_t)), &w, NULL);
            CloseHandle(h);
        }
    }
    BOOL ok = TRUE;
    wchar_t v[16];
    ok &= put(L"Mouse", L"Type", MOUSE_KEYS[s->mouseType]);
    swprintf_s(v, 16, L"%04X", s->productId);
    ok &= put(L"Mouse", L"Product", v);
    ok &= put_int(L"Mouse", L"Index", s->deviceIndex);
    ok &= put(L"Mouse", L"Name", s->deviceName);
    ok &= put_int(L"Mouse", L"ShowMeter", s->showMeter);
    ok &= put_int(L"Mouse", L"Low", s->lowPercent);
    ok &= put_int(L"HotCorners", L"Count", s->ruleCount);
    for (int i = 0; i < MAX_RULES; i++) {
        wchar_t sec[32];
        swprintf_s(sec, 32, L"HotCorner%d", i + 1);
        if (i >= s->ruleCount) { WritePrivateProfileStringW(sec, NULL, NULL, g_file); continue; }   /* drop it */
        const Rule *r = &s->rules[i];
        wchar_t d[32];
        swprintf_s(d, 32, L"%g", r->delayMs / 1000.0);
        ok &= put(sec, L"Corner", CORNER_KEYS[r->corner]);
        ok &= put(sec, L"Where", WHERE_KEYS[r->where]);
        ok &= put(sec, L"Monitor", r->monitor);
        ok &= put(sec, L"MonitorName", r->monitorName);
        ok &= put(sec, L"Action", ACTION_KEYS[r->action]);
        ok &= put(sec, L"Delay", d);
        ok &= put_int(sec, L"Countdown", r->countdown);
        ok &= put_int(sec, L"WarnLow", r->warnLow);
        ok &= put(sec, L"Command", r->command);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, g_file);   /* flush the cache */
    if (!ok) trace("settings: could not write %ls (%lu)", g_file, GetLastError());
    return ok;
}

/* "Top-right of the workspace: Lock the PC after 5 s" */
void rule_describe(const Rule *r, wchar_t *out, int cch)
{
    wchar_t where[96];
    if (r->where == WHERE_WORKSPACE) wcscpy_s(where, 96, L"the workspace");
    else if (r->where == WHERE_ANY) wcscpy_s(where, 96, L"any monitor");
    else swprintf_s(where, 96, L"%s", r->monitorName[0] ? r->monitorName : L"a monitor");
    wchar_t what[160];
    if (r->action == ACT_RUN) swprintf_s(what, 160, L"Run %s", r->command[0] ? r->command : L"(no command yet)");
    else wcscpy_s(what, 160, action_label(r->action));
    swprintf_s(out, cch, L"%s of %s: %s after %g s%s%s", corner_label(r->corner), where, what, r->delayMs / 1000.0,
               r->countdown ? L"" : L", no countdown", r->countdown && r->warnLow ? L", low-battery warning" : L"");
}
