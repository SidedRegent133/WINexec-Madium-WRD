// language: C, file: wrd_executor.c, target: Windows 11 x64
// Executor front-end over wearedevs_exploit_api.dll, with an optional RakNet
// module and a tabbed, hand-drawn dark/light shell.
//
// WRD API exports (x64, MSVC convention):
//   initialize() void  attach() int  isAttached() bool  execute(const char*)
// RakNet module ABI (drop raknet.dll beside the exe; resolved at runtime):
//   init() int  enable() int  disable() void  send(const void*, int) int
//   isEnabled() int  shutdown() void
//   name aliases also tried: RakNet_Init / raknet_init, etc.
//
// CRT-free, no STL. build: see README.md (clang-cl + lld-link + SDK).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#pragma comment(lib, "comctl32.lib")

// ---- ids ------------------------------------------------------------------
#define IDC_EDIT     1001
#define IDC_ATTACH   1002
#define IDC_EXEC     1003
#define IDC_LOAD     1004
#define IDC_STATUS   1005
#define IDC_MIN      1006
#define IDC_MAX      1007
#define IDC_CLOSE    1008
#define IDC_THEME    1009
#define IDC_RAK_LOG  1010
#define IDC_RAK_LOAD 1011
#define IDC_RAK_SEND 1012
#define IDC_RAK_STATUS 1013
#define IDC_TG_RAK   1020
#define IDC_TG_AUTO  1021
#define IDC_TG_DARK  1022

#define HOTKEY_EXEC 1
#define TIMER_POLL  1
#define TIMER_ANIM  2

#define TITLE_H 42
#define TAB_H   50

// ---- theme ----------------------------------------------------------------
typedef struct {
    COLORREF bg, surface, editbg, text, subtext, border;
    COLORREF accent, accent_h, accent_p;
    COLORREF btn, btn_h, btn_p;
    COLORREF green, red, track_off, knob;
} Theme;

static Theme g_th;
static BOOL  g_dark = TRUE;
static HBRUSH g_brBg = 0;
static HBRUSH g_brEdit = 0;

static HWND g_hwnd, g_edit, g_status;

static void ApplyTheme(BOOL dark) {
    if (dark) {
        g_th.bg = RGB(22, 23, 26); g_th.surface = RGB(32, 34, 38); g_th.editbg = RGB(17, 18, 21);
        g_th.text = RGB(236, 236, 240); g_th.subtext = RGB(148, 151, 158); g_th.border = RGB(52, 55, 61);
        g_th.accent = RGB(88, 101, 242); g_th.accent_h = RGB(108, 120, 247); g_th.accent_p = RGB(72, 84, 220);
        g_th.btn = RGB(44, 46, 51); g_th.btn_h = RGB(56, 59, 65); g_th.btn_p = RGB(36, 38, 42);
        g_th.green = RGB(35, 165, 90); g_th.red = RGB(237, 66, 69);
        g_th.track_off = RGB(60, 63, 69); g_th.knob = RGB(240, 241, 244);
    } else {
        g_th.bg = RGB(245, 246, 249); g_th.surface = RGB(255, 255, 255); g_th.editbg = RGB(255, 255, 255);
        g_th.text = RGB(20, 21, 25); g_th.subtext = RGB(106, 109, 118); g_th.border = RGB(219, 221, 227);
        g_th.accent = RGB(79, 91, 232); g_th.accent_h = RGB(96, 108, 240); g_th.accent_p = RGB(64, 74, 200);
        g_th.btn = RGB(234, 235, 239); g_th.btn_h = RGB(224, 226, 231); g_th.btn_p = RGB(212, 214, 220);
        g_th.green = RGB(28, 150, 82); g_th.red = RGB(224, 58, 61);
        g_th.track_off = RGB(205, 208, 214); g_th.knob = RGB(255, 255, 255);
    }
    if (g_brBg) DeleteObject(g_brBg);
    if (g_brEdit) DeleteObject(g_brEdit);
    g_brBg = CreateSolidBrush(g_th.bg);
    g_brEdit = CreateSolidBrush(g_th.editbg);
    if (g_hwnd) InvalidateRect(g_hwnd, 0, TRUE);
}

// ---- WRD API --------------------------------------------------------------
typedef void          (__cdecl *initialize_t)(void);
typedef int           (__cdecl *attach_t)(void);
typedef unsigned char (__cdecl *isAttached_t)(void);
typedef void          (__cdecl *execute_t)(const char*);

static initialize_t p_initialize = 0;
static attach_t     p_attach = 0;
static isAttached_t p_isAttached = 0;
static execute_t    p_execute = 0;
static HMODULE      g_api = 0;

// ---- RakNet module --------------------------------------------------------
typedef int  (__cdecl *rak_init_t)(void);
typedef int  (__cdecl *rak_enable_t)(void);
typedef void (__cdecl *rak_disable_t)(void);
typedef int  (__cdecl *rak_send_t)(const void*, int);
typedef int  (__cdecl *rak_isenabled_t)(void);
typedef void (__cdecl *rak_shutdown_t)(void);

