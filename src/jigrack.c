/*
 * Jig Rack -- a rack of small, purpose-built Windows tools ("jigs") in one notification-area app.
 *   - Mouse battery jig: battery level and charging state of a Logitech wireless mouse, without G HUB.
 *   - Hot corner jig: rest the cursor 5 s in the top-right corner of the whole monitor set to lock the PC,
 *     with an on-screen 5..1 countdown next to the corner.
 *
 * Copyright (C) 2026 Jon Raiford
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. It is distributed WITHOUT ANY WARRANTY; see the LICENSE file for details.
 *
 * Reads the battery over the receiver with Logitech's HID++ 2.0 protocol (read-only queries):
 * UNIFIED_BATTERY (0x1004), falling back to BATTERY_STATUS (0x1000) and BATTERY_VOLTAGE (0x1001).
 * Icon: a mouse over a battery with the percentage; blue charging, red <=20%, yellow <=50%, green above;
 * grey "?" before the first reading. Hover for details; right-click for the menu. Single instance.
 * A monitor thread keeps the receiver open: battery-change events from the mouse show at once, and a check
 * (one 20-byte request + reply) runs every 5 s as a backstop. Log: jigrack.log next to the exe.
 *
 * Build: build.cmd (needs the Visual Studio C++ tools). Libraries come in via #pragma comment(lib, ...).
 */
#include <windows.h>
#include <shellapi.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <wchar.h>
#include <stdio.h>
#include <stdarg.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_TRAY      (WM_APP + 1)
#define WM_RESULT    (WM_APP + 2)
#define ID_REFRESH   1
#define ID_EXIT      2
#define ID_HOTCORNER 3
#define CHECK_MS     5000      /* backstop check; changes normally arrive as events */
#define REOPEN_MS    15000     /* retry while the receiver is unplugged */

typedef struct {
    int ok;             /* 1 if the mouse answered */
    int percent;
    int charging;
    int approx;         /* percent estimated from voltage */
    int stale;          /* mouse not answering now; percent is the last reading */
    wchar_t state[32];
} Battery;

static NOTIFYICONDATAW g_nid;
static HICON g_icon;
static UINT g_taskbarCreated;

/* ---------------------------------------------------------------- HID++ ---------------------------------------- */

