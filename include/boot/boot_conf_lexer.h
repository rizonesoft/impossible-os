/* boot_conf_lexer.h -- the boot.conf line lexer.
 *
 * boot.conf is read from the ESP before anything on it is verified, so every
 * byte is untrusted. This is the only code that splits it into key=value
 * pairs. Pure C, no UEFI types, no state, no allocation: linked into
 * BOOTX64.EFI and built for the host by tools/fuzz.
 *
 * Grammar, one pair per line: optional blanks, a key (blanks inside it are
 * dropped), '=', optional blanks, a value running to the end of the line or a
 * '#', with trailing blanks trimmed. Blank lines, lines starting with '#' and
 * lines with no '=' are skipped. NUL bytes may pad the end of the text; text
 * after a NUL is an error, since it would otherwise be dropped unread.
 */
#ifndef BOOT_CONF_LEXER_H
#define BOOT_CONF_LEXER_H

#define BOOT_CONF_KEY_MAX 64    /* key buffer, including the terminator */
#define BOOT_CONF_VAL_MAX 256   /* value buffer, including the terminator */

typedef enum {
    BOOT_CONF_EOF = 0,          /* no more pairs */
    BOOT_CONF_PAIR,             /* key and val hold one pair */
    BOOT_CONF_KEY_TOO_LONG,     /* a key longer than BOOT_CONF_KEY_MAX - 1 */
    BOOT_CONF_VALUE_TOO_LONG,   /* a value longer than BOOT_CONF_VAL_MAX - 1 */
    BOOT_CONF_EMBEDDED_NUL      /* a NUL byte with more text after it */
} boot_conf_tok_t;

/* Return the next pair at or after *pos in buf[0..len). On BOOT_CONF_PAIR,
 * key and val are terminated and *klen / *vlen give their lengths (either may
 * be 0: "=x" and "key=" are pairs). On a too-long result, key holds the key
 * read so far, so the caller can name it. *pos always advances past the line
 * it read, so a caller that keeps going cannot loop. An overlong key or value
 * is reported, never truncated: a cut-off value is a different setting. A key
 * is overlong only on a line that has an '='. After BOOT_CONF_EMBEDDED_NUL
 * *pos is at the end, so the next call returns BOOT_CONF_EOF. */
boot_conf_tok_t boot_conf_next(const char *buf, unsigned int len, unsigned int *pos,
                               char key[BOOT_CONF_KEY_MAX], unsigned int *klen,
                               char val[BOOT_CONF_VAL_MAX], unsigned int *vlen);

#endif /* BOOT_CONF_LEXER_H */
