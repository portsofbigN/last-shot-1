// Launcher.exe's window, laid out like the Pokopia PC launcher: a sidebar (Play, Settings, Game files, About) and
// pages of cards in a dark theme. Native Win32: GDI for text, GDI+ for the rounded shapes, Windows' own icon font
// (Segoe Fluent Icons / Segoe MDL2 Assets). The settings are splatoon.ini's (nx2_settings.c via nx2_cfg.h).
//
// Play: checks the game files and SplatoonRaiders.exe, prepares the shaders seen before (nx2_pcache.c), then starts
// SplatoonRaiders.exe with NX2_LAUNCHED=1 (the game then skips its own menu) and closes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <winhttp.h>
#define COBJMACROS
#include <dxgi.h>

#include "nx2_cfg.h"

#define APP_VERSION "0.0.3"
#define APP_VERSION_W L"0.0.3"
static char upd_tag[64], upd_url[300];   // a newer release found by update_thread
#define SUPPORTED_VERSION "65536"
#define SUPPORTED_TITLE "0400380020056000"
#define SUPPORTED_SHA256 "35fe1cbe7ea983e1e6ce8716b98e884a2f6cfb02c9514d51ec2c00daa4addcde"

// ---- GDI+ flat API (gdiplus.dll), the few calls used for anti-aliased shapes
typedef struct { UINT32 version; void *debug; BOOL no_thread; BOOL no_codecs; } GpStartupInput;
int __stdcall GdiplusStartup(ULONG_PTR *token, const GpStartupInput *in, void *out);
int __stdcall GdipCreateFromHDC(HDC dc, void **g);
int __stdcall GdipDeleteGraphics(void *g);
int __stdcall GdipSetSmoothingMode(void *g, int mode);
int __stdcall GdipSetPixelOffsetMode(void *g, int mode);
int __stdcall GdipCreatePath(int fill_mode, void **path);
int __stdcall GdipAddPathArc(void *path, float x, float y, float w, float h, float start, float sweep);
int __stdcall GdipClosePathFigure(void *path);
int __stdcall GdipDeletePath(void *path);
int __stdcall GdipCreateSolidFill(DWORD argb, void **brush);
int __stdcall GdipDeleteBrush(void *brush);
int __stdcall GdipFillPath(void *g, void *brush, void *path);
int __stdcall GdipCreatePen1(DWORD argb, float width, int unit, void **pen);
int __stdcall GdipDeletePen(void *pen);
int __stdcall GdipDrawPath(void *g, void *pen, void *path);
typedef struct { float X, Y; } GpPointF;
int __stdcall GdipAddPathClosedCurve2(void *path, const GpPointF *pts, int n, float tension);
int __stdcall GdipAddPathEllipse(void *path, float x, float y, float w, float h);

// ---- colours: Splatoon's - deep ink purple, neon yellow ink, a purple / pink rival ink
#define C_BG RGB(24, 14, 46)
#define C_SIDE RGB(16, 9, 33)
#define C_LINE RGB(52, 34, 92)
#define C_CARD RGB(36, 22, 66)
#define C_CARD_EDGE RGB(70, 46, 120)
#define C_FIELD RGB(22, 13, 42)
#define C_NAV_SEL RGB(230, 245, 52)
#define C_NAV_HOT RGB(44, 28, 80)
#define C_TEXT RGB(246, 242, 255)
#define C_DIM RGB(184, 168, 222)
#define C_FAINT RGB(140, 124, 180)
#define C_ACCENT RGB(230, 245, 52)
#define C_ACCENT_HOT RGB(244, 255, 120)
#define C_ACCENT_DIM RGB(92, 98, 40)
#define C_ON_ACCENT RGB(26, 14, 48)
#define C_RED RGB(255, 82, 120)
#define C_RED_BG RGB(52, 16, 44)
#define C_YEL RGB(255, 196, 52)
#define C_YEL_BG RGB(50, 34, 20)
#define C_BLUE RGB(80, 190, 255)
#define C_GREEN RGB(110, 230, 120)
#define C_GREEN_BG RGB(20, 44, 34)
#define C_BTN RGB(52, 32, 92)
#define C_BTN_HOT RGB(70, 44, 120)
#define C_BTN_EDGE RGB(96, 64, 160)
#define C_INK_A 0xe6f534u      // neon yellow ink (RGB)
#define C_INK_B 0x7a2ff0u      // purple ink
#define C_INK_C 0xff3fa4u      // pink ink

// icon glyphs (Segoe Fluent Icons / Segoe MDL2 Assets)
#define I_PLAY 0xE768
#define I_GEAR 0xE713
#define I_FOLDER 0xE8B7
#define I_FOLDER_OPEN 0xE838
#define I_INFO 0xE946
#define I_ERROR 0xEA39
#define I_WARN 0xE7BA
#define I_OK 0xE73E
#define I_GAME 0xE7FC
#define I_SAVE 0xE74E
#define I_UNDO 0xE7A7
#define I_DRIVE 0xEDA2
#define I_TOOL 0xE90F
#define I_MONITOR 0xE7F4
#define I_IMAGE 0xEB9F
#define I_SPEED 0xEC4A
#define I_PAD 0xE7FC
#define I_GLOBE 0xE774
#define I_SEARCH 0xE721

enum { P_PLAY, P_SETTINGS, P_FILES, P_ABOUT, NPAGE };
enum {   // clickable areas
    Z_NAV = 1, Z_PLAY = 10, Z_CHOOSE, Z_OPEN_DIR, Z_CHANGE, Z_BROWSE, Z_USE, Z_BACK, Z_SAVE, Z_REVERT, Z_PREP, Z_SETUP, Z_IMPORT, Z_UPDATE
};
enum { CK_NONE, CK_BUSY, CK_OK, CK_MISSING, CK_VERSION, CK_UNSET };
#define WM_APP_CHECKED (WM_APP + 1)
#define WM_APP_PREPPED (WM_APP + 2)
#define WM_APP_SHADERS (WM_APP + 3)
#define WM_APP_SETUP (WM_APP + 4)
#define ID_COMBO 300

typedef struct { RECT r; int id; int on; } Zone;
typedef struct {
    char dir[MAX_PATH * 3];        // as chosen (UTF-8, forward slashes)
    char resolved[MAX_PATH * 3];   // the folder with exefs/ and romfs/ in it
    volatile LONG state;
    char sha[65];
} Check;

static double ui;   // DPI scale
#define S(v) ((int)((v) * ui + 0.5))
static HWND w_main, w_page, combos[64];
static int page = P_PLAY, hot_main = -1, hot_page = -1, scroll[NPAGE], content_h[NPAGE];
static Zone zm[16], zp[32];
static int nzm, nzp;
static HFONT f_h1, f_h2, f_body, f_bodyb, f_small, f_mono, f_nav, f_btn, f_play, f_icon, f_icon_s, f_icon_l, f_brand;
static Check ck_cur, ck_new;   // the game files in use, and the ones picked on the Game files page
static int exe_ok, shaders_ok, dirty, prep_state;   // prep_state: 0 idle, 1 preparing, 2 starting the game
// the shaders translated from the game files (SplatoonRaiders.exe with NX2_PREPARE_SHADERS=1, nx2_shaderprep.c)
static int prepared_ok, sp_then_play;
static volatile LONG sp_state, sp_done, sp_total;   // sp_state: 0 idle, 1 reading the files, 2 translating
static char sp_error[200];
static void start_shader_prep(int then_play);
// the Set up: SplatoonRaiders.exe built here from the player's game files (setup\tools\setup_build.py with the bundled
// Python and Zig), as the Pokopia launcher does; the runtime (SplatoonRaiders_rt.dll) comes built
static int setup_ok;   // the Set up's tools are in the folder
static volatile LONG su_kb_done, su_kb_total;   // source compiled (KiB), for the time left
static ULONGLONG su_t0;   // when the compile progress started (ms)
static LONG su_kb0;
static volatile LONG su_state, su_done, su_total;   // 0 idle, 1 lifting the code, 2 glue, 3 compiling, 4 linking
static char su_error[300];
static HANDLE su_job;
static void start_setup(void);
static volatile LONG prep_done, prep_total;
static wchar_t exe_dir[MAX_PATH], status_msg[200];
static const wchar_t *icon_face = L"Segoe Fluent Icons";
static int lang;
#define T(pt, en) (lang ? (en) : (pt))

// ---- small helpers
static DWORD argb(COLORREF c) { return 0xff000000u | (DWORD)GetRValue(c) << 16 | (DWORD)GetGValue(c) << 8 | GetBValue(c); }

static void round_rect(HDC dc, int x, int y, int w, int h, int r, COLORREF fill, COLORREF edge, int has_edge) {
    void *g, *path, *b, *p;
    GdipCreateFromHDC(dc, &g);
    GdipSetSmoothingMode(g, 4);      // anti-alias
    GdipSetPixelOffsetMode(g, 4);    // half
    GdipCreatePath(0, &path);
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f, fw = (float)w - 1, fh = (float)h - 1, d = (float)r * 2;
    if (d > fh) d = fh;
    if (d > fw) d = fw;
    GdipAddPathArc(path, fx, fy, d, d, 180, 90);
    GdipAddPathArc(path, fx + fw - d, fy, d, d, 270, 90);
    GdipAddPathArc(path, fx + fw - d, fy + fh - d, d, d, 0, 90);
    GdipAddPathArc(path, fx, fy + fh - d, d, d, 90, 90);
    GdipClosePathFigure(path);
    GdipCreateSolidFill(argb(fill), &b);
    GdipFillPath(g, b, path);
    GdipDeleteBrush(b);
    if (has_edge) {
        GdipCreatePen1(argb(edge), (float)(ui > 1.4 ? 2 : 1), 2, &p);
        GdipDrawPath(g, p, path);
        GdipDeletePen(p);
    }
    GdipDeletePath(path);
    GdipDeleteGraphics(g);
}

// An ink splat, Splatoon style: an irregular blob (a closed curve through points at random radii) with a few
// droplets thrown around it; seed picks its shape, alpha (0..255) how opaque it is.
static uint32_t ink_rand(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static void ink_splat(HDC dc, int cx, int cy, int r, uint32_t rgb, int alpha, uint32_t seed) {
    void *g, *path, *b;
    GdipCreateFromHDC(dc, &g);
    GdipSetSmoothingMode(g, 4);
    GdipCreatePath(0, &path);
    GpPointF pt[28];
    int n = 22;
    uint32_t st = seed * 2654435761u + 1;
    for (int i = 0; i < n; i++) {
        double a = 6.2831853 * i / n, k = 0.62 + (ink_rand(&st) % 1000) / 1000.0 * 0.5;
        if (ink_rand(&st) % 5 == 0) k += 0.35;   // a lobe sticking out
        pt[i].X = (float)(cx + cos(a) * r * k), pt[i].Y = (float)(cy + sin(a) * r * k);
    }
    GdipAddPathClosedCurve2(path, pt, n, 0.55f);
    for (int i = 0; i < 9; i++) {   // droplets
        double a = 6.2831853 * (ink_rand(&st) % 1000) / 1000.0, d = r * (1.1 + (ink_rand(&st) % 1000) / 1000.0 * 0.7);
        float dr = (float)(r * (0.05 + (ink_rand(&st) % 1000) / 1000.0 * 0.11));
        GdipAddPathEllipse(path, (float)(cx + cos(a) * d) - dr, (float)(cy + sin(a) * d) - dr, dr * 2, dr * 2);
    }
    GdipCreateSolidFill((DWORD)alpha << 24 | (rgb & 0xffffff), &b);
    GdipFillPath(g, b, path);
    GdipDeleteBrush(b);
    GdipDeletePath(path);
    GdipDeleteGraphics(g);
}

static void wide(const char *s, wchar_t *w, int n) { MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); }