static HANDLE open_long_channel(void)
{
    GUID g;
    HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return NULL;
    HANDLE found = NULL;
    SP_DEVICE_INTERFACE_DATA d;
    for (DWORD i = 0; !found; i++) {
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
            if (wcsstr(low, L"vid_046d")) {
                HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    PHIDP_PREPARSED_DATA pp = NULL;
                    HIDP_CAPS caps;
                    BOOL ok = HidD_GetPreparsedData(h, &pp) && HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
                              caps.UsagePage == 0xFF00 && caps.InputReportByteLength == 20;
                    if (pp) HidD_FreePreparsedData(pp);
                    if (ok) found = h; else CloseHandle(h);
                }
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
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

/* Log of what the monitor saw (receiver opened, device found, events, state changes), next to the exe.
   Recreated at each start; only changes are written, so it stays tiny. */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void trace(const char *fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char line[256];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, sizeof line - 2, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = vsprintf_s(line + n, sizeof line - 2 - n, fmt, ap);
    va_end(ap);
    if (m > 0) n += m;
    line[n++] = '\r'; line[n++] = '\n';
    DWORD w;
    WriteFile(g_log, line, n, &w, NULL);
}

static void on_report(const BYTE *r);

/* Send a HID++ 2.0 long request and wait for its matching reply. 1 on success. Unrelated reports that arrive
   meanwhile (battery events, connection notices) are passed to on_report so nothing is lost. */
static int hidpp_call(HANDLE h, BYTE dev, BYTE feat, BYTE func, const BYTE *params, int np, BYTE *reply)
{
    BYTE req[20] = {0};
    BYTE tag = (BYTE)((func << 4) | 0x0A);
    req[0] = 0x11; req[1] = dev; req[2] = feat; req[3] = tag;
    for (int i = 0; i < np && i < 16; i++) req[4 + i] = params[i];
    if (hid_io(h, req, TRUE, 1500) < 0) return 0;
    ULONGLONG end = GetTickCount64() + 2000;
    while (GetTickCount64() < end) {
        BYTE r[20] = {0};
        if (hid_io(h, r, FALSE, 1500) < 4) return 0;
        if (r[0] == 0x11 && r[1] == dev && r[2] == feat && r[3] == tag) { memcpy(reply, r, 20); return 1; }
        if (r[1] == dev && ((r[0] == 0x11 && r[2] == 0xFF) || (r[0] == 0x10 && r[2] == 0x8F))) return 0;  /* error */
        on_report(r);
    }
    return 0;
}

static BYTE feature_index(HANDLE h, BYTE dev, WORD feat)
{
    BYTE p[2] = { (BYTE)(feat >> 8), (BYTE)(feat & 0xFF) }, r[20];
    return hidpp_call(h, dev, 0x00, 0, p, 2, r) ? r[4] : 0;
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

/* ---- monitor thread: everything below runs on it ---- */
static HWND g_hwnd;
static HANDLE g_wake;                       /* auto-reset: Refresh now */
static volatile LONG g_force;
static struct { BYTE dev, idx, kind; } g_bat; /* where the battery feature was found; kind 0 = not yet */
static Battery g_last, g_pub;               /* last real reading; last state sent to the UI */
static ULONGLONG g_checkAt;

/* Send a state to the UI thread, only when it differs from the last one sent. */
static void publish(const Battery *b, const char *why)
{
    if (memcmp(b, &g_pub, sizeof *b) == 0) return;
    g_pub = *b;
    trace("%s: %s%d%% %ls%s", why, b->ok ? "" : "no reading ", b->percent, b->state, b->stale ? " (last reading)" : "");
    Battery *copy = (Battery *)HeapAlloc(GetProcessHeap(), 0, sizeof *copy);
    if (!copy) return;
    *copy = *b;
    if (!PostMessageW(g_hwnd, WM_RESULT, 0, (LPARAM)copy)) HeapFree(GetProcessHeap(), 0, copy);
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
    if (g_bat.kind && r[0] == 0x11 && r[1] == g_bat.dev && r[2] == g_bat.idx && (r[3] & 0x0F) == 0) {
        Battery b;                                       /* the mouse announced a battery change */
        parse(g_bat.kind, r, &b);
        reading(&b, "event");
    } else if (r[0] == 0x10 && r[2] == 0x41) {           /* receiver: a device connected or dropped */
        BOOL up = !(r[4] & 0x40);
        trace("link %s, device %d", up ? "up" : "down", r[1]);
        if (up) g_checkAt = GetTickCount64() + 1000;     /* read it as soon as it has settled */
        else if (!g_bat.kind || r[1] == g_bat.dev) unreachable(L"asleep or off", "link down");
    }
}

static int discover(HANDLE h, Battery *b)
{
    BYTE r[20];
    for (BYTE dev = 1; dev <= 6; dev++)
        for (BYTE k = 1; k <= 3; k++) {
            BYTE i = feature_index(h, dev, FEAT[k]);
            if (i && hidpp_call(h, dev, i, FUNC[k], NULL, 0, r)) {
                g_bat.dev = dev; g_bat.idx = i; g_bat.kind = k;
                trace("found device %d, feature 0x%04X at index %d", dev, FEAT[k], i);
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
    if (!hidpp_call(h, g_bat.dev, g_bat.idx, FUNC[g_bat.kind], NULL, 0, r)) return 0;
    parse(g_bat.kind, r, b);
    return 1;
}

static DWORD WINAPI monitor(LPVOID unused)
{
    (void)unused;
    HANDLE h = NULL, rd = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (;;) {
        if (!h) {
            h = open_long_channel();
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

        if (InterlockedExchange(&g_force, 0)) { g_bat.kind = 0; g_checkAt = 0; }   /* Refresh now: look again */
        if (GetTickCount64() >= g_checkAt) {
            Battery b;
            if (check(h, &b)) reading(&b, "check");
            else unreachable(L"asleep or off", "check");
            g_checkAt = GetTickCount64() + CHECK_MS;
        }

        /* Idle: one pending read; wake for a report, Refresh now, or the next check. No CPU while waiting. */
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
        if (w == WAIT_OBJECT_0 + 1) InterlockedExchange(&g_force, 1);
        if (w != WAIT_OBJECT_0) CancelIo(h);
        if (GetOverlappedResult(h, &ov, &n, TRUE)) {
            if (n >= 4) on_report(r);
        } else if (GetLastError() != ERROR_OPERATION_ABORTED) {
            trace("receiver lost (%lu)", GetLastError());
            CloseHandle(h); h = NULL;
        }
    }
}

/* ---------------------------------------------------------------- GDI+ (flat C API) ---------------------------- */
/* gdiplus.h is C++ only; these are the flat functions gdiplus.dll exports, declared for C. */
typedef void GpGraphics, GpImage, GpBitmap, GpPath, GpBrush, GpPen, GpFontFamily, GpFont, GpStringFormat;
typedef float REAL;
typedef DWORD ARGB;
typedef struct { REAL X, Y; } GpPointF;
typedef struct { REAL X, Y, Width, Height; } GpRectF;
typedef struct { UINT32 GdiplusVersion; void *DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } GdiplusStartupInput;
#define GDIP __stdcall
int GDIP GdiplusStartup(ULONG_PTR *token, const GdiplusStartupInput *in, void *out);
void GDIP GdiplusShutdown(ULONG_PTR token);
int GDIP GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, GpBitmap **b);
int GDIP GdipGetImageGraphicsContext(GpImage *img, GpGraphics **g);
int GDIP GdipSetSmoothingMode(GpGraphics *g, INT mode);
int GDIP GdipSetPixelOffsetMode(GpGraphics *g, INT mode);
int GDIP GdipGraphicsClear(GpGraphics *g, ARGB c);
int GDIP GdipScaleWorldTransform(GpGraphics *g, REAL sx, REAL sy, INT order);
int GDIP GdipCreatePath(INT fillMode, GpPath **p);
int GDIP GdipAddPathArc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
int GDIP GdipAddPathLine(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2);
int GDIP GdipAddPathPolygon(GpPath *p, const GpPointF *pts, INT n);
int GDIP GdipAddPathEllipse(GpPath *p, REAL x, REAL y, REAL w, REAL h);
int GDIP GdipAddPathBezier(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4);
int GDIP GdipSetPenStartCap(GpPen *p, INT cap);
int GDIP GdipSetPenEndCap(GpPen *p, INT cap);
int GDIP GdipAddPathString(GpPath *p, const WCHAR *s, INT len, const GpFontFamily *fam, INT style, REAL em,
                           const GpRectF *layout, const GpStringFormat *fmt);
int GDIP GdipClosePathFigure(GpPath *p);
int GDIP GdipDeletePath(GpPath *p);
int GDIP GdipCreateSolidFill(ARGB c, GpBrush **b);
int GDIP GdipCreateLineBrush(const GpPointF *p1, const GpPointF *p2, ARGB c1, ARGB c2, INT wrap, GpBrush **b);
int GDIP GdipDeleteBrush(GpBrush *b);
int GDIP GdipCreatePen1(ARGB c, REAL width, INT unit, GpPen **p);
int GDIP GdipSetPenLineJoin(GpPen *p, INT join);
int GDIP GdipDeletePen(GpPen *p);
int GDIP GdipFillPath(GpGraphics *g, GpBrush *b, GpPath *p);
int GDIP GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *p);
int GDIP GdipSetClipPath(GpGraphics *g, GpPath *p, INT mode);
int GDIP GdipResetClip(GpGraphics *g);
int GDIP GdipFillRectangle(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h);
int GDIP GdipCreateFontFamilyFromName(const WCHAR *name, void *coll, GpFontFamily **f);
int GDIP GdipDeleteFontFamily(GpFontFamily *f);
int GDIP GdipCreateStringFormat(INT flags, LANGID lang, GpStringFormat **f);
int GDIP GdipSetStringFormatAlign(GpStringFormat *f, INT align);
int GDIP GdipSetStringFormatLineAlign(GpStringFormat *f, INT align);
int GDIP GdipDeleteStringFormat(GpStringFormat *f);
int GDIP GdipCreateHICONFromBitmap(GpBitmap *b, HICON *icon);
int GDIP GdipDeleteGraphics(GpGraphics *g);
int GDIP GdipDisposeImage(GpImage *img);
#pragma comment(lib, "gdiplus.lib")
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")

#define ARGBV(a, r, g, b) (((ARGB)(a) << 24) | ((ARGB)(r) << 16) | ((ARGB)(g) << 8) | (ARGB)(b))

static ULONG_PTR g_gdip;

/* Rounded rectangle as a closed path. */
static GpPath *round_rect(REAL x, REAL y, REAL w, REAL h, REAL r)
{
    GpPath *p = NULL;
    REAL d = r * 2;
    GdipCreatePath(0, &p);
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

/* Mouse outline: domed top (radius rt), softer bottom corners (radius rb). */
static GpPath *mouse_path(REAL x, REAL y, REAL w, REAL h, REAL rt, REAL rb)
{
    GpPath *p = NULL;
    GdipCreatePath(0, &p);
    GdipAddPathArc(p, x, y, rt * 2, rt * 2, 180, 90);
    GdipAddPathArc(p, x + w - rt * 2, y, rt * 2, rt * 2, 270, 90);
    GdipAddPathArc(p, x + w - rb * 2, y + h - rb * 2, rb * 2, rb * 2, 0, 90);
    GdipAddPathArc(p, x, y + h - rb * 2, rb * 2, rb * 2, 90, 90);
    GdipClosePathFigure(p);
    return p;
}

static ARGB lighten(ARGB c, int amt)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r = min(255, r + amt); g = min(255, g + amt); b = min(255, b + amt);
    return ARGBV(255, r, g, b);
}

static ARGB shade(ARGB c, REAL f)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return ARGBV(255, (int)(r * f), (int)(g * f), (int)(b * f));
}

/* Tray glyph on a 32x32 design grid, scaled to `size` pixels, in two rows:
   top: a little mouse (the animal), side view; colour alone shows charging, so `bolt` is unused;
   bottom: a full-width horizontal battery filled left-to-right in the status colour, with the percentage in it.
   level: 0-100, or -1 for unknown. */
static GpBitmap *render_mouse(int size, int level, ARGB fill, BOOL bolt, const wchar_t *label)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GdipCreateBitmapFromScan0(size, size, 0, 0x26200A /* PixelFormat32bppARGB */, NULL, &bmp);
    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, 4);          /* anti-alias */
    GdipSetPixelOffsetMode(g, 4);
    GdipGraphicsClear(g, 0);
    GdipScaleWorldTransform(g, size / 32.0f, size / 32.0f, 0);

    GpPen *halo = NULL, *rim = NULL, *thalo = NULL;
    GdipCreatePen1(ARGBV(170, 0, 0, 0), 2.8f, 0, &halo);
    GdipSetPenLineJoin(halo, 2);
    GdipCreatePen1(ARGBV(255, 240, 240, 244), 1.4f, 0, &rim);
    GdipCreatePen1(ARGBV(210, 0, 0, 0), 2.4f, 0, &thalo);
    GdipSetPenLineJoin(thalo, 2);
    GpBrush *white = NULL;
    GdipCreateSolidFill(ARGBV(255, 255, 255, 255), &white);

    /* ---- top row: a mouse (the animal), side view, facing right ---- */
    {
        GpPen *tail = NULL, *tailhalo = NULL;
        GdipCreatePen1(ARGBV(170, 0, 0, 0), 3.0f, 0, &tailhalo);
        GdipCreatePen1(ARGBV(255, 236, 168, 182), 1.4f, 0, &tail);
        GdipSetPenStartCap(tailhalo, 2); GdipSetPenEndCap(tailhalo, 2);   /* round caps */
        GdipSetPenStartCap(tail, 2); GdipSetPenEndCap(tail, 2);
        GpPath *tp = NULL;                                                /* curled tail, behind the body */
        GdipCreatePath(0, &tp);
        GdipAddPathBezier(tp, 7.5f, 10.8f, 2.4f, 13.4f, 0.0f, 7.6f, 3.4f, 4.8f);
        GdipDrawPath(g, tailhalo, tp);
        GdipDrawPath(g, tail, tp);
        GdipDeletePath(tp);
        GdipDeletePen(tail); GdipDeletePen(tailhalo);

        GpPath *sil = NULL;                                               /* body + head + snout + ear */
        GdipCreatePath(1 /* winding: union of the parts */, &sil);
        GdipAddPathEllipse(sil, 6.0f, 4.4f, 16.5f, 9.8f);                 /* body */
        GdipAddPathEllipse(sil, 16.2f, 4.6f, 9.6f, 8.0f);                 /* head */
        GpPointF snout[] = { {23.0f, 5.6f}, {29.8f, 10.0f}, {23.0f, 12.4f} };
        GdipAddPathPolygon(sil, snout, 3);
        GdipAddPathEllipse(sil, 15.4f, 0.8f, 7.2f, 7.2f);                 /* ear */
        GpBrush *fur = NULL;
        GpPointF ft = { 0, 0.8f }, fb = { 0, 14.2f };
        GdipCreateLineBrush(&ft, &fb, ARGBV(255, 228, 228, 236), ARGBV(255, 150, 150, 164), 0, &fur);
        GdipDrawPath(g, halo, sil);
        GdipFillPath(g, fur, sil);
        GdipDeleteBrush(fur);
        GdipDeletePath(sil);

        GpBrush *pink = NULL, *dark = NULL;
        GdipCreateSolidFill(ARGBV(255, 255, 150, 172), &pink);
        GdipCreateSolidFill(ARGBV(255, 18, 18, 22), &dark);
        GpPath *p = NULL;
        GdipCreatePath(0, &p);
        GdipAddPathEllipse(p, 17.2f, 2.6f, 3.6f, 3.6f);                   /* inner ear */
        GdipAddPathEllipse(p, 28.3f, 8.9f, 2.2f, 2.2f);                   /* nose */
        GdipFillPath(g, pink, p);
        GdipDeletePath(p);
        GdipCreatePath(0, &p);
        GdipAddPathEllipse(p, 23.2f, 6.8f, 2.1f, 2.1f);                   /* eye */
        GdipFillPath(g, dark, p);
        GdipDeletePath(p);
        GdipDeleteBrush(pink); GdipDeleteBrush(dark);
    }

    /* ---- bottom row: horizontal battery ---- */
    const REAL BX = 1.3f, BY = 15.6f, BW = 26.6f, BH = 15.4f;
    GpPath *cell = round_rect(BX, BY, BW, BH, 3.6f);
    GpPath *nub = round_rect(BX + BW + 0.4f, BY + 4.6f, 2.3f, BH - 9.2f, 1.0f);
    GpBrush *empty = NULL;
    GpPointF et = { 0, BY }, eb = { 0, BY + BH };
    GdipCreateLineBrush(&et, &eb, shade(fill, 0.40f), shade(fill, 0.24f), 0, &empty);
    GdipFillPath(g, empty, cell);
    GdipDeleteBrush(empty);
    if (level > 0) {
        REAL lv = (level > 100 ? 100 : level) / 100.0f;
        GdipSetClipPath(g, cell, 0);
        GpBrush *liq = NULL;
        GpPointF a = { 0, BY }, z = { 0, BY + BH };
        GdipCreateLineBrush(&a, &z, lighten(fill, 50), fill, 0, &liq);
        GdipFillRectangle(g, liq, BX, BY, BW * lv, BH);
        GdipDeleteBrush(liq);
        GpBrush *gloss = NULL;                              /* soft highlight along the top */
        GdipCreateSolidFill(ARGBV(55, 255, 255, 255), &gloss);
        GdipFillRectangle(g, gloss, BX, BY, BW * lv, BH * 0.32f);
        GdipDeleteBrush(gloss);
        GdipResetClip(g);
    }
    GdipDrawPath(g, halo, cell);
    GdipDrawPath(g, halo, nub);
    GdipFillPath(g, white, nub);
    GdipDrawPath(g, rim, cell);
    GdipDeletePath(cell);
    GdipDeletePath(nub);

    if (label && *label) {
        GpFontFamily *fam = NULL;
        GpStringFormat *fmt = NULL;
        GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
        if (!fam) GdipCreateFontFamilyFromName(L"Arial", NULL, &fam);
        GdipCreateStringFormat(0x1000 /* NoClip */, 0, &fmt);
        GdipSetStringFormatAlign(fmt, 1);
        GdipSetStringFormatLineAlign(fmt, 1);
        size_t n = wcslen(label);
        REAL em = n >= 3 ? 13.0f : 15.5f;
        GpRectF box = { BX - 4.0f, BY - 0.9f, BW + 8.0f, BH };
        GpPath *tp = NULL;
        GdipCreatePath(0, &tp);
        GdipAddPathString(tp, label, -1, fam, 1 /* bold */, em, &box, fmt);
        GdipDrawPath(g, thalo, tp);
        GdipFillPath(g, white, tp);
        GdipDeletePath(tp);
        GdipDeleteStringFormat(fmt);
        if (fam) GdipDeleteFontFamily(fam);
    }
    GdipDeletePen(thalo); GdipDeletePen(halo); GdipDeletePen(rim);
    GdipDeleteBrush(white);
    GdipDeleteGraphics(g);
    return bmp;
}

/* Status colour: blue charging, red low (<=20%), yellow medium (<=50%), green full. */
#define COL_CHARGING ARGBV(255, 10, 132, 255)
#define COL_LOW      ARGBV(255, 255, 59, 48)
#define COL_MED      ARGBV(255, 255, 204, 0)
#define COL_FULL     ARGBV(255, 48, 199, 89)
static ARGB status_color(int percent, BOOL charging)
{
    return charging ? COL_CHARGING : percent <= 20 ? COL_LOW : percent <= 50 ? COL_MED : COL_FULL;
}

static HICON make_icon(int level, ARGB fill, BOOL bolt, const wchar_t *label)
{
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size < 16) size = 16;
    GpBitmap *bmp = render_mouse(size, level, fill, bolt, label);
    HICON icon = NULL;
    GdipCreateHICONFromBitmap(bmp, &icon);
    GdipDisposeImage(bmp);
    return icon;
}

