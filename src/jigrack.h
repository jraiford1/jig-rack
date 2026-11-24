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
#define WM_EJECTED  (WM_APP + 4)     /* eject worker -> tray window: lp = EjectResult* (HeapAlloc'd) */

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
    int warnLow;                /* while dwelling, warn if the mouse battery is low */
    wchar_t monitor[256];       /* WHERE_MONITOR: the monitor's device path (stable across docking) */
    wchar_t monitorName[64];    /* its friendly name, for showing it while it isn't connected */
    wchar_t command[512];       /* ACT_RUN */
} Rule;

#define MAX_APPS 16
enum { POS_MAXIMIZED, POS_LEFT, POS_RIGHT, POS_TOP, POS_BOTTOM, POS_LEFT_THIRD, POS_MIDDLE_THIRD, POS_RIGHT_THIRD,
       POS_CUSTOM, POSITIONS };
typedef struct {                /* one of the workspace's apps */
    wchar_t name[64];           /* "Google Chrome" */
    wchar_t command[MAX_PATH + 64]; /* what to start: a program, or shell:AppsFolder\<app ID> for a Store app */
    wchar_t args[256];
    wchar_t match[MAX_PATH];    /* the program file its windows belong to (to find it when it's open) */
    wchar_t title[64];          /* only a window whose title contains this; "" = any */
    wchar_t monitor[256];       /* the monitor's device path; "" = the main monitor */
    wchar_t monitorName[64];
    int pos;                    /* POS_* */
    int x, y, w, h;             /* POS_CUSTOM: % of the monitor's work area */
} App;

typedef struct {
    int mouseType;              /* MOUSE_* */
    WORD productId;             /* Logitech: USB product ID of the receiver (or wired mouse) the mouse is on; 0 = none */
    int deviceIndex;            /* its HID++ device index there: 1..6 on a receiver, 255 wired */
    wchar_t deviceName[64];     /* for display */
    int showMeter;              /* tray shows the battery meter instead of the Jig Rack icon */
    int lowPercent;             /* "low battery": at or below this the meter is red and hot corners can warn */
    int ruleCount;
    Rule rules[MAX_RULES];
    int dlButton;               /* Downloads: a button on the taskbar */
    int dlAllMonitors;          /* on every monitor's taskbar, not just the main one */
    wchar_t dlIcon[MAX_PATH + 16];  /* "path,index" (a negative index is a resource ID); "" = Windows' Downloads icon */
    wchar_t dlFolder[MAX_PATH]; /* "" = the Downloads folder */
    int dlCount;                /* how many of the newest items the panel shows */
    int dlSort;                 /* DL_SORT_* */
    int appCount;               /* the workspace */
    App apps[MAX_APPS];
} Settings;
enum { DL_SORT_ADDED, DL_SORT_MODIFIED, DL_SORT_NAME, DL_SORT_KIND, DL_SORTS };
#define WM_DL_COMMAND (WM_APP + 5)       /* "--downloads <command>" (from the jump list) -> running copy: wp = DL_CMD_* */
enum { DL_CMD_SORT_ADDED, DL_CMD_SORT_MODIFIED, DL_CMD_SORT_NAME, DL_CMD_SORT_KIND, DL_CMD_OPEN, DL_CMD_SETTINGS, DL_CMDS };

extern Settings g_set;
BOOL mouse_chosen(const Settings *s);           /* a mouse is picked */
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

extern Battery g_battery;                       /* main.c: the last reading of the chosen mouse */
BOOL mouse_battery_low(void);                   /* chosen, not charging, last reading <= g_set.lowPercent */

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
#define TIMER_WORKSPACE 3
int  monitors_list(MonitorDesc *out, int max);
void hotcorner_init(HINSTANCE inst, HWND owner);
void hotcorner_apply(void);                     /* g_set's rules changed, or the monitors did */
void hotcorner_tick(void);
void hotcorner_layout_changed(void);
int  hotcorner_selftest(void);                  /* returns the number of failures */
void hotcorner_demo_countdown(void);
void hotcorner_write_previews(const wchar_t *dir);   /* --preview: the countdown overlay, with and without the warning */

