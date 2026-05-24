/* ============================================================================
 * bugcheck.h -- Windows-compatible STOP code taxonomy and KeBugCheckEx
 *
 * Defines BUGCHECK_CODE constants matching the Windows NT STOP code namespace.
 * KeBugCheckEx is the single entry point for all kernel panics.
 *
 * XREF: 02-kernel-core/TODO-27-crash-dump-generation.md S1
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- BUGCHECK_CODE type ------------------------------------------------- */

typedef uint32_t BUGCHECK_CODE;

/* ---- Windows-compatible STOP codes -------------------------------------- */

#define BUGCHECK_IRQL_NOT_LESS_OR_EQUAL          ((BUGCHECK_CODE)0x0000000A)
#define BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED     ((BUGCHECK_CODE)0x0000001E)
#define BUGCHECK_PAGE_FAULT_IN_NONPAGED_AREA     ((BUGCHECK_CODE)0x00000050)
#define BUGCHECK_SYSTEM_SERVICE_EXCEPTION        ((BUGCHECK_CODE)0x0000003B)
#define BUGCHECK_MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED ((BUGCHECK_CODE)0x0000003E)
#define BUGCHECK_KERNEL_STACK_INPAGE_ERROR       ((BUGCHECK_CODE)0x00000077)
#define BUGCHECK_KERNEL_DATA_INPAGE_ERROR        ((BUGCHECK_CODE)0x0000007A)
#define BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE   ((BUGCHECK_CODE)0x00000139)
#define BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION   ((BUGCHECK_CODE)0x00000109)
#define BUGCHECK_CRITICAL_PROCESS_DIED           ((BUGCHECK_CODE)0x000000EF)
#define BUGCHECK_DRIVER_CORRUPTED_EXPOOL         ((BUGCHECK_CODE)0x000000C5)
#define BUGCHECK_DRIVER_IRQL_NOT_LESS_OR_EQUAL   ((BUGCHECK_CODE)0x000000D1)
#define BUGCHECK_MANUALLY_INITIATED_CRASH        ((BUGCHECK_CODE)0x000000E2)

/* ---- Impossible OS exclusive STOP codes (0xE0000000 range) -------------- */

#define BUGCHECK_IOS_BOOT_INIT_FAILED            ((BUGCHECK_CODE)0xE0000001)
#define BUGCHECK_IOS_HEAP_CORRUPTION             ((BUGCHECK_CODE)0xE0000002)
#define BUGCHECK_IOS_GUARD_PAGE_VIOLATION        ((BUGCHECK_CODE)0xE0000003)
#define BUGCHECK_IOS_INVARIANT_VIOLATION         ((BUGCHECK_CODE)0xE0000004)

/* ---- Bugcheck info structure -------------------------------------------- */

typedef struct {
    BUGCHECK_CODE code;
    uint64_t      param1;
    uint64_t      param2;
    uint64_t      param3;
    uint64_t      param4;
    uint64_t      timestamp;   /* PIT ticks at crash time */
} BUGCHECK_INFO;

/* ---- API ---------------------------------------------------------------- */

/* Get human-readable name for a STOP code. Returns "UNKNOWN" for unrecognized codes. */
const char *bugcheck_name(BUGCHECK_CODE code);

/* Main kernel crash entry point. Stores bugcheck info, renders BSOD, halts.
 * This function does NOT return. */
__attribute__((noreturn))
void KeBugCheckEx(BUGCHECK_CODE code, uint64_t p1, uint64_t p2,
                  uint64_t p3, uint64_t p4);

/* Read the last bugcheck info (valid after KeBugCheckEx or across boots via registry). */
const BUGCHECK_INFO *bugcheck_get_last(void);

/* Initialize bugcheck subsystem: wire NMI crash handler.
 * Call after IDT is set up (Phase 1). */
void bugcheck_init(void);

/* Called from keyboard IRQ handler to detect Ctrl+ScrollLock x2 crash trigger.
 * Only triggers if HKLM\SYSTEM\CrashControl\CrashOnCtrlScroll == 1. */
void bugcheck_keyboard_check(uint8_t scancode, int ctrl_held);