/* --preview <folder>: save PNGs of each state (64 px and the real tray size) for checking the design. */
int GDIP GdipSaveImageToFile(GpImage *img, const WCHAR *file, const CLSID *encoder, const void *params);
static void write_previews(const wchar_t *dir)
{
    static const CLSID png = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };
    struct { const wchar_t *name; int level; ARGB fill; BOOL bolt; const wchar_t *label; } s[] = {
        { L"full-82",      82, COL_FULL,     FALSE, L"82" },
        { L"med-49",       49, COL_MED,      FALSE, L"49" },
        { L"med-28",       28, COL_MED,      FALSE, L"28" },
        { L"low-9",         9, COL_LOW,      FALSE, L"9" },
        { L"charging-63",  63, COL_CHARGING, FALSE, L"63" },
        { L"hundred-100", 100, COL_FULL,     FALSE, L"100" },
        { L"unknown",      -1, ARGBV(255, 120, 120, 128), FALSE, L"?" },
    };
    int tray = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    int sizes[] = { 64, tray };
    for (int i = 0; i < (int)(sizeof s / sizeof s[0]); i++)
        for (int k = 0; k < 2; k++) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, MAX_PATH, L"%s\\%s-%d.png", dir, s[i].name, sizes[k]);
            GpBitmap *bmp = render_mouse(sizes[k], s[i].level, s[i].fill, s[i].bolt, s[i].label);
            GdipSaveImageToFile(bmp, path, &png, NULL);
            GdipDisposeImage(bmp);
        }
}

