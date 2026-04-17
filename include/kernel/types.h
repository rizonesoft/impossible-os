/* ============================================================================
 * types.h -- Basic integer types for freestanding 64-bit environment
 *
 * Freestanding kernel build has no <stdint.h> / <stddef.h>, so this header
 * provides the fixed-width typedefs every kernel file needs. Host build
 * tools (tools/boot-info-manifest/ dumpers) include this alongside host
 * <stdint.h>; the _STDINT_H_INCLUDED / UINT8_MAX probe below skips the
 * typedef block when the host already supplied them, avoiding "typedef
 * redefinition" errors without changing kernel semantics.
 * ============================================================================ */

#pragma once

/* Host-libc detection. <stdint.h> defines UINT8_MAX (and friends); if the
 * host header already ran, keep its typedefs. Kernel freestanding build
 * never defines UINT8_MAX, so the typedefs below take effect as before. */
#if !defined(UINT8_MAX)
typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long         int64_t;
#endif

/* size_t / ssize_t / uintptr_t: host <stddef.h> / <sys/types.h> may have
 * already defined these. Use per-type probe macros. */
#if !defined(_SIZE_T_IMPOSSIBLE_OS_) && !defined(__size_t_defined) && !defined(_SIZE_T) && !defined(_SIZE_T_DEFINED)
typedef uint64_t            size_t;
#define _SIZE_T_IMPOSSIBLE_OS_
#endif
#if !defined(_SSIZE_T_IMPOSSIBLE_OS_) && !defined(__ssize_t_defined) && !defined(_SSIZE_T_DEFINED)
typedef int64_t             ssize_t;
#define _SSIZE_T_IMPOSSIBLE_OS_
#endif
#if !defined(_UINTPTR_T_IMPOSSIBLE_OS_) && !defined(__uintptr_t_defined) && !defined(UINTPTR_MAX)
typedef uint64_t            uintptr_t;
#define _UINTPTR_T_IMPOSSIBLE_OS_
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif
