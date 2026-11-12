// Copyright 2026 Phinomenon Inc.
// Licensed under the Apache License, Version 2.0; see the LICENSE file.
//
// PhiSetupUI - the NSIS plugin behind the installer's Windows-native look
// (docs/windows-native-ui.md §12, decision D-I1). Win32 cannot paint a
// Fluent accent button, an expander card, a 4 dip progress bar or an
// InfoBar, so this DLL owner-draws them; everything else in the wizard is
// a stock control whose font and colours it sets. One x86-unicode plugin
// serves both the installer and the uninstaller.
//
// Calls (NSIS pushes parameters in order, so the first is popped first):
//   PhiSetupUI::Init                      -> pushes "1" when the Windows app
//                                            mode is dark, else "0"; reads
//                                            the DPI; subclasses $HWNDPARENT
//   PhiSetupUI::Frame                     -> hides the wizard chrome, sizes
//                                            the window to 600x420 dip, lays
//                                            out the footer
//   PhiSetupUI::Dpi                       -> pushes the DPI
//   PhiSetupUI::Create parent kind x y w h text -> pushes the new HWND
//   PhiSetupUI::Style hwnd kind [val]     -> a control nsDialogs created
//   PhiSetupUI::Set hwnd val              -> toggle / check / progress value
//   PhiSetupUI::Text hwnd text            -> sets text and repaints
//   PhiSetupUI::Place hwnd x y w h        -> moves (dip)
//   PhiSetupUI::Show hwnd 0|1
//   PhiSetupUI::Buttons kind primary cancel -> the footer buttons: kind is
//                                            pri | dng | none; an empty
//                                            text hides that button
//   PhiSetupUI::Footer text               -> the footer's left text
//   PhiSetupUI::Progress hwnd label       -> takes over NSIS's progress bar
//   PhiSetupUI::Page hwnd                 -> a built-in NSIS page (instfiles):
//                                            sized to the page area, stray
//                                            template controls hidden
//   PhiSetupUI::Caption text              -> the window title (NSIS appends
//                                            ": Installing" on its own)
//
// Kinds: text.title text.sub text.lead text.label text.hint text.strong
//        text.warn text.warnb text.link text.footer (suffix "/card" or
//        "/warn" for the background they sit on), field (a painted box),
//        edit, mark, card, chev, toggle, check, progress, infobar.warn,
//        okcircle, blockcircle, glyph:XXXX (hex code point), sep,
//        btn.pri btn.sec btn.dng btn.sm (suffix "/card" as above).
//
// Build: installer/ui/build-plugin.sh (clang-cl + lld-link from the
// Chromium toolchain, /MACHINE:X86, static CRT).

#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------- GDI+ flat API
typedef int GpStatus;
typedef void GpGraphics, GpPath, GpBrush, GpSolidFill, GpPen;
typedef struct {
  UINT32 GdiplusVersion;
  void* DebugEventCallback;
  BOOL SuppressBackgroundThread;
  BOOL SuppressExternalCodecs;
} GdiplusStartupInput;
GpStatus __stdcall GdiplusStartup(ULONG_PTR* token, const GdiplusStartupInput* in, void* out);
GpStatus __stdcall GdipCreateFromHDC(HDC hdc, GpGraphics** g);
GpStatus __stdcall GdipDeleteGraphics(GpGraphics* g);
GpStatus __stdcall GdipSetSmoothingMode(GpGraphics* g, int mode);
GpStatus __stdcall GdipSetPixelOffsetMode(GpGraphics* g, int mode);
GpStatus __stdcall GdipCreatePath(int fill_mode, GpPath** path);
GpStatus __stdcall GdipDeletePath(GpPath* path);
GpStatus __stdcall GdipAddPathArc(GpPath* path, float x, float y, float w, float h, float start, float sweep);
GpStatus __stdcall GdipClosePathFigure(GpPath* path);
GpStatus __stdcall GdipCreateSolidFill(DWORD argb, GpSolidFill** brush);
GpStatus __stdcall GdipDeleteBrush(GpBrush* brush);
GpStatus __stdcall GdipFillPath(GpGraphics* g, GpBrush* brush, GpPath* path);
GpStatus __stdcall GdipFillEllipse(GpGraphics* g, GpBrush* brush, float x, float y, float w, float h);
GpStatus __stdcall GdipCreatePen1(DWORD argb, float width, int unit, GpPen** pen);
GpStatus __stdcall GdipDeletePen(GpPen* pen);
GpStatus __stdcall GdipDrawPath(GpGraphics* g, GpPen* pen, GpPath* path);
GpStatus __stdcall GdipDrawEllipse(GpGraphics* g, GpPen* pen, float x, float y, float w, float h);
GpStatus __stdcall GdipDrawLine(GpGraphics* g, GpPen* pen, float x1, float y1, float x2, float y2);
GpStatus __stdcall GdipSetPenLineCap197819(GpPen* pen, int start, int end, int dash);
GpStatus __stdcall GdipSetPenLineJoin(GpPen* pen, int join);
enum { kSmoothingAntiAlias = 4, kPixelOffsetHalf = 4, kUnitPixel = 2, kLineCapRound = 2, kLineJoinRound = 2 };

// ---------------------------------------------------------------- NSIS plugin API
typedef struct _stack_t {
  struct _stack_t* next;
  wchar_t text[1];
} stack_t;

// NSIS unloads a plugin after every call unless it registers a callback;
// this DLL subclasses windows, so it must stay loaded for the whole run.
typedef UINT_PTR (*NsisPluginCallback)(int msg);
typedef struct {
  void* exec_flags;
  int(__stdcall* ExecuteCodeSegment)(int, HWND);
  void(__stdcall* validate_filename)(wchar_t*);
  int(__stdcall* RegisterPluginCallback)(HMODULE, NsisPluginCallback);
} extra_parameters;

static int g_string_size;
static stack_t** g_stacktop;
static HINSTANCE g_hinst;
static int g_registered;

static UINT_PTR PluginCallback(int msg) {
  (void)msg;
  return 0;
}

static void KeepLoaded(extra_parameters* extra) {
  if (!g_registered && extra && extra->RegisterPluginCallback) {
    extra->RegisterPluginCallback(g_hinst, PluginCallback);
    g_registered = 1;
  }
}

#define PLUGIN(name) \
  __declspec(dllexport) void __cdecl name(HWND hwndParent, int string_size, wchar_t* variables, \
                                          stack_t** stacktop, extra_parameters* extra)
#define PLUGIN_INIT()                     \
  do {                                    \
    g_string_size = string_size;          \
    (void)variables;                      \
    g_stacktop = stacktop;                \
    KeepLoaded(extra);                    \
  } while (0)

static int PopString(wchar_t* out, int cap) {
  stack_t* th;
  if (!g_stacktop || !*g_stacktop) {
    out[0] = 0;
    return 0;
  }
  th = *g_stacktop;
  lstrcpynW(out, th->text, cap);
  *g_stacktop = th->next;
  GlobalFree((HGLOBAL)th);
  return 1;
}