// UTF-8 text in a box: returns its height. flags: DT_* (DT_WORDBREAK for paragraphs)
static int text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, const char *s, UINT flags) {
    wchar_t ws[1024];
    wide(s, ws, 1024);
    SelectObject(dc, f);
    RECT r = {x, y, x + w, y + 2000};
    DrawTextW(dc, ws, -1, &r, flags | DT_CALCRECT | DT_NOPREFIX | (flags & DT_WORDBREAK ? DT_EDITCONTROL : 0));
    int h = r.bottom - r.top;
    r = (RECT){x, y, x + w, y + h};
    SetTextColor(dc, c);
    DrawTextW(dc, ws, -1, &r, flags | DT_NOPREFIX | (flags & DT_WORDBREAK ? DT_EDITCONTROL : 0));
    return h;
}
static int text_h(HDC dc, HFONT f, int w, const char *s, UINT flags) {
    wchar_t ws[1024];
    wide(s, ws, 1024);
    SelectObject(dc, f);
    RECT r = {0, 0, w, 2000};
    DrawTextW(dc, ws, -1, &r, flags | DT_CALCRECT | DT_NOPREFIX | (flags & DT_WORDBREAK ? DT_EDITCONTROL : 0));
    return r.bottom - r.top;
}
static int text_w(HDC dc, HFONT f, const char *s) {
    wchar_t ws[512];
    wide(s, ws, 512);
    SelectObject(dc, f);
    SIZE z;
    GetTextExtentPoint32W(dc, ws, (int)wcslen(ws), &z);
    return z.cx;
}
static void glyph(HDC dc, HFONT f, COLORREF c, int x, int y, int box, wchar_t g) {   // an icon centred in a square
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT r = {x, y, x + box, y + box};
    DrawTextW(dc, &g, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void zone(Zone *z, int *n, int x, int y, int w, int h, int id, int on) {
    if (*n < 32) z[(*n)++] = (Zone){{x, y, x + w, y + h}, id, on};
}

// a button: primary (mint), or secondary; returns its width
static int button(HDC dc, Zone *zs, int *nz, int hot, int x, int y, int id, wchar_t icon, const char *label, int primary,
                  int on, int big) {
    HFONT f = big ? f_play : f_btn;
    int h = S(big ? 60 : 40), pad = S(big ? 26 : 16), iw = icon ? S(big ? 30 : 26) : 0;
    int w = pad * 2 + iw + text_w(dc, f, label);
    int is_hot = on && hot >= 0 && zs[hot].id == id;
    COLORREF bg = primary ? (on ? (is_hot ? C_ACCENT_HOT : C_ACCENT) : C_ACCENT_DIM) : (is_hot ? C_BTN_HOT : C_BTN);
    COLORREF fg = primary ? (on ? C_ON_ACCENT : RGB(160, 160, 120)) : (on ? C_TEXT : C_FAINT);
    if (big && on) ink_splat(dc, x + w - S(10), y + h / 2, h * 3 / 4, C_INK_C, 255, 41);   // pink ink behind Play
    round_rect(dc, x, y, w, h, h / 2, bg, C_BTN_EDGE, !primary);   // pill shaped, as Splatoon's buttons
    if (icon) glyph(dc, big ? f_icon_l : f_icon_s, fg, x + pad - S(4), y + (h - iw) / 2, iw, icon);
    text(dc, f, fg, x + pad + iw, y + (h - text_h(dc, f, 400, label, DT_SINGLELINE)) / 2, w, label, DT_SINGLELINE);
    zone(zs, nz, x, y, w, h, id, on);
    return w;
}
static int link_button(HDC dc, Zone *zs, int *nz, int hot, int x, int y, int id, const char *label) {   // flat, text only
    int w = text_w(dc, f_btn, label) + S(24), h = S(40);
    int is_hot = hot >= 0 && zs[hot].id == id;
    if (is_hot) round_rect(dc, x, y, w, h, S(9), C_NAV_HOT, 0, 0);
    text(dc, f_btn, is_hot ? C_TEXT : C_DIM, x + S(12), y + (h - text_h(dc, f_btn, 400, label, DT_SINGLELINE)) / 2, w,
         label, DT_SINGLELINE);
    zone(zs, nz, x, y, w, h, id, 1);
    return w;
}

// a coloured notice (error, warning, info, success); returns its height
enum { B_ERROR, B_WARN, B_INFO, B_OK };
static int banner(HDC dc, int x, int y, int w, int kind, const char *msg, int draw) {
    int pad = S(18), tx = x + S(52), tw = w - S(52) - pad;
    int h = text_h(dc, f_body, tw, msg, DT_WORDBREAK) + pad * 2;
    if (!draw) return h;
    COLORREF edge = kind == B_ERROR ? C_RED : kind == B_WARN ? C_YEL : kind == B_OK ? C_GREEN : C_CARD_EDGE;
    COLORREF bg = kind == B_ERROR ? C_RED_BG : kind == B_WARN ? C_YEL_BG : kind == B_OK ? C_GREEN_BG : C_CARD;
    COLORREF ic = kind == B_ERROR ? C_RED : kind == B_WARN ? C_YEL : kind == B_OK ? C_GREEN : C_BLUE;
    wchar_t g = kind == B_ERROR ? I_ERROR : kind == B_WARN ? I_WARN : kind == B_OK ? I_OK : I_INFO;
    round_rect(dc, x, y, w, h, S(12), bg, edge, 1);
    glyph(dc, f_icon_s, ic, x + S(16), y + pad - S(2), S(24), g);
    text(dc, f_body, C_TEXT, tx, y + pad, tw, msg, DT_WORDBREAK);
    return h;
}

static void card_bg(HDC dc, int x, int y, int w, int h) { round_rect(dc, x, y, w, h, S(14), C_CARD, C_CARD_EDGE, 1); }
static int card_title(HDC dc, int x, int y, wchar_t icon, const char *title) {   // returns the height used
    glyph(dc, f_icon, C_ACCENT, x, y, S(28), icon);
    text(dc, f_h2, C_TEXT, x + S(34), y + S(1), S(600), title, DT_SINGLELINE);
    return S(44);
}

// ---- paths, files and checks
static void program_dir(void) {
    DWORD n = GetModuleFileNameW(NULL, exe_dir, MAX_PATH);
    wchar_t *sl = n ? wcsrchr(exe_dir, L'\\') : NULL;
    if (sl) sl[1] = 0;
}
static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static int dir_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static void refresh_files(void) {
    wchar_t p[MAX_PATH + 40];
    swprintf(p, MAX_PATH + 40, L"%lsSplatoonRaiders.exe", exe_dir);
    exe_ok = file_exists(p);
    swprintf(p, MAX_PATH + 40, L"%lspipeline_recipes.bin", exe_dir);
    shaders_ok = file_exists(p);
    swprintf(p, MAX_PATH + 40, L"%lsshader_cache.prepared", exe_dir);
    prepared_ok = file_exists(p);
    wchar_t q[MAX_PATH + 60];
    swprintf(p, MAX_PATH + 40, L"%lssetup\\python\\python.exe", exe_dir);
    swprintf(q, MAX_PATH + 60, L"%lssetup\\zig\\zig.exe", exe_dir);
    setup_ok = file_exists(p) && file_exists(q);
    swprintf(p, MAX_PATH + 40, L"%lsSplatoonRaiders_rt.dll", exe_dir);
    setup_ok = setup_ok && file_exists(p);
}

static int sha256_file(const wchar_t *path, char out[65]) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hh;
    int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0) {
            static unsigned char buf[1 << 20];
            DWORD got;
            ok = 1;
            while (ReadFile(f, buf, sizeof buf, &got, NULL) && got)
                if (BCryptHashData(hh, buf, got, 0)) ok = 0;
            unsigned char d[32];
            if (ok && BCryptFinishHash(hh, d, 32, 0) == 0)
                for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
            else ok = 0;
            BCryptDestroyHash(hh);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    return ok;
}

// The folder itself, or its updates/65536 (a dump with base/ and updates/): the one with exefs/main and romfs/
static int find_game(const char *dir, char *resolved, size_t n) {
    const char *sub[] = {"", "/updates/" SUPPORTED_VERSION, "/" SUPPORTED_VERSION};
    for (int i = 0; i < 3; i++) {
        char d[MAX_PATH * 3];
        wchar_t w[MAX_PATH + 40];
        snprintf(d, sizeof d, "%s%s", dir, sub[i]);
        wide(d, w, MAX_PATH);
        wcscat(w, L"/exefs/main");
        if (!file_exists(w)) continue;
        wide(d, w, MAX_PATH);
        wcscat(w, L"/romfs");
        if (!dir_exists(w)) continue;
        snprintf(resolved, n, "%s", d);
        return 1;
    }
    return 0;
}
static DWORD WINAPI check_thread(LPVOID a) {
    Check *c = a;
    LONG st = CK_UNSET;
    if (c->dir[0]) {
        st = CK_MISSING;
        if (find_game(c->dir, c->resolved, sizeof c->resolved)) {
            wchar_t w[MAX_PATH + 40];
            char m[MAX_PATH * 3];
            snprintf(m, sizeof m, "%s/exefs/main", c->resolved);
            wide(m, w, MAX_PATH + 40);
            st = sha256_file(w, c->sha) && !strcmp(c->sha, SUPPORTED_SHA256) ? CK_OK : CK_VERSION;
        }
    }
    InterlockedExchange(&c->state, st);
    PostMessageW(w_main, WM_APP_CHECKED, 0, 0);
    return 0;
}
static void start_check(Check *c, const char *dir) {
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    c->resolved[0] = c->sha[0] = 0;
    c->state = CK_BUSY;
    CloseHandle(CreateThread(NULL, 0, check_thread, c, 0, NULL));
}
static const char *check_text(const Check *c) {
    switch (c->state) {
    case CK_BUSY: return T("Verificando os arquivos do jogo...", "Checking the game files...");
    case CK_OK: return T("Versão " SUPPORTED_VERSION " reconhecida.", "Version " SUPPORTED_VERSION " recognised.");
    case CK_MISSING: return T("Essa pasta não tem exefs/main e romfs/ (nem updates/" SUPPORTED_VERSION "/ com eles).",
                              "That folder has no exefs/main and romfs/ (nor updates/" SUPPORTED_VERSION "/ with them).");
    case CK_VERSION: return T("Os arquivos são de outra versão do jogo: este port só funciona com a atualização " SUPPORTED_VERSION ".",
                              "These files are from another version of the game: this port only works with update " SUPPORTED_VERSION ".");
    case CK_UNSET: return T("Nenhuma pasta escolhida.", "No folder chosen.");
    }
    return "";
}

// ---- settings page: one combo per option (the game folder is on the Game files page)
static int shown_setting(int i) { Setting *s = nx2_cfg_at(i); return s && s->nvalues > 0; }
static void fill_combo(int i) {
    Setting *s = nx2_cfg_at(i);
    HWND cb = combos[i];
    int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (sel < 0)
        for (int k = 0; k < s->nvalues; k++)
            if (!strcmp(s->values[k], s->value)) sel = k;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int k = 0; k < s->nvalues; k++) {
        wchar_t w[128];
        wide(s->names[lang][k], w, 128);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)w);
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)(sel < 0 ? 0 : sel), 0);
}
static void reset_combos(void) {   // back to the saved values
    for (int i = 0; i < nx2_cfg_count(); i++) {
        if (!combos[i]) continue;
        Setting *s = nx2_cfg_at(i);
        for (int k = 0; k < s->nvalues; k++)
            if (!strcmp(s->values[k], s->value)) SendMessageW(combos[i], CB_SETCURSEL, (WPARAM)k, 0);
    }
}
static int combo_value(int i) { return combos[i] ? (int)SendMessageW(combos[i], CB_GETCURSEL, 0, 0) : -1; }
// ---- quality presets: the settings each one sets (the "preset" combo; any of these changed by hand: Custom)
typedef struct { const char *key, *v[3]; } PresetRow;   // values for low, balanced, high
static const PresetRow presets[] = {
    {"mode", {"handheld", "docked", "docked"}},
    {"upscaler", {"fsr", "fsr", "nis"}},
    {"antialias", {"fxaa", "fxaa", "smaa"}},
    {"anisotropic", {"0", "0", "4"}},
};
static int setting_index(const char *key) {
    for (int i = 0; i < nx2_cfg_count(); i++)
        if (!strcmp(nx2_cfg_at(i)->key, key)) return i;
    return -1;
}
static int value_index(int i, const char *v) {
    Setting *s = nx2_cfg_at(i);
    for (int k = 0; k < s->nvalues; k++)
        if (!strcmp(s->values[k], v)) return k;
    return -1;
}
static void apply_preset_combos(int p) {   // p: 1 low, 2 balanced, 3 high
    for (size_t r = 0; r < sizeof presets / sizeof *presets; r++) {
        int i = setting_index(presets[r].key), k = i >= 0 ? value_index(i, presets[r].v[p - 1]) : -1;
        if (k >= 0 && combos[i]) SendMessageW(combos[i], CB_SETCURSEL, (WPARAM)k, 0);
    }
}
static int is_preset_key(const char *key) {
    for (size_t r = 0; r < sizeof presets / sizeof *presets; r++)
        if (!strcmp(presets[r].key, key)) return 1;
    return 0;
}
// First start (no splatoon.ini yet): a preset for the graphics card - Low for integrated graphics or under 3 GB of
// video memory, Balanced otherwise. The card is read through DXGI (the one with the most dedicated memory).
static void first_run_preset(void) {
    IDXGIFactory1 *f = NULL;
    size_t vram = 0;
    UINT vendor = 0;
    wchar_t name[128] = L"?";
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
        IDXGIAdapter1 *a;
        for (UINT n = 0; f->lpVtbl->EnumAdapters1(f, n, &a) != DXGI_ERROR_NOT_FOUND; n++) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->lpVtbl->GetDesc1(a, &d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                d.DedicatedVideoMemory > vram)
                vram = d.DedicatedVideoMemory, vendor = d.VendorId, wcsncpy(name, d.Description, 127);
            a->lpVtbl->Release(a);
        }
        f->lpVtbl->Release(f);
    }
    int p = vram && vram < (3ull << 30) ? 1 : 2;
    for (size_t r = 0; r < sizeof presets / sizeof *presets; r++) {
        Setting *s = nx2_cfg_find(presets[r].key);
        if (s) snprintf(s->value, sizeof s->value, "%s", presets[r].v[p - 1]);
    }
    Setting *ps = nx2_cfg_find("preset");
    if (ps) snprintf(ps->value, sizeof ps->value, "%s", p == 1 ? "low" : "balanced");
    nx2_cfg_save();
    (void)vendor, (void)name;
}

