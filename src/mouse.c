/* Jig Rack -- the mouse battery jig: Logitech HID++ 2.0 over a receiver (or a cable), read-only.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * Battery: UNIFIED_BATTERY (0x1004), falling back to BATTERY_STATUS (0x1000) and BATTERY_VOLTAGE (0x1001).
 * The mouse to read is chosen in the Settings window: the receiver's USB product ID plus the device's index on it.
 * A monitor thread keeps that receiver open: battery-change events from the mouse show at once, and a check (one
 * 20-byte request + reply) runs every 5 s as a backstop. With no mouse chosen the thread just sleeps.
 * The device search (Settings > Mouse) runs on its own thread with its own handle and its own HID++ software ID,
 * so its replies are never taken for the monitor's.
 */
#include "jigrack.h"
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdlib.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

#define CHECK_MS    5000        /* backstop check; changes normally arrive as events */
#define REOPEN_MS   15000       /* retry while the receiver is unplugged */
#define BT_POLL_MS  60000       /* Bluetooth: Windows gives no change notification; the level moves slowly */
#define SW_MONITOR  0x0A        /* HID++ software IDs: who sent a request */
#define SW_SEARCH   0x0B

typedef void (*ReportFn)(const BYTE *r);

/* ---------------------------------------------------------------- HID channels -------------------------------- */

typedef struct { wchar_t path[512]; WORD pid; } Channel;

static BOOL is_long_channel(HANDLE h)
{
    PHIDP_PREPARSED_DATA pp = NULL;
    HIDP_CAPS caps;
    BOOL ok = HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
              caps.UsagePage == 0xFF00 && caps.InputReportByteLength == 20;
    if (pp) HidD_FreePreparsedData(pp);
    return ok;
}

static HANDLE open_path(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!is_long_channel(h)) { CloseHandle(h); return NULL; }
    return h;
}

/* Every Logitech HID++ long-report channel present (vendor 046D, usage page 0xFF00, 20-byte reports). */
static int find_channels(Channel *out, int max)
{
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVICE_INTERFACE_DATA d;
    for (DWORD i = 0; n < max; i++) {
        d.cbSize = sizeof d;
        if (!SetupDiEnumDeviceInterfaces(set, NULL, &g, i, &d)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &d, NULL, 0, &need, NULL);
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W det = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)HeapAlloc(GetProcessHeap(), 0, need);
        if (!det) continue;
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &d, det, need, NULL, NULL)) {
            wchar_t low[512];
            wcsncpy_s(low, 512, det->DevicePath, _TRUNCATE);
            _wcslwr_s(low, 512);
            const wchar_t *pid = wcsstr(low, L"pid_");
            if (wcsstr(low, L"vid_046d") && pid) {
                HANDLE h = open_path(det->DevicePath);
                if (h) {
                    CloseHandle(h);
                    wcsncpy_s(out[n].path, 512, det->DevicePath, _TRUNCATE);
                    out[n].pid = (WORD)wcstoul(pid + 4, NULL, 16);
                    n++;
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static HANDLE open_pid(WORD pid)
{
    Channel ch[8];
    int n = find_channels(ch, 8);
    for (int i = 0; i < n; i++)
        if (ch[i].pid == pid) return open_path(ch[i].path);
    return NULL;
}

/* One overlapped 20-byte read or write with a timeout. Returns bytes transferred, or -1. */
static int hid_io(HANDLE h, BYTE *buf, BOOL write, DWORD timeout)
{
    OVERLAPPED ov = {0};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    DWORD n = 0;
    if (write) WriteFile(h, buf, 20, &n, &ov); else ReadFile(h, buf, 20, &n, &ov);
    int result = -1;
    if (WaitForSingleObject(ov.hEvent, timeout) == WAIT_OBJECT_0) {
        if (GetOverlappedResult(h, &ov, &n, FALSE)) result = (int)n;
    } else {
        CancelIo(h);
        GetOverlappedResult(h, &ov, &n, TRUE);
    }
    CloseHandle(ov.hEvent);
    return result;
}

/* Send a HID++ 2.0 long request and wait up to `wait` ms for its reply (or its error). 1 on success.
   Other reports that arrive meanwhile go to `other` (battery events, connection notices), if given. */
static int hidpp_call(HANDLE h, BYTE sw, BYTE dev, BYTE feat, BYTE func, const BYTE *params, int np, BYTE *reply,
                      DWORD wait, ReportFn other)
{
    BYTE req[20] = {0};
    BYTE tag = (BYTE)((func << 4) | sw);
    req[0] = 0x11; req[1] = dev; req[2] = feat; req[3] = tag;
    for (int i = 0; i < np && i < 16; i++) req[4 + i] = params[i];
    if (hid_io(h, req, TRUE, 1500) < 0) return 0;
    ULONGLONG end = GetTickCount64() + wait;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) return 0;
        BYTE r[20] = {0};
        if (hid_io(h, r, FALSE, (DWORD)(end - now)) < 4) return 0;
        if (r[0] == 0x11 && r[1] == dev && r[2] == feat && r[3] == tag) { memcpy(reply, r, 20); return 1; }
        if (r[1] == dev && r[3] == feat && r[4] == tag &&                     /* our request, refused */
            ((r[0] == 0x11 && r[2] == 0xFF) || (r[0] == 0x10 && r[2] == 0x8F))) return 0;
        if (other) other(r);
    }
}

static BYTE feature_index(HANDLE h, BYTE sw, BYTE dev, WORD feat, DWORD wait, ReportFn other)
{
    BYTE p[2] = { (BYTE)(feat >> 8), (BYTE)(feat & 0xFF) }, r[20];
    return hidpp_call(h, sw, dev, 0x00, 0, p, 2, r, wait, other) ? r[4] : 0;
}

static int volt_to_pct(int mv)
{
    static const int t[][2] = { {4186,100},{4067,90},{3989,80},{3922,70},{3859,60},{3808,50},
                                {3770,40},{3734,30},{3684,20},{3621,10},{3500,5} };
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++) if (mv >= t[i][0]) return t[i][1];
    return 0;
}