static void PushString(const wchar_t* str) {
  stack_t* th;
  if (!g_stacktop)
    return;
  th = (stack_t*)GlobalAlloc(GPTR, sizeof(stack_t) + (size_t)g_string_size * sizeof(wchar_t));
  lstrcpynW(th->text, str, g_string_size);
  th->next = *g_stacktop;
  *g_stacktop = th;
}

static int PopInt(void) {
  wchar_t buf[64];
  PopString(buf, 64);
  return _wtoi(buf);
}

static HWND PopHwnd(void) {
  wchar_t buf[64];
  PopString(buf, 64);
  return (HWND)(INT_PTR)_wtoi64(buf);
}

static void PushInt(INT_PTR v) {
  wchar_t buf[32];
  _i64tow_s((__int64)v, buf, 32, 10);
  PushString(buf);
}

// ---------------------------------------------------------------- palette
typedef struct {
  COLORREF ground, card, fill, fill_hover, field, ink, label, dim, tert;
  COLORREF stroke, stroke_strong, sep, accent, accent_hover, accent_press, accent_ink;
  COLORREF danger, danger_hover, danger_ink, dis_fill, dis_ink, track;
  COLORREF ok, ok_soft, warn_bg, warn_ink, warn_border, hover, chev_fill, link;
} Palette;

static Palette g_pal;
static int g_dark;
static int g_dpi = 96;
static HWND g_parent;
static ULONG_PTR g_gdiplus;
static wchar_t g_caption[256];  // pinned by Caption; NSIS's ": Installing" suffixes are dropped
static const wchar_t* g_icon_face = L"Segoe Fluent Icons";
static const wchar_t* g_text_face = L"Segoe UI Variable Text";
static const wchar_t* g_display_face = L"Segoe UI Variable Display";

static void SetPalette(int dark) {
  if (!dark) {
    g_pal.ground = RGB(0xF3, 0xF3, 0xF3);
    g_pal.card = RGB(0xFF, 0xFF, 0xFF);
    g_pal.fill = RGB(0xFB, 0xFB, 0xFB);
    g_pal.fill_hover = RGB(0xF3, 0xF3, 0xF3);
    g_pal.field = RGB(0xFF, 0xFF, 0xFF);
    g_pal.ink = RGB(0x1A, 0x1A, 0x1A);
    g_pal.label = RGB(0x1B, 0x1B, 0x1B);
    g_pal.dim = RGB(0x5D, 0x5D, 0x5D);
    g_pal.tert = RGB(0x8A, 0x8A, 0x8A);
    g_pal.stroke = RGB(0xE3, 0xE3, 0xE3);
    g_pal.stroke_strong = RGB(0xD3, 0xD3, 0xD3);
    g_pal.sep = RGB(0xE2, 0xE2, 0xE2);
    g_pal.accent = RGB(0x2A, 0x86, 0xB5);
    g_pal.accent_hover = RGB(0x26, 0x7A, 0xA6);
    g_pal.accent_press = RGB(0x22, 0x6E, 0x96);
    g_pal.accent_ink = RGB(0xFF, 0xFF, 0xFF);
    g_pal.danger = RGB(0xC4, 0x2B, 0x1C);
    g_pal.danger_hover = RGB(0xB2, 0x27, 0x19);
    g_pal.danger_ink = RGB(0xFF, 0xFF, 0xFF);
    g_pal.dis_fill = RGB(0xE4, 0xE4, 0xE4);
    g_pal.dis_ink = RGB(0x9B, 0x9B, 0x9B);
    g_pal.track = RGB(0xD6, 0xD6, 0xD6);
    g_pal.ok = RGB(0x0F, 0x7B, 0x0F);
    g_pal.ok_soft = RGB(0xE2, 0xEF, 0xE2);
    g_pal.warn_bg = RGB(0xFF, 0xF4, 0xCE);
    g_pal.warn_ink = RGB(0x9D, 0x5D, 0x00);
    g_pal.warn_border = RGB(0xED, 0xE2, 0xBC);
    g_pal.hover = RGB(0xE9, 0xE9, 0xE9);
    g_pal.chev_fill = RGB(0xF2, 0xF2, 0xF2);
    g_pal.link = RGB(0x2A, 0x86, 0xB5);
  } else {
    g_pal.ground = RGB(0x20, 0x20, 0x20);
    g_pal.card = RGB(0x2B, 0x2B, 0x2B);
    g_pal.fill = RGB(0x33, 0x33, 0x33);
    g_pal.fill_hover = RGB(0x3A, 0x3A, 0x3A);
    g_pal.field = RGB(0x33, 0x33, 0x33);
    g_pal.ink = RGB(0xFF, 0xFF, 0xFF);
    g_pal.label = RGB(0xE8, 0xE8, 0xE8);
    g_pal.dim = RGB(0xA1, 0xA1, 0xA1);
    g_pal.tert = RGB(0x73, 0x73, 0x73);
    g_pal.stroke = RGB(0x3C, 0x3C, 0x3C);
    g_pal.stroke_strong = RGB(0x50, 0x50, 0x50);
    g_pal.sep = RGB(0x2F, 0x2F, 0x2F);
    g_pal.accent = RGB(0x5F, 0xB3, 0xDE);
    g_pal.accent_hover = RGB(0x6D, 0xBB, 0xE2);
    g_pal.accent_press = RGB(0x54, 0xA3, 0xCC);
    g_pal.accent_ink = RGB(0x0B, 0x1A, 0x22);
    g_pal.danger = RGB(0xE0, 0x46, 0x3A);
    g_pal.danger_hover = RGB(0xE6, 0x5A, 0x4F);
    g_pal.danger_ink = RGB(0xFF, 0xFF, 0xFF);
    g_pal.dis_fill = RGB(0x32, 0x32, 0x32);
    g_pal.dis_ink = RGB(0x6E, 0x6E, 0x6E);
    g_pal.track = RGB(0x44, 0x44, 0x44);
    g_pal.ok = RGB(0x6C, 0xCB, 0x5F);
    g_pal.ok_soft = RGB(0x35, 0x45, 0x33);
    g_pal.warn_bg = RGB(0x43, 0x35, 0x19);
    g_pal.warn_ink = RGB(0xFC, 0xE1, 0x00);
    g_pal.warn_border = RGB(0x4C, 0x3E, 0x22);
    g_pal.hover = RGB(0x3A, 0x3A, 0x3A);
    g_pal.chev_fill = RGB(0x38, 0x38, 0x38);
    g_pal.link = RGB(0x5F, 0xB3, 0xDE);
  }
}

static DWORD Argb(COLORREF c, BYTE a) {
  return ((DWORD)a << 24) | ((DWORD)GetRValue(c) << 16) | ((DWORD)GetGValue(c) << 8) | GetBValue(c);
}