static void update_dirty(void) {
    int d = 0;
    for (int i = 0; i < nx2_cfg_count(); i++) {
        int k = combo_value(i);
        Setting *s = nx2_cfg_at(i);
        if (k >= 0 && k < s->nvalues && strcmp(s->values[k], s->value)) d = 1;
    }
    if (d != dirty) dirty = d, InvalidateRect(w_main, NULL, FALSE);
}
static void save_settings(void) {
    for (int i = 0; i < nx2_cfg_count(); i++) {
        int k = combo_value(i);
        Setting *s = nx2_cfg_at(i);
        if (k >= 0 && k < s->nvalues) snprintf(s->value, sizeof s->value, "%s", s->values[k]);
    }
    nx2_cfg_save();
    dirty = 0;
}
static void relang(void) {
    lang = nx2_cfg_lang();
    for (int i = 0; i < nx2_cfg_count(); i++)
        if (combos[i]) fill_combo(i);
    InvalidateRect(w_main, NULL, FALSE);
    InvalidateRect(w_page, NULL, FALSE);
}

// ---- the pages (painted into the scrolling area; y grows from 0, the scroll is applied by the caller)
static int page_header(HDC dc, int x, int y, int w, const char *title, const char *sub) {
    int tw = text_w(dc, f_h1, title);
    round_rect(dc, x - S(6), y + S(30), tw + S(12), S(14), S(7), RGB(122, 47, 240), 0, 0);   // a purple ink stroke under it
    ink_splat(dc, x + tw + S(10), y + S(36), S(10), C_INK_A, 255, 53);
    int h = text(dc, f_h1, C_TEXT, x, y, w, title, DT_SINGLELINE);
    h += S(10);
    h += text(dc, f_body, C_DIM, x, y + h, w, sub, DT_WORDBREAK);
    return h + S(24);
}

static int setup_row(HDC dc, int x, int y, int w, const char *k, const char *v, const char *v2, int draw) {
    int kw = S(118), h = text_h(dc, f_small, w - kw, v, DT_WORDBREAK);
    if (v2) h += S(2) + text_h(dc, f_mono, w - kw, v2, DT_WORDBREAK | DT_PATH_ELLIPSIS);
    if (draw) {
        text(dc, f_body, C_DIM, x, y, kw, k, DT_SINGLELINE);
        int hh = text(dc, f_small, C_FAINT, x + kw, y + S(2), w - kw, v, DT_WORDBREAK);
        if (v2) text(dc, f_mono, C_TEXT, x + kw, y + S(4) + hh, w - kw, v2, DT_WORDBREAK);
    }
    return (h < S(20) ? S(20) : h) + S(12);
}

static int paint_play(HDC dc, int x, int y0, int w) {
    int y = y0 + page_header(dc, x, y0, w, "Splatoon Raiders",
                             T("Port nativo para PC. Seus arquivos do jogo ficam onde estão.",
                               "Native PC port. Your game files stay where they are."));
    char msg[1600];
    if (ck_cur.state != CK_OK && ck_cur.state != CK_BUSY) {
        if (ck_cur.state == CK_UNSET)
            snprintf(msg, sizeof msg, "%s", T("Escolha os arquivos do jogo (a sua cópia, atualização " SUPPORTED_VERSION ") para jogar.",
                                              "Choose the game files (your own copy, update " SUPPORTED_VERSION ") to play."));
        else
            snprintf(msg, sizeof msg, T("Os arquivos do jogo em %s não servem: %s", "The game files at %s are not usable: %s"),
                     ck_cur.dir, check_text(&ck_cur));
        y += banner(dc, x, y, w, B_ERROR, msg, 1) + S(16);
    }
    if (su_error[0]) {
        snprintf(msg, sizeof msg, T("A configuração falhou: %s (detalhes em setup_work\\setup.log). Ela continua de onde parou se "
                                    "você clicar em Configurar de novo.",
                                    "The Set up failed: %s (details in setup_work\\setup.log). It continues where it stopped if "
                                    "you click Set up again."), su_error);
        y += banner(dc, x, y, w, B_ERROR, msg, 1) + S(16);
    }
    if (!exe_ok && setup_ok) {
        y += banner(dc, x, y, w, B_INFO,
                    T("Falta gerar o jogo (SplatoonRaiders.exe) a partir dos seus arquivos. Clique em Configurar: o launcher "
                      "converte o código do jogo e o compila neste PC, uma vez só. Leva de 30 a 70 minutos e precisa de uns "
                      "5 GB livres; se for interrompido, continua de onde parou.",
                      "The game (SplatoonRaiders.exe) still has to be built from your game files. Click Set up: the launcher "
                      "converts the game's code and compiles it on this PC, once. It takes 30 to 70 minutes and needs about "
                      "5 GB free; if stopped, it continues where it was."), 1) + S(16);
        if (ck_cur.state == CK_OK) {
            button(dc, zp, &nzp, hot_page, x, y, Z_SETUP, I_TOOL, T("Configurar", "Set up"), 1, !su_state && !sp_state, 0);
            y += S(40) + S(24);
        }
    } else if (!exe_ok) {
        char d[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, exe_dir, -1, d, sizeof d, NULL, NULL);
        snprintf(msg, sizeof msg,
                 T("SplatoonRaiders.exe não foi encontrado em %s. Coloque o launcher ao lado do SplatoonRaiders.exe gerado "
                   "a partir da sua cópia do jogo (veja o README).",
                   "SplatoonRaiders.exe was not found in %s. Put the launcher next to the SplatoonRaiders.exe built from "
                   "your own copy of the game (see the README)."), d);
        y += banner(dc, x, y, w, B_ERROR, msg, 1) + S(16);
    }
    if (sp_error[0]) {
        snprintf(msg, sizeof msg, T("A preparação dos shaders falhou: %s", "The shader preparation failed: %s"), sp_error);
        y += banner(dc, x, y, w, B_WARN, msg, 1) + S(16);
    }
    if (upd_tag[0]) {   // a newer release on GitHub (update_thread)
        snprintf(msg, sizeof msg, T("Nova versão disponível: %s (esta é a " APP_VERSION ").",
                                    "A new version is out: %s (this one is " APP_VERSION ")."), upd_tag);
        y += banner(dc, x, y, w, B_INFO, msg, 1) + S(8);
        button(dc, zp, &nzp, hot_page, x, y, Z_UPDATE, I_FOLDER_OPEN, T("Abrir página de download", "Open the download page"), 0, 1, 0);
        y += S(40) + S(16);
    }
    if (status_msg[0]) {
        char m[600];
        WideCharToMultiByte(CP_UTF8, 0, status_msg, -1, m, sizeof m, NULL, NULL);
        y += banner(dc, x, y, w, B_WARN, m, 1) + S(16);
    }
    if (ck_cur.state != CK_OK && ck_cur.state != CK_BUSY) {
        button(dc, zp, &nzp, hot_page, x, y, Z_CHOOSE, I_FOLDER_OPEN, T("Escolher arquivos do jogo", "Choose game files"), 1, 1, 0);
        y += S(40) + S(24);
    }
    int can = ck_cur.state == CK_OK && exe_ok && !prep_state && !sp_state && !su_state;
    int bw = button(dc, zp, &nzp, hot_page, x, y, Z_PLAY, I_PLAY, T("Jogar", "Play"), 1, can, 1);
    {   // status pill
        char st[200];
        if (su_state == 1) snprintf(st, sizeof st, "%s", T("Convertendo o código do jogo...", "Converting the game's code..."));
        else if (su_state == 2) snprintf(st, sizeof st, "%s", T("Preparando a compilação...", "Preparing the build..."));
        else if (su_state == 3) {
            int n = snprintf(st, sizeof st, T("Compilando %ld/%ld", "Compiling %ld/%ld"), su_done, su_total);
            // the time left, from the rate so far (in source bytes: the files differ a lot in size)
            ULONGLONG el = su_t0 ? GetTickCount64() - su_t0 : 0;
            LONG d = su_kb_done - su_kb0, left = su_kb_total - su_kb_done;
            if (el > 20000 && d > 0 && (LONGLONG)d * 50 > su_kb_total - su_kb0 && left > 0) {
                ULONGLONG min = (ULONGLONG)((double)el * left / d / 60000.0 + 0.5);
                if (min < 1) snprintf(st + n, sizeof st - n, "%s", T(" · menos de 1 min", " · under 1 min"));
                else snprintf(st + n, sizeof st - n, T(" · cerca de %llu min", " · about %llu min left"), min);
            }
        }
        else if (su_state == 4) snprintf(st, sizeof st, "%s", T("Montando o SplatoonRaiders.exe...", "Linking SplatoonRaiders.exe..."));
        else if (sp_state == 1)
            snprintf(st, sizeof st, T("Lendo os arquivos do jogo %ld/%ld", "Reading the game files %ld/%ld"), sp_done, sp_total);
        else if (sp_state == 2)
            snprintf(st, sizeof st, T("Traduzindo shaders %ld/%ld", "Translating shaders %ld/%ld"), sp_done, sp_total);
        else if (prep_state == 1 && prep_total)
            snprintf(st, sizeof st, T("Preparando shaders %ld/%ld", "Preparing shaders %ld/%ld"), prep_done, prep_total);
        else if (prep_state == 1) snprintf(st, sizeof st, "%s", T("Preparando shaders...", "Preparing shaders..."));
        else if (prep_state == 2) snprintf(st, sizeof st, "%s", T("Abrindo o jogo...", "Starting the game..."));
        else if (ck_cur.state == CK_BUSY) snprintf(st, sizeof st, "%s", T("Verificando...", "Checking..."));
        else snprintf(st, sizeof st, "%s", can ? T("Pronto", "Ready") : T("Não está pronto", "Not ready"));
        int pw = text_w(dc, f_small, st) + S(26), ph = S(32), px = x + bw + S(14), py = y + (S(60) - ph) / 2;
        round_rect(dc, px, py, pw, ph, ph / 2, C_BTN, C_BTN_EDGE, 1);
        text(dc, f_small, C_DIM, px + S(13), py + (ph - text_h(dc, f_small, 300, st, DT_SINGLELINE)) / 2, pw, st, DT_SINGLELINE);
        LONG pd = su_state == 3 ? su_done : sp_state ? sp_done : prep_done, pt = su_state == 3 ? su_total : sp_state ? sp_total : prep_total;
        if ((su_state == 3 || sp_state || prep_state == 1) && pt) {   // a thin progress bar under the button
            int bx = x, by = y + S(68), bwid = S(300);
            round_rect(dc, bx, by, bwid, S(6), S(3), C_BTN, 0, 0);
            int f = (int)((double)bwid * pd / pt);
            if (f > S(6)) round_rect(dc, bx, by, f, S(6), S(3), C_ACCENT, 0, 0);
        }
    }
    y += S(60) + S(32);
    // Setup card
    int cx = x + S(24), cw = w - S(48);
    char files_v[300], files_p[MAX_PATH * 3] = "", shader_v[200], dir_v[200], dir_p[MAX_PATH * 3];
    snprintf(files_v, sizeof files_v, "%s",
             ck_cur.state == CK_OK ? T("prontos (atualização " SUPPORTED_VERSION ")", "ready (update " SUPPORTED_VERSION ")")
             : ck_cur.state == CK_BUSY ? T("verificando...", "checking...")
             : ck_cur.state == CK_UNSET ? T("não definidos", "not set") : T("não servem", "not usable"));
    if (ck_cur.dir[0]) snprintf(files_p, sizeof files_p, "%s", ck_cur.state == CK_OK ? ck_cur.resolved : ck_cur.dir);
    snprintf(shader_v, sizeof shader_v, "%s",
             prepared_ok ? T("traduzidos a partir dos arquivos do jogo; os já vistos também são compilados antes de cada partida",
                             "translated from the game files; the ones seen before are also compiled before each session")
                         : T("ainda não traduzidos (o launcher faz isso uma vez, antes de jogar)",
                             "not translated yet (the launcher does it once, before playing)"));
    snprintf(dir_v, sizeof dir_v, "%s", exe_ok ? "SplatoonRaiders.exe OK"
                                        : setup_ok ? T("SplatoonRaiders.exe ainda não gerado (Configurar)", "SplatoonRaiders.exe not built yet (Set up)")
                                                   : T("SplatoonRaiders.exe ausente", "SplatoonRaiders.exe missing"));
    WideCharToMultiByte(CP_UTF8, 0, exe_dir, -1, dir_p, sizeof dir_p, NULL, NULL);
    int ch = S(22) + S(44);
    ch += setup_row(dc, cx, 0, cw, T("Arquivos", "Game files"), files_v, files_p[0] ? files_p : NULL, 0);
    ch += setup_row(dc, cx, 0, cw, "Shaders", shader_v, NULL, 0);
    ch += setup_row(dc, cx, 0, cw, T("Programa", "Program"), dir_v, dir_p, 0);
    ch += S(12) + S(40) + S(24) + S(40) + S(12);
    card_bg(dc, x, y, w, ch);
    int yy = y + S(22);
    yy += card_title(dc, cx, yy, I_TOOL, T("Instalação", "Setup"));
    yy += setup_row(dc, cx, yy, cw, T("Arquivos", "Game files"), files_v, files_p[0] ? files_p : NULL, 1);
    yy += setup_row(dc, cx, yy, cw, "Shaders", shader_v, NULL, 1);
    yy += setup_row(dc, cx, yy, cw, T("Programa", "Program"), dir_v, dir_p, 1);
    yy += S(12);
    int b1 = button(dc, zp, &nzp, hot_page, cx, yy, Z_OPEN_DIR, I_FOLDER_OPEN, T("Abrir pasta do programa", "Open program folder"), 0, 1, 0);
    int b2 = button(dc, zp, &nzp, hot_page, cx + b1 + S(12), yy, Z_PREP, I_TOOL, T("Preparar shaders", "Prepare shaders"), 0,
                    ck_cur.state == CK_OK && exe_ok && !sp_state && !prep_state, 0);
    link_button(dc, zp, &nzp, hot_page, cx + b1 + S(12) + b2 + S(14), yy, Z_CHANGE, T("Trocar arquivos do jogo", "Change game files"));
    yy += S(40) + S(12);
    button(dc, zp, &nzp, hot_page, cx, yy, Z_IMPORT, I_SAVE, T("Importar save (Eden, yuzu ou console)", "Import save (Eden, yuzu or console)"), 0, 1, 0);
    return y + ch + S(32) - y0;
}

