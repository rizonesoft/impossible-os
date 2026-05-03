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

struct cJSON *json_parse(const char *text)
{
    return (struct cJSON *)cJSON_Parse(text);
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

char *json_print(struct cJSON *obj)
{
    return cJSON_PrintUnformatted((cJSON *)obj);
}

void json_free(struct cJSON *obj)
{
    cJSON_Delete((cJSON *)obj);
}