static int Px(int dip) {
  return MulDiv(dip, g_dpi, 96);
}

// ---------------------------------------------------------------- fonts
enum { F_BODY, F_BODY_STRONG, F_CAPTION, F_CAPTION_STRONG, F_TITLE, F_SUBTITLE, F_COUNT };
static HFONT g_fonts[F_COUNT];
static HFONT g_icon_fonts[8];
static int g_icon_sizes[8];

static int CALLBACK FaceExists(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp) {
  (void)lf;
  (void)tm;
  (void)type;
  *(int*)lp = 1;
  return 0;
}

static int HasFace(const wchar_t* face) {
  LOGFONTW lf;
  int found = 0;
  HDC dc = GetDC(NULL);
  ZeroMemory(&lf, sizeof(lf));
  lf.lfCharSet = DEFAULT_CHARSET;
  lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
  EnumFontFamiliesExW(dc, &lf, FaceExists, (LPARAM)&found, 0);
  ReleaseDC(NULL, dc);
  return found;
}

static HFONT MakeFont(const wchar_t* face, int dip, int weight) {
  return CreateFontW(-Px(dip), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void InitFonts(void) {
  if (g_fonts[F_BODY])
    return;
  if (!HasFace(g_text_face))
    g_text_face = L"Segoe UI";
  if (!HasFace(g_display_face))
    g_display_face = L"Segoe UI";
  if (!HasFace(g_icon_face))
    g_icon_face = L"Segoe MDL2 Assets";
  g_fonts[F_BODY] = MakeFont(g_text_face, 14, FW_NORMAL);
  g_fonts[F_BODY_STRONG] = MakeFont(g_text_face, 14, FW_SEMIBOLD);
  g_fonts[F_CAPTION] = MakeFont(g_text_face, 12, FW_NORMAL);
  g_fonts[F_CAPTION_STRONG] = MakeFont(g_text_face, 12, FW_SEMIBOLD);
  g_fonts[F_TITLE] = MakeFont(g_display_face, 28, FW_SEMIBOLD);
  g_fonts[F_SUBTITLE] = MakeFont(g_display_face, 20, FW_SEMIBOLD);
}

static HFONT IconFont(int dip) {
  int i;
  for (i = 0; i < 8; ++i) {
    if (g_icon_sizes[i] == dip)
      return g_icon_fonts[i];
    if (!g_icon_fonts[i]) {
      g_icon_sizes[i] = dip;
      g_icon_fonts[i] = MakeFont(g_icon_face, dip, FW_NORMAL);
      return g_icon_fonts[i];
    }
  }
  return g_icon_fonts[0];
}

// ---------------------------------------------------------------- control registry
enum Kind {
  K_NONE,
  K_TEXT,      // val = font id, extra = colour
  K_FIELD,     // painted field box
  K_EDIT,      // stock edit inside a field
  K_MARK,      // the app icon at 48 dip
  K_CARD,
  K_CHEV,      // val 1 = expanded
  K_TOGGLE,    // val 1 = on
  K_CHECK,     // val 1 = checked
  K_PROGRESS,  // val = percent
  K_INFOBAR,
  K_OKCIRCLE,
  K_BLOCKCIRCLE,
  K_GLYPH,     // val = code point
  K_SEP,
  K_BTN_PRI,
  K_BTN_SEC,
  K_BTN_DNG,
  K_BTN_SM,
  K_LINK,
};

enum Bg { BG_GROUND, BG_CARD, BG_WARN };

typedef struct {
  HWND h;
  int kind;
  int val;
  int bg;
  int font;
  COLORREF color;
  int hover;
  WNDPROC old;
} Ctl;

#define MAX_CTLS 96
static Ctl g_ctls[MAX_CTLS];
static int g_nctls;

typedef struct {
  HWND h;
  WNDPROC old;
} Sub;
#define MAX_SUBS 8
static Sub g_subs[MAX_SUBS];
static int g_nsubs;

static Ctl* FindCtl(HWND h) {
  int i;
  for (i = 0; i < g_nctls; ++i)
    if (g_ctls[i].h == h)
      return &g_ctls[i];
  return NULL;
}

static LRESULT CALLBACK ParentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

static void SubclassParent(HWND parent) {
  int i;
  for (i = 0; i < g_nsubs; ++i)
    if (g_subs[i].h == parent)
      return;
  if (g_nsubs >= MAX_SUBS)
    return;
  g_subs[g_nsubs].h = parent;
  g_subs[g_nsubs].old = (WNDPROC)SetWindowLongPtrW(parent, GWLP_WNDPROC, (LONG_PTR)ParentProc);
  ++g_nsubs;
}

static Ctl* Register(HWND h, int kind, int val, int bg) {
  Ctl* c = FindCtl(h);
  if (!c) {
    if (g_nctls >= MAX_CTLS)
      return NULL;
    c = &g_ctls[g_nctls++];
    ZeroMemory(c, sizeof(*c));
    c->h = h;
  }
  c->kind = kind;
  c->val = val;
  c->bg = bg;
  SubclassParent(GetParent(h));
  return c;
}

static void ForgetChildrenOf(HWND parent) {
  int i = 0;
  while (i < g_nctls) {
    if (GetParent(g_ctls[i].h) == parent || !IsWindow(g_ctls[i].h)) {
      g_ctls[i] = g_ctls[g_nctls - 1];
      --g_nctls;
    } else {
      ++i;
    }
  }
}

static COLORREF BgColor(int bg) {
  return bg == BG_CARD ? g_pal.card : bg == BG_WARN ? g_pal.warn_bg : g_pal.ground;
}

// ---------------------------------------------------------------- drawing
static void AddRoundRect(GpPath* path, float x, float y, float w, float h, float r) {
  float d = r * 2;
  if (d > w)
    d = w;
  if (d > h)
    d = h;
  GdipAddPathArc(path, x, y, d, d, 180, 90);
  GdipAddPathArc(path, x + w - d, y, d, d, 270, 90);
  GdipAddPathArc(path, x + w - d, y + h - d, d, d, 0, 90);
  GdipAddPathArc(path, x, y + h - d, d, d, 90, 90);
  GdipClosePathFigure(path);
}

// A rounded rectangle: solid fill plus, when stroke is not CLR_INVALID, a
// one-pixel stroke inside the rect.
static void RoundRectAA(HDC dc, const RECT* r, int radius_px, COLORREF fill, COLORREF stroke) {
  GpGraphics* g = NULL;
  GpPath* path = NULL;
  float x = (float)r->left, y = (float)r->top, w = (float)(r->right - r->left), h = (float)(r->bottom - r->top);
  if (GdipCreateFromHDC(dc, &g) != 0 || !g)
    return;
  GdipSetSmoothingMode(g, kSmoothingAntiAlias);
  GdipSetPixelOffsetMode(g, kPixelOffsetHalf);
  if (fill != CLR_INVALID) {
    GpSolidFill* brush = NULL;
    GdipCreatePath(0, &path);
    AddRoundRect(path, x, y, w, h, (float)radius_px);
    GdipCreateSolidFill(Argb(fill, 255), &brush);
    GdipFillPath(g, brush, path);
    GdipDeleteBrush(brush);
    GdipDeletePath(path);
  }
  if (stroke != CLR_INVALID) {
    GpPen* pen = NULL;
    GdipCreatePath(0, &path);
    AddRoundRect(path, x + 0.5f, y + 0.5f, w - 1, h - 1, (float)radius_px - 0.5f);
    GdipCreatePen1(Argb(stroke, 255), 1.0f, kUnitPixel, &pen);
    GdipDrawPath(g, pen, path);
    GdipDeletePen(pen);
    GdipDeletePath(path);
  }
  GdipDeleteGraphics(g);
}

static void EllipseAA(HDC dc, const RECT* r, COLORREF fill, COLORREF stroke) {
  GpGraphics* g = NULL;
  float x = (float)r->left, y = (float)r->top, w = (float)(r->right - r->left), h = (float)(r->bottom - r->top);
  if (GdipCreateFromHDC(dc, &g) != 0 || !g)
    return;
  GdipSetSmoothingMode(g, kSmoothingAntiAlias);
  GdipSetPixelOffsetMode(g, kPixelOffsetHalf);
  if (fill != CLR_INVALID) {
    GpSolidFill* brush = NULL;
    GdipCreateSolidFill(Argb(fill, 255), &brush);
    GdipFillEllipse(g, brush, x, y, w, h);
    GdipDeleteBrush(brush);
  }
  if (stroke != CLR_INVALID) {
    GpPen* pen = NULL;
    GdipCreatePen1(Argb(stroke, 255), 1.0f, kUnitPixel, &pen);
    GdipDrawEllipse(g, pen, x + 0.5f, y + 0.5f, w - 1, h - 1);
    GdipDeletePen(pen);
  }
  GdipDeleteGraphics(g);
}

// A Fluent check mark drawn as a stroked polyline (crisper than the glyph
// at 12-16 px).
static void CheckMarkAA(HDC dc, const RECT* r, COLORREF color, float width) {
  GpGraphics* g = NULL;
  GpPen* pen = NULL;
  float cx = (float)(r->left + r->right) / 2, cy = (float)(r->top + r->bottom) / 2;
  float s = (float)(r->right - r->left) / 2;
  if (GdipCreateFromHDC(dc, &g) != 0 || !g)
    return;
  GdipSetSmoothingMode(g, kSmoothingAntiAlias);
  GdipCreatePen1(Argb(color, 255), width, kUnitPixel, &pen);
  GdipSetPenLineCap197819(pen, kLineCapRound, kLineCapRound, 0);
  GdipSetPenLineJoin(pen, kLineJoinRound);
  GdipDrawLine(g, pen, cx - s * 0.55f, cy + s * 0.05f, cx - s * 0.15f, cy + s * 0.45f);
  GdipDrawLine(g, pen, cx - s * 0.15f, cy + s * 0.45f, cx + s * 0.6f, cy - s * 0.4f);
  GdipDeletePen(pen);
  GdipDeleteGraphics(g);
}

static void FillSolid(HDC dc, const RECT* r, COLORREF c) {
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, r, b);
  DeleteObject(b);
}

static void DrawGlyph(HDC dc, const RECT* r, int code, int dip, COLORREF color) {
  wchar_t s[2];
  HFONT old;
  RECT rc = *r;
  s[0] = (wchar_t)code;
  s[1] = 0;
  old = (HFONT)SelectObject(dc, IconFont(dip));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  DrawTextW(dc, s, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old);
}

static HICON g_mark;
static int g_mark_px;

static HICON MarkIcon(int px) {
  if (g_mark && g_mark_px == px)
    return g_mark;
  if (g_mark)
    DestroyIcon(g_mark);
  // 103 is the installer's own icon resource (NSIS's IDI_ICON2).
  g_mark = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(103), IMAGE_ICON, px, px, 0);
  if (!g_mark)
    g_mark = (HICON)SendMessageW(g_parent, WM_GETICON, ICON_BIG, 0);
  g_mark_px = px;
  return g_mark;
}