static int settings_rows_layout(HDC dc, int x, int y0, int w, int draw, int scroll_y) {
    // one card per category: rows of "label + description" on the left, the choice on the right
    int y = y0, n = nx2_cfg_count(), cw = S(330);
    static const wchar_t icons[NCAT] = {I_MONITOR, I_IMAGE, I_SPEED, I_PAD, I_GLOBE};
    for (int c = 0; c < NCAT; c++) {
        int cnt = 0;
        for (int i = 0; i < n; i++)
            if (shown_setting(i) && nx2_cfg_at(i)->cat == c) cnt++;
        if (!cnt) continue;
        int cx = x + S(24), inner = w - S(48), lw = inner - cw - S(24);
        int hcard = S(22) + S(44), k = 0;
        int rows_y[64], rows_h[64];
        for (int i = 0; i < n; i++) {
            if (!shown_setting(i) || nx2_cfg_at(i)->cat != c) continue;
            Setting *s = nx2_cfg_at(i);
            int h = text_h(dc, f_bodyb, lw, s->label[lang], DT_WORDBREAK) + S(4) +
                    text_h(dc, f_small, lw, s->desc[lang], DT_WORDBREAK);
            if (h < S(40)) h = S(40);
            rows_y[i] = hcard, rows_h[i] = h;
            hcard += h + S(28);
            k++;
        }
        hcard += S(2);
        if (draw) {
            card_bg(dc, x, y, w, hcard);
            card_title(dc, cx, y + S(22), icons[c], nx2_cfg_cat_name(c));
        }
        int kk = 0;
        for (int i = 0; i < n; i++) {
            if (!shown_setting(i) || nx2_cfg_at(i)->cat != c) continue;
            Setting *s = nx2_cfg_at(i);
            int ry = y + rows_y[i];
            if (draw) {
                if (kk) {   // a divider between rows
                    HPEN pen = CreatePen(PS_SOLID, 1, C_LINE);
                    HGDIOBJ op = SelectObject(dc, pen);
                    MoveToEx(dc, cx, ry - S(14), NULL);
                    LineTo(dc, cx + inner, ry - S(14));
                    SelectObject(dc, op);
                    DeleteObject(pen);
                }
                int hh = text(dc, f_bodyb, C_TEXT, cx, ry, lw, s->label[lang], DT_WORDBREAK);
                text(dc, f_small, C_FAINT, cx, ry + hh + S(4), lw, s->desc[lang], DT_WORDBREAK);
            } else if (combos[i]) {   // the choice, at the right of the row
                SetWindowPos(combos[i], NULL, cx + inner - cw, ry - scroll_y + (rows_h[i] - S(36)) / 2 - S(2), cw, S(300),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            kk++;
        }
        y += hcard + S(20);
    }
    return y - y0;
}
static int paint_settings(HDC dc, int x, int y0, int w) {
    int y = y0 + page_header(dc, x, y0, w, T("Configurações", "Settings"),
                             T("Ficam no splatoon.ini, ao lado do programa. O jogo lê esse arquivo ao abrir.",
                               "They are kept in splatoon.ini next to the program. The game reads that file when it starts."));
    y += banner(dc, x, y, w, B_INFO,
                T("Durante o jogo: F1 mostra o overlay de desempenho, F11 ou Alt+Enter alterna a tela cheia.",
                  "In game: F1 shows the performance overlay, F11 or Alt+Enter toggles fullscreen."), 1) + S(20);
    y += settings_rows_layout(dc, x, y, w, 1, 0);
    return y + S(8) - y0;
}

static int paint_files(HDC dc, int x, int y0, int w) {
    int y = y0 + page_header(dc, x, y0, w, T("Arquivos do jogo", "Game files"),
                             T("O launcher não inclui o jogo. Você precisa da sua própria cópia, extraída do seu console. "
                               "O launcher só lê essa pasta e nunca grava nela.",
                               "The launcher does not include the game. You need your own legally dumped copy. The "
                               "launcher only reads this folder and never writes to it."));
    Check *c = ck_new.dir[0] ? &ck_new : &ck_cur;
    // dump folder card
    int cx = x + S(24), cw = w - S(48);
    const char *help = T("A pasta da atualização " SUPPORTED_VERSION " com exefs/ (main, sdk, subsdk0, rtld, main.npdm) e "
                         "romfs/ (com o romfs base mesclado). Também serve a pasta acima dela, com updates/" SUPPORTED_VERSION "/.",
                         "The update " SUPPORTED_VERSION " folder with exefs/ (main, sdk, subsdk0, rtld, main.npdm) and romfs/ "
                         "(with the base romfs merged). The folder above it, with updates/" SUPPORTED_VERSION "/, works too.");
    int hh = text_h(dc, f_small, cw, help, DT_WORDBREAK);
    int ch = S(22) + S(44) + S(44) + S(14) + hh + S(24);
    card_bg(dc, x, y, w, ch);
    int yy = y + S(22);
    yy += card_title(dc, cx, yy, I_SEARCH, T("Pasta do jogo", "Dump folder"));
    {   // the path field and Browse
        int bwid = text_w(dc, f_btn, T("Procurar", "Browse")) + S(32) + S(26);
        int fw = cw - bwid - S(12);
        round_rect(dc, cx, yy, fw, S(44), S(9), C_FIELD, C_CARD_EDGE, 1);
        const char *p = c->dir[0] ? c->dir : T("(nenhuma)", "(none)");
        wchar_t wp[MAX_PATH * 2];
        wide(p, wp, MAX_PATH * 2);
        SelectObject(dc, f_body);
        SetTextColor(dc, c->dir[0] ? C_TEXT : C_FAINT);
        RECT r = {cx + S(14), yy, cx + fw - S(12), yy + S(44)};
        DrawTextW(dc, wp, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        zone(zp, &nzp, cx, yy, fw, S(44), Z_BROWSE, 1);
        button(dc, zp, &nzp, hot_page, cx + fw + S(12), yy + S(2), Z_BROWSE, I_FOLDER_OPEN, T("Procurar", "Browse"), 0, 1, 0);
        yy += S(44) + S(14);
    }
    text(dc, f_small, C_FAINT, cx, yy, cw, help, DT_WORDBREAK);
    y += ch + S(20);
    if (c->state != CK_NONE) {
        int k = c->state == CK_OK ? B_OK : c->state == CK_BUSY ? B_INFO : B_ERROR;
        y += banner(dc, x, y, w, k, check_text(c), 1) + S(20);
    }
    if (ck_new.dir[0]) {   // a new folder picked: use it or go back
        int bwid = button(dc, zp, &nzp, hot_page, x, y, Z_USE, I_OK, T("Usar estes arquivos", "Use these game files"), 1,
                          ck_new.state == CK_OK, 0);
        link_button(dc, zp, &nzp, hot_page, x + bwid + S(14), y, Z_BACK, T("Voltar", "Back"));
        y += S(40) + S(24);
    }
    // supported version card
    int rows = 4, ch2 = S(22) + S(44) + rows * S(32) + S(10);
    card_bg(dc, x, y, w, ch2);
    yy = y + S(22);
    yy += card_title(dc, cx, yy, I_INFO, T("Versão suportada", "Supported version"));
    const char *kv[4][2] = {{"Title ID", SUPPORTED_TITLE},
                            {T("Atualização", "Update"), SUPPORTED_VERSION},
                            {"exefs/main SHA-256", SUPPORTED_SHA256},
                            {T("Seu main", "Your main"), c->sha[0] ? c->sha : "-"}};
    for (int i = 0; i < rows; i++) {
        text(dc, f_body, C_DIM, cx, yy, S(170), kv[i][0], DT_SINGLELINE);
        COLORREF vc = i == 3 && c->sha[0] ? (strcmp(c->sha, SUPPORTED_SHA256) ? C_RED : C_GREEN) : C_TEXT;
        text(dc, f_mono, vc, cx + S(176), yy + S(2), cw - S(176), kv[i][1], DT_SINGLELINE | DT_END_ELLIPSIS);
        yy += S(32);
    }
    y += ch2 + S(20);
    return y - y0;
}

static int paint_about(HDC dc, int x, int y0, int w) {
    int y = y0 + page_header(dc, x, y0, w, T("Sobre", "About"),
                             T("Launcher do port de Splatoon Raiders para PC (uma recompilação estática, SplatoonRaiders.exe).",
                               "Launcher for the Splatoon Raiders PC port (a static recompilation, SplatoonRaiders.exe)."));
    int cx = x + S(24), ch = S(22) + S(44) + 4 * S(32) + S(10);
    card_bg(dc, x, y, w, ch);
    int yy = y + S(22);
    yy += card_title(dc, cx, yy, I_INFO, T("Splatoon Raiders Launcher", "Splatoon Raiders Launcher"));
    const char *kv[4][2] = {{T("Versão", "Version"), APP_VERSION},
                            {T("Licença", "Licence"), "GPL-3.0-or-later"},
                            {T("Feito com", "Built with"), "C, Win32, Vulkan"},
                            {T("Jogo suportado", "Supported game"), T("atualização " SUPPORTED_VERSION, "update " SUPPORTED_VERSION)}};
    for (int i = 0; i < 4; i++) {
        text(dc, f_body, C_DIM, cx, yy, S(160), kv[i][0], DT_SINGLELINE);
        text(dc, f_body, C_TEXT, cx + S(166), yy, w - S(220), kv[i][1], DT_SINGLELINE);
        yy += S(32);
    }
    y += ch + S(22);
    y += banner(dc, x, y, w, B_WARN,
                T("Nenhum arquivo do jogo incluído. Você precisa da sua própria cópia do jogo. O launcher e o port não "
                  "contêm dados do jogo, e este projeto não é afiliado nem endossado pela Nintendo.",
                  "No game files included. You need your own legally dumped copy of the game. The launcher and the port "
                  "contain no game data, and this project is not affiliated with or endorsed by Nintendo."), 1) + S(20);
    return y - y0;
}

// ---- the scrolling page area
static int save_bar_h(void) { return page == P_SETTINGS ? S(84) : 0; }
static void layout_page(void) {
    RECT r;
    GetClientRect(w_main, &r);
    MoveWindow(w_page, S(240), 0, r.right - S(240), r.bottom - save_bar_h(), TRUE);
    for (int i = 0; i < nx2_cfg_count(); i++)
        if (combos[i]) ShowWindow(combos[i], page == P_SETTINGS ? SW_SHOW : SW_HIDE);
}
static int content_x(void) { return S(36); }
static int content_w(void) {
    RECT r;
    GetClientRect(w_page, &r);
    int w = r.right - S(36) - S(36);
    return w > S(900) ? S(900) : w;
}
static void place_combos(void) {
    if (page != P_SETTINGS) return;
    HDC dc = GetDC(w_page);
    int x = content_x(), w = content_w(), y = S(32);
    // the same layout as paint_settings, without drawing
    y += text_h(dc, f_h1, w, "X", DT_SINGLELINE) + S(10) +
         text_h(dc, f_body, w, T("Ficam no splatoon.ini, ao lado do programa. O jogo lê esse arquivo ao abrir.",
                                 "They are kept in splatoon.ini next to the program. The game reads that file when it starts."),
                DT_WORDBREAK) + S(24);
    y += banner(dc, x, y, w, B_INFO,
                T("Durante o jogo: F1 mostra o overlay de desempenho, F11 ou Alt+Enter alterna a tela cheia.",
                  "In game: F1 shows the performance overlay, F11 or Alt+Enter toggles fullscreen."), 0) + S(20);
    settings_rows_layout(dc, x, y, w, 0, scroll[P_SETTINGS]);
    ReleaseDC(w_page, dc);
}
static void scroll_by(int d) {
    RECT r;
    GetClientRect(w_page, &r);
    int max = content_h[page] - r.bottom;
    if (max < 0) max = 0;
    int s = scroll[page] + d;
    if (s < 0) s = 0;
    if (s > max) s = max;
    if (s != scroll[page]) {
        scroll[page] = s;
        place_combos();
        InvalidateRect(w_page, NULL, FALSE);
        UpdateWindow(w_page);
    }
}

static void paint_page(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    HDC dc = CreateCompatibleDC(wdc);
    int vh = 6000;   // painted tall, shown from the scroll position
    HBITMAP bm = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ obm = SelectObject(dc, bm);
    SetBkMode(dc, TRANSPARENT);
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    // ink on the walls: a big yellow splat top right, a purple one bottom left, a pink one (behind the cards)
    ink_splat(dc, r.right + S(20), S(-10), S(130), C_INK_A, 255, 7 + page);   // solid yellow ink in the corner
    ink_splat(dc, S(40), r.bottom - S(30), S(230), C_INK_B, 70, 19 + page);
    ink_splat(dc, r.right - S(160), r.bottom - S(120), S(90), C_INK_C, 40, 31 + page);
    // the content is drawn shifted up by the scroll
    SetViewportOrgEx(dc, 0, -scroll[page], NULL);
    nzp = 0;
    int x = content_x(), w = content_w(), y = S(32), hgt = 0;
    switch (page) {
    case P_PLAY: hgt = paint_play(dc, x, y, w); break;
    case P_SETTINGS: hgt = paint_settings(dc, x, y, w); break;
    case P_FILES: hgt = paint_files(dc, x, y, w); break;
    case P_ABOUT: hgt = paint_about(dc, x, y, w); break;
    }
    (void)vh;
    content_h[page] = y + hgt + S(16);
    for (int i = 0; i < nzp; i++) OffsetRect(&zp[i].r, 0, -scroll[page]);
    SetViewportOrgEx(dc, 0, 0, NULL);
    if (content_h[page] > r.bottom) {   // a thin scroll bar
        int track = r.bottom - S(16), th = track * r.bottom / content_h[page];
        int ty = S(8) + (track - th) * scroll[page] / (content_h[page] - r.bottom);
        round_rect(dc, r.right - S(10), ty, S(5), th, S(2), C_CARD_EDGE, 0, 0);
    }
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void paint_main(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, r.right, r.bottom);
    HGDIOBJ obm = SelectObject(dc, bm);
    SetBkMode(dc, TRANSPARENT);
    HBRUSH b = CreateSolidBrush(C_BG);
    FillRect(dc, &r, b);
    DeleteObject(b);
    RECT side = {0, 0, S(240), r.bottom};
    b = CreateSolidBrush(C_SIDE);
    FillRect(dc, &side, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, C_LINE);
    HGDIOBJ op = SelectObject(dc, pen);
    MoveToEx(dc, S(240) - 1, 0, NULL);
    LineTo(dc, S(240) - 1, r.bottom);
    SelectObject(dc, op);
    DeleteObject(pen);
    nzm = 0;
    ink_splat(dc, S(112), S(50), S(84), C_INK_A, 255, 3);   // the brand on a splat of yellow ink
    ink_splat(dc, S(200), r.bottom - S(90), S(110), C_INK_B, 90, 11);
    ink_splat(dc, S(30), r.bottom - S(170), S(50), C_INK_C, 70, 23);
    glyph(dc, f_icon, C_ON_ACCENT, S(36), S(30), S(28), I_GAME);
    text(dc, f_brand, C_ON_ACCENT, S(70), S(22), S(160), "SPLATOON", DT_SINGLELINE);
    text(dc, f_brand, RGB(122, 47, 240), S(70), S(46), S(160), "RAIDERS", DT_SINGLELINE);
    static const wchar_t icons[NPAGE] = {I_PLAY, I_GEAR, I_FOLDER, I_INFO};
    const char *names[NPAGE] = {T("Jogar", "Play"), T("Configurações", "Settings"), T("Arquivos do jogo", "Game files"),
                                T("Sobre", "About")};
    for (int i = 0; i < NPAGE; i++) {
        int y = S(150) + i * S(52), x = S(14), w = S(240) - S(28), hh = S(44);
        int hot = hot_main >= 0 && zm[hot_main].id == Z_NAV + i;
        if (i == page) round_rect(dc, x, y, w, hh, S(22), C_NAV_SEL, 0, 0);
        else if (hot) round_rect(dc, x, y, w, hh, S(9), C_NAV_HOT, 0, 0);
        glyph(dc, f_icon_s, i == page ? C_ON_ACCENT : C_DIM, x + S(12), y + (hh - S(24)) / 2, S(24), icons[i]);
        text(dc, f_nav, i == page ? C_ON_ACCENT : C_DIM, x + S(48), y + (hh - text_h(dc, f_nav, 200, names[i], DT_SINGLELINE)) / 2,
             w - S(52), names[i], DT_SINGLELINE);
        zone(zm, &nzm, x, y, w, hh, Z_NAV + i, 1);
    }
    text(dc, f_small, C_FAINT, S(24), r.bottom - S(36), S(200), "v" APP_VERSION "  ·  GPL-3.0", DT_SINGLELINE);
    if (page == P_SETTINGS) {   // the save bar
        int bx = S(240) + S(36), by = r.bottom - S(76), bw = r.right - bx - S(36), bh = S(62);
        if (bw > S(900)) bw = S(900);
        round_rect(dc, bx, by, bw, bh, S(14), C_CARD, C_CARD_EDGE, 1);
        text(dc, f_body, dirty ? C_TEXT : C_DIM, bx + S(20), by + (bh - text_h(dc, f_body, 300, "X", DT_SINGLELINE)) / 2, S(320),
             dirty ? T("Alterações não salvas", "Unsaved changes") : T("Sem alterações", "No changes"), DT_SINGLELINE);
        int sw = text_w(dc, f_btn, T("Salvar", "Save")) + S(32) + S(26);
        int rw = text_w(dc, f_btn, T("Reverter", "Revert")) + S(32) + S(26);
        Zone *z = zm;
        button(dc, z, &nzm, hot_main, bx + bw - S(11) - sw, by + S(11), Z_SAVE, I_SAVE, T("Salvar", "Save"), 1, dirty, 0);
        button(dc, z, &nzm, hot_main, bx + bw - S(11) - sw - S(10) - rw, by + S(11), Z_REVERT, I_UNDO, T("Reverter", "Revert"), 0,
               dirty, 0);
    }
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

// ---- update check: the latest release of UPDATE_REPO on GitHub, asked once at start in the background (setting
// "updates"). A newer tag than APP_VERSION shows a banner on the Play page with a button to its download page.
#ifndef UPDATE_REPO
#define UPDATE_REPO "portsofbigN/last-shot-1"   // "owner/name" of the GitHub repository; empty: no check
#endif
#define WM_APP_UPDATE (WM_APP + 9)
static int ver_newer(const char *tag, const char *cur) {   // "v1.2.3" against "1.2.0"
    int a[3] = {0}, b[3] = {0};
    while (*tag && (*tag < '0' || *tag > '9')) tag++;
    sscanf(tag, "%d.%d.%d", &a[0], &a[1], &a[2]);
    sscanf(cur, "%d.%d.%d", &b[0], &b[1], &b[2]);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return 0;
}
static int json_str(const char *j, const char *key, char *out, size_t n) {   // "key": "value" (no escapes needed here)
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), '"');
    if (!p) return 0;
    const char *e = strchr(++p, '"');
    if (!e || (size_t)(e - p) >= n) return 0;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = 0;
    return 1;
}
static DWORD WINAPI update_thread(void *arg) {
    (void)arg;
    wchar_t path[200];
    swprintf(path, 200, L"/repos/%hs/releases/latest", UPDATE_REPO);
    HINTERNET s = WinHttpOpen(L"SplatoonRaidersRecomp-Launcher/" APP_VERSION_W, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE) : NULL;
    static char body[1 << 16];
    DWORD len = 0;
    if (r) WinHttpSetTimeouts(r, 5000, 5000, 5000, 5000);
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, NULL)) {
        DWORD got;
        while (len < sizeof body - 1 && WinHttpReadData(r, body + len, sizeof body - 1 - len, &got) && got) len += got;
    }
    body[len] = 0;
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    char tag[64], url[300];
    if (json_str(body, "tag_name", tag, sizeof tag) && json_str(body, "html_url", url, sizeof url) &&
        !strncmp(url, "https://github.com/", 19) && ver_newer(tag, APP_VERSION)) {
        snprintf(upd_tag, sizeof upd_tag, "%s", tag);
        snprintf(upd_url, sizeof upd_url, "%s", url);
        PostMessageW(w_main, WM_APP_UPDATE, 0, 0);
    }
    return 0;
}
static void start_update_check(void) {
    Setting *u = nx2_cfg_find("updates");
    if (!UPDATE_REPO[0] || (u && !strcmp(u->value, "0"))) return;
    CloseHandle(CreateThread(NULL, 0, update_thread, NULL, 0, NULL));
}
static void open_update(void) {
    wchar_t w[300];
    MultiByteToWideChar(CP_UTF8, 0, upd_url, -1, w, 300);
    ShellExecuteW(w_main, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

// ---- save import: a save from an emulator (Eden, yuzu and its forks) or a console dump (a folder with save.dat)
// becomes the port's save (profile/save/<account>). The save there before is moved to profile/save-backup.
#define SAVE_TITLE L"0400380020056000"
static int game_running(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e = {sizeof e};
    int found = 0;
    for (BOOL ok = Process32FirstW(s, &e); ok && !found; ok = Process32NextW(s, &e))
        found = !_wcsicmp(e.szExeFile, L"SplatoonRaiders.exe");
    CloseHandle(s);
    return found;
}
static int has_files(const wchar_t *dir, FILETIME *newest) {   // files directly in dir; newest write time
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            n++;
            if (newest && CompareFileTime(&fd.ftLastWriteTime, newest) > 0) *newest = fd.ftLastWriteTime;
        }
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}
// The newest emulator save of the game: <APPDATA>/<emulator>/nand/user/save/0000000000000000/<user>/<title>
static int find_emulator_save(wchar_t *out, wchar_t *emu) {
    static const wchar_t *emus[] = {L"eden", L"yuzu", L"suyu", L"citron", L"sudachi", L"torzu"};
    wchar_t app[MAX_PATH];
    if (!GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH)) return 0;
    FILETIME best = {0};
    int found = 0;
    for (size_t i = 0; i < sizeof emus / sizeof *emus; i++) {
        wchar_t base[MAX_PATH], pat[MAX_PATH];
        swprintf(base, MAX_PATH, L"%ls\\%ls\\nand\\user\\save\\0000000000000000", app, emus[i]);
        swprintf(pat, MAX_PATH, L"%ls\\*", base);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            wchar_t d[MAX_PATH];
            swprintf(d, MAX_PATH, L"%ls\\%ls\\" SAVE_TITLE, base, fd.cFileName);
            FILETIME t = {0};
            if (has_files(d, &t) && (!found || CompareFileTime(&t, &best) > 0)) {
                found = 1, best = t;
                wcscpy(out, d);
                wcscpy(emu, emus[i]);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found;
}
static int copy_tree(const wchar_t *from, const wchar_t *to) {
    CreateDirectoryW(to, NULL);
    wchar_t pat[MAX_PATH];
    swprintf(pat, MAX_PATH, L"%ls\\*", from);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t a[MAX_PATH], b[MAX_PATH];
        swprintf(a, MAX_PATH, L"%ls\\%ls", from, fd.cFileName);
        swprintf(b, MAX_PATH, L"%ls\\%ls", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(a, b);
        else ok &= CopyFileW(a, b, FALSE) != 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
// profile/save/<account>, the folder the game reads (account.uid made as the game would when it is missing)
static int save_target(wchar_t *out) {
    wchar_t prof[MAX_PATH], uidp[MAX_PATH];
    swprintf(prof, MAX_PATH, L"%lsprofile", exe_dir);
    CreateDirectoryW(prof, NULL);
    swprintf(uidp, MAX_PATH, L"%ls\\account.uid", prof);
    uint64_t uid[2] = {0, 0};   // lo, hi
    FILE *f = _wfopen(uidp, L"rb");
    if (f) {
        size_t got = fread(uid, 1, sizeof uid, f);
        fclose(f);
        if (got != sizeof uid) return 0;
    } else {
        if (BCryptGenRandom(NULL, (PUCHAR)uid, sizeof uid, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 || !(uid[0] | uid[1])) return 0;
        f = _wfopen(uidp, L"wb");
        if (!f) return 0;
        fwrite(uid, 1, sizeof uid, f);
        fclose(f);
    }
    swprintf(out, MAX_PATH, L"%ls\\save", prof);
    CreateDirectoryW(out, NULL);
    swprintf(out, MAX_PATH, L"%ls\\save\\%016llx%016llx", prof, (unsigned long long)uid[1], (unsigned long long)uid[0]);
    return 1;
}
static void import_save(void) {
    if (game_running()) {
        MessageBoxW(w_main, lang ? L"Close the game before importing a save." : L"Feche o jogo antes de importar um save.",
                    L"Splatoon Raiders Recomp", MB_ICONWARNING);
        return;
    }
    wchar_t src[MAX_PATH] = L"", emu[32] = L"", msg[1024];
    if (find_emulator_save(src, emu)) {
        swprintf(msg, 1024,
                 lang ? L"Found a %ls save of the game:\n%ls\n\nImport it? Your current save is kept in profile\\save-backup.\n\n"
                        L"No: choose another folder."
                      : L"Encontrei um save do jogo no %ls:\n%ls\n\nImportar? O save atual fica guardado em profile\\save-backup.\n\n"
                        L"Não: escolher outra pasta.",
                 emu, src);
        int r = MessageBoxW(w_main, msg, L"Splatoon Raiders Recomp", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return;
        if (r == IDNO) src[0] = 0;
    }
    if (!src[0]) {
        BROWSEINFOW bi = {0};
        bi.hwndOwner = w_main;
        bi.lpszTitle = lang ? L"Choose the save folder (the one with save.dat)" : L"Escolha a pasta do save (a que tem o save.dat)";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
        if (!id) return;
        int got = SHGetPathFromIDListW(id, src);
        CoTaskMemFree((void *)id);
        if (!got) return;
    }
    wchar_t chk[MAX_PATH];
    swprintf(chk, MAX_PATH, L"%ls\\save.dat", src);
    if (GetFileAttributesW(chk) == INVALID_FILE_ATTRIBUTES) {
        swprintf(msg, 1024, lang ? L"%ls has no save.dat: it is not a Splatoon Raiders save." :
                                   L"%ls não tem save.dat: não é um save de Splatoon Raiders.", src);
        MessageBoxW(w_main, msg, L"Splatoon Raiders Recomp", MB_ICONWARNING);
        return;
    }
    wchar_t dst[MAX_PATH];
    if (!save_target(dst)) {
        MessageBoxW(w_main, lang ? L"Could not read profile\\account.uid." : L"Não foi possível ler profile\\account.uid.",
                    L"Splatoon Raiders Recomp", MB_ICONERROR);
        return;
    }
    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {   // the current save to profile/save-backup/<time>-import
        wchar_t bk[MAX_PATH];
        SYSTEMTIME t;
        GetLocalTime(&t);
        swprintf(bk, MAX_PATH, L"%lsprofile\\save-backup", exe_dir);
        CreateDirectoryW(bk, NULL);
        swprintf(bk, MAX_PATH, L"%lsprofile\\save-backup\\%04u%02u%02u-%02u%02u%02u-import", exe_dir, t.wYear, t.wMonth, t.wDay,
                 t.wHour, t.wMinute, t.wSecond);
        if (!MoveFileW(dst, bk)) {
            swprintf(msg, 1024, lang ? L"Could not move the current save aside (error %lu): nothing changed." :
                                       L"Não foi possível guardar o save atual (erro %lu): nada foi alterado.", GetLastError());
            MessageBoxW(w_main, msg, L"Splatoon Raiders Recomp", MB_ICONERROR);
            return;
        }
    }
    if (!copy_tree(src, dst)) {
        MessageBoxW(w_main, lang ? L"Some files could not be copied. The previous save is in profile\\save-backup." :
                                   L"Alguns arquivos não foram copiados. O save anterior está em profile\\save-backup.",
                    L"Splatoon Raiders Recomp", MB_ICONERROR);
        return;
    }
    MessageBoxW(w_main, lang ? L"Save imported. The previous one is in profile\\save-backup." :
                               L"Save importado. O anterior está em profile\\save-backup.",
                L"Splatoon Raiders Recomp", MB_ICONINFORMATION);
}

// ---- actions
static void set_page(int p) {
    if (p == page) return;
    page = p;
    hot_page = -1;
    layout_page();
    place_combos();
    InvalidateRect(w_main, NULL, FALSE);
    InvalidateRect(w_page, NULL, FALSE);
}
static void browse(void) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = w_main;
    bi.lpszTitle = lang ? L"Choose the game folder (update " SUPPORTED_VERSION L", with exefs and romfs)"
                        : L"Escolha a pasta do jogo (atualização " SUPPORTED_VERSION L", com exefs e romfs)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t d[MAX_PATH];
    int got = SHGetPathFromIDListW(id, d);
    CoTaskMemFree((void *)id);
    if (!got) return;
    char u[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, d, -1, u, sizeof u, NULL, NULL);
    for (char *c = u; *c; c++)
        if (*c == '\\') *c = '/';
    start_check(&ck_new, u);
    set_page(P_FILES);
    InvalidateRect(w_page, NULL, FALSE);
}
static void use_new_files(void) {
    if (ck_new.state != CK_OK) return;
    Setting *g = nx2_cfg_find("game_dir");
    snprintf(g->value, sizeof g->value, "%s", ck_new.resolved);
    nx2_cfg_save();
    ck_cur = ck_new;
    snprintf(ck_cur.dir, sizeof ck_cur.dir, "%s", ck_new.resolved);
    memset(&ck_new, 0, sizeof ck_new);
    set_page(P_PLAY);
    refresh_files();
    if (!exe_ok && setup_ok) start_setup();   // the game built first (then its shaders)
    else if (!prepared_ok) start_shader_prep(0);   // the game's shaders, now that the files are known
    InvalidateRect(w_page, NULL, FALSE);
}

// ---- shader translation from the game files: the game's own program in its preparation mode, its progress read
// from its output ("scan i n", "prep i n", "done ...", "error ...")
static DWORD WINAPI sp_reader(LPVOID a) {
    HANDLE rd = a;
    char buf[4096], line[512];
    size_t ln = 0;
    DWORD got;
    int ok = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] != 10 && ln < sizeof line - 1) {
                if (buf[i] != 13) line[ln++] = buf[i];
                continue;
            }
            line[ln] = 0, ln = 0;
            long x = 0, y = 0;
            if (sscanf(line, "scan %ld %ld", &x, &y) == 2)
                InterlockedExchange(&sp_state, 1), InterlockedExchange(&sp_done, x), InterlockedExchange(&sp_total, y);
            else if (sscanf(line, "prep %ld %ld", &x, &y) == 2)
                InterlockedExchange(&sp_state, 2), InterlockedExchange(&sp_done, x), InterlockedExchange(&sp_total, y);
            else if (!strncmp(line, "done", 4)) ok = 1;
            else if (!strncmp(line, "error ", 6)) snprintf(sp_error, sizeof sp_error, "%s", line + 6);
        }
    }
    CloseHandle(rd);
    PostMessageW(w_main, WM_APP_SHADERS, (WPARAM)ok, 0);
    return 0;
}
static void start_shader_prep(int then_play) {
    if (sp_state || ck_cur.state != CK_OK || !exe_ok) return;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t game[MAX_PATH + 40], gd[MAX_PATH * 2];
    swprintf(game, MAX_PATH + 40, L"%lsSplatoonRaiders.exe", exe_dir);
    MultiByteToWideChar(CP_UTF8, 0, ck_cur.resolved, -1, gd, MAX_PATH * 2);
    SetEnvironmentVariableW(L"NX2_PREPARE_SHADERS", L"1");
    SetEnvironmentVariableW(L"NX2_GAME_DIR", gd);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr, si.hStdError = wr, si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(game, NULL, NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL, exe_dir,
                             &si, &pi);
    DWORD err = GetLastError();
    SetEnvironmentVariableW(L"NX2_PREPARE_SHADERS", NULL);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        swprintf(status_msg, 200,
                 lang ? L"Could not run SplatoonRaiders.exe to prepare the shaders (error %lu)."
                      : L"Não foi possível rodar o SplatoonRaiders.exe para preparar os shaders (erro %lu).",
                 err);
        InvalidateRect(w_page, NULL, FALSE);
        return;
    }
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    sp_error[0] = 0, status_msg[0] = 0;
    sp_then_play = then_play;
    InterlockedExchange(&sp_done, 0), InterlockedExchange(&sp_total, 0), InterlockedExchange(&sp_state, 1);
    SetTimer(w_main, 2, 150, NULL);
    CloseHandle(CreateThread(NULL, 0, sp_reader, rd, 0, NULL));
    InvalidateRect(w_page, NULL, FALSE);
}