/* Battery features, in the order tried. The query reply and the feature's change event share one layout. */
static const WORD FEAT[4] = { 0, 0x1004 /* UNIFIED_BATTERY */, 0x1000 /* BATTERY_STATUS */, 0x1001 /* BATTERY_VOLTAGE */ };
static const BYTE FUNC[4] = { 0, 1, 0, 0 };

static void parse(BYTE kind, const BYTE *r, Battery *b)
{
    ZeroMemory(b, sizeof *b);
    b->ok = 1;
    if (kind == 1) {
        static const wchar_t *s[] = { L"discharging", L"charging", L"charging (slow)", L"full", L"charging error" };
        b->percent = r[4]; b->charging = (r[6] == 1 || r[6] == 2);
        wcscpy_s(b->state, 32, r[6] < 5 ? s[r[6]] : L"unknown");
    } else if (kind == 2) {
        static const wchar_t *s[] = { L"discharging", L"charging", L"almost full", L"full", L"charging (slow)",
                                      L"invalid battery", L"thermal error" };
        b->percent = r[4]; b->charging = (r[6] == 1 || r[6] == 2 || r[6] == 4);
        wcscpy_s(b->state, 32, r[6] < 7 ? s[r[6]] : L"unknown");
    } else {
        b->approx = 1; b->percent = volt_to_pct((r[4] << 8) | r[5]); b->charging = (r[6] & 0x80) != 0;
        wcscpy_s(b->state, 32, b->charging ? L"charging" : L"discharging");
    }
}

/* ---------------------------------------------------------------- monitor thread ------------------------------ */

typedef struct { int type; WORD pid; BYTE index; wchar_t id[200]; } Choice;

static CRITICAL_SECTION g_choiceLock;
static Choice g_choice;                     /* written by the UI thread (mouse_apply) */
static HWND g_notify;
static HANDLE g_wake;                       /* auto-reset: something changed, or Refresh */
static volatile LONG g_force;

/* Everything below is the monitor thread's own. */
static Choice g_cur;
static struct { BYTE idx, kind; } g_bat;    /* where the battery feature was found; kind 0 = not yet */
static Battery g_last, g_pub;               /* last real reading; last state sent to the UI */
static BOOL g_pubSent;
static ULONGLONG g_checkAt;