/* ---------------------------------------------------------------- tray icon ------------------------------------ */

static Battery g_shown;                     /* UI thread: what the icon shows now */

static void show(const Battery *b)
{
    wchar_t text[8], tip[128];
    ARGB fill = ARGBV(255, 120, 120, 128);
    if (b->ok) {
        swprintf_s(text, 8, L"%d", b->percent);
        fill = status_color(b->percent, b->charging && !b->stale);
        if (b->stale)
            swprintf_s(tip, 128, L"Mouse battery %s%d%% (last reading; %s)", b->approx ? L"~" : L"", b->percent, b->state);
        else
            swprintf_s(tip, 128, L"Mouse battery %s%d%% - %s", b->approx ? L"~" : L"", b->percent, b->state);
    } else {
        wcscpy_s(text, 8, L"?");
        swprintf_s(tip, 128, L"Mouse battery: %s", b->state);
    }
    HICON icon = make_icon(b->ok ? b->percent : -1, fill, FALSE, text);
    g_nid.hIcon = icon;
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (g_icon) DestroyIcon(g_icon);
    g_icon = icon;
    g_shown = *b;
}

static void add_tray(HWND hwnd)
{
    wchar_t tip[128];
    wcscpy_s(tip, 128, g_nid.cbSize ? g_nid.szTip : L"Jig Rack: reading the mouse battery...");
    ZeroMemory(&g_nid, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    if (!g_icon) g_icon = make_icon(-1, ARGBV(255, 120, 120, 128), FALSE, L"");
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), tip);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

