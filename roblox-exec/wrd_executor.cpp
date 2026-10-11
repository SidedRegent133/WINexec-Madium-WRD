// language: C++, file: wrd_executor.cpp, target: Windows 11 x64, MSVC
// Executor front-end over wearedevs_exploit_api.dll.
// The API DLL is loaded into THIS process; it performs the attach to
// RobloxPlayerBeta and hosts the script bridge. Exports (confirmed from the
// shipped binary, all Microsoft x64 convention):
//
//   initialize()            void  — one-time setup, call first
//   attach()                int   — attach to the client, returns status
//   isAttached()            bool  — poll until true
//   execute(const char*)    void  — run a Luau script
//   DllMain                 — exported for manual-mapping, not called here
//
// build: cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE wrd_executor.cpp /link /SUBSYSTEM:WINDOWS /Fe:wrd_executor.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <fstream>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' "
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---- API surface (real exports) -------------------------------------------
using initialize_t = void (*)();
using attach_t     = int  (*)();
using isAttached_t = bool (*)();
using execute_t    = void (*)(const char* script);

static initialize_t p_initialize  = nullptr;
static attach_t     p_attach      = nullptr;
static isAttached_t p_isAttached  = nullptr;
static execute_t    p_execute     = nullptr;
static HMODULE      g_api         = nullptr;

// ---- UI ids ---------------------------------------------------------------
#define IDC_EDIT   1001
#define IDC_ATTACH 1002
#define IDC_EXEC   1003
#define IDC_LOAD   1004
#define IDC_AUTO   1005
#define IDC_STATUS 1006
#define HOTKEY_EXEC 1
#define TIMER_POLL  1

static HWND g_edit = nullptr;
static HWND g_status = nullptr;
static HWND g_hwnd = nullptr;

// ---- helpers --------------------------------------------------------------
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? L"." : s.substr(0, slash);
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring ReadEdit() {
    int len = GetWindowTextLengthW(g_edit);
    if (len <= 0) return {};
    std::wstring buf(len + 1, L'\0');
    GetWindowTextW(g_edit, buf.data(), len + 1);
    buf.resize(len);
    return buf;
}

static void Status(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

// Try the DLL beside the exe, then in Downloads, then the bare name.
static bool LoadApi() {
    if (g_api) return true;

    wchar_t user[MAX_PATH]{};
    std::wstring downloads;
    if (GetEnvironmentVariableW(L"USERPROFILE", user, MAX_PATH))
        downloads = std::wstring(user) + L"\\Downloads\\wearedevs_exploit_api.dll";

    std::wstring local = ExeDir() + L"\\wearedevs_exploit_api.dll";
    const wchar_t* candidates[] = { local.c_str(), downloads.c_str(), L"wearedevs_exploit_api.dll" };

    for (const wchar_t* p : candidates) {
        g_api = LoadLibraryW(p);
        if (g_api) break;
    }
    if (!g_api) return false;

    p_initialize = (initialize_t)GetProcAddress(g_api, "initialize");
    p_attach     = (attach_t)    GetProcAddress(g_api, "attach");
    p_isAttached = (isAttached_t)GetProcAddress(g_api, "isAttached");
    p_execute    = (execute_t)   GetProcAddress(g_api, "execute");

    if (!p_initialize || !p_attach || !p_isAttached || !p_execute) return false;

    p_initialize();
    return true;
}

static void DoAttach() {
    if (!LoadApi()) { Status(L"wearedevs_exploit_api.dll not found / bad exports"); return; }
    int rc = p_attach();
    wchar_t s[96];
    wsprintfW(s, L"attach() -> %d", rc);
    Status(s);
}

static void DoExecute() {
    if (!p_execute) { Status(L"not initialised — press Attach"); return; }
    if (!p_isAttached()) { Status(L"not attached yet"); return; }
    std::wstring w = ReadEdit();
    if (w.empty()) { Status(L"empty script"); return; }
    std::string utf8 = WideToUtf8(w);
    p_execute(utf8.c_str());
    Status(L"executed");
}

static void DoLoad() {
    OPENFILENAMEW ofn{};
    wchar_t file[MAX_PATH]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = L"Lua scripts\0*.lua;*.txt\0All\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    std::ifstream f(file, std::ios::binary);
    if (!f) { Status(L"file read failed"); return; }
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    int n = MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.data(), (int)data.size(), w.data(), n);
    SetWindowTextW(g_edit, w.c_str());
    Status(L"script loaded");
}

static void DoAutoExecute() {
    if (!p_execute) { Status(L"not initialised"); return; }
    std::wstring w = ReadEdit();
    if (w.empty()) { Status(L"empty script"); return; }
    std::string utf8 = WideToUtf8(w);
    // no dedicated export — re-execute on this and future attaches.
    p_execute(utf8.c_str());
    Status(L"re-executed");
}

// ---- window ---------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"-- script\nprint('injected')",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            10, 10, 760, 420, hwnd, (HMENU)IDC_EDIT, nullptr, nullptr);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)font, TRUE);

        CreateWindowExW(0, L"BUTTON", L"Attach",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 440, 100, 28, hwnd, (HMENU)IDC_ATTACH, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Execute",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 118, 440, 100, 28, hwnd, (HMENU)IDC_EXEC, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Load .lua",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 226, 440, 100, 28, hwnd, (HMENU)IDC_LOAD, nullptr, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Re-execute",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 334, 440, 110, 28, hwnd, (HMENU)IDC_AUTO, nullptr, nullptr);

        g_status = CreateWindowExW(0, L"STATIC", L"loading API...",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 456, 446, 314, 20, hwnd, (HMENU)IDC_STATUS, nullptr, nullptr);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);

        RegisterHotKey(hwnd, HOTKEY_EXEC, MOD_CONTROL, VK_RETURN);
        Status(LoadApi() ? L"API ready — press Attach" : L"wearedevs_exploit_api.dll not found");
        SetTimer(hwnd, TIMER_POLL, 1000, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_POLL && p_isAttached) {
            static bool last = false;
            bool now = p_isAttached();
            if (now != last) {
                Status(now ? L"attached" : L"detached");
                last = now;
            }
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ATTACH: if (HIWORD(wp) == BN_CLICKED) DoAttach();      break;
        case IDC_EXEC:   if (HIWORD(wp) == BN_CLICKED) DoExecute();     break;
        case IDC_LOAD:   if (HIWORD(wp) == BN_CLICKED) DoLoad();        break;
        case IDC_AUTO:   if (HIWORD(wp) == BN_CLICKED) DoAutoExecute(); break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_EXEC) DoExecute();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_POLL);
        UnregisterHotKey(hwnd, HOTKEY_EXEC);
        if (g_api) FreeLibrary(g_api);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WrdExecutor";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"WrdExecutor", L"WRD executor",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 530, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