/* Send a state to the UI thread, only when it differs from the last one sent. */
static void publish(const Battery *b, const char *why)
{
    if (g_pubSent && memcmp(b, &g_pub, sizeof *b) == 0) return;
    g_pub = *b;
    g_pubSent = TRUE;
    trace("%s: %s%d%% %ls%s", why, b->ok ? "" : "no reading ", b->percent, b->state, b->stale ? " (last reading)" : "");
    Battery *copy = (Battery *)HeapAlloc(GetProcessHeap(), 0, sizeof *copy);
    if (!copy) return;
    *copy = *b;
    if (!PostMessageW(g_notify, WM_RESULT, 0, (LPARAM)copy)) HeapFree(GetProcessHeap(), 0, copy);
}

static void reading(const Battery *b, const char *why) { g_last = *b; publish(b, why); }

/* Mouse not answering: keep showing the last reading (the level doesn't change while it sleeps). */
static void unreachable(const wchar_t *state, const char *why)
{
    Battery b = g_last;
    if (!b.ok) ZeroMemory(&b, sizeof b);
    b.stale = b.ok;
    wcscpy_s(b.state, 32, state);
    publish(&b, why);
}

static void on_report(const BYTE *r)
{
    if (g_bat.kind && r[0] == 0x11 && r[1] == g_cur.index && r[2] == g_bat.idx && (r[3] & 0x0F) == 0) {
        Battery b;                                       /* the mouse announced a battery change */
        parse(g_bat.kind, r, &b);
        reading(&b, "event");
    } else if (r[0] == 0x10 && r[2] == 0x41 && r[1] == g_cur.index) {   /* receiver: our mouse connected or dropped */
        BOOL up = !(r[4] & 0x40);
        trace("link %s, device %d", up ? "up" : "down", r[1]);
        if (up) g_checkAt = GetTickCount64() + 1000;     /* read it as soon as it has settled */
        else unreachable(L"asleep or off", "link down");
    }
}

static int discover(HANDLE h, Battery *b)
{
    BYTE r[20];
    for (BYTE k = 1; k <= 3; k++) {
        BYTE i = feature_index(h, SW_MONITOR, g_cur.index, FEAT[k], 2000, on_report);
        if (i && hidpp_call(h, SW_MONITOR, g_cur.index, i, FUNC[k], NULL, 0, r, 2000, on_report)) {
            g_bat.idx = i; g_bat.kind = k;
            trace("device %d: feature 0x%04X at index %d", g_cur.index, FEAT[k], i);
            parse(k, r, b);
            return 1;
        }
    }
    return 0;
}

/* The periodic check: one request and one reply once the feature is known. */
static int check(HANDLE h, Battery *b)
{
    BYTE r[20];
    if (!g_bat.kind) return discover(h, b);
    if (!hidpp_call(h, SW_MONITOR, g_cur.index, g_bat.idx, FUNC[g_bat.kind], NULL, 0, r, 2000, on_report)) return 0;
    parse(g_bat.kind, r, b);
    return 1;
}

