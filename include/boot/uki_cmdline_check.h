/* uki_cmdline_check.h -- whole-chain Secure Boot cmdline scanner.
 *
 * Static-inline helper shared by the UEFI bootloader (bootx64.c) and
 * the kernel-side unit tests (test_uefi_boot.c). Scans a buffer for
 * the EXACT tokens that the boot.conf parser (parse_conf_kv in
 * bootx64.c) consumes to stage disk-side payloads: "initrd=",
 * "module=" (singular), "recovery_image=". Token names track the
 * parser keys; rejecting "modules=" plural or "recovery=" short-form
 * would not match real bypass attempts since the parser doesn't
 * accept those keys (bootx64.c:2313 = "module="; 2336 = "recovery_image=").
 *
 * Returns the matched token literal on first hit, or NULL when no
 * disk-payload override token is present. The matched literal is
 * one of "initrd=", "module=", "recovery_image=".
 *
 * Header-only because bootx64.c links separately from the kernel
 * unit-test binary; static inline avoids the duplicate-symbol +
 * cross-target linker issue while keeping a single source of truth
 * for the rejection logic.
 *
 * Type discipline: uses `unsigned char` and __SIZE_TYPE__ rather
 * than UINT8 / size_t because the bootloader uses UEFI types and
 * the kernel uses C99 stdint, and this file must compile under
 * both. Callers pass a byte pointer + length; no NUL termination
 * is required (the function honors the length argument).
 */
#ifndef UKI_CMDLINE_CHECK_H
#define UKI_CMDLINE_CHECK_H

static inline const char *
uki_find_disk_override_token(const unsigned char *buf,
                             __SIZE_TYPE__ buf_size)
{
    if (!buf || buf_size == 0)
        return (const char *)0;

    /* Token starts at scan_i if scan_i == 0 or buf[scan_i-1] is
     * whitespace. Then check the literal token bytes followed by
     * '=' at the matching position. */
    __SIZE_TYPE__ scan_i;
    for (scan_i = 0; scan_i < buf_size; scan_i++) {
        int at_token_start =
            (scan_i == 0) ||
            (buf[scan_i - 1] == ' ') ||
            (buf[scan_i - 1] == '\t') ||
            (buf[scan_i - 1] == '\n') ||
            (buf[scan_i - 1] == '\r');
        if (!at_token_start)
            continue;

        const unsigned char *p = buf + scan_i;
        __SIZE_TYPE__ rem = buf_size - scan_i;

        /* "initrd=" -- 7 bytes. */
        if (rem >= 7 &&
            p[0] == 'i' && p[1] == 'n' && p[2] == 'i' &&
            p[3] == 't' && p[4] == 'r' && p[5] == 'd' &&
            p[6] == '=') {
            return "initrd=";
        }
        /* "module=" -- 7 bytes. NOTE: singular, matching the actual
         * boot.conf parser key in bootx64.c parse_conf_kv. */
        if (rem >= 7 &&
            p[0] == 'm' && p[1] == 'o' && p[2] == 'd' &&
            p[3] == 'u' && p[4] == 'l' && p[5] == 'e' &&
            p[6] == '=') {
            return "module=";
        }
        /* "recovery_image=" -- 15 bytes. NOTE: full key name, matching
         * the actual boot.conf parser key in bootx64.c parse_conf_kv. */
        if (rem >= 15 &&
            p[0] == 'r' && p[1] == 'e' && p[2] == 'c' &&
            p[3] == 'o' && p[4] == 'v' && p[5] == 'e' &&
            p[6] == 'r' && p[7] == 'y' && p[8] == '_' &&
            p[9] == 'i' && p[10] == 'm' && p[11] == 'a' &&
            p[12] == 'g' && p[13] == 'e' && p[14] == '=') {
            return "recovery_image=";
        }
    }
    return (const char *)0;
}

#endif /* UKI_CMDLINE_CHECK_H */
