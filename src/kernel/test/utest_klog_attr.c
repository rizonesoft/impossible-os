/* utest_klog_attr.c -- decode a manifest `expects_klog=` / `allows_klog=` value.
 *
 * The manifest is read from the disk image, so every byte here is untrusted:
 * nothing is truncated, every refusal is a named code, and the caller turns
 * any refusal into a refused manifest line. Contract in
 * include/kernel/test/utest_klog_attr.h. */
#include "kernel/test/utest_klog_attr.h"

static int tag_byte(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int utest_klog_attr_decode(char *val, unsigned int tag_cap, unsigned int text_cap,
                           char **text_out)
{
    unsigned int colon = 0, r, w;

    if (!val || !text_out)
        return UTEST_KLOG_ATTR_E_SHAPE;
    /* Split first, on the literal byte: decoding happens only to the right. */
    while (val[colon] && val[colon] != ':')
        colon++;
    if (val[colon] != ':' || colon == 0 || val[colon + 1] == '\0')
        return UTEST_KLOG_ATTR_E_SHAPE;
    for (unsigned int i = 0; i < colon; i++)
        if (!tag_byte(val[i]))
            return UTEST_KLOG_ATTR_E_TAG;
    if (colon + 1 > tag_cap)
        return UTEST_KLOG_ATTR_E_LONG;

    /* Decode the text in place. `w` never passes `r`, so the write side only
     * ever overwrites bytes already read. */
    r = w = colon + 1;
    while (val[r]) {
        int byte;

        if (val[r] == '%') {
            int hi = hex_value(val[r + 1]);
            int lo = hi < 0 ? -1 : hex_value(val[r + 2]);

            if (hi < 0 || lo < 0)
                return UTEST_KLOG_ATTR_E_ESCAPE;
            byte = hi * 16 + lo;
            r += 3;
        } else {
            byte = (unsigned char)val[r];
            r++;
        }
        if (byte < 0x20 || byte > 0x7E)
            return UTEST_KLOG_ATTR_E_BYTE;
        if (w - colon >= text_cap)   /* w - colon - 1 bytes kept, plus this one, plus NUL */
            return UTEST_KLOG_ATTR_E_LONG;
        val[w++] = (char)byte;
    }
    val[w] = '\0';
    val[colon] = '\0';
    *text_out = &val[colon + 1];
    return UTEST_KLOG_ATTR_OK;
}