static DWORD WINAPI monitor(LPVOID unused)
{
    (void)unused;
    HANDLE h = NULL, rd = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_cur.type = -1;
    for (;;) {
        Choice c;
        EnterCriticalSection(&g_choiceLock);
        c = g_choice;
        LeaveCriticalSection(&g_choiceLock);
        if (memcmp(&c, &g_cur, sizeof c) != 0) {         /* a different mouse (or none): start over */
            if (h) { CloseHandle(h); h = NULL; }
            g_cur = c;
            g_bat.kind = 0;
            g_checkAt = 0;
            ZeroMemory(&g_last, sizeof g_last);
            if (c.type == MOUSE_LOGITECH && c.pid) trace("mouse: receiver %04X, device %d", c.pid, c.index);
            if (c.type == MOUSE_BLUETOOTH && c.id[0]) trace("mouse: Bluetooth %ls", c.id);
        }
        if (g_cur.type == MOUSE_BLUETOOTH && g_cur.id[0]) {   /* read what Windows last heard, once a minute */
            InterlockedExchange(&g_force, 0);
            int pct = bt_battery(g_cur.id);
            if (pct >= 0) {
                Battery b = {0};
                b.ok = 1;
                b.percent = pct;
                wcscpy_s(b.state, 32, L"Bluetooth");
                reading(&b, "bluetooth");
            } else {
                unreachable(L"not connected", "bluetooth");
            }
            WaitForSingleObject(g_wake, BT_POLL_MS);
            continue;
        }
        if (g_cur.type != MOUSE_LOGITECH || !g_cur.pid) {
            Battery b = {0};
            wcscpy_s(b.state, 32, L"no mouse chosen");
            publish(&b, "mouse");
            WaitForSingleObject(g_wake, INFINITE);       /* nothing to read: sleep until the settings change */
            continue;
        }
        if (!h) {
            h = open_pid(g_cur.pid);
            if (!h) {
                Battery b = {0};
                wcscpy_s(b.state, 32, L"no receiver");
                publish(&b, "open");
                WaitForSingleObject(g_wake, REOPEN_MS);
                continue;
            }
            trace("receiver opened");
            g_bat.kind = 0;
            g_checkAt = 0;
        }

        if (InterlockedExchange(&g_force, 0)) { g_bat.kind = 0; g_checkAt = 0; }   /* Refresh: look again */
        if (GetTickCount64() >= g_checkAt) {
            Battery b;
            if (check(h, &b)) reading(&b, "check");
            else unreachable(L"asleep or off", "check");
            g_checkAt = GetTickCount64() + CHECK_MS;
        }

        /* Idle: one pending read; wake for a report, a settings change or Refresh, or the next check. No CPU. */
        BYTE r[20] = {0};
        OVERLAPPED ov = {0};
        ov.hEvent = rd;
        ResetEvent(rd);
        DWORD n = 0;
        if (!ReadFile(h, r, 20, &n, &ov) && GetLastError() != ERROR_IO_PENDING) {
            trace("receiver lost (%lu)", GetLastError());
            CloseHandle(h); h = NULL;
            continue;
        }
        ULONGLONG now = GetTickCount64();
        HANDLE ws[2] = { rd, g_wake };
        DWORD w = WaitForMultipleObjects(2, ws, FALSE, g_checkAt > now ? (DWORD)(g_checkAt - now) : 0);
        if (w != WAIT_OBJECT_0) CancelIo(h);
        if (GetOverlappedResult(h, &ov, &n, TRUE)) {
            if (n >= 4) on_report(r);
        } else if (GetLastError() != ERROR_OPERATION_ABORTED) {
            trace("receiver lost (%lu)", GetLastError());
            CloseHandle(h); h = NULL;
        }
    }
}

void mouse_apply(void)
{
    if (!g_wake) return;                    /* not started (diagnostic modes) */
    EnterCriticalSection(&g_choiceLock);
    g_choice.type = g_set.mouseType;
    g_choice.pid = g_set.productId;
    g_choice.index = (BYTE)g_set.deviceIndex;
    ZeroMemory(g_choice.id, sizeof g_choice.id);
    wcscpy_s(g_choice.id, 200, g_set.deviceId);
    LeaveCriticalSection(&g_choiceLock);
    if (g_wake) SetEvent(g_wake);
}

void mouse_refresh(void)
{
    InterlockedExchange(&g_force, 1);
    if (g_wake) SetEvent(g_wake);
}

