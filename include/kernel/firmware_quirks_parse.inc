/* SPDX-License-Identifier: MIT */
/* Shared tokenizer for `firmware_quirk_disable=` boot-config value.
 *
 * Single source of truth used by:
 *   - src/kernel/firmware_quirks.c  (post-handoff parser)
 *   - src/boot/uefi/bootx64.c       (boot.conf parser, UEFI environment)
 *
 * Format: comma-separated quirk names, optionally separated by spaces/tabs.
 * Token max length 63 chars (oversized tokens are silently truncated; the
 * truncated form will not match any quirk name). Returns a bitmask of
 * matching quirks; unknown names are ignored.
 *
 * The qmap[] is generated from `firmware_quirks_table.inc` via the shared
 * X-macro so {name, bit} parity is enforced at compile time across both
 * sides. Extracting the tokenizer here closes the second drift surface
 * (whitespace handling, oversize truncation, separator semantics).
 *
 * Type-agnostic to remain includable from both freestanding kernel C
 * (kernel/types.h: uint32_t) and gnu-efi bootloader (UINT8/UINTN); uses
 * plain `unsigned` (32-bit on x86_64 in both environments). */

/* Compile-time guard: every quirk bit must fit in the 8-bit
 * `boot_config.firmware_quirk_disable` handoff field. Without this,
 * a future quirk with bit > 0x80 would parse correctly here but get
 * silently truncated when the bootloader narrows the inline result
 * to UINT8 at handoff time. Fires in every TU that includes this
 * file (kernel firmware_quirks.c + bootloader bootx64.c). */
#define FW_QUIRK_DEF(id, bit_, name_) \
    _Static_assert((bit_) <= 0xFFu, \
                   "quirk bit must fit in u8 boot_info handoff field");
#include "firmware_quirks_table.inc"
#undef FW_QUIRK_DEF

static inline unsigned
firmware_quirks_parse_disable_inline(const char *list)
{
    static const struct { const char *name; unsigned bit; } qmap[] = {
#define FW_QUIRK_DEF(id, bit_, name_) { name_, (unsigned)(bit_) },
#include "firmware_quirks_table.inc"
#undef FW_QUIRK_DEF
    };
    enum { qmap_n = (unsigned)(sizeof(qmap) / sizeof(qmap[0])) };

    if (!list || !*list) return 0;

    unsigned mask = 0;
    char     tok[64];
    unsigned pos = 0;
    const char *p = list;

    for (;;) {
        char c = *p;
        if (c == ',' || c == 0 || c == ' ' || c == '\t') {
            if (pos > 0) {
                tok[pos] = 0;
                for (unsigned i = 0; i < qmap_n; i++) {
                    /* Inline string compare; both environments have one
                     * but they live in different headers. */
                    const char *a = tok;
                    const char *b = qmap[i].name;
                    while (*a && *a == *b) { a++; b++; }
                    if (*a == 0 && *b == 0) {
                        mask |= qmap[i].bit;
                        break;
                    }
                }
                pos = 0;
            }
            if (c == 0) break;
        } else if (pos < sizeof(tok) - 1) {
            tok[pos++] = c;
        }
        p++;
    }
    return mask;
}
