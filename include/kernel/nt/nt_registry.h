/* ============================================================================
 * nt_registry.h -- NT registry syscall SSDT handlers
 *
 * Wires NtCreateKey, NtOpenKey, NtOpenKeyEx, NtDeleteKey, NtSetValueKey,
 * NtQueryValueKey, NtDeleteValueKey, NtEnumerateKey, NtEnumerateValueKey,
 * and NtQueryKey into the SSDT.  Each handler translates NT object-namespace
 * registry paths (\Registry\Machine\...) into Win32 RegXxx calls.
 *
 * TODO-05 section 14 (core CRUD).  Advanced operations in section 15.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"

/* ---- NT registry information classes ------------------------------------ */

/* KEY_INFORMATION_CLASS: used by NtEnumerateKey / NtQueryKey */
typedef enum {
    KeyBasicInformation      = 0,
    KeyNodeInformation       = 1,
    KeyFullInformation       = 2,
    KeyNameInformation       = 3,
} KEY_INFORMATION_CLASS;

/* KEY_VALUE_INFORMATION_CLASS: used by NtQueryValueKey / NtEnumerateValueKey */
typedef enum {
    KeyValueBasicInformation = 0,
    KeyValueFullInformation  = 1,
    KeyValuePartialInformation = 2,
} KEY_VALUE_INFORMATION_CLASS;

/* ---- NT registry output structures -------------------------------------- */

/* KEY_BASIC_INFORMATION: returned by NtEnumerateKey(KeyBasicInformation) */
typedef struct {
    uint64_t LastWriteTime;
    uint32_t TitleIndex;
    uint32_t NameLength;       /* bytes, not including NUL */
    char     Name[1];          /* variable-length, not NUL-terminated */
} KEY_BASIC_INFORMATION;

/* KEY_FULL_INFORMATION: returned by NtQueryKey(KeyFullInformation) */
typedef struct {
    uint64_t LastWriteTime;
    uint32_t TitleIndex;
    uint32_t ClassOffset;
    uint32_t ClassLength;
    uint32_t SubKeys;
    uint32_t MaxNameLen;
    uint32_t MaxClassLen;
    uint32_t Values;
    uint32_t MaxValueNameLen;
    uint32_t MaxValueDataLen;
} KEY_FULL_INFORMATION;

/* KEY_NAME_INFORMATION: returned by NtQueryKey(KeyNameInformation) */
typedef struct {
    uint32_t NameLength;       /* bytes */
    char     Name[1];          /* variable-length */
} KEY_NAME_INFORMATION;

/* KEY_VALUE_BASIC_INFORMATION */
typedef struct {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t NameLength;       /* bytes */
    char     Name[1];          /* variable-length */
} KEY_VALUE_BASIC_INFORMATION;

/* KEY_VALUE_FULL_INFORMATION */
typedef struct {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t DataOffset;
    uint32_t DataLength;
    uint32_t NameLength;       /* bytes */
    char     Name[1];          /* variable-length; data follows after Name */
} KEY_VALUE_FULL_INFORMATION;

/* KEY_VALUE_PARTIAL_INFORMATION */
typedef struct {
    uint32_t TitleIndex;
    uint32_t Type;
    uint32_t DataLength;
    uint8_t  Data[1];          /* variable-length */
} KEY_VALUE_PARTIAL_INFORMATION;

/* ---- CreateOptions for NtCreateKey -------------------------------------- */

#define REG_OPTION_NON_VOLATILE     0x00000000
#define REG_OPTION_VOLATILE         0x00000001
#define REG_OPTION_CREATE_LINK      0x00000002
#define REG_OPTION_BACKUP_RESTORE   0x00000004

/* ---- SSDT registration -------------------------------------------------- */

/* Register all core registry NtXxx handlers in the SSDT.
 * Called from boot_desktop.c after registry_init(). */
void nt_registry_register_ssdt(void);
