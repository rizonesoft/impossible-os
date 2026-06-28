/* ============================================================================
 * ntstatus.h -- NT status type and canonical status codes
 *
 * NTSTATUS is a 32-bit signed value used by all NtXxx syscalls.
 * Bit 31 (sign bit) distinguishes success (0) from error (1).
 * Bits 31-30 encode severity: 00=success, 01=informational,
 * 10=warning, 11=error.
 *
 * This is the canonical home for NTSTATUS. Previously defined
 * temporarily in uefi_vars.h (which now forward-includes this).
 *
 * Reference: ntstatus.h from Windows SDK / ReactOS
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- NTSTATUS type ------------------------------------------------------- */

typedef int32_t NTSTATUS;

/* ---- Severity macros ----------------------------------------------------- */

/* ---- Status translation -------------------------------------------------- */

/* Translate an NTSTATUS to a Win32 error code (GetLastError value).
 * Returns 0 (ERROR_SUCCESS) for success statuses, or the mapped Win32
 * error for known codes, or 0x13D (ERROR_MR_MID_NOT_FOUND) for unknown. */
uint32_t RtlNtStatusToDosError(NTSTATUS status);

/* ---- Severity macros ----------------------------------------------------- */

#define NT_SUCCESS(s)       (((NTSTATUS)(s)) >= 0)
#define NT_INFORMATION(s)   ((((uint32_t)(s)) >> 30) == 1)
#define NT_WARNING(s)       ((((uint32_t)(s)) >> 30) == 2)
#define NT_ERROR(s)         ((((uint32_t)(s)) >> 30) == 3)

/* ---- Success / informational --------------------------------------------- */

#define STATUS_SUCCESS                      ((NTSTATUS)0x00000000)
#define STATUS_WAIT_0                       ((NTSTATUS)0x00000000)  /* wait satisfied (= SUCCESS) */
#define STATUS_ABANDONED                    ((NTSTATUS)0x00000080)  /* mutex abandoned by owning thread */
#define STATUS_PENDING                      ((NTSTATUS)0x00000103)  /* async I/O not yet complete */
#define STATUS_ALERTED                      ((NTSTATUS)0x00000101)  /* thread alerted during wait */
#define STATUS_TIMEOUT                      ((NTSTATUS)0x00000102)  /* wait timed out (not error) */

/* Warning (severity 10) -- partial success */
#define STATUS_BUFFER_OVERFLOW              ((NTSTATUS)0x80000005)  /* data truncated; partial result */
#define STATUS_NO_MORE_FILES                ((NTSTATUS)0x80000006)  /* directory enum exhausted */
#define STATUS_NO_MORE_ENTRIES              ((NTSTATUS)0x8000001A)  /* registry/object enum exhausted */
#define STATUS_DATATYPE_MISALIGNMENT        ((NTSTATUS)0x80000002)  /* buffer not aligned (warning severity) */

/* ---- Error codes -- object / handle -------------------------------------- */

#define STATUS_UNSUCCESSFUL                 ((NTSTATUS)0xC0000001)  /* generic failure */
#define STATUS_NOT_IMPLEMENTED              ((NTSTATUS)0xC0000002)  /* syscall not yet implemented */
#define STATUS_INVALID_INFO_CLASS           ((NTSTATUS)0xC0000003)  /* unknown information class */
#define STATUS_ACCESS_VIOLATION             ((NTSTATUS)0xC0000005)  /* user-buffer probe failed */
#define STATUS_INVALID_HANDLE               ((NTSTATUS)0xC0000008)  /* handle not in table or wrong type */
#define STATUS_INVALID_PARAMETER            ((NTSTATUS)0xC000000D)  /* bad argument value */
#define STATUS_INFO_LENGTH_MISMATCH         ((NTSTATUS)0xC0000004)  /* length field vs buffer */
#define STATUS_NO_MEMORY                    ((NTSTATUS)0xC0000017)  /* allocation failed */
#define STATUS_ACCESS_DENIED                ((NTSTATUS)0xC0000022)  /* SeAccessCheck denied */
#define STATUS_BUFFER_TOO_SMALL             ((NTSTATUS)0xC0000023)  /* output buffer too small */
#define STATUS_OBJECT_TYPE_MISMATCH         ((NTSTATUS)0xC0000024)  /* handle is wrong object type */
#define STATUS_OBJECT_NAME_INVALID          ((NTSTATUS)0xC0000033)  /* name fails validation (e.g. bad chars) */
#define STATUS_OBJECT_NAME_NOT_FOUND        ((NTSTATUS)0xC0000034)  /* named object not in namespace */
#define STATUS_OBJECT_NAME_COLLISION        ((NTSTATUS)0xC0000035)  /* name already exists */
#define STATUS_PORT_DISCONNECTED            ((NTSTATUS)0xC0000037)  /* ALPC port closed by peer */
#define STATUS_OBJECT_PATH_NOT_FOUND        ((NTSTATUS)0xC000003A)  /* intermediate path component missing */
#define STATUS_PORT_CONNECTION_REFUSED       ((NTSTATUS)0xC0000041)  /* ALPC connection rejected by server */
/* handle is not an ALPC port / wrong port subtype */
#define STATUS_INVALID_PORT_HANDLE           ((NTSTATUS)0xC0000042)
#define STATUS_REPLY_MESSAGE_MISMATCH       ((NTSTATUS)0xC000021F)  /* ALPC reply MessageId not in PendingQueue */
#define STATUS_NOT_FOUND                    ((NTSTATUS)0xC0000225)  /* generic not-found (UEFI vars etc.) */