/* ---------------------------------------------------------------- hot corner ----------------------------------- */
/* Rest the cursor in the top-right corner of the whole monitor set for 5 s to lock the PC (LockWorkStation).
   "Top-right of the set" = the top-right corner of the monitor reaching furthest right (the highest one if several
   tie), so other monitors' top-right corners never trigger. Recomputed on WM_DISPLAYCHANGE (monitor added, removed
   or rearranged) and every 5 s as a backstop. The cursor is polled with GetCursorPos (works over elevated windows,
   unlike a low-level mouse hook). A mouse click while in the corner (e.g. a maximized window's close button)
   cancels it; after a lock or a cancel the cursor must leave the corner before it can trigger again. */

#define TIMER_CORNER      2
#define CORNER_POLL_MS    100
#define CORNER_DWELL_MS   5000
#define CORNER_RECHECK_MS 5000      /* layout recheck backstop, same cadence as the mouse check */
#define CORNER_ZONE       3         /* px from the corner, both ways */

static BOOL g_hcOn = TRUE;          /* menu toggle, saved in HKCU\Software\JigRack\HotCornerLock */
static BOOL g_hcDry;                /* --selftest: log instead of locking */
static POINT g_corner;
static BOOL g_cornerValid;
static ULONGLONG g_cornerAt;        /* when the corner was last computed */
static ULONGLONG g_cornerSince;     /* when the cursor entered the corner; 0 = not in it */
static BOOL g_cornerSpent;          /* locked or cancelled: wait for the cursor to leave */

