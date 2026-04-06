/* ============================================================================
 * nt_types.h -- Foundational NT types for syscall interfaces
 *
 * Re-exports types already defined elsewhere (HANDLE, UNICODE_STRING,
 * LARGE_INTEGER, CLIENT_ID) and defines new types needed by NtXxx APIs
 * (IO_STATUS_BLOCK, OBJECT_ATTRIBUTES, ACCESS_MASK).
 *
 * Include this single header to get all NT primitive types.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- Re-exported types (canonical definitions elsewhere) ----------------- */

/* HANDLE -- kernel handle (int32_t, multiples of 4).
 * Canonical: include/kernel/ob/handle_table.h */
#include "kernel/ob/handle_table.h"

/* UNICODE_STRING, LARGE_INTEGER, LIST_ENTRY.
 * Canonical: include/kernel/ob/peb.h */
#include "kernel/ob/peb.h"

/* CLIENT_ID -- {UniqueProcess, UniqueThread}.
 * Canonical: include/kernel/ob/teb.h */
#include "kernel/ob/teb.h"

/* ---- ACCESS_MASK --------------------------------------------------------- */

typedef uint32_t ACCESS_MASK;

#define GENERIC_READ                0x80000000
#define GENERIC_WRITE               0x40000000
#define GENERIC_EXECUTE             0x20000000
#define GENERIC_ALL                 0x10000000

#define DELETE                      0x00010000
#define READ_CONTROL                0x00020000
#define WRITE_DAC                   0x00040000
#define WRITE_OWNER                 0x00080000
#define SYNCHRONIZE                 0x00100000

#define STANDARD_RIGHTS_READ        READ_CONTROL
#define STANDARD_RIGHTS_WRITE       READ_CONTROL
#define STANDARD_RIGHTS_EXECUTE     READ_CONTROL
#define STANDARD_RIGHTS_ALL         (DELETE | READ_CONTROL | WRITE_DAC | WRITE_OWNER | SYNCHRONIZE)

#define MAXIMUM_ALLOWED             0x02000000

/* ---- IO_STATUS_BLOCK ----------------------------------------------------- */

typedef struct {
    NTSTATUS Status;
    uint32_t _pad;              /* align Information to 8 bytes */
    uint64_t Information;       /* bytes transferred or enum context */
} IO_STATUS_BLOCK;

/* ---- OBJECT_ATTRIBUTES --------------------------------------------------- */

/* OBJ_INHERIT and OBJ_PROTECT_CLOSE are defined in handle_table.h with
 * Windows-compatible values. The remaining OBJ_* flags are OBJECT_ATTRIBUTES
 * flags used by NtCreateXxx APIs -- defined here as the canonical location. */
#define OBJ_PERMANENT               0x00000010
#define OBJ_EXCLUSIVE               0x00000020
#define OBJ_CASE_INSENSITIVE        0x00000040
#define OBJ_OPENIF                  0x00000080
#define OBJ_OPENLINK                0x00000100
#define OBJ_KERNEL_HANDLE           0x00000200
#define OBJ_FORCE_ACCESS_CHECK      0x00000400
#define OBJ_VALID_ATTRIBUTES        0x000007F2

typedef struct {
    uint64_t        Length;             /* sizeof(OBJECT_ATTRIBUTES) */
    HANDLE          RootDirectory;      /* optional -- relative open */
    uint32_t        _pad1;             /* align ObjectName to 8 bytes */
    UNICODE_STRING *ObjectName;         /* path in kernel namespace */
    uint32_t        Attributes;         /* OBJ_* flags */
    uint32_t        _pad2;             /* align to 8 bytes */
    void           *SecurityDescriptor; /* optional SECURITY_DESCRIPTOR */
    void           *SecurityQualityOfService;  /* optional SQOS */
} OBJECT_ATTRIBUTES;

/* Helper macro -- matches Windows InitializeObjectAttributes */
#define InitializeObjectAttributes(p, n, a, r, s) do { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); \
    (p)->RootDirectory = (r); \
    (p)->_pad1 = 0; \
    (p)->ObjectName = (n); \
    (p)->Attributes = (a); \
    (p)->_pad2 = 0; \
    (p)->SecurityDescriptor = (s); \
    (p)->SecurityQualityOfService = (void *)0; \
} while (0)

/* ---- CONTEXT forward declaration ----------------------------------------- */
/* Full definition in include/kernel/panic.h (crash dump use).
 * Will move to except.h when TODO-10 (Exception Dispatch & SEH) lands.
 * Forward-declared here so function signatures can use CONTEXT*. */
struct _CONTEXT;
#ifndef _CONTEXT_TYPEDEF_DEFINED
#define _CONTEXT_TYPEDEF_DEFINED
typedef struct _CONTEXT CONTEXT;
#endif