static HMODULE g_rak = 0;
static rak_init_t      p_rak_init = 0;
static rak_enable_t    p_rak_enable = 0;
static rak_disable_t   p_rak_disable = 0;
static rak_send_t      p_rak_send = 0;
static rak_isenabled_t p_rak_isenabled = 0;
static rak_shutdown_t  p_rak_shutdown = 0;

// ---- UI handles -----------------------------------------------------------
static HWND g_btn_attach, g_btn_load, g_btn_exec;
static HWND g_btn_min, g_btn_max, g_btn_close, g_btn_theme;
static HWND g_rak_log, g_rak_load, g_rak_send, g_rak_status;
static HWND g_tg_rak, g_tg_auto, g_tg_dark;

static HFONT g_fontTitle, g_fontUI, g_fontCode, g_fontGlyph, g_fontH2;

static int   g_tab = 0;              // 0 exec, 1 raknet, 2 settings
static RECT  g_tabRect[3];
static float g_underX = 20.0f;       // animated underline left
static float g_underW = 120.0f;

static BOOL  g_attached = FALSE;
static const wchar_t* g_status_text = L"loading API...";

// toggle state + animation (0..1 knob progress)
static BOOL  g_setRak = FALSE, g_setAuto = FALSE;
static float g_animRak = 0.0f, g_animAuto = 0.0f, g_animDark = 1.0f;
static BOOL  g_animating = FALSE;

static RECT g_editRect = { 0,0,0,0 };

// ---- heap helpers ---------------------------------------------------------
static void* xalloc(unsigned __int64 n) { return HeapAlloc(GetProcessHeap(), 0, n); }
static void  xfree(void* p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static void SetStatus(const wchar_t* s) {
    g_status_text = s;
    if (g_status) InvalidateRect(g_status, 0, TRUE);
}

// append a line to the RakNet log
static void RakLog(const wchar_t* line) {
    if (!g_rak_log) return;
    int len = GetWindowTextLengthW(g_rak_log);
    SendMessageW(g_rak_log, EM_SETSEL, len, len);
    SendMessageW(g_rak_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_rak_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_rak_log, EM_SCROLLCARET, 0, 0);
}

static void RakLogA(const char* prefix, const wchar_t* tail) {
    wchar_t buf[256];
    lstrcpynW(buf, L"", 256);
    if (prefix) { int n = MultiByteToWideChar(CP_UTF8, 0, prefix, -1, buf, 250); (void)n; }
    if (tail) lstrcatW(buf, tail);
    RakLog(buf);
}

static wchar_t* ReadEdit(void) {
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) return 0;
    wchar_t* buf = (wchar_t*)xalloc((unsigned __int64)(len + 1) * sizeof(wchar_t));
    if (!buf) return 0;
    GetWindowTextW(g_edit, buf, len + 1);
    return buf;
}

static char* WideToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    if (n <= 0) return 0;
    char* out = (char*)xalloc((unsigned __int64)n);
    if (!out) return 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, 0, 0);
    return out;
}

// ---- module loading -------------------------------------------------------
static void SplitDir(wchar_t* path) {
    int i = lstrlenW(path);
    while (i > 0 && path[i - 1] != L'\\' && path[i - 1] != L'/') i--;
    path[i] = 0;
}

static HMODULE LoadFromPaths(const wchar_t* leaf, BOOL also_downloads) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(0, exe, MAX_PATH);
    SplitDir(exe);

    wchar_t local[MAX_PATH];
    lstrcpynW(local, exe, MAX_PATH);
    lstrcatW(local, leaf);

    wchar_t downloads[MAX_PATH];
    downloads[0] = 0;
    wchar_t user[MAX_PATH];
    if (also_downloads && GetEnvironmentVariableW(L"USERPROFILE", user, MAX_PATH)) {
        lstrcpynW(downloads, user, MAX_PATH);
        lstrcatW(downloads, L"\\Downloads\\");
        lstrcatW(downloads, leaf);
    }

    const wchar_t* cands[3];
    cands[0] = local;
    cands[1] = downloads[0] ? downloads : 0;
    cands[2] = leaf;
    int k;
    for (k = 0; k < 3; k++) {
        if (!cands[k]) continue;
        HMODULE m = LoadLibraryW(cands[k]);
        if (m) return m;
    }
    return 0;
}

static BOOL LoadApi(void) {
    if (g_api) return TRUE;
    g_api = LoadFromPaths(L"wearedevs_exploit_api.dll", TRUE);
    if (!g_api) return FALSE;
    p_initialize = (initialize_t)GetProcAddress(g_api, "initialize");
    p_attach     = (attach_t)    GetProcAddress(g_api, "attach");
    p_isAttached = (isAttached_t)GetProcAddress(g_api, "isAttached");
    p_execute    = (execute_t)   GetProcAddress(g_api, "execute");
    if (!p_initialize || !p_attach || !p_isAttached || !p_execute) return FALSE;
    p_initialize();
    return TRUE;
}