static BOOL CALLBACK corner_mon(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)m; (void)dc;
    RECT *best = (RECT *)lp;
    if (best->left > best->right || r->right > best->right || (r->right == best->right && r->top < best->top))
        *best = *r;
    return TRUE;
}

static void find_corner(void)
{
    RECT best = { 1, 0, 0, 0 };     /* left > right marks "none yet" */
    EnumDisplayMonitors(NULL, NULL, corner_mon, (LPARAM)&best);
    POINT old = g_corner;
    BOOL wasValid = g_cornerValid;
    g_cornerValid = best.left <= best.right;
    g_corner.x = best.right - 1;
    g_corner.y = best.top;
    g_cornerAt = GetTickCount64();
    if (g_cornerValid != wasValid || old.x != g_corner.x || old.y != g_corner.y)
        trace("hot corner at %ld,%ld", g_corner.x, g_corner.y);
}

/* One poll. ok = cursor position known (it isn't while the lock screen is up); button = a mouse button went down. */
static BOOL corner_step(POINT p, BOOL ok, BOOL button, ULONGLONG now)
{
    if (!ok) return FALSE;                                   /* lock screen etc.: keep state as it is */
    BOOL in = g_cornerValid && p.x >= g_corner.x - (CORNER_ZONE - 1) && p.x <= g_corner.x &&
              p.y <= g_corner.y + (CORNER_ZONE - 1) && p.y >= g_corner.y;
    if (!in) { g_cornerSince = 0; g_cornerSpent = FALSE; return FALSE; }
    if (g_cornerSpent) return FALSE;
    if (button) { g_cornerSpent = TRUE; g_cornerSince = 0; trace("hot corner: cancelled by a click"); return FALSE; }
    if (!g_cornerSince) { g_cornerSince = now; return FALSE; }
    if (now - g_cornerSince < CORNER_DWELL_MS) return FALSE;
    g_cornerSpent = TRUE;
    g_cornerSince = 0;
    return TRUE;                                             /* lock */
}

/* Countdown overlay: while the cursor rests in the hot corner, a big white number with a black outline counts
   5..1 just below-left of the corner. A layered, click-through, never-activated topmost window, so it can't take
   a click or the focus. Shown from 0.5 s in (a quick trip to a close button doesn't flash it); hidden when the
   cursor leaves, a click cancels, or the PC locks. Redrawn only when the number changes. */
static HWND g_cd;
static int g_cdShown;               /* number on screen; 0 = hidden */

static void countdown_create(HINSTANCE inst)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"JigRackCountdown";
    RegisterClassW(&wc);
    g_cd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
}

