/* libFuzzer harness: the boot.conf key=value lexer.
 *
 * Input is boot.conf as the loader reads it from the ESP, before any
 * signature check. boot_conf_next (src/boot/uefi/boot_conf_lexer.c) is the
 * loader's only reader of the grammar. Contract: no input reads outside the
 * buffer or writes past the key and value buffers, every pair is terminated
 * at the length it reports, and every call advances, so the loader's loop
 * ends.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/boot/boot_conf_lexer.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char key[BOOT_CONF_KEY_MAX], val[BOOT_CONF_VAL_MAX];
    unsigned int pos = 0, klen, vlen, last;
    boot_conf_tok_t t;

    if (size > 1u << 20)
        return 0;
    for (;;) {
        last = pos;
        t = boot_conf_next((const char *)data, (unsigned int)size, &pos,
                           key, &klen, val, &vlen);
        if (t == BOOT_CONF_EOF)
            break;
        if (pos <= last || pos > size)
            abort();
        if (klen >= BOOT_CONF_KEY_MAX || strlen(key) != klen)
            abort();
        if (t == BOOT_CONF_PAIR &&
            (vlen >= BOOT_CONF_VAL_MAX || strlen(val) != vlen))
            abort();
    }
    return 0;
}