/* ---- Error codes -- sync ------------------------------------------------- */

#define STATUS_SEMAPHORE_LIMIT_EXCEEDED     ((NTSTATUS)0xC0000047)  /* semaphore count exceeded max */
#define STATUS_MUTANT_NOT_OWNED             ((NTSTATUS)0xC0000046)  /* release mutex not owned by caller */

/* ---- Error codes -- sharing / lock ---------------------------------------- */

#define STATUS_SHARING_VIOLATION            ((NTSTATUS)0xC0000043)  /* conflicting share mode */

/* ---- Error codes -- file I/O --------------------------------------------- */

#define STATUS_INVALID_DEVICE_REQUEST       ((NTSTATUS)0xC0000010)  /* IRP to wrong device type */
#define STATUS_END_OF_FILE                  ((NTSTATUS)0xC0000011)  /* read past end of file */
#define STATUS_LOCK_NOT_GRANTED             ((NTSTATUS)0xC0000055)  /* byte-range lock denied */
#define STATUS_FILE_LOCK_CONFLICT           ((NTSTATUS)0xC0000054)  /* overlapping byte-range lock */
#define STATUS_INSUFFICIENT_RESOURCES       ((NTSTATUS)0xC000009A)  /* pool/resource exhausted */
#define STATUS_DELETE_PENDING               ((NTSTATUS)0xC0000056)  /* file marked for deletion */
#define STATUS_RANGE_NOT_LOCKED             ((NTSTATUS)0xC000007E)  /* unlock on non-locked range */
#define STATUS_FILE_IS_A_DIRECTORY          ((NTSTATUS)0xC00000BA)  /* file op on a directory */
#define STATUS_DIRECTORY_NOT_EMPTY          ((NTSTATUS)0xC0000101)  /* rmdir on non-empty dir */
#define STATUS_NOT_A_DIRECTORY              ((NTSTATUS)0xC0000103)  /* path component is not a dir */
#define STATUS_CANNOT_DELETE                ((NTSTATUS)0xC0000121)  /* file cannot be deleted */

/* ---- Error codes -- process / thread ------------------------------------- */

#define STATUS_SUSPEND_COUNT_EXCEEDED       ((NTSTATUS)0xC000004A)  /* too many suspends */
#define STATUS_THREAD_IS_TERMINATING        ((NTSTATUS)0xC000004B)  /* op on dying thread */
#define STATUS_PROCESS_IS_TERMINATING       ((NTSTATUS)0xC000010A)  /* op on dying process */
#define STATUS_THREAD_NOT_IN_PROCESS        ((NTSTATUS)0xC000012A)  /* thread not in target process */

/* ---- Error codes -- memory ----------------------------------------------- */

#define STATUS_CONFLICTING_ADDRESSES        ((NTSTATUS)0xC0000018)  /* VA range overlap */
#define STATUS_ALREADY_COMMITTED            ((NTSTATUS)0xC0000021)  /* double-commit VA region */
#define STATUS_INVALID_PAGE_PROTECTION      ((NTSTATUS)0xC0000045)  /* bad PAGE_* flags */
#define STATUS_SECTION_NOT_EXTENDED         ((NTSTATUS)0xC0000087)  /* section extend failed */
#define STATUS_MEMORY_NOT_ALLOCATED         ((NTSTATUS)0xC00000A0)  /* free on non-allocated VA */

/* ---- Error codes -- security --------------------------------------------- */

#define STATUS_PRIVILEGE_NOT_HELD           ((NTSTATUS)0xC0000061)  /* required privilege missing */
#define STATUS_BAD_IMPERSONATION_LEVEL      ((NTSTATUS)0xC00000A5)  /* impersonation level too low */

/* ---- Error codes -- registry --------------------------------------------- */

#define STATUS_KEY_DELETED                  ((NTSTATUS)0xC000017C)  /* op on deleted key */
#define STATUS_KEY_HAS_CHILDREN             ((NTSTATUS)0xC0000180)  /* delete key with subkeys */
#define STATUS_CHILD_MUST_BE_VOLATILE       ((NTSTATUS)0xC0000181)  /* non-volatile child under volatile parent */
#define STATUS_REGISTRY_CORRUPT             ((NTSTATUS)0xC000014C)  /* hive file failed integrity check */
#define STATUS_REGISTRY_IO_FAILED           ((NTSTATUS)0xC000014D)  /* hive I/O failed */

/* ---- Error codes -- debug ------------------------------------------------ */

#define STATUS_PORT_NOT_SET                 ((NTSTATUS)0xC0000353)  /* debug port not assigned */
#define STATUS_DEBUGGER_INACTIVE            ((NTSTATUS)0xC0000354)  /* no debugger attached */

/* ---- Error codes -- power / misc ----------------------------------------- */

#define STATUS_NOT_SUPPORTED                ((NTSTATUS)0xC00000BB)  /* feature not supported */
#define STATUS_DEVICE_NOT_READY             ((NTSTATUS)0xC00000A3)  /* device not initialized */
#define STATUS_MEDIA_WRITE_PROTECTED        ((NTSTATUS)0xC00000A2)  /* media is write-protected */
#define STATUS_IO_DEVICE_ERROR              ((NTSTATUS)0xC0000185)  /* I/O device error */
