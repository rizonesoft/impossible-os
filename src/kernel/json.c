/* ============================================================================
 * json.c -- Kernel JSON API (thin wrapper over cJSON)
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */

#include "kernel/json.h"
#include "kernel/mm/heap.h"

/* Pull in cJSON header (compiled via src/libs/cjson/ with SSE2 rule) */
#include "libs/cjson/cJSON.h"

void json_init(void)
{
    /* cJSON already uses our malloc/free/realloc macros via the
     * freestanding shim in cJSON.c. This function exists for the
     * explicit Phase 2 init contract. Nothing to do here -- the
     * macros handle allocation redirection at compile time. */
}

struct cJSON *json_parse_len(const char *text, size_t len)
{
    if (!text || len == 0 || len > JSON_MAX_INPUT)
        return (struct cJSON *)0;

    /* Whole-buffer raw-control-byte reject BEFORE parsing: cJSON's whitespace
     * skipper treats every byte <= 32 (incl NUL) as whitespace and its string
     * parser accepts raw control bytes by length, so an embedded NUL/control
     * byte (e.g. "{\0}", a NUL inside a string) would otherwise be consumed
     * silently. JSON forbids raw control bytes anyway (they must be \u-escaped),
     * so reject any byte < 0x20 that is not JSON whitespace. */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r')
            return (struct cJSON *)0;
    }

    /* ParseWithLengthOpts bounds reads to `len` (safe on non-NUL-terminated
     * untrusted buffers) and reports where parsing stopped via `end`. */
    const char *end = (const char *)0;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &end, 0);
    if (!root)
        return (struct cJSON *)0;

    /* require_null_terminated=0 does not require the whole buffer be consumed,
     * so a valid top-level value followed by appended junk would otherwise pass.
     * Reject any trailing non-whitespace (a NUL cleanly ends the buffer). */
    if (end) {
        const char *stop = text + len;
        for (const char *p = end; p < stop; p++) {
            char c = *p;
            /* Only JSON whitespace is allowed after the value. An embedded NUL
             * is NOT an out-of-band terminator here -- treating it as one would
             * let "{}\0JUNK" (len covering JUNK) hide trailing garbage. */
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                cJSON_Delete(root);
                return (struct cJSON *)0;
            }
        }
    }
    return (struct cJSON *)root;
}

struct cJSON *json_parse(const char *text)
{
    if (!text)
        return (struct cJSON *)0;
    /* Owned NUL-terminated string: find the terminator within the cap. Reading
     * text[len] is in-bounds for a NUL-terminated string (the NUL itself). */
    size_t len = 0;
    while (len < JSON_MAX_INPUT && text[len] != '\0')
        len++;
    /* If we stopped at the cap, the terminator was NOT found within the first
     * JSON_MAX_INPUT bytes. Reject WITHOUT reading text[len] -- probing one byte
     * past an owned, unterminated buffer could fault on a guard/page boundary. */
    if (len == JSON_MAX_INPUT)
        return (struct cJSON *)0;
    return json_parse_len(text, len);
}

struct cJSON *json_get(struct cJSON *obj, const char *key)
{
    return (struct cJSON *)cJSON_GetObjectItemCaseSensitive(
        (cJSON *)obj, key);
}

const char *json_str(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    if (c && cJSON_IsString(c) && c->valuestring)
        return c->valuestring;
    return (const char *)0;
}

int json_int(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    if (c && cJSON_IsNumber(c))
        return c->valueint;
    return 0;
}

uint32_t json_u32(struct cJSON *item, int *valid)
{
    cJSON *c = (cJSON *)item;
    if (valid) *valid = 0;
    if (!c || !cJSON_IsNumber(c))
        return 0;
    /* cJSON stores numbers in both valueint (saturated to INT_MIN/INT_MAX)
     * and valuedouble (raw). For u32 extraction the saturating signed int
     * is unusable -- a JSON literal of 0xFFFFFFFF appears as valueint
     * INT_MAX, which collapses real version numbers in the firmware
     * advisor's classify path. Read via valuedouble and gate on the u32
     * range explicitly. Reject negatives, non-integral values, and any
     * value that would lose precision casting through double (UINT32_MAX
     * is well within double's 2^53 mantissa headroom). */
    double d = c->valuedouble;
    if (!(d >= 0.0 && d <= 4294967295.0))   /* NaN/inf/oor -> 0 */
        return 0;
    /* Reject fractional component: JSON allows 1.5 here, but version
     * numbers must be integral. Truncating silently would mis-classify. */
    double truncated = (double)(uint64_t)d;
    if (truncated != d)
        return 0;
    if (valid) *valid = 1;
    return (uint32_t)d;
}

double json_double(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    if (c && cJSON_IsNumber(c))
        return c->valuedouble;
    return 0.0;
}

int json_bool(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    return (c && cJSON_IsTrue(c)) ? 1 : 0;
}

uint64_t json_u64(struct cJSON *item, int *valid)
{
    cJSON *c = (cJSON *)item;
    if (valid) *valid = 0;
    if (!c || !cJSON_IsNumber(c))
        return 0;
    /* Same valuedouble discipline as json_u32. Cap at 2^53 (double's
     * exact-integer mantissa range) -- a JSON literal larger than that
     * already lost precision in cJSON's parser, so trusting it would
     * publish a corrupted timestamp. */
    double d = c->valuedouble;
    if (!(d >= 0.0 && d <= 9007199254740992.0))   /* 2^53 */
        return 0;
    double truncated = (double)(uint64_t)d;
    if (truncated != d)
        return 0;
    if (valid) *valid = 1;
    return (uint64_t)d;
}

int json_is_array(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    return (c && cJSON_IsArray(c)) ? 1 : 0;
}

uint32_t json_array_size(struct cJSON *arr)
{
    cJSON *c = (cJSON *)arr;
    if (!c || !cJSON_IsArray(c))
        return 0;
    int n = cJSON_GetArraySize(c);
    return n < 0 ? 0 : (uint32_t)n;
}

struct cJSON *json_array_get(struct cJSON *arr, uint32_t index)
{
    cJSON *c = (cJSON *)arr;
    if (!c || !cJSON_IsArray(c))
        return (struct cJSON *)0;
    /* cJSON_GetArrayItem accepts int; reject huge indices to keep the
     * signed conversion well-defined. */
    if (index > 0x7FFFFFFFu)
        return (struct cJSON *)0;
    return (struct cJSON *)cJSON_GetArrayItem(c, (int)index);
}

struct cJSON *json_array_first(struct cJSON *arr)
{
    cJSON *c = (cJSON *)arr;
    if (!c || !cJSON_IsArray(c))
        return (struct cJSON *)0;
    return (struct cJSON *)c->child;
}

struct cJSON *json_array_next(struct cJSON *item)
{
    cJSON *c = (cJSON *)item;
    if (!c)
        return (struct cJSON *)0;
    return (struct cJSON *)c->next;
}

char *json_print(struct cJSON *obj)
{
    return cJSON_PrintUnformatted((cJSON *)obj);
}

void json_free(struct cJSON *obj)
{
    cJSON_Delete((cJSON *)obj);
}