/* ---------------------------------------------------------------- tray + settings window -------------------- */
void tray_refresh(void);                        /* main.c: redraw the tray icon from g_set and the last reading */
void settings_window_open(HINSTANCE inst);      /* settingswnd.c */
void tray_notify(BOOL ok, const wchar_t *title, const wchar_t *text);   /* main.c: a notification from the tray icon */
void settings_window_open_page(HINSTANCE inst, int page);   /* 0 Mouse, 1 Hot Corners, 2 Downloads */
void settings_window_refresh(void);             /* settings changed elsewhere (the jump list): show them */
HWND settings_window(void);
int  settings_window_snapshots(HINSTANCE inst, const wchar_t *dir);   /* --snap-settings: PNG of each tab */

/* ---------------------------------------------------------------- eject media (media.c) ----------------------- */
#define MAX_DRIVES 26
typedef struct {
    DWORD devType, devNum;      /* the disk (STORAGE_DEVICE_NUMBER) */
    DWORD letterMask;           /* bit 0 = A: */
    int removable;              /* DRIVE_REMOVABLE (a stick or card) rather than a USB disk */
    ULONGLONG bytes;            /* total size of its volumes */
    wchar_t letters[80];        /* "E:" or "E:, F:" */
    wchar_t volume[64];         /* the volume label, when there's one volume */
    wchar_t product[100];       /* "Samsung PSSD T7" */
    wchar_t label[200];         /* menu text */
} Drive;
typedef struct { int ok; wchar_t title[64]; wchar_t text[256]; } EjectResult;
int media_list(Drive *out, int max);                 /* the drives that can be safely removed */
void media_label(Drive *d);                          /* fills d->label from the rest */
BOOL media_eject(const Drive *d, wchar_t *why, int cch);
BOOL media_eject_async(HWND notify, const Drive *list, int n);  /* FALSE if one is already running */

/* ---------------------------------------------------------------- Downloads (downloads.c) --------------------- */
typedef struct {
    wchar_t name[MAX_PATH];
    FILETIME added, modified;   /* created in the folder; last written */
    ULONGLONG size;
    BOOL dir;
} DlItem;
void downloads_init(HINSTANCE inst);
void downloads_apply(void);                          /* (re)make the taskbar buttons */
BOOL downloads_is_message(MSG *m);                   /* keyboard for the open panel */
void downloads_folder(wchar_t *out, int cch);        /* the folder in use */
void downloads_icon_location(wchar_t *path, int cch, int *index);
HICON downloads_load_icon(int size);
int downloads_collect(const wchar_t *folder, int sort, DlItem *out, int max);
void downloads_when(FILETIME ft, FILETIME now, wchar_t *out, int cch);
void downloads_jumplist(void);                       /* the button's right-click menu */
void downloads_command(int cmd);                     /* DL_CMD_*, in the running copy */
int downloads_command_id(const wchar_t *name);       /* "sort-name" -> DL_CMD_SORT_NAME; -1 if unknown */
extern const wchar_t *DL_SORT_LABELS[DL_SORTS];

/* ---------------------------------------------------------------- Workspace (workspace.c) --------------------- */
extern const wchar_t *POSITION_LABELS[POSITIONS];
void workspace_launch(HWND owner);                   /* start the apps and place their windows */
void workspace_tick(void);                           /* TIMER_WORKSPACE */
void workspace_target(const RECT *work, int pos, int x, int y, int w, int h, RECT *out);
HWND workspace_find(const App *a);                   /* the app's open window, or NULL */
int  workspace_open_windows(HWND *out, int max);     /* open app windows, for "add from an open window" */
void workspace_app_from_window(HWND w, App *a);
void workspace_app_from_file(const wchar_t *path, App *a);
BOOL workspace_use_current(App *a);                  /* its open window's place now; FALSE if it isn't open */