// ---- RakNet enable/disable ------------------------------------------------
static FARPROC RakResolve(HMODULE m, const char* a, const char* b) {
    FARPROC p = GetProcAddress(m, a);
    if (!p && b) p = GetProcAddress(m, b);
    return p;
}

static BOOL RakNetLoadNow(void) {
    if (g_rak) return TRUE;
    g_rak = LoadFromPaths(L"raknet.dll", TRUE);
    if (!g_rak) g_rak = LoadFromPaths(L"RakNet.dll", TRUE);
    if (!g_rak) { RakLog(L"[raknet] module not found (drop raknet.dll beside the exe)"); return FALSE; }

    p_rak_init      = (rak_init_t)     RakResolve(g_rak, "init", "RakNet_Init");
    p_rak_enable    = (rak_enable_t)   RakResolve(g_rak, "enable", "RakNet_Enable");
    p_rak_disable   = (rak_disable_t)  RakResolve(g_rak, "disable", "RakNet_Disable");
    p_rak_send      = (rak_send_t)     RakResolve(g_rak, "send", "RakNet_Send");
    p_rak_isenabled = (rak_isenabled_t)RakResolve(g_rak, "isEnabled", "RakNet_IsEnabled");
    p_rak_shutdown  = (rak_shutdown_t) RakResolve(g_rak, "shutdown", "RakNet_Shutdown");

    if (p_rak_init) { int r = p_rak_init(); if (r != 0) RakLog(L"[raknet] init() != 0"); }
    if (p_rak_enable) { int r = p_rak_enable(); RakLog(r ? L"[raknet] enable() failed" : L"[raknet] enabled"); }
    else RakLog(L"[raknet] no enable() export");
    return TRUE;
}

static void RakNetUnload(void) {
    if (!g_rak) return;
    if (p_rak_disable) p_rak_disable();
    if (p_rak_shutdown) p_rak_shutdown();
    FreeLibrary(g_rak);
    g_rak = 0; p_rak_init = 0; p_rak_enable = 0; p_rak_disable = 0;
    p_rak_send = 0; p_rak_isenabled = 0; p_rak_shutdown = 0;
    RakLog(L"[raknet] unloaded");
}

static void UpdateRakStatus(void) {
    if (!g_rak_status) return;
    const wchar_t* s;
    if (!g_setRak) s = L"RakNet: disabled (enable it in Settings)";
    else if (g_rak) s = L"RakNet: module loaded";
    else s = L"RakNet: enabled, module not found";
    SetWindowTextW(g_rak_status, s);
}

// ---- actions --------------------------------------------------------------
static void DoAttach(void) {
    if (!LoadApi()) { SetStatus(L"API dll missing"); return; }
    int rc = p_attach();
    wchar_t* s = (wchar_t*)xalloc(96);
    if (s) { wsprintfW(s, L"attach -> %d", rc); SetStatus(s); }
}

static void DoExecute(void) {
    if (!p_execute) { SetStatus(L"press Attach first"); return; }
    if (!p_isAttached()) { SetStatus(L"not attached"); return; }
    wchar_t* w = ReadEdit();
    if (!w || !w[0]) { xfree(w); SetStatus(L"empty script"); return; }
    char* u = WideToUtf8(w);
    if (u) { p_execute(u); xfree(u); SetStatus(L"executed"); }
    xfree(w);
}

static void DoLoad(void) {
    static wchar_t file[MAX_PATH];
    file[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Lua scripts\0*.lua;*.txt\0All\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (f == INVALID_HANDLE_VALUE) { SetStatus(L"file read failed"); return; }
    DWORD sz = GetFileSize(f, 0);
    if (sz > 0x100000) sz = 0x100000;
    char* buf = (char*)xalloc(sz + 1);
    DWORD got = 0;
    if (buf) {
        ReadFile(f, buf, sz, &got, 0);
        buf[got] = 0;
        int wn = MultiByteToWideChar(CP_UTF8, 0, buf, -1, 0, 0);
        wchar_t* wbuf = (wchar_t*)xalloc((unsigned __int64)wn * sizeof(wchar_t));
        if (wbuf) {
            MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, wn);
            SetWindowTextW(g_edit, wbuf);
            xfree(wbuf);
            SetStatus(L"script loaded");
        }
        xfree(buf);
    }
    CloseHandle(f);
}

static void DoRakSend(void) {
    if (!g_rak || !p_rak_send) { RakLog(L"[raknet] send: module not loaded"); return; }
    unsigned char payload[8] = { 0x52, 0x41, 0x4B, 0x01, 0, 0, 0, 0 };  // 'RAK' + type
    int r = p_rak_send(payload, (int)sizeof(payload));
    wchar_t buf[64];
    wsprintfW(buf, L"[raknet] send() -> %d", r);
    RakLog(buf);
}

// ---- animation ------------------------------------------------------------
static void EnsureAnimRunning(void) {
    if (!g_animating) { g_animating = TRUE; SetTimer(g_hwnd, TIMER_ANIM, 15, 0); }
}