// A focus ring only after the keyboard was used, as WinUI does; the dialog
// manager's own hide-focus state is not reliable for owner-drawn children.
static int g_keyboard;

static int FocusVisible(HWND h) {
  return g_keyboard && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

static void PaintButton(Ctl* c, const DRAWITEMSTRUCT* di) {
  HDC dc = di->hDC;
  RECT r = di->rcItem;
  COLORREF fill, ink, stroke;
  int disabled = (di->itemState & ODS_DISABLED) != 0;
  int pressed = (di->itemState & ODS_SELECTED) != 0;
  int focused = (di->itemState & ODS_FOCUS) && FocusVisible(c->h);
  wchar_t text[128];
  HFONT old;
  FillSolid(dc, &r, BgColor(c->bg));
  if (c->kind == K_LINK) {
    GetWindowTextW(c->h, text, 128);
    old = (HFONT)SelectObject(dc, g_fonts[F_CAPTION]);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_pal.link);
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (c->hover) {
      SIZE sz;
      RECT u;
      GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
      u.left = r.left;
      u.right = r.left + sz.cx;
      u.bottom = (r.top + r.bottom) / 2 + sz.cy / 2;
      u.top = u.bottom - 1;
      FillSolid(dc, &u, g_pal.link);
    }
    SelectObject(dc, old);
    return;
  }
  if (disabled) {
    fill = g_pal.dis_fill;
    ink = g_pal.dis_ink;
    stroke = CLR_INVALID;
  } else if (c->kind == K_BTN_PRI) {
    fill = pressed ? g_pal.accent_press : c->hover ? g_pal.accent_hover : g_pal.accent;
    ink = g_pal.accent_ink;
    stroke = g_dark ? CLR_INVALID : RGB(0x1F, 0x6E, 0x96);
  } else if (c->kind == K_BTN_DNG) {
    fill = pressed || c->hover ? g_pal.danger_hover : g_pal.danger;
    ink = g_pal.danger_ink;
    stroke = g_dark ? CLR_INVALID : RGB(0xA6, 0x24, 0x17);
  } else {
    fill = pressed ? g_pal.hover : c->hover ? g_pal.fill_hover : g_pal.fill;
    ink = pressed ? g_pal.dim : g_pal.label;
    stroke = g_pal.stroke_strong;
  }
  RoundRectAA(dc, &r, Px(4), fill, stroke);
  if (focused) {
    RECT f = r;
    InflateRect(&f, -Px(2), -Px(2));
    RoundRectAA(dc, &f, Px(3), CLR_INVALID, ink);
  }
  GetWindowTextW(c->h, text, 128);
  old = (HFONT)SelectObject(dc, g_fonts[c->kind == K_BTN_SM ? F_CAPTION : F_BODY]);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, ink);
  DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old);
}

