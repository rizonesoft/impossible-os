/* devpath_filepath.h -- pure, bounded extraction of the ESP-relative file path
 *                       from a LoadedImage FilePath device path.
 *
 * WHY THIS IS A SEPARATE PURE HEADER. The bytes are firmware-owned and hostile
 * by assumption: the loader must never self-walk a device path to find its own
 * end. bootx64.c:14022 records that rule and the reason -- a heuristic cap is not
 * an object bound -- so the CALLER measures the object with
 * EFI_DEVICE_PATH_UTILITIES_PROTOCOL->GetDevicePathSize and passes the measure
 * in. Everything here is arithmetic over `dp[0 .. dp_size)` and nothing else,
 * which is also what makes it testable from the kernel test suite
 * (test_uefi_boot.c) where malformed node lengths can actually be constructed;
 * inside the loader they cannot.
 *
 * WHAT IT REFUSES, AND WHY REFUSING IS THE POINT. Every ambiguous shape returns
 * a distinct status instead of a best guess, because the consumer is a
 * self-measurement: a path assembled from a device path it did not fully
 * understand would hash the wrong file and report it with exactly the confidence
 * of the right one. An END_INSTANCE node (multi-instance path), a node type
 * other than MEDIA/FILEPATH, an embedded NUL with characters behind it, an odd
 * payload length and a missing END_ENTIRE are all refusals, not recoveries.
 *
 * Reference: UEFI 2.10 spec 10.3.5.4 (Media / File Path device-path node) and
 * Table 10-1 (End of Hardware Device Path node subtypes).
 *
 * Type discipline: plain C types rather than UEFI CHAR16/UINTN or kernel stdint,
 * following include/boot/uki_cmdline_check.h -- the bootloader uses UEFI types
 * and the kernel uses C99 stdint, and this file must compile under both.
 * `unsigned short` is exactly CHAR16 (efi.h:22,30), so a loader caller passes a
 * CHAR16 buffer with no cast.
 */

#ifndef DEVPATH_FILEPATH_H
#define DEVPATH_FILEPATH_H

_Static_assert(sizeof(unsigned short) == 2,
               "devpath_filepath.h needs a 16-bit unsigned short to be CHAR16");

#define DPFP_FN static __attribute__((unused))

/* Device-path node type/subtype constants. Spelled out here rather than taken
 * from efi.h because this header also compiles in kernel test context, where
 * efi.h is not (and must not be) included. The loader-side values are pinned
 * against efi.h by a _Static_assert at the bootx64.c call site. */
#define DPFP_TYPE_MEDIA           0x04  /* UEFI 2.10 spec 10.3.5 */
#define DPFP_TYPE_END             0x7F  /* UEFI 2.10 spec Table 10-1 */
#define DPFP_SUBTYPE_FILEPATH     0x04  /* UEFI 2.10 spec 10.3.5.4 */
#define DPFP_SUBTYPE_END_ENTIRE   0xFF  /* UEFI 2.10 spec Table 10-1 */
#define DPFP_SUBTYPE_END_INSTANCE 0x01  /* UEFI 2.10 spec Table 10-1 */

enum dpfp_status {
    DPFP_OK = 0,
    DPFP_BADARG,          /* NULL buffer or zero-capacity output */
    DPFP_SHORT,           /* measure cannot hold even one node header */
    DPFP_BAD_NODE,        /* length < 4, odd payload, or a node past the measure */
    DPFP_NO_END,          /* measure exhausted with no END_ENTIRE node */
    DPFP_MULTI_INSTANCE,  /* END_INSTANCE: which instance holds the file is undefined */
    DPFP_UNSUPPORTED,     /* a non-FILEPATH node: not a file on a filesystem */
    DPFP_EMBEDDED_NUL,    /* NUL with characters behind it inside one node */
    DPFP_NONE,            /* well-formed path carrying no file path at all */
    DPFP_OVERFLOW,        /* assembled path does not fit the caller buffer */
};

/* Read a 16-bit little-endian field a byte at a time: device-path nodes are
 * packed and a node can begin at any offset, so a wider load could be
 * unaligned. */
DPFP_FN unsigned short dpfp_le16(const unsigned char *p)
{
    return (unsigned short)((unsigned short)p[0]
                          | (unsigned short)((unsigned short)p[1] << 8));
}

