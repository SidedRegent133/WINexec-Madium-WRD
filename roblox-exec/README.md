# roblox-exec

Two paths live here. Pick by what you actually want.

## Path 1 — WRD API front-end (what you asked for first)

`wrd_executor.exe` is **already built** (x64, GUI). Double-click it, press
**Attach**, paste a script, press **Execute** (Ctrl+Enter also runs it). It
looks for `wearedevs_exploit_api.dll` beside itself, then in
`%USERPROFILE%\Downloads`, then on PATH.

Source is `wrd_executor.c` — pure C, CRT-free, links only system DLLs
(KERNEL32/USER32/GDI32/COMDLG32). No MSVC runtime, no C++ standard library.

Rebuild (clang-cl + lld-link + Windows SDK; works without the MSVC workload):

```
set SDK=C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0
set LIBUM=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.28000.0\um\x64
set LIBUC=C:\Program Files (x86)\Windows Kits\10\Lib\10.0.28000.0\ucrt\x64
set RT=C:\Program Files\LLVM\lib\clang\23\lib\windows\clang_rt.builtins-x86_64.lib

clang-cl /c /TC /O2 /GS- /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
  /I vcstub /imsvc "%SDK%\um" /imsvc "%SDK%\ucrt" /imsvc "%SDK%\shared" /imsvc "%SDK%\winrt" ^
  /Fo:wrd_executor.obj wrd_executor.c

lld-link /nologo /subsystem:windows /entry:WinMainCRTStartup /nodefaultlib ^
  /libpath:"%LIBUM%" /libpath:"%LIBUC%" ^
  wrd_executor.obj "%RT%" kernel32.lib user32.lib gdi32.lib comdlg32.lib comctl32.lib ucrt.lib ^
  /out:wrd_executor.exe
```

`vcstub\` holds the two MSVC-compat headers clang needs when the VC++ workload
isn't installed (`sal.h`/`stdint.h` come from the SDK; `vcruntime.h`,
`vcruntime_string.h`, `vadefs.h`, `excpt.h` are shimmed). With MSVC installed,
`wrd_executor.cpp` (the STL version) compiles with the plain `cl` line below.

### Confirmed exports of wearedevs_exploit_api.dll

Read straight from the shipped binary (x64, MSVC convention):

| export | proto | note |
|---|---|---|
| `initialize` | `void initialize()` | one-time setup, call first |
| `attach` | `int attach()` | binds to the client, returns status |
| `isAttached` | `bool isAttached()` | poll until true |
| `execute` | `void execute(const char* script)` | prologue null-checks rcx = async-safe on empty input |
| `DllMain` | `BOOL DllMain(HINSTANCE, DWORD, LPVOID)` | exported for manual-mapping, not needed here |

## Path 2 — private injection chain (no WRD)

| file | role |
|---|---|
| `injector/injector.cpp` | manual-maps a DLL into RobloxPlayerBeta, no LoadLibrary, no PEB entry |
| `client/client.cpp` | injected DLL: resolves Luau, hooks luaD_throw, pipe server |
| `push.cpp` | external console -> named pipe |

```
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE injector\injector.cpp /Fe:injector.exe
cl /std:c++17 /EHsc /O2 /LD /DUNICODE /D_UNICODE client\client.cpp /link /ENTRY:DllMain /Fe:client.dll
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE push.cpp /Fe:push.exe
injector.exe client.dll
push.exe script.lua
```

## Reality (both paths)

`wearedevs_exploit_api.dll` is a public binary — its exports and code are
fingerprinted. Loading it in your process is safe; what it does to the client
is what Byfron (Hyperion) watches for. Nothing here is "100% undetectable" —
Path 2 removes the *static* evidence, not the *behavioral* evidence. Path 2's
per-build signature in `client.cpp::ResolveFunctions()` is the piece that rots
weekly.
