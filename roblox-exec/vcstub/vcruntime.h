#ifndef _VCSTUB_VCRUNTIME_H
#define _VCSTUB_VCRUNTIME_H
// Minimal MSVC-compat shim so clang can parse the UCRT/SDK headers without
// the Visual C++ toolchain installed. Defines only the interface corecrt.h
// and friends consume; clang supplies the language keywords and intrinsics.
#include <sal.h>
#include <vadefs.h>
#include <stdint.h>

#ifndef _VCRTIMP
#define _VCRTIMP
#endif
#ifndef _CRTIMP
#define _CRTIMP
#endif
#ifndef _CRTDECL
#define _CRTDECL __cdecl
#endif
#ifndef __CRTDECL
#define __CRTDECL __cdecl
#endif

// wide/unsigned integer primitives the real header supplies for C
#ifndef __cplusplus
#ifndef _WCHAR_T_DEFINED
typedef unsigned short wchar_t;
#define _WCHAR_T_DEFINED
#endif
#endif

#ifndef _CRT_BEGIN_C_HEADER
#ifdef __cplusplus
#define _CRT_BEGIN_C_HEADER extern "C" {
#define _CRT_END_C_HEADER   }
#else
#define _CRT_BEGIN_C_HEADER
#define _CRT_END_C_HEADER
#endif
#endif

#ifndef _CRT_MEMORY_DEFINED
#define _CRT_MEMORY_DEFINED
#endif

// deprecation / warning shims referenced across UCRT
#ifndef _CRT_DEPRECATE_TEXT
#define _CRT_DEPRECATE_TEXT(_Text) __declspec(deprecated(_Text))
#endif
#ifndef _CRT_INSECURE_DEPRECATE
#define _CRT_INSECURE_DEPRECATE(_Replacement)
#endif
#ifndef _CRT_INSECURE_DEPRECATE_MEMORY
#define _CRT_INSECURE_DEPRECATE_MEMORY(_Replacement)
#endif
#ifndef _CRT_INSECURE_DEPRECATE_GLOBALS
#define _CRT_INSECURE_DEPRECATE_GLOBALS(_Replacement)
#endif
#ifndef _CRT_OBSOLETE
#define _CRT_OBSOLETE(_NewItem)
#endif

// compiler-provided SEH intrinsics are clang builtins; nothing else needed.

#endif
