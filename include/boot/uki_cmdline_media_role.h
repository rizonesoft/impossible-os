/* uki_cmdline_media_role.h -- media-role token parser for UKI .cmdline.
 *
 * Static-inline helper shared by the UEFI bootloader (bootx64.c) and
 * the kernel-side unit tests. Scans a UKI .cmdline buffer for the
 * `media_role=NAME` token and returns the parsed BOOT_MEDIA_ROLE_*
 * enum value. NAME is matched case-insensitively against the same
 * canonical lower-case strings the disk-side parser
 * (media_role_parse in bootx64.c) accepts:
 *   normal / installer / live / recovery / manufacturing / diagnostics
 *
 * Token recognition is whitespace-bounded: the matched key starts at
 * buffer offset 0 OR after a space/tab/newline/CR; the value runs
 * until the next whitespace or end of buffer. Trailing whitespace
 * around the value is trimmed.
 *
 * Returns BOOT_MEDIA_ROLE_UNSET when:
 *   - buf is NULL or buf_size == 0
 *   - no `media_role=` token at a valid position
 *   - the value is empty or longer than 14 bytes (longest legal
 *     name is "manufacturing" = 13)
 *   - the value does not match any canonical name
 *
 * Caller uses UNSET vs a real role to decide whether to fall through
 * to the next precedence source (DHCP option in the future, then
 * disk /IPOS/role.txt, then default NORMAL).
 *
 * Header-only because bootx64.c links separately from the kernel
 * unit-test binary; static inline avoids the duplicate-symbol +
 * cross-target linker issue while keeping a single source of truth.
 *
 * Type discipline: uses `unsigned char` and __SIZE_TYPE__ rather
 * than UINT8 / size_t because the bootloader uses UEFI types and
 * the kernel uses C99 stdint, and this file must compile under
 * both. The role-enum constants are from include/kernel/boot_info.h
 * (kernel side) and src/boot/uefi/boot_info_mirror.h (bootloader
 * side); both define byte-for-byte identical values, so a static-
 * inline that uses them works on both sides.
 */
#ifndef UKI_CMDLINE_MEDIA_ROLE_H
#define UKI_CMDLINE_MEDIA_ROLE_H

static inline unsigned int
uki_cmdline_match_role_name(const char *lc, __SIZE_TYPE__ n)
{
    /* Lower-case name + length match against the canonical six. */
    if (n == 6  && lc[0]=='n' && lc[1]=='o' && lc[2]=='r' && lc[3]=='m' &&
                   lc[4]=='a' && lc[5]=='l')
        return BOOT_MEDIA_ROLE_NORMAL;
    if (n == 9  && lc[0]=='i' && lc[1]=='n' && lc[2]=='s' && lc[3]=='t' &&
                   lc[4]=='a' && lc[5]=='l' && lc[6]=='l' && lc[7]=='e' &&
                   lc[8]=='r')
        return BOOT_MEDIA_ROLE_INSTALLER;
    if (n == 4  && lc[0]=='l' && lc[1]=='i' && lc[2]=='v' && lc[3]=='e')
        return BOOT_MEDIA_ROLE_LIVE;
    if (n == 8  && lc[0]=='r' && lc[1]=='e' && lc[2]=='c' && lc[3]=='o' &&
                   lc[4]=='v' && lc[5]=='e' && lc[6]=='r' && lc[7]=='y')
        return BOOT_MEDIA_ROLE_RECOVERY;
    if (n == 13 && lc[0]=='m' && lc[1]=='a' && lc[2]=='n' && lc[3]=='u' &&
                   lc[4]=='f' && lc[5]=='a' && lc[6]=='c' && lc[7]=='t' &&
                   lc[8]=='u' && lc[9]=='r' && lc[10]=='i'&& lc[11]=='n'&&
                   lc[12]=='g')
        return BOOT_MEDIA_ROLE_MANUFACTURING;
    if (n == 11 && lc[0]=='d' && lc[1]=='i' && lc[2]=='a' && lc[3]=='g' &&
                   lc[4]=='n' && lc[5]=='o' && lc[6]=='s' && lc[7]=='t' &&
                   lc[8]=='i' && lc[9]=='c' && lc[10]=='s')
        return BOOT_MEDIA_ROLE_DIAGNOSTICS;
    return BOOT_MEDIA_ROLE_UNSET;
}

static inline unsigned int
uki_cmdline_extract_media_role(const unsigned char *buf,
                               __SIZE_TYPE__ buf_size)
{
    if (!buf || buf_size == 0)
        return BOOT_MEDIA_ROLE_UNSET;

    static const char KEY[] = "media_role=";
    const __SIZE_TYPE__ KEY_LEN = sizeof(KEY) - 1;

    __SIZE_TYPE__ scan_i;
    for (scan_i = 0; scan_i < buf_size; scan_i++) {
        int at_token_start =
            (scan_i == 0) ||
            (buf[scan_i - 1] == ' ')  ||
            (buf[scan_i - 1] == '\t') ||
            (buf[scan_i - 1] == '\n') ||
            (buf[scan_i - 1] == '\r');
        if (!at_token_start)
            continue;
        if (scan_i + KEY_LEN > buf_size)
            return BOOT_MEDIA_ROLE_UNSET;

        __SIZE_TYPE__ k;
        int match = 1;
        for (k = 0; k < KEY_LEN; k++) {
            if (buf[scan_i + k] != (unsigned char)KEY[k]) { match = 0; break; }
        }
        if (!match)
            continue;

        /* Found "media_role=" at a token boundary. Read the value
         * until next whitespace or end of buffer. Cap at 14 bytes
         * (longest legal name + slack). */
        __SIZE_TYPE__ val_start = scan_i + KEY_LEN;
        __SIZE_TYPE__ val_end = val_start;
        while (val_end < buf_size) {
            unsigned char c = buf[val_end];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                break;
            val_end++;
        }
        __SIZE_TYPE__ n = val_end - val_start;
        if (n == 0 || n > 14)
            return BOOT_MEDIA_ROLE_UNSET;

        char lc[16];
        __SIZE_TYPE__ i;
        for (i = 0; i < n; i++) {
            char c = (char)buf[val_start + i];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            lc[i] = c;
        }
        lc[n] = '\0';
        return uki_cmdline_match_role_name(lc, n);
    }
    return BOOT_MEDIA_ROLE_UNSET;
}

#endif /* UKI_CMDLINE_MEDIA_ROLE_H */