void mouse_start(HWND notify)
{
    InitializeCriticalSection(&g_choiceLock);
    g_notify = notify;
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    mouse_apply();
    HANDLE t = CreateThread(NULL, 0, monitor, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

/* ---------------------------------------------------------------- device search ------------------------------- */

/* HID++ DEVICE_NAME (0x0005): the name the device reports, and its type. */
static BOOL device_name(HANDLE h, BYTE dev, wchar_t *out, int cch, int *type)
{
    BYTE r[20];
    *type = -1;
    out[0] = 0;
    BYTE fi = feature_index(h, SW_SEARCH, dev, 0x0005, 1000, NULL);
    if (!fi || !hidpp_call(h, SW_SEARCH, dev, fi, 0, NULL, 0, r, 1000, NULL)) return FALSE;
    int len = r[4];
    char name[64];
    int got = 0;
    while (got < len && got < 63) {
        BYTE p = (BYTE)got;
        if (!hidpp_call(h, SW_SEARCH, dev, fi, 1, &p, 1, r, 1000, NULL)) break;
        int before = got;
        for (int k = 4; k < 20 && got < len && got < 63; k++) name[got++] = (char)r[k];
        if (got == before) break;
    }
    name[got] = 0;
    MultiByteToWideChar(CP_UTF8, 0, name, -1, out, cch);
    if (hidpp_call(h, SW_SEARCH, dev, fi, 2, NULL, 0, r, 1000, NULL)) *type = r[4];
    return out[0] != 0;
}

int mouse_list_devices(DeviceList *out)
{
    ZeroMemory(out, sizeof *out);
    Channel ch[8];
    int n = find_channels(ch, 8);
    static const BYTE devs[] = { 1, 2, 3, 4, 5, 6, 0xFF };
    for (int c = 0; c < n; c++) {
        HANDLE h = open_path(ch[c].path);
        if (!h) continue;
        wchar_t product[64] = L"";
        HidD_GetProductString(h, product, sizeof product);
        for (int k = 0; k < (int)sizeof devs && out->count < (int)(sizeof out->d / sizeof out->d[0]); k++) {
            BYTE dev = devs[k], r[20];
            if (!hidpp_call(h, SW_SEARCH, dev, 0x00, 1, NULL, 0, r, 400, NULL) || r[4] < 2) continue;  /* HID++ 2.0? (an empty slot may never answer) */
            FoundDevice *d = &out->d[out->count];
            ZeroMemory(d, sizeof *d);
            d->productId = ch[c].pid;
            d->index = dev;
            d->percent = -1;
            device_name(h, dev, d->name, 64, &d->type);
            if (!d->name[0]) swprintf_s(d->name, 64, L"Logitech device %d", dev);
            if (dev == 0xFF) wcscpy_s(d->via, 64, L"cable");
            else swprintf_s(d->via, 64, L"%s, slot %d", product[0] ? product : L"receiver", dev);
            BOOL battery = FALSE;
            for (BYTE f = 1; f <= 3 && !battery; f++) {
                BYTE fi = feature_index(h, SW_SEARCH, dev, FEAT[f], 1000, NULL);
                if (fi && hidpp_call(h, SW_SEARCH, dev, fi, FUNC[f], NULL, 0, r, 1000, NULL)) {
                    Battery b;
                    parse(f, r, &b);
                    d->percent = b.percent;
                    battery = TRUE;
                }
            }
            BOOL pointer = d->type < 0 || d->type == 3 || d->type == 4 || d->type == 5;  /* mouse, trackpad, trackball */
            trace("search: %04X device %d \"%ls\" type %d battery %d%%%s", d->productId, dev, d->name, d->type,
                  d->percent, battery && pointer ? "" : " (not listed)");
            if (battery && pointer) out->count++;
        }
        CloseHandle(h);
    }
    return out->count;
}

typedef struct { HWND reply; int type; } SearchJob;

static DWORD WINAPI search_thread(LPVOID p)
{
    SearchJob job = *(SearchJob *)p;
    HeapFree(GetProcessHeap(), 0, p);
    DeviceList *l = (DeviceList *)HeapAlloc(GetProcessHeap(), 0, sizeof *l);
    if (!l) return 0;
    if (job.type == MOUSE_BLUETOOTH) bt_list_devices(l); else mouse_list_devices(l);
    if (!IsWindow(job.reply) || !PostMessageW(job.reply, WM_DEVICES, job.type, (LPARAM)l)) HeapFree(GetProcessHeap(), 0, l);
    return 0;
}

/* The reply's wParam is the type searched, so a late answer for another type can be told apart. */
void mouse_search(HWND reply, int type)
{
    SearchJob *job = (SearchJob *)HeapAlloc(GetProcessHeap(), 0, sizeof *job);
    if (!job) return;
    job->reply = reply;
    job->type = type;
    HANDLE t = CreateThread(NULL, 0, search_thread, job, 0, NULL);
    if (t) CloseHandle(t); else HeapFree(GetProcessHeap(), 0, job);
}
