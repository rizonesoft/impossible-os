/* boot_audit.h -- the BlackBox boot-audit sequence counter.
 *
 * Every published history.jsonl line carries a boot_seq that must be strictly
 * greater than every boot_seq published before it. The counter lives in two
 * files on the BlackBox partition (BOOT_AUDIT_SEQUENCE_PATH and
 * BOOT_AUDIT_SEQUENCE_TEMP_PATH); the reader takes the larger valid value.
 *
 * File access goes through struct boot_audit_seq_io so the unit tests can drive
 * the real advance logic against an in-memory pair of files with injected write
 * failures. boot_audit_publish() passes the VFS-backed ops.
 */
#ifndef KERNEL_BOOT_AUDIT_H
#define KERNEL_BOOT_AUDIT_H

#include "kernel/types.h"

struct boot_audit_seq_io {
    /* Read up to `cap` bytes of `path` into `buf`. Returns the byte count,
     * BOOT_AUDIT_SEQ_READ_ABSENT when the file does not exist, or
     * BOOT_AUDIT_SEQ_READ_ERROR when it exists but its contents could not all
     * be read (a short read against the file size, or a file larger than cap). */
    int (*read)(void *ctx, const char *path, uint8_t *buf, uint32_t cap);
    /* Replace the contents of `path` with `buf[0..n)`. Returns 1 only when all
     * n bytes were written. A failed write may have destroyed the old
     * contents (the open truncates), so callers must assume it did. */
    int (*write)(void *ctx, const char *path, const char *buf, uint32_t n);
    void *ctx;
};

#define BOOT_AUDIT_SEQ_READ_ABSENT (-1)
#define BOOT_AUDIT_SEQ_READ_ERROR  (-2)

/* What one counter file says. */
enum boot_audit_seq_state {
    BOOT_AUDIT_SEQ_VALID   = 0,  /* 1-10 digits then "\n" (or "\r\n"): the writer's format */
    BOOT_AUDIT_SEQ_STALE   = 1,  /* absent, empty, or digits with no terminator: what an
                                  * interrupted write of this writer leaves; reads as 0 */
    BOOT_AUDIT_SEQ_UNKNOWN = 2,  /* read error, garbage, above UINT32_MAX: the file may
                                  * have held the high-water mark, so nothing is safe */
};

enum boot_audit_seq_result {
    BOOT_AUDIT_SEQ_OK        = 0,  /* both files hold the new value */
    BOOT_AUDIT_SEQ_PARTIAL   = 1,  /* one file holds it: durable, publish */
    BOOT_AUDIT_SEQ_FAILED    = 2,  /* not durable: do not publish */
    BOOT_AUDIT_SEQ_EXHAUSTED = 3,  /* counter at UINT32_MAX: do not publish */
    BOOT_AUDIT_SEQ_UNREADABLE = 4, /* a file is UNKNOWN: nothing written, do not publish */
};

/* Classify one file from what io->read returned (`n` bytes in `buf`, or a
 * BOOT_AUDIT_SEQ_READ_* code). *value is set for VALID and is 0 otherwise.
 * STALE is safe to read as 0 because this writer leaves it only on the file
 * the protocol did not need: the second write of a PARTIAL boot, or the first
 * write of a FAILED one, so the other file holds the high-water mark. An
 * external truncation to a digit prefix looks the same and is not detected. */
enum boot_audit_seq_state boot_audit_seq_classify(const uint8_t *buf, int n, uint32_t *value);

/* Format `seq` as decimal plus '\n' into `buf` (cap >= BOOT_AUDIT_SEQUENCE_LEN_MAX).
 * Returns the byte count, or 0 when `cap` is too small. */
uint32_t boot_audit_seq_format(uint32_t seq, char *buf, uint32_t cap);

/* Allocate the next boot_seq and persist it. On OK or PARTIAL, *out_seq is the
 * value to publish. Writes the file holding the lower value first and touches
 * the file holding the high-water mark only after that write succeeded, so at
 * every point one file still holds a value >= every published boot_seq. Fails
 * closed (UNREADABLE, nothing written) when either file is UNKNOWN; the next
 * boot retries, and a damaged file stops publishing until it is repaired. */
enum boot_audit_seq_result boot_audit_sequence_advance(const struct boot_audit_seq_io *io,
                                                       uint32_t *out_seq);

#endif /* KERNEL_BOOT_AUDIT_H */
