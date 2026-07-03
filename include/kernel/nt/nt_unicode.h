/* ============================================================================
 * nt_unicode.h -- Canonical UNICODE_STRING validation + bounded-copy primitive
 *
 * The single authority for handling NT UNICODE_STRING (counted UTF-16LE) inputs
 * at the syscall boundary. Every nt_*.c handler should decode user-supplied
 * UNICODE_STRING arguments through these helpers instead of casting
 * `ObjectName->Buffer` to `char *` (the ad-hoc ASCII-treating pattern this layer
 * replaces).
 *
 * Design:
 *   - Fixed caller-provided kernel buffers (no kmalloc in the syscall hot path).
 *   - Caller buffer capacity is the effective length cap; NT_UNICODE_MAX_BYTES
 *     is only the absolute uint16 ceiling.
 *   - TOCTOU-safe: the struct is snapshot-copied, then the buffer is copied once
 *     into kernel memory before any validation acts on the contents.
 *   - Stateless / re-entrant: no shared mutable state, safe on all CPUs.
 *   - Transcoding (UTF-16 to/from UTF-8 or code pages) is owned by the code-page
 *     conversion layer, NOT here. This layer keeps validated data as UTF-16 and
 *     offers only a lossless ASCII-range narrowing bridge (`nt_unicode_to_ascii`)
 *     for interim char* consumers.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/ob/peb.h"   /* UNICODE_STRING */

/* Absolute ceiling: UNICODE_STRING.Length is uint16, so the largest even byte
 * count is 65534 (32767 WCHARs). This is a hard upper bound; the real limit for
 * any given call is the caller-provided buffer capacity. */
#define NT_UNICODE_MAX_BYTES   65534u
#define NT_UNICODE_MAX_WCHARS  (NT_UNICODE_MAX_BYTES / 2u)   /* 32767 */

/* Overflow-safe WCHAR-count -> byte-count. Returns 0 and leaves *out_bytes
 * untouched on overflow past NT_UNICODE_MAX_BYTES; returns 1 on success. */
int nt_unicode_wchars_to_bytes(uint32_t wchars, uint32_t *out_bytes);

/*
 * nt_unicode_string_validate -- snapshot-copy and validate a UNICODE_STRING.
 *
 * When prev_mode == SSDT_USER_MODE, `us` and its Buffer are probed and the
 * struct fields are copied out of user memory before validation (TOCTOU-safe).
 * Validates: Buffer non-NULL, Length even (WCHAR-aligned), Length <=
 * MaximumLength, Length <= NT_UNICODE_MAX_BYTES.
 *
 * On success writes the snapshotted Buffer pointer to *out_buffer and the byte
 * Length to *out_length (either out pointer may be NULL). Returns
 * STATUS_SUCCESS, STATUS_INVALID_PARAMETER, or a probe/copy fault status.
 */
NTSTATUS nt_unicode_string_validate(const UNICODE_STRING *us, uint32_t prev_mode,
                                    uint16_t **out_buffer, uint32_t *out_length);

/*
 * nt_decode_unicode_string -- validate then copy the UTF-16 payload into a
 * caller-provided kernel WCHAR buffer, NUL-terminated.
 *
 * kbuf_wchars is the capacity of kbuf in WCHARs INCLUDING the terminating NUL.
 * *out_wchars (may be NULL) receives the code-unit count written (excluding NUL).
 * Returns STATUS_BUFFER_TOO_SMALL if the string plus terminator does not fit,
 * STATUS_INVALID_PARAMETER on a malformed UNICODE_STRING, or a copy-fault status.
 * A zero-Length string is valid and yields an empty (single-NUL) result.
 */
NTSTATUS nt_decode_unicode_string(const UNICODE_STRING *us, uint16_t *kbuf,
                                  uint32_t kbuf_wchars, uint32_t *out_wchars,
                                  uint32_t prev_mode);

/*
 * nt_encode_unicode_string -- build a UNICODE_STRING from a kernel WCHAR buffer.
 * Sets Length = wchars*2, MaximumLength = Length + 2 (room for a NUL), Buffer =
 * wbuf. Returns STATUS_INVALID_PARAMETER if wchars*2 would exceed the uint16
 * ceiling. Does not copy; the caller owns wbuf's lifetime.
 */
NTSTATUS nt_encode_unicode_string(UNICODE_STRING *us, uint16_t *wbuf,
                                  uint32_t wchars);

/*
 * nt_unicode_to_ascii -- lossless ASCII-range narrowing bridge.
 *
 * Copies a kernel-resident UTF-16 buffer to abuf as bytes, NUL-terminated.
 * Rejects (STATUS_INVALID_PARAMETER) any code unit > 0x7F or any embedded NUL
 * (0x0000) -- lossless: a byte-string consumer sees exactly the intended name or
 * a clean error, never a truncated/misrouted path. abuf_size is the byte
 * capacity INCLUDING the terminator; STATUS_BUFFER_TOO_SMALL if it does not fit.
 * *out_len (may be NULL) receives the byte length written (excluding NUL).
 * Interim bridge only; full transcoding is owned by the code-page layer.
 */
NTSTATUS nt_unicode_to_ascii(const uint16_t *wbuf, uint32_t wchars, char *abuf,
                             uint32_t abuf_size, uint32_t *out_len);
