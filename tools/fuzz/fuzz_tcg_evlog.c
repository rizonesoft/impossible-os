/* libFuzzer harness: the loader's TCG event-log walkers (include/boot/tcg_evlog.h).
 *
 * Input is firmware memory the loader reads before ExitBootServices: the event
 * log GetEventLog() returns, up to the memory-descriptor ceiling, plus the
 * LastEntry offset the same call reports. Layout of a fuzz input: byte 0 picks
 * the format (odd = TCG_1_2 legacy, even = crypto-agile), bytes 1..4 are
 * LastEntry's offset (little endian), the rest is the log. The log is copied
 * into an exact-size heap buffer so any read past the ceiling is an ASan
 * report. Contract: no out-of-bounds read; an accepted chain ends inside the
 * buffer, counts at least one record, and a crypto-agile end is never before
 * the SpecID record; the image-load walk over the same log never crashes.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/boot/tcg_evlog.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const unsigned char want_dp[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned long long end = 0, last;
    unsigned int count = 0, cand = 0;
    unsigned char *log;
    size_t n;
    int rc;

    if (size < 5u || size > (1u << 20))
        return 0;
    last = (unsigned long long)data[1] | ((unsigned long long)data[2] << 8)
         | ((unsigned long long)data[3] << 16) | ((unsigned long long)data[4] << 24);
    n = size - 5u;
    log = malloc(n ? n : 1u);
    if (!log)
        return 0;
    memcpy(log, data + 5, n);

    if (data[0] & 1u) {
        rc = tcgl_legacy_chain_end(log, n, last, &end, &count);
        if (rc == TCGL_OK && (end > n || end < TCGL_LEGACY_HDR || count == 0u))
            abort();
    } else {
        struct tcgl_log lg;
        struct tcgl_match m;
        rc = tcgl_open(log, n, &lg);
        if (rc == TCGL_OK) {
            rc = tcgl_chain_end(&lg, last, &end, &count);
            if (rc == TCGL_OK && (end > n || end < lg.first_event2 || count == 0u))
                abort();
            if (rc != TCGL_OK && (end != 0u || count != 0u))
                abort();
            rc = tcgl_find_image_load(&lg, 4u, want_dp, sizeof(want_dp), &m, &cand);
            if (rc == TCGL_OK && (m.devpath < log || m.devpath + m.devpath_len > log + n))
                abort();
        }
    }
    free(log);
    return 0;
}