static void countdown_show(int n)
{
    if (n == g_cdShown) return;
    g_cdShown = n;
    if (!g_cd) return;
    if (!n) { ShowWindow(g_cd, SW_HIDE); return; }

    UINT dpi = 96, dx = 96, dy = 96;
    HMONITOR mon = MonitorFromPoint(g_corner, MONITOR_DEFAULTTONEAREST);
    if (GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy) == S_OK) dpi = dx;
    int size = MulDiv(76, dpi, 96), margin = MulDiv(14, dpi, 96);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;                       /* top-down */
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
    GdipCreateBitmapFromScan0(size, size, size * 4, 0x000E200B /* PixelFormat32bppPARGB */, (BYTE *)bits, &bmp);
    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, 4);
    GdipSetPixelOffsetMode(g, 4);
    GdipGraphicsClear(g, 0);
    GpFontFamily *fam = NULL;
    GpStringFormat *fmt = NULL;
    GdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fam);
    if (!fam) GdipCreateFontFamilyFromName(L"Arial", NULL, &fam);
    GdipCreateStringFormat(0x1000 /* NoClip */, 0, &fmt);
    GdipSetStringFormatAlign(fmt, 1);
    GdipSetStringFormatLineAlign(fmt, 1);
    wchar_t text[4];
    swprintf_s(text, 4, L"%d", n);
    GpRectF box = { 0, -size * 0.06f, (REAL)size, (REAL)size };
    GpPath *tp = NULL;
    GdipCreatePath(0, &tp);
    GdipAddPathString(tp, text, -1, fam, 1 /* bold */, size * 0.86f, &box, fmt);
    GpPen *outline = NULL;
    GdipCreatePen1(ARGBV(255, 0, 0, 0), size * 0.11f, 0, &outline);
    GdipSetPenLineJoin(outline, 2 /* round */);
    GpBrush *white = NULL;
    GdipCreateSolidFill(ARGBV(255, 255, 255, 255), &white);
    GdipDrawPath(g, outline, tp);
    GdipFillPath(g, white, tp);
    GdipDeleteBrush(white);
    GdipDeletePen(outline);
    GdipDeletePath(tp);
    GdipDeleteStringFormat(fmt);
    if (fam) GdipDeleteFontFamily(fam);
    GdipDeleteGraphics(g);
    GdipDisposeImage(bmp);

    POINT dst = { g_corner.x - margin - size + 1, g_corner.y + margin }, src = { 0, 0 };
    SIZE sz = { size, size };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_cd, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SetWindowPos(g_cd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
}

/* Number to show for the current dwell: 5..1 from 0.5 s in, else 0. */
static int countdown_number(ULONGLONG now)
{
    if (!g_cornerSince || g_cornerSpent) return 0;
    ULONGLONG el = now - g_cornerSince;
    if (el < 500 || el >= CORNER_DWELL_MS) return 0;
    return (int)((CORNER_DWELL_MS - el + 999) / 1000);
}

static void corner_tick(void)
{
    if (GetTickCount64() - g_cornerAt >= CORNER_RECHECK_MS) find_corner();
    POINT p;
    BOOL ok = GetCursorPos(&p);
    BOOL button = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8001) != 0;
    ULONGLONG now = GetTickCount64();
    if (corner_step(p, ok, button, now)) {
        countdown_show(0);
        trace("hot corner: lock");
        if (!g_hcDry) LockWorkStation();
    } else {
        countdown_show(ok ? countdown_number(now) : 0);
    }
}

static void corner_enable(HWND hwnd, BOOL on)
{
    g_hcOn = on;
    g_cornerSince = 0;
    g_cornerSpent = TRUE;                                    /* don't fire if the cursor is already there */
    if (on) { find_corner(); SetTimer(hwnd, TIMER_CORNER, CORNER_POLL_MS, NULL); }
    else { KillTimer(hwnd, TIMER_CORNER); countdown_show(0); }
}

static BOOL load_hc_setting(void)
{
    DWORD v = 1, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\JigRack", L"HotCornerLock", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS)
        v = 1;
    return v != 0;
}

static void save_hc_setting(BOOL on)
{
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\JigRack", L"HotCornerLock", REG_DWORD, &v, sizeof v);
}

/* --selftest: run the corner logic on a scripted cursor timeline against the real monitor layout; results go to
   the log. Never locks. */
static BOOL CALLBACK other_corner(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    (void)m; (void)dc;
    POINT *p = (POINT *)lp;                                  /* p[0] = corner found, p[1].x = 1 once found */
    if (!p[1].x && (r->right - 1 != g_corner.x || r->top != g_corner.y)) {
        p[0].x = r->right - 1; p[0].y = r->top; p[1].x = 1;
    }
    return TRUE;
}