static float Approach(float cur, float target) {
    float d = target - cur;
    if (d > 0.12f) return cur + 0.12f;
    if (d < -0.12f) return cur - 0.12f;
    return target;
}

// ---- hover subclass -------------------------------------------------------
static LRESULT CALLBACK BtnSubclass(HWND h, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR ref) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetWindowLongPtrW(h, GWLP_USERDATA)) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 1);
            InvalidateRect(h, 0, TRUE);
            TRACKMOUSEEVENT tme;
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = h; tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        InvalidateRect(h, 0, TRUE);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ---- drawing --------------------------------------------------------------
static void FillSolid(HDC dc, RECT* r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void RoundFill(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = (HBRUSH)SelectObject(dc, br);
    HPEN op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void DrawRoundedButton(DRAWITEMSTRUCT* d, const wchar_t* label, BOOL primary) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    BOOL pressed = (d->itemState & ODS_SELECTED) != 0;
    BOOL hot = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) != 0;
    COLORREF bg, textc, border;
    if (primary) {
        bg = pressed ? g_th.accent_p : (hot ? g_th.accent_h : g_th.accent);
        textc = RGB(255, 255, 255); border = bg;
    } else {
        bg = pressed ? g_th.btn_p : (hot ? g_th.btn_h : g_th.btn);
        textc = g_th.text; border = hot ? g_th.subtext : g_th.border;
    }
    FillRect(dc, &r, g_brBg);
    RECT rr = r; rr.right -= 1; rr.bottom -= 1;
    RoundFill(dc, rr, 10, bg, border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, textc);
    SelectObject(dc, g_fontUI);
    RECT tr = r;
    DrawTextW(dc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawCaptionButton(DRAWITEMSTRUCT* d, UINT glyph, BOOL is_close) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    BOOL pressed = (d->itemState & ODS_SELECTED) != 0;
    BOOL hot = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) != 0;
    COLORREF bg = 0;
    if (hot) bg = is_close ? g_th.red : g_th.btn_h;
    if (pressed && is_close) bg = RGB(200, 40, 43);
    if (bg) FillSolid(dc, &r, bg); else FillRect(dc, &r, g_brBg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, (hot && is_close) ? RGB(255, 255, 255) : g_th.text);
    SelectObject(dc, g_fontGlyph);
    wchar_t g[2]; g[0] = (wchar_t)glyph; g[1] = 0;
    RECT tr = r;
    DrawTextW(dc, g, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawThemeButton(DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    BOOL hot = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) != 0;
    RECT rr = r; rr.right -= 1; rr.bottom -= 1;
    RoundFill(dc, rr, 8, hot ? g_th.btn_h : g_th.btn, g_th.border);

    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    if (g_dark) {
        HBRUSH sun = CreateSolidBrush(g_th.accent);
        HBRUSH ob = (HBRUSH)SelectObject(dc, sun);
        HPEN pn = CreatePen(PS_SOLID, 1, g_th.accent);
        HPEN op = (HPEN)SelectObject(dc, pn);
        Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
        int i;
        for (i = 0; i < 8; i++) {
            int dx = (i == 0) ? 1 : (i == 4) ? -1 : 0;
            int dy = (i == 2) ? 1 : (i == 6) ? -1 : 0;
            MoveToEx(dc, cx + dx * 6, cy + dy * 6, 0);
            LineTo(dc, cx + dx * 8, cy + dy * 8);
        }
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(sun); DeleteObject(pn);
    } else {
        HBRUSH moon = CreateSolidBrush(g_th.accent);
        HBRUSH ob = (HBRUSH)SelectObject(dc, moon);
        HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 6, cy - 6, cx + 6, cy + 6);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(moon);
        HBRUSH carve = CreateSolidBrush(g_th.btn);
        HBRUSH ob3 = (HBRUSH)SelectObject(dc, carve);
        HPEN op3 = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, cx - 2, cy - 9, cx + 10, cy + 3);
        SelectObject(dc, ob3); SelectObject(dc, op3);
        DeleteObject(carve);
    }
}

static void DrawStatus(DRAWITEMSTRUCT* d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, g_brBg);
    RoundFill(dc, r, (r.bottom - r.top), g_th.surface, g_th.border);
    COLORREF c = g_attached ? g_th.green : g_th.red;
    int cy = (r.top + r.bottom) / 2, rad = 4, dotx = r.left + 12;
    HBRUSH dot = CreateSolidBrush(c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, dot);
    HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dotx, cy - rad, dotx + 2 * rad, cy + rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(dot);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_th.text);
    SelectObject(dc, g_fontUI);
    RECT tr = r; tr.left = dotx + 2 * rad + 8;
    DrawTextW(dc, g_status_text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void DrawSwitch(HDC dc, int x, int y, int w, int h, float t, BOOL on) {
    RECT tr = { x, y, x + w, y + h };
    COLORREF track = on ? g_th.accent : g_th.track_off;
    RoundFill(dc, tr, h, track, track);
    int d = h - 6;
    int kx = x + 3 + (int)(t * (float)(w - d - 6));
    int ky = y + 3;
    HBRUSH kb = CreateSolidBrush(g_th.knob);
    HBRUSH ob = (HBRUSH)SelectObject(dc, kb);
    HPEN op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, kx, ky, kx + d, ky + d);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(kb);
}

