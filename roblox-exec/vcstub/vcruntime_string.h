#ifndef _VCSTUB_VCRUNTIME_STRING_H
#define _VCSTUB_VCRUNTIME_STRING_H
// mem* surface normally declared here by vcruntime.h. Size type spelled as
// an intrinsic to dodge size_t guard collisions during the include cycle.
#include <vcruntime.h>
#ifdef __cplusplus
extern "C" {
#endif
void* __cdecl memchr(void const* _Buf, int _Val, unsigned __int64 _MaxCount);
int   __cdecl memcmp(void const* _Buf1, void const* _Buf2, unsigned __int64 _Size);
void* __cdecl memcpy(void* _Dst, void const* _Src, unsigned __int64 _Size);
void* __cdecl memmove(void* _Dst, void const* _Src, unsigned __int64 _Size);
void* __cdecl memset(void* _Dst, int _Val, unsigned __int64 _Size);
#ifdef __cplusplus
}
#endif
#endif
