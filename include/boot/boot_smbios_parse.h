/* ============================================================================
 * boot_smbios_parse.h -- pure SMBIOS Type 1 (System Information) parsing
 *
 * Shared between bootloader (pre-EBS UUID extraction for the policy
 * ladder's machine_id filter) and kernel unit tests (pure-byte walker).
 * No UEFI types -- only the freestanding-safe `unsigned char` /
 * `unsigned int` / `unsigned long` types so both the bootloader (with
 * UINT8 = unsigned char) and kernel tests (with uint8_t = unsigned char)
 * can include it.
 *
 * Inline definitions so the bootloader and the kernel test binary each
 * get their own copy at compile time -- no separate linkage / .c file.
 *
 * Threat model: every length / offset is bounded; the walker bails on
 * the first inconsistency (short header, unterminated string area,
 * type 127 end-of-table marker, or zero-length entry). All-zero and
 * all-FF UUIDs are the firmware "not-specified" sentinel and are
 * rejected.
 * ============================================================================ */

#ifndef BOOT_SMBIOS_PARSE_H
#define BOOT_SMBIOS_PARSE_H

/* SMBIOS structure header -- common 4-byte prefix on every type. */
struct boot_smbios_header {
    unsigned char  type;
    unsigned char  length;
    unsigned short handle;
} __attribute__((packed));

/* Find the SMBIOS Type 1 (System Information) header within a raw
 * table. Walks the type-length-strings shape per SMBIOS spec section
 * 6.1.2. Returns a pointer to the Type 1 header, or NULL if absent /
 * malformed. Bounds:
 *   - len must hold at least one header (>= 4 bytes)
 *   - len capped at 1 MiB to defeat a malicious firmware that
 *     advertises a giant table
 *   - each header's length field must fit within remaining bytes
 *   - string area termination requires a double-NUL within `len`
 *
 * Walker stops at type==127 (end-of-table). */
static inline const struct boot_smbios_header *
boot_smbios_find_type1(const unsigned char *base, unsigned int len)
{
    unsigned int off = 0;
    if (!base || len < sizeof(struct boot_smbios_header) || len > (1u << 20))
        return (const struct boot_smbios_header *)0;
    while (off + sizeof(struct boot_smbios_header) <= len) {
        const struct boot_smbios_header *h =
            (const struct boot_smbios_header *)(base + off);
        if (h->length < sizeof(struct boot_smbios_header)) break;
        if ((unsigned int)off + h->length > len) break;
        if (h->type == 127) break;     /* End-of-table marker */
        if (h->type == 1) return h;
        unsigned int p = off + h->length;
        while (p + 1 < len) {
            if (base[p] == 0 && base[p + 1] == 0) {
                off = p + 2;
                goto next;
            }
            p++;
        }
        break;
next:
        ;
    }
    return (const struct boot_smbios_header *)0;
}

/* Format the 16-byte SMBIOS Type 1 UUID into RFC 4122 textual form.
 * Per SMBIOS 2.6+ the first three fields are little-endian-on-wire
 * and must be byte-swapped to RFC 4122 textual form (Data1 4 bytes
 * LE, Data2 2 bytes LE, Data3 2 bytes LE; Data4 8 bytes big-endian).
 *
 * Output: 36 lowercase hex chars + NUL = 37 bytes (caller-owned).
 * Returns 1 on success, 0 if the UUID is the all-zero or all-FF
 * "not specified" sentinel. The same byte-order rule applies to GPT
 * partition GUIDs, which is why this helper is also used for
 * LoaderDevicePartUUID. */
static inline int
boot_smbios_format_uuid(const unsigned char src[16], char out[37])
{
    int all_zero = 1, all_ff = 1;
    for (unsigned int i = 0; i < 16u; i++) {
        if (src[i] != 0x00u) all_zero = 0;
        if (src[i] != 0xFFu) all_ff = 0;
    }
    if (all_zero || all_ff) {
        out[0] = 0;
        return 0;
    }
    static const char hex[] = "0123456789abcdef";
    unsigned char ord[16];
    ord[0] = src[3]; ord[1] = src[2]; ord[2] = src[1]; ord[3] = src[0];
    ord[4] = src[5]; ord[5] = src[4];
    ord[6] = src[7]; ord[7] = src[6];
    for (unsigned int i = 8; i < 16u; i++) ord[i] = src[i];
    unsigned int op = 0;
    for (unsigned int i = 0; i < 16u; i++) {
        out[op++] = hex[(ord[i] >> 4) & 0xFu];
        out[op++] = hex[ord[i] & 0xFu];
        if (i == 3u || i == 5u || i == 7u || i == 9u) out[op++] = '-';
    }
    out[op] = 0;
    return 1;
}

#endif /* BOOT_SMBIOS_PARSE_H */
