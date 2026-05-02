/* SPDX-License-Identifier: MIT */
/* Kernel-side JSON builder with fail-closed truncation semantics.
 *
 * Used by every kernel JSON publisher (firmware-tables.json, future
 * BlackBox JSON writers, boot-health.json) to share one truncation
 * policy: if the buffer fills, the `truncated` flag latches and every
 * subsequent write is dropped. The CALLER is responsible for checking
 * `jb_truncated()` after the build completes and SKIPPING the file
 * write entirely on truncation -- a half-written JSON prefix without
 * its closing braces is non-parseable garbage that downstream consumers
 * cannot use. (jb_putc could reserve a tail budget for closing syntax;
 * fail-closed is simpler and the same outcome -- no consumer ever
 * receives malformed JSON.)
 *
 * Conventions:
 *   - All writes go through jb_putc / jb_puts.
 *   - String fields go through jb_str (RFC 8259 escape).
 *   - Integer fields go through jb_u32_dec.
 *   - Hex addresses go through jb_hex64 ("0x" + 16 lowercase digits).
 *
 * Not thread-safe; intended for one-shot Phase-3 publishers on the
 * BSP. */

#ifndef KERNEL_UTIL_JSON_BUILDER_H
#define KERNEL_UTIL_JSON_BUILDER_H

#include "kernel/types.h"

struct json_builder {
    char    *buf;
    size_t   pos;
    size_t   cap;
    int      truncated;
};

void jb_init(struct json_builder *j, char *buf, size_t cap);
void jb_putc(struct json_builder *j, char c);
void jb_puts(struct json_builder *j, const char *s);
void jb_putesc(struct json_builder *j, unsigned char c);
void jb_str(struct json_builder *j, const char *s);
void jb_hex64(struct json_builder *j, uint64_t v);
void jb_u32_dec(struct json_builder *j, uint32_t v);

static inline int    jb_truncated(const struct json_builder *j) { return j->truncated; }
static inline size_t jb_pos(const struct json_builder *j)       { return j->pos; }
static inline char * jb_buf(struct json_builder *j)             { return j->buf; }

#endif /* KERNEL_UTIL_JSON_BUILDER_H */
