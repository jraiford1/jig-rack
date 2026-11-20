/* Jig Rack -- shared declarations.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE. */
#pragma once
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>

#define APP_NAME L"Jig Rack"

/* Messages */
#define WM_TRAY     (WM_APP + 1)     /* tray icon callback */
#define WM_RESULT   (WM_APP + 2)     /* monitor thread -> tray window: lp = Battery* (HeapAlloc'd) */
#define WM_DEVICES  (WM_APP + 3)     /* device search -> settings window: lp = DeviceList* (HeapAlloc'd) */

/* ---------------------------------------------------------------- log (main.c) -------------------------------- */
void trace(const char *fmt, ...);
extern HANDLE g_log;

/* ---------------------------------------------------------------- settings (settings.c) ----------------------- */
enum { MOUSE_NONE, MOUSE_LOGITECH, MOUSE_TYPES };
enum { CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR, CORNERS };
enum { WHERE_WORKSPACE, WHERE_ANY, WHERE_MONITOR };
enum { ACT_LOCK, ACT_SLEEP, ACT_SCREENS_OFF, ACT_SHOW_DESKTOP, ACT_TASK_VIEW, ACT_RUN, ACTIONS };
#define MAX_RULES 16

typedef struct {
    int corner;                 /* CORNER_* */
    int where;                  /* WHERE_* */
    int action;                 /* ACT_* */
    int delayMs;                /* how long the cursor rests in the corner before the action */
    int countdown;              /* show the on-screen countdown */
    wchar_t monitor[256];       /* WHERE_MONITOR: the monitor's device path (stable across docking) */
    wchar_t monitorName[64];    /* its friendly name, for showing it while it isn't connected */
    wchar_t command[512];       /* ACT_RUN */
} Rule;

typedef struct {
    int mouseType;              /* MOUSE_* */
    WORD productId;             /* Logitech: USB product ID of the receiver (or wired mouse) the mouse is on; 0 = none */
    int deviceIndex;            /* its HID++ device index there: 1..6 on a receiver, 255 wired */
    wchar_t deviceName[64];     /* for display */
    int showMeter;              /* tray shows the battery meter instead of the Jig Rack icon */
    int ruleCount;
    Rule rules[MAX_RULES];
} Settings;

extern Settings g_set;
void settings_defaults(Settings *s);
void settings_load(Settings *s);
BOOL settings_save(const Settings *s);
void settings_use_file(const wchar_t *path);    /* --settings <file>: use this file instead (tests, diagnostics) */
const wchar_t *settings_file(void);
void rule_defaults(Rule *r);
const wchar_t *corner_label(int corner);        /* "Top-right" */
const wchar_t *action_label(int action);        /* "Lock the PC" */
void rule_describe(const Rule *r, wchar_t *out, int cch);

/* ---------------------------------------------------------------- mouse battery (mouse.c) -------------------- */
typedef struct {
    int ok;                     /* 1 if the mouse answered (now or earlier: see stale) */
    int percent;
    int charging;
    int approx;                 /* percent estimated from voltage */
    int stale;                  /* mouse not answering now; percent is the last reading */
    wchar_t state[32];
} Battery;

typedef struct {
    WORD productId;
    BYTE index;
    int type;                   /* HID++ device type: 3 mouse, 4 trackpad, 5 trackball; -1 unknown */
    int percent;                /* -1 if no reading */
    wchar_t name[64];
    wchar_t via[64];            /* "USB Receiver" (product string of the receiver), or "cable" */
} FoundDevice;

typedef struct { int count; FoundDevice d[24]; } DeviceList;

void mouse_start(HWND notify);                  /* start the monitor thread; results arrive as WM_RESULT */
void mouse_apply(void);                         /* g_set's mouse choice changed */
void mouse_refresh(void);                       /* read the battery now */
void mouse_search(HWND reply);                  /* find Logitech devices on a worker thread; replies WM_DEVICES */
int  mouse_list_devices(DeviceList *out);       /* the same search, on the calling thread */

/* ---------------------------------------------------------------- icons (icons.c) ---------------------------- */
HICON icon_meter(const Battery *b);             /* the battery meter for the tray */
HICON icon_app(int size);                       /* the Jig Rack icon; size 0 = tray size */
void icons_write_previews(const wchar_t *dir);
BOOL icons_write_ico(const wchar_t *path);
ULONG_PTR icons_startup(void);

/* ---------------------------------------------------------------- hot corners (hotcorner.c) ------------------- */
typedef struct {
    RECT r;
    wchar_t path[256];          /* device path: identifies the monitor */
    wchar_t name[64];           /* friendly name ("LG HDR 4K"), or "Built-in display" */
    wchar_t gdi[32];            /* \\.\DISPLAY2 */
} MonitorDesc;

#define TIMER_CORNER 2
int  monitors_list(MonitorDesc *out, int max);
void hotcorner_init(HINSTANCE inst, HWND owner);
void hotcorner_apply(void);                     /* g_set's rules changed, or the monitors did */
void hotcorner_tick(void);
void hotcorner_layout_changed(void);
int  hotcorner_selftest(void);                  /* returns the number of failures */
void hotcorner_demo_countdown(void);

/* ---------------------------------------------------------------- tray + settings window -------------------- */
void tray_refresh(void);                        /* main.c: redraw the tray icon from g_set and the last reading */
void settings_window_open(HINSTANCE inst);      /* settingswnd.c */
HWND settings_window(void);
int  settings_window_snapshots(HINSTANCE inst, const wchar_t *dir);   /* --snap-settings: PNG of each tab */
