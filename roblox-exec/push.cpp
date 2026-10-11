// language: C++, file: push.cpp, target: Windows 11 x64, MSVC
// External console — writes a script into the injected client's named pipe.
// build: cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE push.cpp /Fe:push.exe
//
// usage: push.exe script.lua

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <vector>
#include <string>

static bool ReadAll(const wchar_t* path, std::string& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(f, nullptr);
    out.resize(sz);
    DWORD read = 0;
    bool ok = ReadFile(f, out.data(), sz, &read, nullptr) && read == sz;
    CloseHandle(f);
    return ok;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { wprintf(L"usage: push.exe script.lua\n"); return 1; }

    std::string script;
    if (!ReadAll(argv[1], script)) { printf("read failed\n"); return 1; }

    HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\exec_client", GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        printf("pipe not found — client not injected?\n");
        return 1;
    }
    DWORD written = 0;
    WriteFile(pipe, script.data(), (DWORD)script.size(), &written, nullptr);
    CloseHandle(pipe);
    printf("pushed %lu bytes\n", written);
    return 0;
}
