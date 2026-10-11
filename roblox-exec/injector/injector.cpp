// language: C++, file: injector.cpp, target: Windows 11 x64, MSVC
// Manual-map injector. Reads a DLL from disk, maps it into a target process
// WITHOUT LoadLibrary, applies base relocations + resolves imports, then runs
// the DLL entry point (DllMain) through a native x64 stub. No module entry
// is added to the target PEB module list, so a module-walk sees nothing.
//
// build: cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE injector.cpp /Fe:injector.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// ---- process helpers ------------------------------------------------------
static DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool EnableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok;
}

static bool ReadFileBytes(const wchar_t* path, std::vector<uint8_t>& out) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    bool ok = ReadFile(f, out.data(), (DWORD)out.size(), &read, nullptr) &&
              read == (DWORD)out.size();
    CloseHandle(f);
    return ok;
}

// ---- little-endian immediate append ---------------------------------------
static void Append64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (i * 8)));
}

// ---- manual map -----------------------------------------------------------
static bool ManualMap(HANDLE proc, const std::vector<uint8_t>& file, LPVOID& outBase) {
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    auto dos = (const IMAGE_DOS_HEADER*)file.data();
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (const IMAGE_NT_HEADERS*)(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const uint64_t pref = nt->OptionalHeader.ImageBase;
    const SIZE_T   size = nt->OptionalHeader.SizeOfImage;

    // reserve at preferred base first (keeps relocations zero), fall back to any.
    LPVOID base = VirtualAllocEx(proc, (LPVOID)pref, size,
                                 MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base)
        base = VirtualAllocEx(proc, nullptr, size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) return false;

    // headers
    WriteProcessMemory(proc, base, file.data(),
                       nt->OptionalHeader.SizeOfHeaders, nullptr);

    // sections
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (sec->SizeOfRawData == 0) continue;
        LPVOID dst = (uint8_t*)base + sec->VirtualAddress;
        LPVOID src = (uint8_t*)file.data() + sec->PointerToRawData;
        WriteProcessMemory(proc, dst, src, sec->SizeOfRawData, nullptr);
    }

    // base relocations (DIR64 only — x64)
    const uint64_t delta = (uint64_t)base - pref;
    if (delta) {
        auto reloc = (const IMAGE_DATA_DIRECTORY*)
            &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc->Size) {
            uint8_t* block = (uint8_t*)file.data() + reloc->VirtualAddress;
            uint8_t* end = block + reloc->Size;
            while (block < end) {
                auto rb = (const IMAGE_BASE_RELOCATION*)block;
                if (!rb->SizeOfBlock) break;
                uint32_t count = (rb->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(uint16_t);
                auto entry = (const uint16_t*)(block + sizeof(IMAGE_BASE_RELOCATION));
                for (uint32_t i = 0; i < count; i++) {
                    if ((entry[i] >> 12) != IMAGE_REL_BASED_DIR64) continue;
                    uintptr_t addr = (uintptr_t)base + rb->VirtualAddress + (entry[i] & 0xFFF);
                    uint64_t orig = 0;
                    ReadProcessMemory(proc, (LPCVOID)addr, &orig, sizeof(orig), nullptr);
                    orig += delta;
                    WriteProcessMemory(proc, (LPVOID)addr, &orig, sizeof(orig), nullptr);
                }
                block += rb->SizeOfBlock;
            }
        }
    }

    // imports — resolve against the injector's loaded system DLLs, write into
    // the remote IAT. Valid because kernel32/user32/ntdll share a base across
    // processes on the same session. A DLL importing a non-system module would
    // need LoadLibrary-in-target instead.
    auto impDir = (const IMAGE_DATA_DIRECTORY*)
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir->Size) {
        auto imp = (const IMAGE_IMPORT_DESCRIPTOR*)
            (file.data() + impDir->VirtualAddress);
        for (; imp->Name; imp++) {
            const char* modName = (const char*)(file.data() + imp->Name);
            HMODULE local = LoadLibraryA(modName);
            if (!local) continue;
            auto origThunk  = (const IMAGE_THUNK_DATA*)
                (file.data() + imp->OriginalFirstThunk);
            auto firstThunk = (const IMAGE_THUNK_DATA*)
                (file.data() + imp->FirstThunk);
            for (; origThunk->u1.AddressOfData; origThunk++, firstThunk++) {
                uintptr_t fn = 0;
                if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                    fn = (uintptr_t)GetProcAddress(local,
                            (LPCSTR)(origThunk->u1.Ordinal & 0xFFFF));
                else {
                    auto ibn = (const IMAGE_IMPORT_BY_NAME*)
                        (file.data() + origThunk->u1.AddressOfData);
                    fn = (uintptr_t)GetProcAddress(local, ibn->Name);
                }
                uintptr_t slot = (uintptr_t)base + imp->FirstThunk +
                    ((uintptr_t)firstThunk - (uintptr_t)(file.data() + imp->FirstThunk));
                WriteProcessMemory(proc, (LPVOID)slot, &fn, sizeof(fn), nullptr);
            }
        }
    }

    // per-section protections
    sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        DWORD c = sec->Characteristics;
        DWORD prot = PAGE_READONLY;
        if (c & IMAGE_SCN_MEM_EXECUTE)
            prot = (c & IMAGE_SCN_MEM_WRITE) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        else if (c & IMAGE_SCN_MEM_WRITE)
            prot = PAGE_READWRITE;
        DWORD old = 0;
        VirtualProtectEx(proc, (uint8_t*)base + sec->VirtualAddress,
                         sec->Misc.VirtualSize, prot, &old);
    }

    outBase = base;
    return true;
}

