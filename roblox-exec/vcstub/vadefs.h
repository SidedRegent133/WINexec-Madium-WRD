#ifndef _VCSTUB_VADEFS_H
#define _VCSTUB_VADEFS_H
// Minimal va_list surface for the clang-without-MSVC build. clang's resource
// vadefs.h does `#include_next <vadefs.h>` expecting the VC copy; this stands
// in for it.
#ifdef __cplusplus
extern "C" {
#endif
typedef __builtin_va_list va_list;
typedef __builtin_va_list __va_list;
#ifdef __cplusplus
}
#endif
#ifndef __va_start
#define __va_start(ap, last) __builtin_va_start(ap, last)
#endif
#ifndef __va_end
#define __va_end(ap) __builtin_va_end(ap)
#endif
#ifndef __va_arg
#define __va_arg(ap, t) __builtin_va_arg(ap, t)
#endif
#ifndef __va_copy
#define __va_copy(d, s) __builtin_va_copy(d, s)
#endif
#endif
