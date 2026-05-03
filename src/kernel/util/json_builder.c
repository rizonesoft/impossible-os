/* SPDX-License-Identifier: MIT */
/* Kernel-side JSON builder. See include/kernel/util/json_builder.h. */

#include "kernel/util/json_builder.h"

void jb_init(struct json_builder *j, char *buf, size_t cap)
{
    j->buf = buf;
    j->pos = 0;
    j->cap = cap;
    j->truncated = 0;
}

void jb_putc(struct json_builder *j, char c)
{
    if (j->cap == 0) { j->truncated = 1; return; }
    if (j->pos < j->cap - 1)
        j->buf[j->pos++] = c;
    else
        j->truncated = 1;
}

void jb_puts(struct json_builder *j, const char *s)
{
    if (!s) return;
    while (*s)
        jb_putc(j, *s++);
}

/* JSON-escape a single char per RFC 8259 section 7. All bytes in
 * 0x00..0x1F not covered by \n \r \t emit as \u00XX. */
void jb_putesc(struct json_builder *j, unsigned char c)
{
    static const char hex[] = "0123456789abcdef";
    switch (c) {
    case '\\': jb_puts(j, "\\\\"); return;
    case '"':  jb_puts(j, "\\\""); return;
    case '\n': jb_puts(j, "\\n");  return;
    case '\r': jb_puts(j, "\\r");  return;
    case '\t': jb_puts(j, "\\t");  return;
    default:
        if (c < 0x20) {
            jb_puts(j, "\\u00");
            jb_putc(j, hex[(c >> 4) & 0xF]);
            jb_putc(j, hex[c & 0xF]);
            return;
        }
        jb_putc(j, (char)c);
    }
}

void jb_str(struct json_builder *j, const char *s)
{
    jb_putc(j, '"');
    if (s) {
        while (*s)
            jb_putesc(j, (unsigned char)*s++);
    }
    jb_putc(j, '"');
}

void jb_hex64(struct json_builder *j, uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    jb_puts(j, "\"0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        jb_putc(j, hex[(v >> shift) & 0xF]);
    jb_putc(j, '"');
}

void jb_u32_dec(struct json_builder *j, uint32_t v)
{
    char tmp[11];
    int n = 0;
    if (v == 0) { jb_putc(j, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) jb_putc(j, tmp[--n]);
}

void jb_u64_dec(struct json_builder *j, uint64_t v)
{
    char tmp[21];
    int n = 0;
    if (v == 0) { jb_putc(j, '0'); return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) jb_putc(j, tmp[--n]);
}