static DWORD WINAPI su_reader(LPVOID a) {
    HANDLE rd = a;
    char buf[4096], line[600];
    size_t ln = 0;
    DWORD got;
    int ok = 0;
    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) {
            if (buf[i] != 10 && ln < sizeof line - 1) {
                if (buf[i] != 13) line[ln++] = buf[i];
                continue;
            }
            line[ln] = 0, ln = 0;
            long x = 0, y = 0;
            if (!strcmp(line, "step lift")) InterlockedExchange(&su_state, 1);
            else if (!strcmp(line, "step glue")) InterlockedExchange(&su_state, 2);
            else if (!strcmp(line, "step compile")) InterlockedExchange(&su_state, 3);
            else if (!strcmp(line, "step link")) InterlockedExchange(&su_state, 4);
            else if (sscanf(line, "build %ld %ld", &x, &y) == 2) InterlockedExchange(&su_done, x), InterlockedExchange(&su_total, y);
            else if (sscanf(line, "bytes %ld %ld", &x, &y) == 2) {
                if (!su_t0) su_t0 = GetTickCount64(), su_kb0 = x;
                InterlockedExchange(&su_kb_done, x), InterlockedExchange(&su_kb_total, y);
            }
            else if (!strcmp(line, "done")) ok = 1;
            else if (!strncmp(line, "error ", 6)) snprintf(su_error, sizeof su_error, "%s", line + 6);
        }
    }
    CloseHandle(rd);
    PostMessageW(w_main, WM_APP_SETUP, (WPARAM)ok, 0);
    return 0;
}
static void start_setup(void) {
    if (su_state || sp_state || prep_state || ck_cur.state != CK_OK || !setup_ok) return;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    wchar_t work[MAX_PATH + 40], logp[MAX_PATH + 60], cmd[MAX_PATH * 6], py[MAX_PATH + 40], gd[MAX_PATH * 2];
    swprintf(work, MAX_PATH + 40, L"%lssetup_work", exe_dir);
    CreateDirectoryW(work, NULL);
    swprintf(logp, MAX_PATH + 60, L"%ls\\setup.log", work);   // the lifter's and compiler's messages
    HANDLE log = CreateFileW(logp, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    swprintf(py, MAX_PATH + 40, L"%lssetup\\python\\python.exe", exe_dir);
    MultiByteToWideChar(CP_UTF8, 0, ck_cur.resolved, -1, gd, MAX_PATH * 2);
    wchar_t dir[MAX_PATH];   // without the trailing backslash (it would escape the closing quote)
    wcscpy(dir, exe_dir);
    size_t dl = wcslen(dir);
    if (dl && dir[dl - 1] == L'\\') dir[dl - 1] = 0;
    swprintf(cmd, MAX_PATH * 6,
             L"\"%ls\" -I \"%ls\\setup\\tools\\setup_build.py\" --game \"%ls\" --program \"%ls\" --work \"%ls\" --zig \"%ls\\setup\\zig\\zig.exe\"",
             py, dir, gd, dir, work, dir);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr, si.hStdError = log != INVALID_HANDLE_VALUE ? log : wr, si.hStdInput = NULL;
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                             NULL, exe_dir, &si, &pi);
    DWORD err = GetLastError();
    CloseHandle(wr);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        CloseHandle(rd);
        swprintf(status_msg, 200, lang ? L"Could not start the Set up (error %lu)." : L"Não foi possível iniciar a configuração (erro %lu).", err);
        InvalidateRect(w_page, NULL, FALSE);
        return;
    }
    // every process of the Set up in a job: closing the launcher stops them all (the Set up continues next time)
    if (!su_job) {
        su_job = CreateJobObjectW(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (su_job) SetInformationJobObject(su_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (su_job) AssignProcessToJobObject(su_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
    su_error[0] = 0, status_msg[0] = 0, sp_error[0] = 0;
    InterlockedExchange(&su_done, 0), InterlockedExchange(&su_total, 0), InterlockedExchange(&su_state, 1);
    su_t0 = 0, su_kb0 = 0, su_kb_done = su_kb_total = 0;
    SetTimer(w_main, 3, 250, NULL);
    CloseHandle(CreateThread(NULL, 0, su_reader, rd, 0, NULL));
    InvalidateRect(w_page, NULL, FALSE);
}

static void prep_progress(int done, int total, void *u) {
    (void)u;
    InterlockedExchange(&prep_done, done), InterlockedExchange(&prep_total, total);
}
static DWORD WINAPI prep_thread(LPVOID a) {
    (void)a;
    int nx2_pcache_prebuild(void (*)(int, int, void *), void *);
    nx2_pcache_prebuild(prep_progress, NULL);
    PostMessageW(w_main, WM_APP_PREPPED, 0, 0);
    return 0;
}
// After Play the launcher stays, without a window, until the game ends. When the game stops on an error (a non-zero
// exit code: closing the window exits with 0) it says so, with the error line from logs/splatoon.log, instead of the
// game window just disappearing.
static HANDLE game_proc;
static void watch_game(void) {
    if (!game_proc) return;
    WaitForSingleObject(game_proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(game_proc, &code);
    CloseHandle(game_proc);
    if (!code) return;
    wchar_t logp[MAX_PATH + 32], logs[MAX_PATH + 16];
    swprintf(logs, MAX_PATH + 16, L"%lslogs", exe_dir);
    swprintf(logp, MAX_PATH + 32, L"%ls\\splatoon.log", logs);
    char last[400] = "";
    FILE *f = _wfopen(logp, L"rb");
    if (f) {   // the last FATAL / crash line of the log's end
        static char buf[65536];
        fseek(f, 0, SEEK_END);
        long n = ftell(f), from = n > (long)sizeof buf - 1 ? n - (long)sizeof buf + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[got] = 0;
        for (char *l = buf; l && *l;) {
            char *e = strchr(l, 10);
            if (e) *e = 0;
            if (strstr(l, "FATAL") || strstr(l, "crash") || strstr(l, "exception")) snprintf(last, sizeof last, "%s", l);
            l = e ? e + 1 : NULL;
        }
    }
    wchar_t wl[400], msg[1400];
    MultiByteToWideChar(CP_UTF8, 0, last, -1, wl, 400);
    swprintf(msg, 1400,
             lang ? L"The game closed because of an error (code %lu).\n\n%ls%lsThe details are in:\n%ls\n\nWhen reporting it, "
                    L"attach that file. Open the logs folder?"
                  : L"O jogo fechou por causa de um erro (código %lu).\n\n%ls%lsOs detalhes estão em:\n%ls\n\nAo relatar o "
                    L"problema, anexe esse arquivo. Abrir a pasta de logs?",
             code, wl, wl[0] ? L"\n\n" : L"", logp);
    if (MessageBoxW(NULL, msg, L"Splatoon Raiders Recomp", MB_YESNO | MB_ICONERROR) == IDYES)
        ShellExecuteW(NULL, L"open", logs, NULL, NULL, SW_SHOWNORMAL);
}

static void start_game(void) {
    wchar_t game[MAX_PATH + 40];
    swprintf(game, MAX_PATH + 40, L"%lsSplatoonRaiders.exe", exe_dir);
    SetEnvironmentVariableW(L"NX2_LAUNCHED", L"1");   // the game skips its own menu and shader preparation
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(game, NULL, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
        swprintf(status_msg, 200, lang ? L"Could not start SplatoonRaiders.exe (error %lu)." : L"Não foi possível abrir o SplatoonRaiders.exe (erro %lu).",
                 GetLastError());
        prep_state = 0;
        InvalidateRect(w_page, NULL, FALSE);
        return;
    }
    CloseHandle(pi.hThread);
    game_proc = pi.hProcess;   // watched after the window closes (watch_game)
    DestroyWindow(w_main);
}
static void play(void) {
    refresh_files();
    if (ck_cur.state != CK_OK || !exe_ok || prep_state || sp_state) {
        InvalidateRect(w_page, NULL, FALSE);
        return;
    }
    if (dirty) save_settings();
    if (!prepared_ok) {   // the game's shaders translated first (once), then the game
        start_shader_prep(1);
        return;
    }
    status_msg[0] = 0;
    if (shaders_ok && !(getenv("NX2_PCACHE") && *getenv("NX2_PCACHE") == '0')) {
        prep_state = 1, prep_done = prep_total = 0;
        SetTimer(w_main, 1, 100, NULL);
        CloseHandle(CreateThread(NULL, 0, prep_thread, NULL, 0, NULL));
    } else {
        prep_state = 2;
        start_game();
    }
    InvalidateRect(w_page, NULL, FALSE);
}
static void click(int id) {
    if (id >= Z_NAV && id < Z_NAV + NPAGE) set_page(id - Z_NAV);
    else if (id == Z_PLAY) play();
    else if (id == Z_CHOOSE || id == Z_CHANGE) { set_page(P_FILES); browse(); }
    else if (id == Z_BROWSE) browse();
    else if (id == Z_USE) use_new_files();
    else if (id == Z_BACK) { memset(&ck_new, 0, sizeof ck_new); set_page(P_PLAY); InvalidateRect(w_page, NULL, FALSE); }
    else if (id == Z_PREP) start_shader_prep(0);
    else if (id == Z_SETUP) start_setup();
    else if (id == Z_IMPORT) import_save();
    else if (id == Z_UPDATE) open_update();
    else if (id == Z_OPEN_DIR) ShellExecuteW(w_main, L"open", exe_dir, NULL, NULL, SW_SHOWNORMAL);
    else if (id == Z_SAVE && dirty) { save_settings(); nx2_cfg_set_lang(nx2_cfg_find("language")->value); relang(); }
    else if (id == Z_REVERT && dirty) {
        reset_combos();
        nx2_cfg_set_lang(nx2_cfg_find("language")->value);
        dirty = 0;
        relang();
    }
}

static int hit(const Zone *z, int n, POINT p) {
    for (int i = n - 1; i >= 0; i--)
        if (z[i].on && PtInRect(&z[i].r, p)) return i;
    return -1;
}

// ---- combos in the dark theme
static void draw_combo_item(const DRAWITEMSTRUCT *di) {
    if (di->itemID == (UINT)-1) return;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int edit = (di->itemState & ODS_COMBOBOXEDIT) != 0, sel = (di->itemState & ODS_SELECTED) != 0;
    HBRUSH b = CreateSolidBrush(edit ? C_FIELD : sel ? C_NAV_SEL : C_CARD);
    FillRect(dc, &r, b);
    DeleteObject(b);
    wchar_t t[160] = L"";
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)t);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !edit && sel ? C_ON_ACCENT : C_TEXT);   // dark on the yellow highlight
    SelectObject(dc, f_body);
    r.left += S(10);
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static LRESULT CALLBACK page_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: paint_page(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEWHEEL: scroll_by(-GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA); return 0;
    case WM_SIZE: place_combos(); return 0;
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int k = hit(zp, nzp, p);
        if (k != hot_page) hot_page = k, InvalidateRect(h, NULL, FALSE);
        SetCursor(LoadCursor(NULL, k >= 0 ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) return TRUE;
        break;
    case WM_MOUSELEAVE: hot_page = -1, InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int k = hit(zp, nzp, p);
        SetFocus(h);
        if (k >= 0) click(zp[k].id);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_DOWN) scroll_by(S(40));
        else if (w == VK_UP) scroll_by(-S(40));
        else if (w == VK_NEXT) scroll_by(S(400));
        else if (w == VK_PRIOR) scroll_by(-S(400));
        else if (w == VK_RETURN && page == P_PLAY) play();
        return 0;
    case WM_DRAWITEM: draw_combo_item((const DRAWITEMSTRUCT *)l); return TRUE;
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT *)l)->itemHeight = S(30); return TRUE;
    case WM_COMMAND:
        if (HIWORD(w) == CBN_SELCHANGE) {
            int i = LOWORD(w) - ID_COMBO;
            Setting *s = nx2_cfg_at(i);
            if (s && !strcmp(s->key, "preset")) {   // the preset's values into their lists
                int k = combo_value(i);
                if (k > 0) apply_preset_combos(k);
            } else if (s && is_preset_key(s->key)) {   // changed by hand: no longer a preset
                int pi = setting_index("preset");
                if (pi >= 0 && combos[pi]) SendMessageW(combos[pi], CB_SETCURSEL, 0, 0);
            }
            if (s && !strcmp(s->key, "language")) {   // the new language at once
                int k = combo_value(i);
                if (k >= 0) nx2_cfg_set_lang(s->values[k]);
                relang();
                place_combos();
            }
            update_dirty();
        }
        return 0;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        static HBRUSH br;
        if (!br) br = CreateSolidBrush(C_CARD);
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_CARD);
        return (LRESULT)br;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void make_fonts(void) {
    HFONT *all[] = {&f_h1, &f_h2, &f_body, &f_bodyb, &f_small, &f_mono, &f_nav, &f_btn, &f_play, &f_icon, &f_icon_s, &f_icon_l, &f_brand};
    for (size_t i = 0; i < sizeof all / sizeof *all; i++)
        if (*all[i]) DeleteObject(*all[i]), *all[i] = NULL;
#define MF(px, wt, face) CreateFontW(-S(px), 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face)
    f_h1 = MF(36, FW_BLACK, L"Segoe UI Black");
    f_h2 = MF(19, FW_BLACK, L"Segoe UI Black");
    f_body = MF(15, FW_NORMAL, L"Segoe UI");
    f_bodyb = MF(15, FW_SEMIBOLD, L"Segoe UI Semibold");
    f_small = MF(13, FW_NORMAL, L"Segoe UI");
    f_mono = MF(13, FW_NORMAL, L"Consolas");
    f_nav = MF(15, FW_BOLD, L"Segoe UI");
    f_btn = MF(15, FW_BOLD, L"Segoe UI");
    f_play = MF(20, FW_BLACK, L"Segoe UI Black");
    f_brand = MF(21, FW_BLACK, L"Segoe UI Black");
    f_icon = MF(20, FW_NORMAL, icon_face);
    f_icon_s = MF(16, FW_NORMAL, icon_face);
    f_icon_l = MF(20, FW_NORMAL, icon_face);
#undef MF
    for (int i = 0; i < nx2_cfg_count(); i++)
        if (combos[i]) {
            SendMessageW(combos[i], WM_SETFONT, (WPARAM)f_body, TRUE);
            SendMessageW(combos[i], CB_SETITEMHEIGHT, (WPARAM)-1, S(30));
            SendMessageW(combos[i], CB_SETITEMHEIGHT, 0, S(30));
        }
}

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD t, LPARAM l) {
    (void)lf, (void)tm, (void)t;
    *(int *)l = 1;
    return 0;
}
static void pick_icon_font(void) {   // Windows 11: Segoe Fluent Icons; Windows 10: Segoe MDL2 Assets
    HDC dc = GetDC(NULL);
    int found = 0;
    LOGFONTW lf = {0};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy(lf.lfFaceName, L"Segoe Fluent Icons");
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    if (!found) icon_face = L"Segoe MDL2 Assets";
    ReleaseDC(NULL, dc);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: paint_main(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (w_page) layout_page(), place_combos();
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize = (POINT){S(860), S(560)};
        return 0;
    case WM_DPICHANGED: {
        ui = HIWORD(w) / 96.0;
        make_fonts();
        const RECT *r = (const RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        place_combos();
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int k = hit(zm, nzm, p);
        if (k != hot_main) hot_main = k, InvalidateRect(h, NULL, FALSE);
        SetCursor(LoadCursor(NULL, k >= 0 ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (HWND)w == h) return TRUE;
        break;
    case WM_MOUSELEAVE: hot_main = -1, InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int k = hit(zm, nzm, p);
        if (k >= 0) click(zm[k].id);
        return 0;
    }
    case WM_TIMER:
        if (prep_state == 1 || sp_state || su_state) InvalidateRect(w_page, NULL, FALSE);
        return 0;
    case WM_APP_UPDATE:
        layout_page();
        InvalidateRect(w_page, NULL, FALSE);
        return 0;
    case WM_APP_SETUP:
        KillTimer(h, 3);
        InterlockedExchange(&su_state, 0);
        refresh_files();
        if (!w && !su_error[0])
            snprintf(su_error, sizeof su_error, "%s", T("o processo parou antes de terminar", "the process stopped before finishing"));
        if (w && exe_ok && !prepared_ok) start_shader_prep(0);   // the game is built: its shaders next
        InvalidateRect(w_page, NULL, FALSE);
        return 0;
    case WM_APP_SHADERS:
        KillTimer(h, 2);
        InterlockedExchange(&sp_state, 0);
        refresh_files();
        if (!w && !sp_error[0])
            snprintf(sp_error, sizeof sp_error, "%s", T("o programa parou antes de terminar", "the program stopped before finishing"));
        InvalidateRect(w_page, NULL, FALSE);
        if (w && sp_then_play) play();
        sp_then_play = 0;
        return 0;
    case WM_APP_CHECKED:
        InvalidateRect(w_page, NULL, FALSE);
        return 0;
    case WM_APP_PREPPED:
        KillTimer(h, 1);
        prep_state = 2;
        InvalidateRect(w_page, NULL, FALSE);
        UpdateWindow(w_page);
        start_game();
        return 0;
    case WM_CLOSE:
        if (prep_state == 1 || sp_state) return 0;   // the shader preparation finishes first
        if (su_state && MessageBoxW(h, lang ? L"Stop the Set up? It continues where it stopped the next time."
                                            : L"Parar a configuração? Ela continua de onde parou na próxima vez.",
                                    L"Splatoon Raiders", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int launcher_ui_run(void) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ULONG_PTR gp;
    GpStartupInput gi = {1, NULL, FALSE, FALSE};
    GdiplusStartup(&gp, &gi, NULL);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    program_dir();
    wchar_t ini[MAX_PATH + 16];
    _snwprintf(ini, MAX_PATH + 16, L"%lssplatoon.ini", exe_dir);
    int first_run = GetFileAttributesW(ini) == INVALID_FILE_ATTRIBUTES;
    nx2_cfg_load();
    if (first_run) first_run_preset();
    nx2_cfg_set_lang(nx2_cfg_find("language")->value);
    lang = nx2_cfg_lang();
    refresh_files();
    pick_icon_font();
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = main_proc, wc.hInstance = inst, wc.lpszClassName = L"SplatoonRaidersLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(2));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    WNDCLASSW pc = {0};
    pc.lpfnWndProc = page_proc, pc.hInstance = inst, pc.lpszClassName = L"SplatoonRaidersLauncherPage";
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&pc);
    POINT pt = {0, 0};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    HMODULE shc = LoadLibraryA("shcore.dll");
    typedef HRESULT(WINAPI * GetDpiFn)(HMONITOR, int, UINT *, UINT *);
    GetDpiFn gd = shc ? (GetDpiFn)(void *)GetProcAddress(shc, "GetDpiForMonitor") : NULL;
    if (gd) gd(mon, 0, &dx, &dy);
    ui = dx / 96.0;
    make_fonts();
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r = {0, 0, S(1080), S(740)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dx);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int sw = mi.rcWork.right - mi.rcWork.left, sh = mi.rcWork.bottom - mi.rcWork.top;
    if (wh > sh) wh = sh;
    if (ww > sw) ww = sw;
    w_main = CreateWindowExW(0, wc.lpszClassName, L"Splatoon Raiders Launcher", style, mi.rcWork.left + (sw - ww) / 2,
                             mi.rcWork.top + (sh - wh) / 2, ww, wh, NULL, NULL, inst, NULL);
    {   // dark title bar
        BOOL dark = TRUE;
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetAttr set = dwm ? (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        if (set) set(w_main, 20, &dark, sizeof dark);
    }
    w_page = CreateWindowExW(0, pc.lpszClassName, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, w_main, NULL,
                             inst, NULL);
    typedef HRESULT(WINAPI * SetTheme)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = LoadLibraryA("uxtheme.dll");
    SetTheme theme = ux ? (SetTheme)(void *)GetProcAddress(ux, "SetWindowTheme") : NULL;
    for (int i = 0; i < nx2_cfg_count() && i < 64; i++) {
        if (!shown_setting(i)) continue;
        combos[i] = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST |
                                        CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                    0, 0, S(330), S(300), w_page, (HMENU)(INT_PTR)(ID_COMBO + i), inst, NULL);
        if (theme) theme(combos[i], L"DarkMode_CFD", NULL);
        fill_combo(i);
    }
    make_fonts();
    start_check(&ck_cur, nx2_cfg_find("game_dir")->value);
    layout_page();
    place_combos();
    ShowWindow(w_main, SW_SHOW);
    SetForegroundWindow(w_main);
    SetFocus(w_page);
    start_update_check();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_MOUSEWHEEL) {   // the wheel scrolls the page (never changes a closed combo's choice)
            int combo = 0;
            for (int i = 0; i < 64; i++)
                if (combos[i] && msg.hwnd == combos[i] && SendMessageW(combos[i], CB_GETDROPPEDSTATE, 0, 0)) combo = 1;
            if (!combo) msg.hwnd = w_page;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    watch_game();
    return 0;
}