static void PaintStatic(Ctl* c, const DRAWITEMSTRUCT* di) {
  HDC dc = di->hDC;
  RECT r = di->rcItem;
  FillSolid(dc, &r, BgColor(c->bg));
  switch (c->kind) {
    case K_MARK: {
      int px = r.right - r.left;
      HICON icon = MarkIcon(px);
      if (icon)
        DrawIconEx(dc, r.left, r.top, icon, px, px, 0, NULL, DI_NORMAL);
      break;
    }
    case K_CARD:
      RoundRectAA(dc, &r, Px(4), g_pal.card, g_pal.stroke);
      break;
    case K_FIELD:
      RoundRectAA(dc, &r, Px(4), g_pal.field, g_pal.stroke_strong);
      break;
    case K_INFOBAR:
      RoundRectAA(dc, &r, Px(4), g_pal.warn_bg, g_pal.warn_border);
      break;
    case K_OKCIRCLE: {
      RECT m = r;
      EllipseAA(dc, &r, g_pal.ok_soft, CLR_INVALID);
      InflateRect(&m, -Px(11), -Px(11));
      CheckMarkAA(dc, &m, g_pal.ok, (float)Px(2));
      break;
    }
    case K_BLOCKCIRCLE:
      EllipseAA(dc, &r, g_pal.dis_fill, CLR_INVALID);
      DrawGlyph(dc, &r, 0xE733, 20, g_pal.label);
      break;
    case K_GLYPH:
      DrawGlyph(dc, &r, c->val, 16, c->color ? c->color : g_pal.label);
      break;
    case K_SEP:
      FillSolid(dc, &r, g_pal.sep);
      break;
    case K_PROGRESS: {
      RECT f = r;
      RoundRectAA(dc, &r, Px(2), g_pal.track, CLR_INVALID);
      f.right = r.left + MulDiv(r.right - r.left, c->val, 100);
      if (f.right > r.left)
        RoundRectAA(dc, &f, Px(2), g_pal.accent, CLR_INVALID);
      break;
    }
    default:
      break;
  }
}

static void PaintControlButton(Ctl* c, const DRAWITEMSTRUCT* di) {
  HDC dc = di->hDC;
  RECT r = di->rcItem;
  int pressed = (di->itemState & ODS_SELECTED) != 0;
  int focused = (di->itemState & ODS_FOCUS) && FocusVisible(c->h);
  FillSolid(dc, &r, BgColor(c->bg));
  switch (c->kind) {
    case K_CHEV:
      if (c->hover || pressed)
        RoundRectAA(dc, &r, Px(4), pressed ? g_pal.hover : g_pal.chev_fill, CLR_INVALID);
      DrawGlyph(dc, &r, c->val ? 0xE70E : 0xE70D, 12, g_pal.label);
      break;
    case K_TOGGLE: {
      RECT t = r, knob;
      int h = Px(20), w = Px(40), k = Px(12);
      t.top = (r.top + r.bottom - h) / 2;
      t.bottom = t.top + h;
      t.right = t.left + w;
      if (c->val) {
        RoundRectAA(dc, &t, h / 2, c->hover ? g_pal.accent_hover : g_pal.accent, CLR_INVALID);
        knob.right = t.right - Px(4);
        knob.left = knob.right - k;
      } else {
        RoundRectAA(dc, &t, h / 2, c->hover ? g_pal.hover : BgColor(c->bg), g_pal.dim);
        knob.left = t.left + Px(4);
        knob.right = knob.left + k;
      }
      knob.top = t.top + (h - k) / 2;
      knob.bottom = knob.top + k;
      EllipseAA(dc, &knob, c->val ? g_pal.accent_ink : g_pal.dim, CLR_INVALID);
      break;
    }
    case K_CHECK: {
      RECT b = r;
      int s = Px(20);
      b.top = (r.top + r.bottom - s) / 2;
      b.bottom = b.top + s;
      b.right = b.left + s;
      if (c->val) {
        RECT m = b;
        RoundRectAA(dc, &b, Px(4), c->hover ? g_pal.accent_hover : g_pal.accent, CLR_INVALID);
        InflateRect(&m, -Px(4), -Px(4));
        CheckMarkAA(dc, &m, g_pal.accent_ink, (float)Px(1) + 0.5f);
      } else {
        RoundRectAA(dc, &b, Px(4), c->hover ? g_pal.hover : g_pal.fill, g_pal.dim);
      }
      break;
    }
    default:
      break;
  }
  if (focused) {
    RECT f = r;
    InflateRect(&f, -1, -1);
    RoundRectAA(dc, &f, Px(4), CLR_INVALID, g_pal.label);
  }
}

static void PaintSeparatorLine(HWND hwnd) {
  HDC dc = GetDC(hwnd);
  RECT rc, line;
  GetClientRect(hwnd, &rc);
  line.left = 0;
  line.right = rc.right;
  line.top = Px(344);
  line.bottom = line.top + 1;
  FillSolid(dc, &line, g_pal.sep);
  ReleaseDC(hwnd, dc);
}

// ---------------------------------------------------------------- window procs
static HBRUSH g_brush_ground, g_brush_card, g_brush_warn, g_brush_field;

static HBRUSH BgBrush(int bg) {
  if (bg == BG_CARD)
    return g_brush_card;
  if (bg == BG_WARN)
    return g_brush_warn;
  return g_brush_ground;
}

static LRESULT CALLBACK ParentProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  int i;
  WNDPROC old = NULL;
  for (i = 0; i < g_nsubs; ++i)
    if (g_subs[i].h == hwnd)
      old = g_subs[i].old;
  if (!old)
    return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_CTLCOLORDLG:
      return (LRESULT)g_brush_ground;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      Ctl* c = FindCtl((HWND)lp);
      HDC dc = (HDC)wp;
      if (c && c->kind == K_TEXT) {
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, BgColor(c->bg));
        SetTextColor(dc, c->color);
        return (LRESULT)BgBrush(c->bg);
      }
      if (c && c->kind == K_EDIT) {
        SetBkColor(dc, g_pal.field);
        SetTextColor(dc, g_pal.label);
        return (LRESULT)g_brush_field;
      }
      SetBkMode(dc, TRANSPARENT);
      SetBkColor(dc, g_pal.ground);
      SetTextColor(dc, g_pal.label);
      return (LRESULT)g_brush_ground;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = (HDC)wp;
      SetBkColor(dc, g_pal.field);
      SetTextColor(dc, g_pal.label);
      return (LRESULT)g_brush_field;
    }
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
      Ctl* c = FindCtl(di->hwndItem);
      if (c) {
        if (di->CtlType == ODT_BUTTON) {
          if (c->kind == K_CHEV || c->kind == K_TOGGLE || c->kind == K_CHECK)
            PaintControlButton(c, di);
          else
            PaintButton(c, di);
        } else {
          PaintStatic(c, di);
        }
        return TRUE;
      }
      break;
    }
    case WM_PAINT: {
      LRESULT r = CallWindowProcW(old, hwnd, msg, wp, lp);
      if (hwnd == g_parent)
        PaintSeparatorLine(hwnd);
      return r;
    }
    case WM_SETTEXT:
      if (hwnd == g_parent && g_caption[0] && lp && wcscmp((const wchar_t*)lp, g_caption))
        return CallWindowProcW(old, hwnd, msg, wp, (LPARAM)g_caption);
      break;
    case WM_NCDESTROY: {
      LRESULT r = CallWindowProcW(old, hwnd, msg, wp, lp);
      ForgetChildrenOf(hwnd);
      for (i = 0; i < g_nsubs; ++i) {
        if (g_subs[i].h == hwnd) {
          g_subs[i] = g_subs[g_nsubs - 1];
          --g_nsubs;
          break;
        }
      }
      return r;
    }
    default:
      break;
  }
  return CallWindowProcW(old, hwnd, msg, wp, lp);
}

