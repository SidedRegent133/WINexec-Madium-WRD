# roblox-exec — build + derivation notes

Private executor chain, WRD removed. Three moving parts:

| file | role |
|---|---|
| `injector/injector.cpp` | manual-maps `client.dll` into `RobloxPlayerBeta.exe`, no LoadLibrary, no PEB entry |
| `client/client.cpp` | injected DLL: resolves Luau API, hooks `luaD_throw` to capture a state, pipe server |
| `push.cpp` | external console that pushes a `.lua` into the pipe |

## Build (x64 Native Tools, MSVC)

```
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE injector\injector.cpp /Fe:injector.exe
cl /std:c++17 /EHsc /O2 /LD /DUNICODE /D_UNICODE client\client.cpp /link /ENTRY:DllMain /Fe:client.dll
cl /std:c++17 /EHsc /O2 /DUNICODE /D_UNICODE push.cpp /Fe:push.exe
```

## Run

```
injector.exe client.dll
push.exe script.lua
```

## The per-build part (the only thing that rots)

`client.cpp::ResolveFunctions()` carries one illustrative signature. On every
Roblox push the compiled bodies change and the sigs drift. Derivation loop:

1. Open the current `RobloxPlayerBeta.exe` in IDA/Ghidra (it ships with PDBs
   stripped but the RTTI/strings for Luau survive).
2. Find the Luau C-API by anchoring on exported strings: `"stack overflow"`,
   `"C stack overflow"`, `"attempt to index"`. `luaD_throw` sits in the error
   path that references these.
3. Dump the first ~16 bytes of each target and rebuild the sig + mask here.
4. `luaL_loadbuffer`, `lua_pcall`, `lua_newthread` — locate via the import
   graph around the state init or by tracing from `luaD_throw` callers.

## What this does NOT survive (name it before you run it)

- **Hyperion (Byfron), the live client protector.** It scans for foreign
  threads, external memory writes into the client, and hook bytes in `.text`.
  Manual mapping removes the static LoadLibrary evidence; it does not remove
  the *behavioral* evidence of an external process writing and a remote thread
  landing. On protected sessions this is caught at inject time.
- **Identity elevation.** This scaffold runs at default identity. Access to
  `game`/`workspace`/elevated funcs needs the ScriptContext identity setter,
  which is a separate per-build hook (`getglobal` patching or the thread's
  identity field). Not included above.
- **The 14-byte hook.** Fine for `luaD_throw`'s push-prologue. For any target
  whose first bytes are RIP-relative, replace the fixed patch with an
  instruction-length decoder (HDE64) and relocate.

## Next rung if you want it

1. Identity elevation hook (`luaD_precall` / thread identity field).
2. Scheduler-queued execution instead of pipe-thread execution — removes the
   cross-thread race on `g_state` and matches how the client runs scripts.
3. Thread-hiding for the worker + APC/section-based injection to drop the
   `CreateRemoteThread` detection vector.
