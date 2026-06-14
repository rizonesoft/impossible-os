/* ============================================================================
 * time_iso.c -- Unix-time to ISO-8601 formatting (freestanding, no libc).
 *
 * Extracted from the jb_iso8601() helper in firmware_tables_json.c so the
 * civil-date math has a single home (firmware-tables.json + the bootloader
 * build-identity BlackBox dump both consume it).
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/time_iso.h"
#include "libc/string.h"

void kdate_iso8601(uint64_t unix_time, char out[21])
{
    if (unix_time == 0) {
        memcpy(out, "1970-01-01T00:00:00Z", 21);
        return;
    }

    int64_t  z       = (int64_t)(unix_time / 86400);
    uint32_t sod     = (uint32_t)(unix_time % 86400);
    int64_t  z_shift = z + 719468;
    int64_t  era     = (z_shift >= 0 ? z_shift : z_shift - 146096) / 146097;
    uint32_t doe     = (uint32_t)(z_shift - era * 146097);
    uint32_t yoe     = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t  y       = (int64_t)yoe + era * 400;
    uint32_t doy     = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint32_t mp      = (5 * doy + 2) / 153;
    uint32_t d       = doy - (153 * mp + 2) / 5 + 1;
    uint32_t m       = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        y++;
    uint32_t hh = sod / 3600;
    uint32_t mm = (sod / 60) % 60;
    uint32_t ss = sod % 60;
    if (y < 1970) y = 1970;
    if (y > 9999) y = 9999;

    out[0]  = (char)('0' + (y / 1000) % 10);
    out[1]  = (char)('0' + (y / 100) % 10);
    out[2]  = (char)('0' + (y / 10) % 10);
    out[3]  = (char)('0' + y % 10);
    out[4]  = '-';
    out[5]  = (char)('0' + (m / 10) % 10);
    out[6]  = (char)('0' + m % 10);
    out[7]  = '-';
    out[8]  = (char)('0' + (d / 10) % 10);
    out[9]  = (char)('0' + d % 10);
    out[10] = 'T';
    out[11] = (char)('0' + (hh / 10) % 10);
    out[12] = (char)('0' + hh % 10);
    out[13] = ':';
    out[14] = (char)('0' + (mm / 10) % 10);
    out[15] = (char)('0' + mm % 10);
    out[16] = ':';
    out[17] = (char)('0' + (ss / 10) % 10);
    out[18] = (char)('0' + ss % 10);
    out[19] = 'Z';
    out[20] = '\0';
}