static void corner_selftest(void)
{
    g_hcDry = TRUE;
    find_corner();
    POINT c = g_corner, away = { g_corner.x - 400, g_corner.y + 400 }, nearIn = { g_corner.x - 2, g_corner.y + 2 };
    POINT other[2] = { {0, 0}, {0, 0} };
    EnumDisplayMonitors(NULL, NULL, other_corner, (LPARAM)other);
    POINT inner = other[0];                                  /* another monitor's top-right corner, if any */
    int fails = 0;
#define EXPECT(cond, what) do { BOOL ok_ = (cond); trace("selftest %s: %s", ok_ ? "ok  " : "FAIL", what); if (!ok_) fails++; } while (0)
    ULONGLONG t = 1000000;
    g_cornerSince = 0; g_cornerSpent = FALSE;
    EXPECT(!corner_step(away, TRUE, FALSE, t), "away from the corner: nothing");
    BOOL fired = FALSE;
    for (int ms = 0; ms < 4900; ms += 100) fired |= corner_step(c, TRUE, FALSE, t + ms);
    EXPECT(!fired, "4.9 s in the corner: no lock yet");
    EXPECT(corner_step(c, TRUE, FALSE, t + 5000), "5 s in the corner: lock");
    EXPECT(!corner_step(c, TRUE, FALSE, t + 5100), "still there after the lock: no second lock");
    EXPECT(!corner_step(c, FALSE, FALSE, t + 9000), "lock screen up (no cursor): no change");
    fired = FALSE;
    for (int ms = 0; ms < 20000; ms += 100) fired |= corner_step(c, TRUE, FALSE, t + 10000 + ms);
    EXPECT(!fired, "back from the lock, cursor still in the corner 20 s: no relock");
    EXPECT(!corner_step(away, TRUE, FALSE, t + 31000), "leave the corner");
    corner_step(nearIn, TRUE, FALSE, t + 32000);
    EXPECT(corner_step(nearIn, TRUE, FALSE, t + 37000), "re-enter 2 px from the corner, 5 s: lock again");
    corner_step(away, TRUE, FALSE, t + 38000);
    corner_step(c, TRUE, FALSE, t + 39000);
    corner_step(c, TRUE, TRUE, t + 40000);
    EXPECT(!corner_step(c, TRUE, FALSE, t + 46000), "click in the corner (close button): cancelled");
    corner_step(away, TRUE, FALSE, t + 47000);
    if (other[1].x) {
        fired = FALSE;
        for (int ms = 0; ms <= 10000; ms += 100) fired |= corner_step(inner, TRUE, FALSE, t + 48000 + ms);
        EXPECT(!fired, "another monitor's top-right corner, 10 s: nothing");
    } else trace("selftest skip: only one monitor");
    corner_step(c, TRUE, FALSE, t + 60000);
    corner_step(away, TRUE, FALSE, t + 62000);
    EXPECT(!corner_step(c, TRUE, FALSE, t + 64000), "dwell restarts after leaving (2 s + 2 s): no lock");
#undef EXPECT
    trace("selftest done: %d failure(s)", fails);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated) { add_tray(hwnd); return 0; }  /* Explorer restarted */
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        add_tray(hwnd);
        corner_enable(hwnd, load_hc_setting());
        HANDLE t = CreateThread(NULL, 0, monitor, NULL, 0, NULL);
        if (t) CloseHandle(t);
        return 0;
    }
    case WM_RESULT: {
        Battery *b = (Battery *)lp;
        show(b);
        HeapFree(GetProcessHeap(), 0, b);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh mouse battery");
            AppendMenuW(m, MF_STRING | (g_hcOn ? MF_CHECKED : 0), ID_HOTCORNER, L"Hot corner: top-right 5 s locks");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == ID_REFRESH) SetEvent(g_wake);
            else if (cmd == ID_HOTCORNER) { corner_enable(hwnd, !g_hcOn); save_hc_setting(g_hcOn); }
            else if (cmd == ID_EXIT) DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_CORNER) corner_tick();
        return 0;
    case WM_DISPLAYCHANGE:                                  /* monitor added, removed or rearranged */
        if (g_hcOn) find_corner();
        break;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_CORNER);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_icon) DestroyIcon(g_icon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show_cmd)
{
    (void)prev; (void)cmd; (void)show_cmd;
    {   /* --selftest <logfile>: check the hot-corner logic against the real monitor layout; runs beside the tray app */
        int ac = 0;
        LPWSTR *av = CommandLineToArgvW(GetCommandLineW(), &ac);
        if (av && ac >= 2 && wcscmp(av[1], L"--demo-countdown") == 0) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            GdiplusStartupInput dg = { 1, NULL, FALSE, FALSE };
            if (GdiplusStartup(&g_gdip, &dg, NULL) != 0) return 1;
            find_corner();
            countdown_create(GetModuleHandleW(NULL));
            for (int n = 5; n >= 1; n--) {
                countdown_show(n);
                ULONGLONG until = GetTickCount64() + 1000;
                MSG dm;
                while (GetTickCount64() < until) {
                    while (PeekMessageW(&dm, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&dm);
                    Sleep(20);
                }
            }
            countdown_show(0);
            GdiplusShutdown(g_gdip);
            return 0;
        }
        if (av && ac >= 3 && wcscmp(av[1], L"--selftest") == 0) {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            g_log = CreateFileW(av[2], GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            corner_selftest();
            CloseHandle(g_log);
            return 0;
        }
    }
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\JigRack");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);  /* real icon size, not scaled */
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    if (GdiplusStartup(&g_gdip, &gsi, NULL) != 0) return 1;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3 && wcscmp(argv[1], L"--preview") == 0) {   /* design check: no tray, just PNGs */
        write_previews(argv[2]);
        GdiplusShutdown(g_gdip);
        return 0;
    }
    wchar_t logpath[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, logpath, MAX_PATH);
    while (len && logpath[len - 1] != L'\\') len--;
    logpath[len] = 0;
    wcscat_s(logpath, MAX_PATH, L"jigrack.log");
    g_log = CreateFileW(logpath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    trace("Jig Rack started");
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = L"JigRackTrayWindow";
    RegisterClassW(&wc);
    countdown_create(inst);
    /* A hidden top-level tool window, not a message-only one: only top-level windows receive the TaskbarCreated
       broadcast, which is how the icon comes back after Explorer restarts. Never shown, not on the taskbar. */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Jig Rack", WS_POPUP, 0, 0, 0, 0,
                                NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    GdiplusShutdown(g_gdip);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