static void DrawToggleRow(DRAWITEMSTRUCT* d, const wchar_t* label, float* anim, BOOL on) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    FillRect(dc, &r, g_brBg);
    RECT card = r; card.right -= 1; card.bottom -= 1;
    RoundFill(dc, card, 12, g_th.surface, g_th.border);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_th.text);
    SelectObject(dc, g_fontUI);
    RECT lr = r; lr.left += 18; lr.right -= 84;
    DrawTextW(dc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    int sw = 46, sh = 26;
    int sx = r.right - 18 - sw, sy = (r.top + r.bottom) / 2 - sh / 2;
    DrawSwitch(dc, sx, sy, sw, sh, *anim, on);
}

// ---- layout + tabs --------------------------------------------------------
static const wchar_t* TAB_LABEL[3] = { L"Executor", L"RakNet", L"Settings" };
static const UINT      TAB_ICON[3] = { 0xE943, 0xE774, 0xE713 };

static void ShowTab(int t) {
    BOOL e = (t == 0), k = (t == 1), s = (t == 2);
    ShowWindow(g_status, e ? SW_SHOW : SW_HIDE);
    ShowWindow(g_edit, e ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btn_attach, e ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btn_load, e ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btn_exec, e ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rak_status, k ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rak_log, k ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rak_load, k ? SW_SHOW : SW_HIDE);
    ShowWindow(g_rak_send, k ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tg_rak, s ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tg_auto, s ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tg_dark, s ? SW_SHOW : SW_HIDE);
    UpdateRakStatus();
}

static void Layout(HWND hwnd) {
    RECT cr;
    GetClientRect(hwnd, &cr);
    int w = cr.right, h = cr.bottom;
    int pad = 20, capW = 46;
    int contentTop = TITLE_H + TAB_H;

    if (g_btn_close) MoveWindow(g_btn_close, w - capW, 0, capW, TITLE_H, TRUE);
    if (g_btn_max)   MoveWindow(g_btn_max, w - 2 * capW, 0, capW, TITLE_H, TRUE);
    if (g_btn_min)   MoveWindow(g_btn_min, w - 3 * capW, 0, capW, TITLE_H, TRUE);
    if (g_btn_theme) MoveWindow(g_btn_theme, w - 3 * capW - 50, 7, 40, 28, TRUE);

    // tab strip
    int tx = pad;
    int i;
    for (i = 0; i < 3; i++) {
        g_tabRect[i].left = tx;
        g_tabRect[i].top = TITLE_H;
        g_tabRect[i].right = tx + 126;
        g_tabRect[i].bottom = TITLE_H + TAB_H;
        tx += 126;
    }

    int btnH = 42, btnW = 120, gap = 10;

    // executor
    if (g_status) MoveWindow(g_status, pad, contentTop + 16, 230, 28, TRUE);
    int editTop = contentTop + 58;
    int editBottom = h - pad - btnH - 12;
    if (editBottom < editTop + 60) editBottom = editTop + 60;
    g_editRect.left = pad; g_editRect.top = editTop;
    g_editRect.right = w - pad; g_editRect.bottom = editBottom;
    if (g_edit)
        MoveWindow(g_edit, g_editRect.left, g_editRect.top,
                   g_editRect.right - g_editRect.left, g_editRect.bottom - g_editRect.top, TRUE);

    int by = h - pad - btnH;
    if (g_btn_exec)   MoveWindow(g_btn_exec, w - pad - btnW, by, btnW, btnH, TRUE);
    if (g_btn_load)   MoveWindow(g_btn_load, w - pad - 2 * btnW - gap, by, btnW, btnH, TRUE);
    if (g_btn_attach) MoveWindow(g_btn_attach, w - pad - 3 * btnW - 2 * gap, by, btnW, btnH, TRUE);

    // raknet tab
    if (g_rak_status) MoveWindow(g_rak_status, pad, contentTop + 58, w - 2 * pad, 22, TRUE);
    int rlogTop = contentTop + 88;
    int rlogBottom = h - pad - btnH - 12;
    if (rlogBottom < rlogTop + 60) rlogBottom = rlogTop + 60;
    if (g_rak_log) MoveWindow(g_rak_log, pad, rlogTop, w - 2 * pad, rlogBottom - rlogTop, TRUE);
    if (g_rak_load) MoveWindow(g_rak_load, w - pad - 160, by, 160, btnH, TRUE);
    if (g_rak_send) MoveWindow(g_rak_send, w - pad - 160 - gap - 160, by, 160, btnH, TRUE);

    // settings tab (cards stack)
    int sy = contentTop + 72;
    if (g_tg_rak)  MoveWindow(g_tg_rak,  pad, sy, w - 2 * pad, 54, TRUE); sy += 66;
    if (g_tg_auto) MoveWindow(g_tg_auto, pad, sy, w - 2 * pad, 54, TRUE); sy += 66;
    if (g_tg_dark) MoveWindow(g_tg_dark, pad, sy, w - 2 * pad, 54, TRUE);

    if (g_btn_max) InvalidateRect(g_btn_max, 0, TRUE);
    InvalidateRect(hwnd, 0, TRUE);
}

