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

/* ---- Sticky record writes outside the Phase-3 ack -------------------------
 *
 * Every kernel write of ImpossibleOS-BootSticky is a read-modify-write of the
 * record now in NVRAM, so a writer changes only the field it owns. A fresh
 * record is built ONLY when the read says the variable does not exist; any
 * other read failure, or a record that fails validation, refuses the write,
 * because a fresh record would erase a pending watchdog request and the
 * sequence fields. */

struct boot_sticky_record;

/* Pure. `read_status` and `read_size` are what uefi_var_get returned for
 * `cur`. Returns 0 with *out the record to modify (a copy of cur, or a fresh
 * zeroed record on STATUS_NOT_FOUND), or -1 when no write may happen. */
int boot_sticky_rmw_prepare(int32_t read_status, size_t read_size,
                            const struct boot_sticky_record *cur,
                            struct boot_sticky_record *out);

/* Pure. Apply the Phase-3 ack to `rec`: clear each bit the ladder consumed,
 * except that a recovery trigger THIS boot wrote (`trigger_written` 0 spent,
 * 1 armed, -1 untouched) keeps the value written. */
void boot_sticky_ack_apply(struct boot_sticky_record *rec, int recovery_consumed,
                           int watchdog_consumed, int trigger_written);

/* Pure. The Phase-3 ack's whole decision: prepare the live record read
 * (`read_status`, `read_size`, `cur`) and apply the ack to it in *out.
 * Returns -1 when no write may happen, so a record that could not be read is
 * never rebuilt from the loader snapshot, which lacks the kernel-owned
 * fields (the offer dismissal). */
int boot_sticky_ack_build(int32_t read_status, size_t read_size,
                          const struct boot_sticky_record *cur,
                          int recovery_consumed, int watchdog_consumed,
                          int trigger_written, struct boot_sticky_record *out);

enum boot_sticky_field {
    BOOT_STICKY_FIELD_RECOVERY_TRIGGER = 0,
    BOOT_STICKY_FIELD_OFFER_DISMISSED  = 1,
};

/* Set one field durably. 0 = written, -1 = refused or the write failed. A
 * recovery-trigger write is also remembered for this boot, so the Phase-3 ack
 * never resurrects a spent trigger or wipes a fresh arm. */
int boot_sticky_set_field(enum boot_sticky_field field, uint8_t value);

/* Read the enrollment-offer dismissal from NVRAM: 1 dismissed, 0 not, -1 the
 * record could not be read authoritatively (treat as dismissed: no prompt). */
int boot_sticky_offer_dismissed(void);

#endif /* KERNEL_BOOT_AUDIT_H */
