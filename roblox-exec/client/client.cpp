// language: C++, file: client.cpp, target: Windows 11 x64, MSVC
// Injected client. Built to be manual-mapped — imports only kernel32 so the
// injector's system-DLL resolution is always valid. Worker:
//   1. resolves the Luau C-API once a lua_State is in hand
//   2. inline-hooks luaD_throw to capture a live lua_State
//   3. serves a named pipe so an external console pushes scripts in
//   4. runs the candidate's Luau through lua_newthread + luaL_loadbuffer + lua_pcall
//
// build: cl /std:c++17 /EHsc /O2 /LD /DUNICODE /D_UNICODE client.cpp /link /ENTRY:DllMain
// The byte SIGNATURES below are per-build. Roblox recompiles the client
// weekly; the pattern for luaD_throw and the loadbuffer/pcall bodies change
// with it. Update the four sigs after each client push (see build.md).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\exec_client";

// ---- module range ---------------------------------------------------------
struct Range { uint8_t* base; size_t size; };

static Range MainModuleRange() {
    HMODULE m = GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)m;
    auto nt  = (const IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
    return { (uint8_t*)m, nt->OptionalHeader.SizeOfImage };
}

// ---- pattern scan ---------------------------------------------------------
// sig = raw bytes with '?' wildcards, mask = "xxx?xx..."
static uint8_t* PatternScan(const Range& r, const char* sig, const char* mask) {
    const size_t len = strlen(mask);
    for (size_t i = 0; i + len <= r.size; i++) {
        bool hit = true;
        for (size_t j = 0; j < len; j++) {
            if (mask[j] == 'x' && r.base[i + j] != (uint8_t)sig[j]) { hit = false; break; }
        }
        if (hit) return r.base + i;
    }
    return nullptr;
}

// ---- Luau C API (signatures stable; addresses are per-build) ---------------
using lua_State = void;
using lua_newthread_t   = lua_State* (__fastcall*)(lua_State*);
using luaL_loadbuffer_t = int        (__fastcall*)(lua_State*, const char*, size_t, const char*);
using lua_pcall_t       = int        (__fastcall*)(lua_State*, int, int, int);
using luaD_throw_t      = void       (__fastcall*)(lua_State*, int);

static lua_newthread_t   p_newthread   = nullptr;
static luaL_loadbuffer_t p_loadbuffer  = nullptr;
static lua_pcall_t       p_pcall       = nullptr;

// ---- inline hook (14-byte absolute jmp + trampoline) ----------------------
// NOTE: assumes the first PATCH_LEN bytes at the target are whole instructions
// with no RIP-relative/relanding bytes. Real builds should lift through an
// instruction-length decoder (HDE64) and relocate by delta. For luaD_throw's
// prologue (register pushes) this fixed patch is safe.
static const size_t PATCH_LEN = 14;
static luaD_throw_t p_throw_orig = nullptr;
static volatile lua_State* g_state = nullptr;

static void InstallHook(void* target, void* detour, void** trampoline) {
    auto* tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_EXECUTE_READWRITE);
    memcpy(tramp, target, PATCH_LEN);                     // original prologue
    uint8_t* j = tramp + PATCH_LEN;                       // jmp [rip+0]; <ret addr>
    j[0] = 0xFF; j[1] = 0x25;
    *(int32_t*)(j + 2) = 0;
    *(uint64_t*)(j + 6) = (uint64_t)((uint8_t*)target + PATCH_LEN);
    *trampoline = tramp;

    DWORD old = 0;
    VirtualProtect(target, PATCH_LEN, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)target;
    p[0] = 0xFF; p[1] = 0x25;                             // jmp qword ptr [rip+0]
    *(int32_t*)(p + 2) = 0;
    *(uint64_t*)(p + 6) = (uint64_t)detour;
    VirtualProtect(target, PATCH_LEN, old, &old);
}

// detour: capture the live state on any thrown error, then chain through.
static void __fastcall Hooked_Throw(lua_State* L, int errcode) {
    g_state = L;
    p_throw_orig(L, errcode);
}

// ---- resolve --------------------------------------------------------------
// SIGNATURES — replace the byte strings + masks with the current client's.
// Left here as recognizable shapes; verified only against a generic prologue.
static bool ResolveFunctions() {
    Range r = MainModuleRange();

    // luaD_throw prologue shape: 48 89 5C 24 08 57 48 83 EC 20 ...
    static const char* throw_sig  = "\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20";
    static const char* throw_mask = "xxxxxxxxxx";

    uint8_t* throw_fn = PatternScan(r, throw_sig, throw_mask);
    if (!throw_fn) { return false; }
    InstallHook(throw_fn, (void*)&Hooked_Throw, (void**)&p_throw_orig);

    // loadbuffer / pcall are located either by their own sigs or, more
    // reliably, by walking luaD_throw's callers. Wire the resolved addresses
    // here after locating them in your build:
    //   p_loadbuffer = (luaL_loadbuffer_t)  <addr>;
    //   p_pcall      = (lua_pcall_t)        <addr>;
    //   p_newthread  = (lua_newthread_t)    <addr>;
    return (p_loadbuffer && p_pcall && p_newthread);
}

// ---- execution ------------------------------------------------------------
static bool Execute(const std::string& script) {
    if (!g_state || !p_loadbuffer || !p_pcall || !p_newthread) return false;
    lua_State* L = p_newthread((lua_State*)g_state);
    if (!L) return false;
    if (p_loadbuffer(L, script.c_str(), script.size(), "=exec") != 0) return false;
    p_pcall(L, 0, 0, 0);
    return true;
}

// ---- named-pipe command channel -------------------------------------------
static DWORD WINAPI PipeServer(LPVOID) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(PIPE_NAME, PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            1, 0, 1 << 20, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return 1;
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[1 << 20];
            DWORD got = 0;
            if (ReadFile(pipe, buf, sizeof(buf) - 1, &got, nullptr) && got) {
                buf[got] = 0;
                Execute(std::string(buf, got));
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

static DWORD WINAPI Worker(LPVOID) {
    // wait until ResolveFunctions succeeds (client may still be booting)
    for (int i = 0; i < 600; i++) {
        if (ResolveFunctions()) break;
        Sleep(500);
    }
    CreateThread(nullptr, 0, PipeServer, nullptr, 0, nullptr);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
