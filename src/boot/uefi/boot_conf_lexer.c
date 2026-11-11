/* boot_conf_lexer.c -- the boot.conf line lexer (see the header for the
 * grammar). Pure C, no UEFI types, no static state: BOOTX64's .bss is not
 * zeroed by firmware, so this file keeps none.
 */

#include "../../../include/boot/boot_conf_lexer.h"

static int is_blank(char c) { return c == ' ' || c == '\t'; }

/* The byte at i, or NUL past the end: a NUL byte and the end are the same. */
static char at(const char *buf, unsigned int len, unsigned int i)
{
    return i < len ? buf[i] : '\0';
}

static unsigned int skip_line(const char *buf, unsigned int len, unsigned int p)
{
    while (at(buf, len, p) != '\0' && at(buf, len, p) != '\n')
        p++;
    if (at(buf, len, p) == '\n')
        p++;
    return p;
}

boot_conf_tok_t boot_conf_next(const char *buf, unsigned int len, unsigned int *pos,
                               char key[BOOT_CONF_KEY_MAX], unsigned int *klen,
                               char val[BOOT_CONF_VAL_MAX], unsigned int *vlen)
{
    unsigned int p = *pos;

    key[0] = '\0';
    val[0] = '\0';
    *klen = 0;
    *vlen = 0;

    while (at(buf, len, p) != '\0') {
        unsigned int ki = 0, vi = 0, vend = 0;
        char c;

        while (is_blank(at(buf, len, p)))
            p++;
        c = at(buf, len, p);
        if (c == '\n' || c == '\r' || c == '#' || c == '\0') {
            p = skip_line(buf, len, p);
            continue;
        }

        /* Key: up to '=', end of line or '#'; blanks inside are dropped. */
        while ((c = at(buf, len, p)) != '\0' && c != '=' && c != '\n' && c != '#') {
            if (!is_blank(c)) {
                if (ki == BOOT_CONF_KEY_MAX - 1) {
                    key[ki] = '\0';
                    *klen = ki;
                    *pos = skip_line(buf, len, p);
                    return BOOT_CONF_KEY_TOO_LONG;
                }
                key[ki++] = c;
            }
            p++;
        }
        key[ki] = '\0';

        if (at(buf, len, p) != '=') {
            p = skip_line(buf, len, p);
            continue;
        }
        p++;

        while (is_blank(at(buf, len, p)))
            p++;

        /* Value: to end of line, '\r' or '#'. Trailing blanks do not count
         * toward the limit, since they are trimmed. */
        while ((c = at(buf, len, p)) != '\0' && c != '\n' && c != '\r' && c != '#') {
            if (vi == BOOT_CONF_VAL_MAX - 1) {
                /* Blanks past a full buffer are trimmed anyway; anything
                 * else is value the buffer cannot hold. */
                if (!is_blank(c)) {
                    *klen = ki;
                    *pos = skip_line(buf, len, p);
                    return BOOT_CONF_VALUE_TOO_LONG;
                }
                p++;
                continue;
            }
            val[vi++] = c;
            if (!is_blank(c))
                vend = vi;
            p++;
        }
        val[vend] = '\0';

        *klen = ki;
        *vlen = vend;
        *pos = skip_line(buf, len, p);
        return BOOT_CONF_PAIR;
    }

    *pos = p;
    return BOOT_CONF_EOF;
}