// Hover tracking for owner-drawn buttons.
static LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Ctl* c = FindCtl(hwnd);
  WNDPROC old = c ? c->old : NULL;
  if (!old)
    return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_MOUSEMOVE:
      if (!c->hover) {
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        c->hover = 1;
        InvalidateRect(hwnd, NULL, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      c->hover = 0;
      InvalidateRect(hwnd, NULL, FALSE);
      break;
    case WM_LBUTTONDOWN:
      g_keyboard = 0;
      break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if (!g_keyboard) {
        g_keyboard = 1;
        InvalidateRect(hwnd, NULL, FALSE);
      }
      break;
    case WM_SETTEXT: {
      LRESULT r = CallWindowProcW(old, hwnd, msg, wp, lp);
      InvalidateRect(hwnd, NULL, TRUE);
      return r;
    }
    case WM_ENABLE:
      InvalidateRect(hwnd, NULL, TRUE);
      break;
    default:
      break;
  }
  return CallWindowProcW(old, hwnd, msg, wp, lp);
}

static void HookButton(Ctl* c) {
  if (!c->old)
    c->old = (WNDPROC)SetWindowLongPtrW(c->h, GWLP_WNDPROC, (LONG_PTR)ButtonProc);
}

// NSIS's progress bar (msctls_progress32): painted as the 4 dip Fluent bar,
// with the percent mirrored into a label.
static HWND g_progress_label;

static LRESULT CALLBACK ProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  Ctl* c = FindCtl(hwnd);
  WNDPROC old = c ? c->old : NULL;
  if (!old)
    return DefWindowProcW(hwnd, msg, wp, lp);
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd, &ps);
      DRAWITEMSTRUCT di;
      int lo = (int)CallWindowProcW(old, hwnd, PBM_GETRANGE, TRUE, 0);
      int hi = (int)CallWindowProcW(old, hwnd, PBM_GETRANGE, FALSE, 0);
      int pos = (int)CallWindowProcW(old, hwnd, PBM_GETPOS, 0, 0);
      int pct = hi > lo ? MulDiv(pos - lo, 100, hi - lo) : 0;
      ZeroMemory(&di, sizeof(di));
      di.hDC = dc;
      GetClientRect(hwnd, &di.rcItem);
      c->val = pct < 0 ? 0 : pct > 100 ? 100 : pct;
      PaintStatic(c, &di);
      EndPaint(hwnd, &ps);
      if (g_progress_label && IsWindow(g_progress_label)) {
        wchar_t t[16];
        wsprintfW(t, L"%d%%", c->val);
        SetWindowTextW(g_progress_label, t);
      }
      return 0;
    }
    case PBM_SETPOS:
    case PBM_DELTAPOS:
    case PBM_STEPIT:
    case PBM_SETRANGE:
    case PBM_SETRANGE32: {
      LRESULT r = CallWindowProcW(old, hwnd, msg, wp, lp);
      InvalidateRect(hwnd, NULL, FALSE);
      return r;
    }
    default:
      break;
  }
  return CallWindowProcW(old, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- kinds
// "kind[/bg][!r]": the paint kind, the background it sits on, and "!r"
// for right-aligned text.
static int ParseKind(const wchar_t* spec_in, int* bg, int* font, COLORREF* color, int* val) {
  wchar_t spec[64], kind[64];
  const wchar_t* slash;
  wchar_t* bang;
  size_t n;
  lstrcpynW(spec, spec_in, 64);
  bang = wcschr(spec, L'!');
  if (bang)
    *bang = 0;
  slash = wcschr(spec, L'/');
  n = slash ? (size_t)(slash - spec) : wcslen(spec);
  wmemcpy(kind, spec, n);
  kind[n] = 0;
  *bg = BG_GROUND;
  if (slash) {
    if (!wcscmp(slash + 1, L"card"))
      *bg = BG_CARD;
    else if (!wcscmp(slash + 1, L"warn"))
      *bg = BG_WARN;
  }
  *font = F_BODY;
  *color = g_pal.label;
  if (!wcsncmp(kind, L"text.", 5)) {
    const wchar_t* t = kind + 5;
    if (!wcscmp(t, L"title")) {
      *font = F_TITLE;
      *color = g_pal.ink;
    } else if (!wcscmp(t, L"subtitle")) {
      *font = F_SUBTITLE;
      *color = g_pal.ink;
    } else if (!wcscmp(t, L"sub") || !wcscmp(t, L"hint") || !wcscmp(t, L"footer")) {
      *font = F_CAPTION;
      *color = g_pal.dim;
    } else if (!wcscmp(t, L"strong")) {
      *font = F_BODY_STRONG;
    } else if (!wcscmp(t, L"warn")) {
      *bg = BG_WARN;
    } else if (!wcscmp(t, L"warnb")) {
      *font = F_BODY_STRONG;
      *bg = BG_WARN;
    } else if (!wcscmp(t, L"caption")) {
      *font = F_CAPTION;
    }
    return K_TEXT;
  }
  if (!wcscmp(kind, L"field"))
    return K_FIELD;
  if (!wcscmp(kind, L"edit"))
    return K_EDIT;
  if (!wcscmp(kind, L"mark"))
    return K_MARK;
  if (!wcscmp(kind, L"card"))
    return K_CARD;
  if (!wcscmp(kind, L"chev"))
    return K_CHEV;
  if (!wcscmp(kind, L"toggle"))
    return K_TOGGLE;
  if (!wcscmp(kind, L"check"))
    return K_CHECK;
  if (!wcscmp(kind, L"progress"))
    return K_PROGRESS;
  if (!wcscmp(kind, L"infobar.warn"))
    return K_INFOBAR;
  if (!wcscmp(kind, L"okcircle"))
    return K_OKCIRCLE;
  if (!wcscmp(kind, L"blockcircle"))
    return K_BLOCKCIRCLE;
  if (!wcsncmp(kind, L"glyph:", 6)) {
    *val = (int)wcstol(kind + 6, NULL, 16);
    if (slash && !wcscmp(slash + 1, L"warn"))
      *color = g_pal.warn_ink;
    return K_GLYPH;
  }
  if (!wcscmp(kind, L"sep"))
    return K_SEP;
  if (!wcscmp(kind, L"btn.pri"))
    return K_BTN_PRI;
  if (!wcscmp(kind, L"btn.sec"))
    return K_BTN_SEC;
  if (!wcscmp(kind, L"btn.dng"))
    return K_BTN_DNG;
  if (!wcscmp(kind, L"btn.sm"))
    return K_BTN_SM;
  if (!wcscmp(kind, L"link"))
    return K_LINK;
  return K_NONE;
}

static int IsButtonKind(int k) {
  return k == K_BTN_PRI || k == K_BTN_SEC || k == K_BTN_DNG || k == K_BTN_SM || k == K_LINK || k == K_CHEV ||
         k == K_TOGGLE || k == K_CHECK;
}

static void ApplyKind(HWND h, const wchar_t* spec, int val_in) {
  int bg, font, val = val_in;
  COLORREF color;
  int kind = ParseKind(spec, &bg, &font, &color, &val);
  Ctl* c = Register(h, kind, val, bg);
  if (!c)
    return;
  c->font = font;
  c->color = color;
  if (kind == K_TEXT) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fonts[font], TRUE);
  } else if (kind == K_EDIT) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fonts[F_BODY], TRUE);
    SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : L"Explorer", NULL);
  } else if (IsButtonKind(kind)) {
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    SetWindowLongPtrW(h, GWL_STYLE, (style & ~(LONG_PTR)0xF) | BS_OWNERDRAW);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fonts[kind == K_BTN_SM || kind == K_LINK ? F_CAPTION : F_BODY], TRUE);
    HookButton(c);
  } else {
    LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE);
    SetWindowLongPtrW(h, GWL_STYLE, (style & ~(LONG_PTR)0x1F) | SS_OWNERDRAW);
  }
  InvalidateRect(h, NULL, TRUE);
}

