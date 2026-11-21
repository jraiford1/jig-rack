/* Jig Rack -- icons: the battery meter, the Jig Rack icon, previews and the .ico file. GDI+ flat API.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE. */
#include "jigrack.h"
#include "gdip.h"
#include <math.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

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

/* Status colour: blue charging, red low (at or below the "low battery" level, 20% unless set), yellow medium
   (<=50%), green full. */
#define COL_CHARGING ARGBV(255, 10, 132, 255)
#define COL_LOW      ARGBV(255, 255, 59, 48)
#define COL_MED      ARGBV(255, 255, 204, 0)
#define COL_FULL     ARGBV(255, 48, 199, 89)
static ARGB status_color(int percent, BOOL charging)
{
    int low = g_set.lowPercent > 0 ? g_set.lowPercent : 20;
    return charging ? COL_CHARGING : percent <= low ? COL_LOW : percent <= 50 ? COL_MED : COL_FULL;
}

static int tray_size(void)
{
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    return size < 16 ? 16 : size;
}

HICON icon_meter(const Battery *b)
{
    wchar_t text[8];
    ARGB fill = ARGBV(255, 120, 120, 128);
    if (b->ok) {
        swprintf_s(text, 8, L"%d", b->percent);
        fill = status_color(b->percent, b->charging && !b->stale);
    } else {
        wcscpy_s(text, 8, L"?");
    }
    GpBitmap *bmp = render_mouse(tray_size(), b->ok ? b->percent : -1, fill, FALSE, text);
    HICON icon = NULL;
    GdipCreateHICONFromBitmap(bmp, &icon);
    GdipDisposeImage(bmp);
    return icon;
}

/* ---------------------------------------------------------------- the Jig Rack icon ---------------------------- */
/* A workshop pegboard (the rack) with a wrench and a screwdriver crossed on it (the jigs), on a 32x32 grid. */

static void tool_halo_fill(GpGraphics *g, GpPath **parts, int n, GpBrush *fill)
{
    GpPen *halo = NULL;
    GdipCreatePen1(ARGBV(190, 40, 24, 10), 1.7f, 0, &halo);
    GdipSetPenLineJoin(halo, 2);
    for (int i = 0; i < n; i++) GdipDrawPath(g, halo, parts[i]);
    for (int i = 0; i < n; i++) GdipFillPath(g, fill, parts[i]);
    GdipDeletePen(halo);
}

