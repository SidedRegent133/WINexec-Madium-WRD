#ifndef _VCSTUB_EXCPT_H
#define _VCSTUB_EXCPT_H
// Minimal MSVC-compat shim. clang provides the SEH keywords and the
// _exception_code/_exception_info/_abnormal_termination intrinsics; windows.h
// only needs the disposition enum to parse.
#ifdef __cplusplus
extern "C" {
#endif
typedef enum _EXCEPTION_DISPOSITION {
    ExceptionContinueExecution = 0,
    ExceptionContinueSearch    = 1,
    ExceptionNestedException   = 2,
    ExceptionCollidedUnwind    = 3
} EXCEPTION_DISPOSITION;
#ifdef __cplusplus
}
#endif
#endif
