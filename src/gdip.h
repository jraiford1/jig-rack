/* Jig Rack -- the GDI+ flat API, declared for C (gdiplus.h is C++ only).
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE. */
#pragma once
#include <windows.h>
#include <objidl.h>

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
int GDIP GdipCreateBitmapFromHBITMAP(HBITMAP hbm, HPALETTE pal, GpBitmap **b);
int GDIP GdipGetImageGraphicsContext(GpImage *img, GpGraphics **g);
int GDIP GdipSetSmoothingMode(GpGraphics *g, INT mode);
int GDIP GdipSetPixelOffsetMode(GpGraphics *g, INT mode);
int GDIP GdipGraphicsClear(GpGraphics *g, ARGB c);
int GDIP GdipScaleWorldTransform(GpGraphics *g, REAL sx, REAL sy, INT order);
int GDIP GdipTranslateWorldTransform(GpGraphics *g, REAL dx, REAL dy, INT order);
int GDIP GdipRotateWorldTransform(GpGraphics *g, REAL angle, INT order);
int GDIP GdipSaveGraphics(GpGraphics *g, UINT *state);
int GDIP GdipRestoreGraphics(GpGraphics *g, UINT state);
int GDIP GdipCreatePath(INT fillMode, GpPath **p);
int GDIP GdipAddPathArc(GpPath *p, REAL x, REAL y, REAL w, REAL h, REAL start, REAL sweep);
int GDIP GdipAddPathLine(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2);
int GDIP GdipAddPathPolygon(GpPath *p, const GpPointF *pts, INT n);
int GDIP GdipAddPathEllipse(GpPath *p, REAL x, REAL y, REAL w, REAL h);
int GDIP GdipAddPathRectangle(GpPath *p, REAL x, REAL y, REAL w, REAL h);
int GDIP GdipAddPathBezier(GpPath *p, REAL x1, REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4);
int GDIP GdipStartPathFigure(GpPath *p);
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
int GDIP GdipFillEllipse(GpGraphics *g, GpBrush *b, REAL x, REAL y, REAL w, REAL h);
int GDIP GdipCreateFontFamilyFromName(const WCHAR *name, void *coll, GpFontFamily **f);
int GDIP GdipDeleteFontFamily(GpFontFamily *f);
int GDIP GdipCreateStringFormat(INT flags, LANGID lang, GpStringFormat **f);
int GDIP GdipSetStringFormatAlign(GpStringFormat *f, INT align);
int GDIP GdipSetStringFormatLineAlign(GpStringFormat *f, INT align);
int GDIP GdipDeleteStringFormat(GpStringFormat *f);
int GDIP GdipCreateHICONFromBitmap(GpBitmap *b, HICON *icon);
int GDIP GdipDeleteGraphics(GpGraphics *g);
int GDIP GdipDisposeImage(GpImage *img);
int GDIP GdipSaveImageToFile(GpImage *img, const WCHAR *file, const CLSID *encoder, const void *params);
int GDIP GdipSaveImageToStream(GpImage *img, IStream *stream, const CLSID *encoder, const void *params);

#define ARGBV(a, r, g, b) (((ARGB)(a) << 24) | ((ARGB)(r) << 16) | ((ARGB)(g) << 8) | (ARGB)(b))
#define PixelFormat32bppARGB  0x26200A
#define PixelFormat32bppPARGB 0x0E200B
