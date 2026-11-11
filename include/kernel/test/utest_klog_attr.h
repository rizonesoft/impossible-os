/* utest_klog_attr.h -- the value grammar of a manifest's kernel-error lines.
 *
 * tests/usermode.manifest declares, per ring-3 binary, the kernel error lines
 * that binary provokes on purpose:
 *
 *     expects_klog=<tag>:<text>   the line must appear while the binary runs
 *     allows_klog=<tag>:<text>    the line may appear (a probe whose outcome varies)
 *
 * A manifest token ends at whitespace, so <text> is percent-encoded: `%20` is a
 * space, `%25` a percent sign, `%3A` may spell a colon. The value is split at
 * its FIRST literal `:` before anything is decoded, so an encoded colon can
 * never move the split. <tag> is a klog tag and is never encoded.
 *
 * Freestanding and allocation-free, with no kernel headers, so the host fuzz
 * target (tools/fuzz/fuzz_utest_klog_attr.c) compiles this same file.
 *
 * XREF: 00-infrastructure/TODO-03-kernel-test-harness.md section 13 */
#ifndef KERNEL_TEST_UTEST_KLOG_ATTR_H
#define KERNEL_TEST_UTEST_KLOG_ATTR_H

#define UTEST_KLOG_ATTR_OK          0
#define UTEST_KLOG_ATTR_E_SHAPE    -1   /* no `:`, or an empty tag or text      */
#define UTEST_KLOG_ATTR_E_TAG      -2   /* a tag byte outside [A-Za-z0-9._-]     */
#define UTEST_KLOG_ATTR_E_ESCAPE   -3   /* `%` not followed by two hex digits    */
#define UTEST_KLOG_ATTR_E_BYTE     -4   /* a decoded byte outside printable ASCII */
#define UTEST_KLOG_ATTR_E_LONG     -5   /* tag or text does not fit its cap      */

/* Decode one value (the bytes after `=`, NUL-terminated) in place.
 *
 * On success the buffer holds "<tag>\0<text>\0", *text_out points at the text,
 * strlen(tag) < tag_cap and strlen(text) < text_cap (both caps count the NUL),
 * and every text byte is printable ASCII (0x20..0x7E). Decoding never grows a
 * string, so it fits where it was read.
 *
 * On failure the buffer may be partly rewritten and *text_out is left alone;
 * the caller refuses the whole manifest line, so nothing reads it. */
int utest_klog_attr_decode(char *val, unsigned int tag_cap, unsigned int text_cap,
                           char **text_out);

#endif /* KERNEL_TEST_UTEST_KLOG_ATTR_H */