/* Extract the concatenated file path into `out` as a NUL-terminated CHAR16
 * string. `dp_size` MUST be a firmware-measured object size, never a guess.
 * `out_chars` is the capacity of `out` in 16-bit units, terminator included.
 * On any non-OK return `out` is left holding an empty string, so a caller that
 * ignores the status still cannot hash a partially assembled path. */
DPFP_FN enum dpfp_status dpfp_extract(const void *dp, __SIZE_TYPE__ dp_size,
                                      unsigned short *out, __SIZE_TYPE__ out_chars)
{
    const unsigned char *bytes = (const unsigned char *)dp;
    __SIZE_TYPE__ off = 0;
    __SIZE_TYPE__ used = 0;      /* 16-bit units written, terminator excluded */
    int saw_filepath = 0;
    int saw_end = 0;

    if (!dp || !out || out_chars == 0)
        return DPFP_BADARG;

    out[0] = 0;

    if (dp_size < 4u)
        return DPFP_SHORT;

    while (off + 4u <= dp_size) {
        unsigned char type = bytes[off];
        unsigned char subtype = bytes[off + 1u];
        __SIZE_TYPE__ len = (__SIZE_TYPE__)dpfp_le16(&bytes[off + 2u]);
        __SIZE_TYPE__ payload;
        __SIZE_TYPE__ chars;
        __SIZE_TYPE__ i;

        if (len < 4u || off + len > dp_size) {
            out[0] = 0;
            return DPFP_BAD_NODE;
        }

        if (type == DPFP_TYPE_END) {
            if (subtype == DPFP_SUBTYPE_END_ENTIRE) {
                saw_end = 1;
                break;
            }
            out[0] = 0;
            return (subtype == DPFP_SUBTYPE_END_INSTANCE) ? DPFP_MULTI_INSTANCE
                                                          : DPFP_BAD_NODE;
        }

        if (type != DPFP_TYPE_MEDIA || subtype != DPFP_SUBTYPE_FILEPATH) {
            out[0] = 0;
            return DPFP_UNSUPPORTED;
        }

        payload = len - 4u;
        if ((payload & 1u) != 0u) {
            out[0] = 0;
            return DPFP_BAD_NODE;
        }
        chars = payload / 2u;

        /* The spec says the string is NUL terminated; firmware has been seen
         * omitting it, so BOTH shapes are accepted and only a NUL with
         * characters behind it is a refusal. */
        for (i = 0; i < chars; i++) {
            unsigned short ch = dpfp_le16(&bytes[off + 4u + i * 2u]);

            if (ch == 0) {
                if (i + 1u != chars) {
                    out[0] = 0;
                    return DPFP_EMBEDDED_NUL;
                }
                break;              /* trailing terminator: drop it */
            }

            /* NODE BOUNDARY (i == 0) only. The concatenation is per UEFI 2.10
             * spec 10.3.5.4, and all four separator combinations are decided
             * explicitly rather than left to fall through:
             *   neither side has one -> insert one
             *   exactly one side has one -> copy as-is
             *   BOTH sides have one -> collapse, or the path gains an empty
             *     component and Open fails on a device path that was valid.
             * Applying any of this per CHARACTER would insert a separator
             * between every letter, which is what the single-node fixture in
             * test_uefi_boot.c caught. */
            if (i == 0 && used > 0) {
                int prev_sep = (out[used - 1u] == (unsigned short)'\\');
                int this_sep = (ch == (unsigned short)'\\');

                if (prev_sep && this_sep)
                    continue;
                if (!prev_sep && !this_sep) {
                    if (used + 2u > out_chars) {
                        out[0] = 0;
                        return DPFP_OVERFLOW;
                    }
                    out[used++] = (unsigned short)'\\';
                }
            }

            if (used + 2u > out_chars) {
                out[0] = 0;
                return DPFP_OVERFLOW;
            }
            out[used++] = ch;
        }
        saw_filepath = 1;

        off += len;
    }

    if (!saw_end) {
        out[0] = 0;
        return DPFP_NO_END;
    }
    if (!saw_filepath || used == 0) {
        out[0] = 0;
        return DPFP_NONE;
    }

    out[used] = 0;
    return DPFP_OK;
}

#endif /* DEVPATH_FILEPATH_H */