// ---- run DllMain(DLL_PROCESS_ATTACH) via remote stub -----------------------
static bool RunEntry(HANDLE proc, LPVOID base, uintptr_t entry) {
    std::vector<uint8_t> s;
    // mov rcx, base          (hinstDLL)
    s.insert(s.end(), {0x48, 0xB9}); Append64(s, (uint64_t)base);
    // mov rdx, 1             (fdwReason = DLL_PROCESS_ATTACH)
    s.insert(s.end(), {0x48, 0xBA}); Append64(s, 1);
    // xor r8, r8             (lpvReserved = NULL)
    s.insert(s.end(), {0x4D, 0x31, 0xC0});
    // mov rax, entry
    s.insert(s.end(), {0x48, 0xB8}); Append64(s, (uint64_t)entry);
    // call rax; ret
    s.insert(s.end(), {0xFF, 0xD0, 0xC3});

    LPVOID code = VirtualAllocEx(proc, nullptr, s.size(),
                                 MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return false;
    WriteProcessMemory(proc, code, s.data(), s.size(), nullptr);

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)code, nullptr, 0, nullptr);
    if (!th) { VirtualFreeEx(proc, code, 0, MEM_RELEASE); return false; }
    WaitForSingleObject(th, 15000);
    CloseHandle(th);
    VirtualFreeEx(proc, code, 0, MEM_RELEASE);
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        wprintf(L"usage: injector.exe <client.dll> [target.exe]\n");
        return 1;
    }
    const wchar_t* dllPath  = argv[1];
    const wchar_t* procName = (argc > 2) ? argv[2] : L"RobloxPlayerBeta.exe";

    EnableDebugPrivilege();

    DWORD pid = FindProcess(procName);
    if (!pid) { wprintf(L"target not running: %s\n", procName); return 1; }
    printf("target pid = %lu\n", pid);

    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(dllPath, bytes)) { printf("read dll failed\n"); return 1; }

    HANDLE proc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!proc) {
        printf("OpenProcess failed (%lu) — run as admin, target must not hold us out\n",
               GetLastError());
        return 1;
    }

    LPVOID base = nullptr;
    if (!ManualMap(proc, bytes, base)) { printf("manual map failed\n"); return 1; }

    auto dos = (const IMAGE_DOS_HEADER*)bytes.data();
    auto nt  = (const IMAGE_NT_HEADERS*)(bytes.data() + dos->e_lfanew);
    uintptr_t entry = (uintptr_t)base + nt->OptionalHeader.AddressOfEntryPoint;

    if (RunEntry(proc, base, entry))
        printf("mapped at %p — DllMain dispatched\n", base);
    else
        printf("remote entry stub failed\n");

    CloseHandle(proc);
    return 0;
}