static void SwitchTab(int t) {
    if (t < 0 || t > 2) return;
    g_tab = t;
    g_underW = (float)(g_tabRect[t].right - g_tabRect[t].left);
    EnsureAnimRunning();
    ShowTab(t);
    InvalidateRect(g_hwnd, 0, TRUE);
}

// ---- window proc ----------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        ApplyTheme(TRUE);

        g_fontTitle = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontH2    = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontUI    = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_fontCode  = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        g_fontGlyph = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");

        g_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_STATUS, 0, 0);

        g_edit = CreateWindowExW(0, L"EDIT", L"-- script\nprint('injected')",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, hwnd, (HMENU)IDC_EDIT, 0, 0);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
        SendMessageW(g_edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));

        g_btn_attach = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_ATTACH, 0, 0);
        g_btn_load   = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_LOAD, 0, 0);
        g_btn_exec   = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_EXEC, 0, 0);
        g_btn_min    = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_MIN, 0, 0);
        g_btn_max    = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_MAX, 0, 0);
        g_btn_close  = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_CLOSE, 0, 0);
        g_btn_theme  = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_THEME, 0, 0);

        g_rak_status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT, 0, 0, 10, 10, hwnd, (HMENU)IDC_RAK_STATUS, 0, 0);
        SendMessageW(g_rak_status, WM_SETFONT, (WPARAM)g_fontUI, TRUE);

        g_rak_log = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)IDC_RAK_LOG, 0, 0);
        SendMessageW(g_rak_log, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
        SendMessageW(g_rak_log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));

        g_rak_load = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_RAK_LOAD, 0, 0);
        g_rak_send = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_RAK_SEND, 0, 0);

        g_tg_rak  = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_TG_RAK, 0, 0);
        g_tg_auto = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_TG_AUTO, 0, 0);
        g_tg_dark = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_TG_DARK, 0, 0);

        SetWindowSubclass(g_btn_attach, BtnSubclass, 1, 0);
        SetWindowSubclass(g_btn_load,   BtnSubclass, 2, 0);
        SetWindowSubclass(g_btn_exec,   BtnSubclass, 3, 0);
        SetWindowSubclass(g_btn_min,    BtnSubclass, 4, 0);
        SetWindowSubclass(g_btn_max,    BtnSubclass, 5, 0);
        SetWindowSubclass(g_btn_close,  BtnSubclass, 6, 0);
        SetWindowSubclass(g_btn_theme,  BtnSubclass, 7, 0);
        SetWindowSubclass(g_rak_load,   BtnSubclass, 8, 0);
        SetWindowSubclass(g_rak_send,   BtnSubclass, 9, 0);
        SetWindowSubclass(g_tg_rak,     BtnSubclass, 10, 0);
        SetWindowSubclass(g_tg_auto,    BtnSubclass, 11, 0);
        SetWindowSubclass(g_tg_dark,    BtnSubclass, 12, 0);

        Layout(hwnd);
        SwitchTab(0);
        RegisterHotKey(hwnd, HOTKEY_EXEC, MOD_CONTROL, VK_RETURN);
        SetStatus(LoadApi() ? L"ready - press Attach" : L"API dll missing");
        RakLog(L"[raknet] ready");
        SetTimer(hwnd, TIMER_POLL, 1000, 0);
        return 0;
    }
    case WM_NCCALCSIZE:
        if (wp == TRUE) return 0;
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        RECT wr; GetWindowRect(hwnd, &wr);
        int x = pt.x - wr.left, y = pt.y - wr.top;
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        int b = 6;
        if (!IsZoomed(hwnd)) {
            BOOL L = x < b, R = x >= w - b, T = y < b, B = y >= h - b;
            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }
        if (y < TITLE_H) return HTCAPTION;
        return hit;
    }
    case WM_LBUTTONDOWN: {
        POINT pt; pt.x = GET_X_LPARAM(lp); pt.y = GET_Y_LPARAM(lp);
        if (pt.y >= TITLE_H && pt.y < TITLE_H + TAB_H) {
            int i;
            for (i = 0; i < 3; i++) {
                if (PtInRect(&g_tabRect[i], pt)) { SwitchTab(i); return 0; }
            }
        }
        return 0;
    }
    case WM_SIZE:
        Layout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = 660;
        mmi->ptMinTrackSize.y = 480;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr; GetClientRect(hwnd, &cr);
        FillRect(dc, &cr, g_brBg);

        // title bar
        RECT tb = { 0, 0, cr.right, TITLE_H };
        FillSolid(dc, &tb, g_th.surface);
        RECT accent = { 20, 13, 24, TITLE_H - 13 };
        FillSolid(dc, &accent, g_th.accent);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_th.text);
        SelectObject(dc, g_fontTitle);
        RECT nr = { 32, 0, cr.right - 220, TITLE_H };
        DrawTextW(dc, L"WRD EXECUTOR", -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // tab strip bg + hairline
        RECT ts = { 0, TITLE_H, cr.right, TITLE_H + TAB_H };
        FillSolid(dc, &ts, g_th.bg);
        RECT hl = { 0, TITLE_H + TAB_H, cr.right, TITLE_H + TAB_H + 1 };
        FillSolid(dc, &hl, g_th.border);

        // tabs
        int i;
        SetBkMode(dc, TRANSPARENT);
        for (i = 0; i < 3; i++) {
            RECT r = g_tabRect[i];
            BOOL active = (i == g_tab);
            SetTextColor(dc, active ? g_th.text : g_th.subtext);
            SelectObject(dc, g_fontGlyph);
            RECT ir = r; ir.right = ir.left + 18 + 20;
            DrawTextW(dc, (wchar_t[]){ (wchar_t)TAB_ICON[i], 0 }, -1, &ir, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, g_fontH2);
            RECT lr = r; lr.left += 44;
            DrawTextW(dc, TAB_LABEL[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // animated underline
        int uy = TITLE_H + TAB_H - 3;
        RECT ul = { (int)g_underX, uy, (int)(g_underX + g_underW), uy + 3 };
        FillSolid(dc, &ul, g_th.accent);

        // per-tab headers + editor frame
        int contentTop = TITLE_H + TAB_H;
        if (g_tab == 0) {
            RECT eb = g_editRect;
            HBRUSH frame = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH ob = (HBRUSH)SelectObject(dc, frame);
            HPEN pen = CreatePen(PS_SOLID, 1, g_th.border);
            HPEN op = (HPEN)SelectObject(dc, pen);
            RoundRect(dc, eb.left - 1, eb.top - 1, eb.right + 1, eb.bottom + 1, 10, 10);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(pen);
            SetTextColor(dc, g_th.subtext);
            SelectObject(dc, g_fontUI);
            RECT fr = { 20, cr.bottom - 46, cr.right - 420, cr.bottom - 18 };
            DrawTextW(dc, L"Ctrl+Enter to run", -1, &fr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (g_tab == 1) {
            SetTextColor(dc, g_th.text);
            SelectObject(dc, g_fontTitle);
            RECT tr = { 20, contentTop + 14, cr.right - 40, contentTop + 46 };
            DrawTextW(dc, L"RakNet", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, g_th.subtext);
            SelectObject(dc, g_fontUI);
            RECT sr = { 20, contentTop + 46, cr.right - 40, contentTop + 66 };
            DrawTextW(dc, L"Raw socket module, loaded at runtime. Enable it in Settings.", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(dc, g_th.text);
            SelectObject(dc, g_fontTitle);
            RECT tr = { 20, contentTop + 14, cr.right - 40, contentTop + 46 };
            DrawTextW(dc, L"Settings", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, g_th.subtext);
            SelectObject(dc, g_fontUI);
            RECT sr = { 20, contentTop + 46, cr.right - 40, contentTop + 66 };
            DrawTextW(dc, L"Features", -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        HWND ctl = (HWND)lp;
        if (ctl == g_rak_log) {
            SetTextColor(dc, g_th.text);
            SetBkColor(dc, g_th.editbg);
            return (LRESULT)g_brEdit;
        }
        if (ctl == g_rak_status) {
            SetTextColor(dc, g_th.subtext);
            SetBkColor(dc, g_th.bg);
            return (LRESULT)g_brBg;
        }
        SetTextColor(dc, g_th.text);
        SetBkColor(dc, g_th.bg);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, g_th.text);
        SetBkColor(dc, g_th.editbg);
        return (LRESULT)g_brEdit;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
        switch (d->CtlID) {
        case IDC_ATTACH: DrawRoundedButton(d, L"Attach", FALSE);   return TRUE;
        case IDC_LOAD:   DrawRoundedButton(d, L"Load .lua", FALSE); return TRUE;
        case IDC_EXEC:   DrawRoundedButton(d, L"Execute", TRUE);    return TRUE;
        case IDC_RAK_LOAD: DrawRoundedButton(d, L"Load module", FALSE); return TRUE;
        case IDC_RAK_SEND: DrawRoundedButton(d, L"Send test packet", TRUE); return TRUE;
        case IDC_MIN:    DrawCaptionButton(d, 0xE921, FALSE);       return TRUE;
        case IDC_MAX:    DrawCaptionButton(d, IsZoomed(hwnd) ? 0xE923 : 0xE922, FALSE); return TRUE;
        case IDC_CLOSE:  DrawCaptionButton(d, 0xE8BB, TRUE);        return TRUE;
        case IDC_THEME:  DrawThemeButton(d);                        return TRUE;
        case IDC_STATUS: DrawStatus(d);                             return TRUE;
        case IDC_TG_RAK:  DrawToggleRow(d, L"Enable RakNet support", &g_animRak, g_setRak); return TRUE;
        case IDC_TG_AUTO: DrawToggleRow(d, L"Auto-attach on launch", &g_animAuto, g_setAuto); return TRUE;
        case IDC_TG_DARK: DrawToggleRow(d, L"Dark theme", &g_animDark, g_dark); return TRUE;
        }
        return FALSE;
    }
    case WM_TIMER:
        if (wp == TIMER_POLL && p_isAttached) {
            BOOL now = p_isAttached() ? TRUE : FALSE;
            if (now != g_attached) { g_attached = now; SetStatus(now ? L"attached" : L"detached"); }
        } else if (wp == TIMER_ANIM) {
            float tx = (float)g_tabRect[g_tab].left;
            g_underX = Approach(g_underX, tx);
            g_animRak  = Approach(g_animRak,  g_setRak ? 1.0f : 0.0f);
            g_animAuto = Approach(g_animAuto, g_setAuto ? 1.0f : 0.0f);
            g_animDark = Approach(g_animDark, g_dark ? 1.0f : 0.0f);
            RECT ur = { (int)g_underX - 4, TITLE_H, (int)(g_underX + g_underW) + 4, TITLE_H + TAB_H };
            InvalidateRect(hwnd, &ur, FALSE);
            if (g_tg_rak)  InvalidateRect(g_tg_rak, 0, TRUE);
            if (g_tg_auto) InvalidateRect(g_tg_auto, 0, TRUE);
            if (g_tg_dark) InvalidateRect(g_tg_dark, 0, TRUE);
            BOOL settled = (g_underX == tx) &&
                           (g_animRak == (g_setRak ? 1.0f : 0.0f)) &&
                           (g_animAuto == (g_setAuto ? 1.0f : 0.0f)) &&
                           (g_animDark == (g_dark ? 1.0f : 0.0f));
            if (settled) { KillTimer(hwnd, TIMER_ANIM); g_animating = FALSE; }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ATTACH: if (HIWORD(wp) == BN_CLICKED) DoAttach();  break;
        case IDC_EXEC:   if (HIWORD(wp) == BN_CLICKED) DoExecute(); break;
        case IDC_LOAD:   if (HIWORD(wp) == BN_CLICKED) DoLoad();    break;
        case IDC_RAK_LOAD: if (HIWORD(wp) == BN_CLICKED) { RakNetLoadNow(); UpdateRakStatus(); } break;
        case IDC_RAK_SEND: if (HIWORD(wp) == BN_CLICKED) DoRakSend(); break;
        case IDC_MIN:    if (HIWORD(wp) == BN_CLICKED) ShowWindow(hwnd, SW_MINIMIZE); break;
        case IDC_MAX:    if (HIWORD(wp) == BN_CLICKED) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case IDC_CLOSE:  if (HIWORD(wp) == BN_CLICKED) PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case IDC_THEME:  if (HIWORD(wp) == BN_CLICKED) { g_dark = !g_dark; ApplyTheme(g_dark); EnsureAnimRunning(); } break;
        case IDC_TG_RAK:
            if (HIWORD(wp) == BN_CLICKED) {
                g_setRak = !g_setRak;
                if (g_setRak) RakNetLoadNow(); else RakNetUnload();
                UpdateRakStatus();
                EnsureAnimRunning();
            }
            break;
        case IDC_TG_AUTO:
            if (HIWORD(wp) == BN_CLICKED) { g_setAuto = !g_setAuto; EnsureAnimRunning(); }
            break;
        case IDC_TG_DARK:
            if (HIWORD(wp) == BN_CLICKED) { g_dark = !g_dark; ApplyTheme(g_dark); EnsureAnimRunning(); }
            break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_EXEC) DoExecute();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        KillTimer(hwnd, TIMER_ANIM);
        UnregisterHotKey(hwnd, HOTKEY_EXEC);
        RakNetUnload();
        if (g_api) FreeLibrary(g_api);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int Run(HINSTANCE hInst) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = L"WrdExecutor";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"WrdExecutor", L"WRD executor",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 920, 660, 0, 0, hInst, 0);
    if (!hwnd) return 1;

    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        typedef HRESULT (WINAPI *DwmSetAttr)(HWND, DWORD, LPCVOID, DWORD);
        DwmSetAttr f = (DwmSetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (f) {
            BOOL dark = g_dark;
            f(hwnd, 20, &dark, sizeof(dark));
            f(hwnd, 19, &dark, sizeof(dark));
            int pref = 2;
            f(hwnd, 33, &pref, sizeof(pref));
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

void __stdcall WinMainCRTStartup(void) {
    SetProcessDPIAware();
    int rc = Run(GetModuleHandleW(0));
    ExitProcess((UINT)rc);
}