static GpBitmap *render_app(int size)
{
    GpBitmap *bmp = NULL;
    GpGraphics *g = NULL;
    GdipCreateBitmapFromScan0(size, size, 0, PixelFormat32bppARGB, NULL, &bmp);
    GdipGetImageGraphicsContext(bmp, &g);
    GdipSetSmoothingMode(g, 4);
    GdipSetPixelOffsetMode(g, 4);
    GdipGraphicsClear(g, 0);
    GdipScaleWorldTransform(g, size / 32.0f, size / 32.0f, 0);

    /* the board */
    GpPath *board = round_rect(1.0f, 1.0f, 30.0f, 30.0f, 5.5f);
    GpBrush *wood = NULL;
    GpPointF wt = { 0, 1 }, wb = { 0, 31 };
    GdipCreateLineBrush(&wt, &wb, ARGBV(255, 232, 186, 124), ARGBV(255, 186, 128, 70), 0, &wood);
    GpPen *edge = NULL;
    GdipCreatePen1(ARGBV(230, 74, 44, 18), 1.3f, 0, &edge);
    GdipFillPath(g, wood, board);
    GdipDrawPath(g, edge, board);
    GdipDeleteBrush(wood);
    GdipDeletePen(edge);
    GdipDeletePath(board);
    GpBrush *hole = NULL;
    GdipCreateSolidFill(ARGBV(210, 96, 58, 26), &hole);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            GdipFillEllipse(g, hole, 6.1f + i * 6.6f - 1.0f, 6.1f + j * 6.6f - 1.0f, 2.0f, 2.0f);
    GdipDeleteBrush(hole);

    GpBrush *steel = NULL;
    GpPointF st = { 0, -4.5f }, sb = { 0, 4.5f };
    GdipCreateLineBrush(&st, &sb, ARGBV(255, 250, 251, 253), ARGBV(255, 136, 144, 158), 0, &steel);

    /* screwdriver: handle bottom-right, tip top-left (drawn first, under the wrench) */
    UINT state;
    GdipSaveGraphics(g, &state);
    GdipTranslateWorldTransform(g, 16.0f, 16.0f, 0);
    GdipRotateWorldTransform(g, 225.0f, 0);
    {
        GpPath *shaft = NULL, *tip = NULL;
        GdipCreatePath(0, &shaft);
        GdipAddPathRectangle(shaft, -3.0f, -1.0f, 12.0f, 2.0f);
        GdipCreatePath(0, &tip);
        GpPointF tp[] = { {8.6f, -1.0f}, {12.6f, -1.6f}, {12.6f, 1.6f}, {8.6f, 1.0f} };
        GdipAddPathPolygon(tip, tp, 4);
        GpPath *metal[] = { shaft, tip };
        tool_halo_fill(g, metal, 2, steel);
        GpPath *handle = round_rect(-13.4f, -3.0f, 11.4f, 6.0f, 2.6f);
        GpBrush *red = NULL;
        GpPointF ht = { 0, -3.0f }, hb = { 0, 3.0f };
        GdipCreateLineBrush(&ht, &hb, ARGBV(255, 255, 112, 80), ARGBV(255, 186, 36, 20), 0, &red);
        tool_halo_fill(g, &handle, 1, red);
        GdipDeleteBrush(red);
        GdipDeletePath(handle); GdipDeletePath(shaft); GdipDeletePath(tip);
    }
    GdipRestoreGraphics(g, state);

    /* wrench: open end top-right, ring end bottom-left */
    GdipSaveGraphics(g, &state);
    GdipTranslateWorldTransform(g, 16.0f, 16.0f, 0);
    GdipRotateWorldTransform(g, -45.0f, 0);
    {
        GpPath *handle = round_rect(-9.0f, -1.9f, 17.0f, 3.8f, 1.9f);
        GpPath *head = NULL;                                  /* a C: circle with a notch opening outwards */
        GdipCreatePath(0, &head);
        const REAL cx = 9.6f, r = 4.4f, half = 1.6f;
        REAL a = (REAL)(asin(half / r) * 180.0 / 3.14159265);
        GdipAddPathArc(head, cx - r, -r, 2 * r, 2 * r, a, 360.0f - 2 * a);
        GdipAddPathLine(head, cx + r * (REAL)cos(a * 3.14159265 / 180.0), -half, cx + 0.4f, -half);
        GdipAddPathLine(head, cx + 0.4f, -half, cx + 0.4f, half);
        GdipClosePathFigure(head);
        GpPath *ring = NULL;                                  /* ring end: alternate fill makes the hole */
        GdipCreatePath(0, &ring);
        GdipAddPathEllipse(ring, -12.6f, -3.4f, 6.8f, 6.8f);
        GdipAddPathEllipse(ring, -10.6f, -1.4f, 2.8f, 2.8f);
        GpPath *parts[] = { handle, head, ring };
        tool_halo_fill(g, parts, 3, steel);
        GdipDeletePath(handle); GdipDeletePath(head); GdipDeletePath(ring);
    }
    GdipRestoreGraphics(g, state);
    GdipDeleteBrush(steel);
    GdipDeleteGraphics(g);
    return bmp;
}

HICON icon_app(int size)
{
    GpBitmap *bmp = render_app(size > 0 ? size : tray_size());
    HICON icon = NULL;
    GdipCreateHICONFromBitmap(bmp, &icon);
    GdipDisposeImage(bmp);
    return icon;
}

static const CLSID PNG_ENCODER = { 0x557cf406, 0x1a04, 0x11d3, { 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e } };