// ---------------------------------------------------------------- exports
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    g_hinst = inst;
    GdiplusStartupInput in;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    GdiplusStartup(&g_gdiplus, &in, NULL);
  }
  return TRUE;
}

static int ReadDarkAppMode(void) {
  HKEY key;
  DWORD light = 1, size = sizeof(light), type;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                    KEY_READ, &key) == ERROR_SUCCESS) {
    if (RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (BYTE*)&light, &size) != ERROR_SUCCESS)
      light = 1;
    RegCloseKey(key);
  }
  return light == 0;
}

static int ReadDpi(HWND hwnd) {
  typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
  GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
  if (fn) {
    UINT d = fn(hwnd);
    if (d)
      return (int)d;
  }
  {
    HDC dc = GetDC(NULL);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    return d ? d : 96;
  }
}

PLUGIN(Init) {
  BOOL dark;
  PLUGIN_INIT();
  g_parent = hwndParent;
  g_dpi = ReadDpi(hwndParent);
  g_dark = ReadDarkAppMode();
  SetPalette(g_dark);
  InitFonts();
  if (!g_brush_ground) {
    g_brush_ground = CreateSolidBrush(g_pal.ground);
    g_brush_card = CreateSolidBrush(g_pal.card);
    g_brush_warn = CreateSolidBrush(g_pal.warn_bg);
    g_brush_field = CreateSolidBrush(g_pal.field);
  }
  dark = g_dark ? TRUE : FALSE;
  DwmSetWindowAttribute(hwndParent, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
  SubclassParent(hwndParent);
  PushString(g_dark ? L"1" : L"0");
}

static BOOL CALLBACK HideChrome(HWND child, LPARAM lp) {
  int id = GetDlgCtrlID(child);
  (void)lp;
  if (GetParent(child) != g_parent)
    return TRUE;
  if (id == 1018 || id == 1 || id == 2 || id == 1028)
    return TRUE;
  ShowWindow(child, SW_HIDE);
  return TRUE;
}

enum { kClientW = 600, kClientH = 420, kFooterTop = 344, kMargin = 32, kButtonH = 32, kButtonGap = 8 };

PLUGIN(Frame) {
  RECT want, cur, work;
  DWORD style, ex;
  HWND page, footer, next, cancel;
  int w, h, x, y;
  PLUGIN_INIT();
  EnumChildWindows(g_parent, HideChrome, 0);
  style = (DWORD)GetWindowLongPtrW(g_parent, GWL_STYLE);
  ex = (DWORD)GetWindowLongPtrW(g_parent, GWL_EXSTYLE);
  SetRect(&want, 0, 0, Px(kClientW), Px(kClientH));
  {
    typedef BOOL(WINAPI * AdjustFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    AdjustFn fn = (AdjustFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (fn)
      fn(&want, style, FALSE, ex, (UINT)g_dpi);
    else
      AdjustWindowRectEx(&want, style, FALSE, ex);
  }
  w = want.right - want.left;
  h = want.bottom - want.top;
  GetWindowRect(g_parent, &cur);
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  x = (work.left + work.right - w) / 2;
  y = (work.top + work.bottom - h) / 2;
  SetWindowPos(g_parent, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  page = GetDlgItem(g_parent, 1018);
  if (page)
    MoveWindow(page, 0, 0, Px(kClientW), Px(kFooterTop), TRUE);
  footer = GetDlgItem(g_parent, 1028);
  if (footer) {
    LONG_PTR s = GetWindowLongPtrW(footer, GWL_STYLE);
    SetWindowLongPtrW(footer, GWL_STYLE, (s & ~(LONG_PTR)0x1F) | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS);
    ApplyKind(footer, L"text.footer", 0);
    SetWindowTextW(footer, L"");
  }
  next = GetDlgItem(g_parent, 1);
  cancel = GetDlgItem(g_parent, 2);
  if (next)
    ApplyKind(next, L"btn.pri", 0);
  if (cancel)
    ApplyKind(cancel, L"btn.sec", 0);
  InvalidateRect(g_parent, NULL, TRUE);
}

PLUGIN(Dpi) {
  PLUGIN_INIT();
  PushInt(g_dpi);
}

static int ButtonWidth(HWND h) {
  wchar_t text[128];
  SIZE sz;
  HDC dc = GetDC(h);
  HFONT old = (HFONT)SelectObject(dc, g_fonts[F_BODY]);
  GetWindowTextW(h, text, 128);
  GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
  SelectObject(dc, old);
  ReleaseDC(h, dc);
  {
    int w = sz.cx + Px(40);
    return w < Px(96) ? Px(96) : w;
  }
}

PLUGIN(Buttons) {
  wchar_t kind[16], primary[128], cancel_text[128];
  HWND next, cancel, footer;
  int right = Px(kClientW - kMargin), y = Px(kFooterTop + (kClientH - kFooterTop - kButtonH) / 2);
  Ctl* c;
  PLUGIN_INIT();
  PopString(kind, 16);
  PopString(primary, 128);
  PopString(cancel_text, 128);
  next = GetDlgItem(g_parent, 1);
  cancel = GetDlgItem(g_parent, 2);
  if (GetDlgItem(g_parent, 3))
    ShowWindow(GetDlgItem(g_parent, 3), SW_HIDE);
  if (cancel) {
    if (cancel_text[0]) {
      int w;
      SetWindowTextW(cancel, cancel_text);
      w = ButtonWidth(cancel);
      MoveWindow(cancel, right - w, y, w, Px(kButtonH), TRUE);
      ShowWindow(cancel, SW_SHOW);
      // NSIS disables Cancel and the caption's close while a section runs
      // and never re-enables them for a page after it.
      EnableWindow(cancel, TRUE);
      EnableMenuItem(GetSystemMenu(g_parent, FALSE), SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
      right -= w + Px(kButtonGap);
    } else {
      ShowWindow(cancel, SW_HIDE);
    }
  }
  if (next) {
    c = FindCtl(next);
    if (!wcscmp(kind, L"none") || !primary[0]) {
      ShowWindow(next, SW_HIDE);
    } else {
      int w;
      if (c)
        c->kind = !wcscmp(kind, L"dng") ? K_BTN_DNG : !wcscmp(kind, L"sec") ? K_BTN_SEC : K_BTN_PRI;
      SetWindowTextW(next, primary);
      w = ButtonWidth(next);
      MoveWindow(next, right - w, y, w, Px(kButtonH), TRUE);
      ShowWindow(next, SW_SHOW);
      right -= w + Px(kButtonGap);
    }
  }
  footer = GetDlgItem(g_parent, 1028);
  if (footer)
    MoveWindow(footer, Px(kMargin), Px(kFooterTop) + Px((kClientH - kFooterTop - 16) / 2), right - Px(kMargin) - Px(8),
               Px(16), TRUE);
}

PLUGIN(Footer) {
  wchar_t text[512];
  HWND footer;
  PLUGIN_INIT();
  PopString(text, 512);
  footer = GetDlgItem(g_parent, 1028);
  if (footer) {
    SetWindowTextW(footer, text);
    InvalidateRect(footer, NULL, TRUE);
  }
}

PLUGIN(Create) {
  wchar_t kind[64], text[1024];
  HWND parent, h;
  int x, y, w, h_dip, bg, font, val = 0, k;
  COLORREF color;
  DWORD style = WS_CHILD | WS_VISIBLE;
  const wchar_t* cls = L"STATIC";
  PLUGIN_INIT();
  parent = PopHwnd();
  PopString(kind, 64);
  x = PopInt();
  y = PopInt();
  w = PopInt();
  h_dip = PopInt();
  PopString(text, 1024);
  k = ParseKind(kind, &bg, &font, &color, &val);
  if (k == K_TEXT) {
    style |= (wcsstr(kind, L"!r") ? SS_RIGHT : SS_LEFT) | SS_NOPREFIX | SS_EDITCONTROL;
  } else if (k == K_EDIT) {
    cls = L"EDIT";
    style |= WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT;
  } else if (IsButtonKind(k)) {
    cls = L"BUTTON";
    style |= WS_TABSTOP | BS_OWNERDRAW;
  } else {
    style |= SS_OWNERDRAW;
  }
  h = CreateWindowExW(0, cls, text, style, Px(x), Px(y), Px(w), Px(h_dip), parent, NULL, GetModuleHandleW(NULL),
                      NULL);
  if (h) {
    ApplyKind(h, kind, val);
    if (k == K_EDIT)
      SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(Px(2), Px(2)));
  }
  PushInt((INT_PTR)h);
}

PLUGIN(Style) {
  wchar_t kind[64], v[32];
  HWND h;
  PLUGIN_INIT();
  h = PopHwnd();
  PopString(kind, 64);
  v[0] = 0;
  PopString(v, 32);
  ApplyKind(h, kind, _wtoi(v));
}

PLUGIN(Set) {
  HWND h;
  int v;
  Ctl* c;
  PLUGIN_INIT();
  h = PopHwnd();
  v = PopInt();
  c = FindCtl(h);
  if (c) {
    c->val = v;
    InvalidateRect(h, NULL, TRUE);
  }
}

PLUGIN(Text) {
  wchar_t text[1024];
  HWND h;
  PLUGIN_INIT();
  h = PopHwnd();
  PopString(text, 1024);
  SetWindowTextW(h, text);
  InvalidateRect(h, NULL, TRUE);
}

PLUGIN(Place) {
  HWND h;
  int x, y, w, hh;
  PLUGIN_INIT();
  h = PopHwnd();
  x = PopInt();
  y = PopInt();
  w = PopInt();
  hh = PopInt();
  MoveWindow(h, Px(x), Px(y), Px(w), Px(hh), TRUE);
  InvalidateRect(GetParent(h), NULL, TRUE);
}

PLUGIN(Show) {
  HWND h;
  int v;
  PLUGIN_INIT();
  h = PopHwnd();
  v = PopInt();
  ShowWindow(h, v ? SW_SHOW : SW_HIDE);
}

static BOOL CALLBACK HidePageStrays(HWND child, LPARAM lp) {
  int id = GetDlgCtrlID(child);
  if (GetParent(child) != (HWND)lp)
    return TRUE;
  if (id != 1004 && id != 1006)
    ShowWindow(child, SW_HIDE);
  return TRUE;
}

// NSIS positions a built-in page at the page area's corner but keeps the
// dialog template's size (SWP_NOSIZE); the page also carries controls the
// design has no place for (the template's icon, the details button, the
// log). Size it to the page area, hide everything but the progress bar and
// the status line, and paint its ground.
PLUGIN(Page) {
  HWND page;
  PLUGIN_INIT();
  page = PopHwnd();
  if (!page)
    return;
  MoveWindow(page, 0, 0, Px(kClientW), Px(kFooterTop), TRUE);
  EnumChildWindows(page, HidePageStrays, (LPARAM)page);
  SubclassParent(page);
  InvalidateRect(page, NULL, TRUE);
}

PLUGIN(Caption) {
  PLUGIN_INIT();
  PopString(g_caption, 256);
  SetWindowTextW(g_parent, g_caption);
}

PLUGIN(Progress) {
  HWND bar, label;
  Ctl* c;
  PLUGIN_INIT();
  bar = PopHwnd();
  label = PopHwnd();
  g_progress_label = label;
  c = Register(bar, K_PROGRESS, 0, BG_GROUND);
  if (c && !c->old) {
    SetWindowTheme(bar, L"", L"");
    c->old = (WNDPROC)SetWindowLongPtrW(bar, GWLP_WNDPROC, (LONG_PTR)ProgressProc);
    InvalidateRect(bar, NULL, TRUE);
  }
}