/* --preview <folder>: PNGs of each state (64 px and the real tray size) for checking the design. */
void icons_write_previews(const wchar_t *dir)
{
    struct { const wchar_t *name; int level; ARGB fill; const wchar_t *label; } s[] = {
        { L"full-82",      82, COL_FULL,     L"82" },
        { L"med-49",       49, COL_MED,      L"49" },
        { L"med-28",       28, COL_MED,      L"28" },
        { L"low-9",         9, COL_LOW,      L"9" },
        { L"charging-63",  63, COL_CHARGING, L"63" },
        { L"hundred-100", 100, COL_FULL,     L"100" },
        { L"unknown",      -1, ARGBV(255, 120, 120, 128), L"?" },
    };
    int sizes[] = { 64, tray_size() };
    wchar_t path[MAX_PATH];
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < (int)(sizeof s / sizeof s[0]); i++) {
            swprintf_s(path, MAX_PATH, L"%s\\%s-%d.png", dir, s[i].name, sizes[k]);
            GpBitmap *bmp = render_mouse(sizes[k], s[i].level, s[i].fill, FALSE, s[i].label);
            GdipSaveImageToFile(bmp, path, &PNG_ENCODER, NULL);
            GdipDisposeImage(bmp);
        }
        swprintf_s(path, MAX_PATH, L"%s\\jigrack-%d.png", dir, sizes[k]);
        GpBitmap *bmp = render_app(sizes[k]);
        GdipSaveImageToFile(bmp, path, &PNG_ENCODER, NULL);
        GdipDisposeImage(bmp);
    }
    int more[] = { 16, 20, 24, 32, 256 };
    for (int k = 0; k < 5; k++) {
        swprintf_s(path, MAX_PATH, L"%s\\jigrack-%d.png", dir, more[k]);
        GpBitmap *bmp = render_app(more[k]);
        GdipSaveImageToFile(bmp, path, &PNG_ENCODER, NULL);
        GdipDisposeImage(bmp);
    }
}

/* --write-ico <file>: the Jig Rack icon as a multi-size .ico (PNG entries), for the exe's icon resource. */
BOOL icons_write_ico(const wchar_t *path)
{
    static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    enum { N = sizeof sizes / sizeof sizes[0] };
    BYTE *data[N] = {0};
    DWORD len[N] = {0};
    BOOL ok = TRUE;
    for (int i = 0; i < N && ok; i++) {
        GpBitmap *bmp = render_app(sizes[i]);
        IStream *s = NULL;
        ok = CreateStreamOnHGlobal(NULL, TRUE, &s) == S_OK && GdipSaveImageToStream(bmp, s, &PNG_ENCODER, NULL) == 0;
        if (ok) {
            HGLOBAL hg = NULL;
            STATSTG ss;
            ok = s->lpVtbl->Stat(s, &ss, STATFLAG_NONAME) == S_OK && GetHGlobalFromStream(s, &hg) == S_OK;
            if (ok) {
                len[i] = ss.cbSize.LowPart;
                data[i] = (BYTE *)HeapAlloc(GetProcessHeap(), 0, len[i]);
                void *p = GlobalLock(hg);
                if (data[i] && p) memcpy(data[i], p, len[i]); else ok = FALSE;
                GlobalUnlock(hg);
            }
        }
        if (s) s->lpVtbl->Release(s);
        GdipDisposeImage(bmp);
    }
    if (ok) {
        HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            DWORD w, offset = 6 + 16 * N;
            WORD head[3] = { 0, 1, N };
            WriteFile(h, head, sizeof head, &w, NULL);
            for (int i = 0; i < N; i++) {
                BYTE e[16] = {0};
                e[0] = e[1] = (BYTE)(sizes[i] >= 256 ? 0 : sizes[i]);
                e[4] = 1; e[6] = 32;
                memcpy(e + 8, &len[i], 4);
                memcpy(e + 12, &offset, 4);
                WriteFile(h, e, 16, &w, NULL);
                offset += len[i];
            }
            for (int i = 0; i < N; i++) WriteFile(h, data[i], len[i], &w, NULL);
            CloseHandle(h);
        }
    }
    for (int i = 0; i < N; i++) if (data[i]) HeapFree(GetProcessHeap(), 0, data[i]);
    return ok;
}

static ULONG_PTR g_gdip;
ULONG_PTR icons_startup(void)
{
    GdiplusStartupInput in = { 1, NULL, FALSE, FALSE };
    if (!g_gdip && GdiplusStartup(&g_gdip, &in, NULL) != 0) g_gdip = 0;
    return g_gdip;
}
