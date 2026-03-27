/* ============================================================================
 * json.c -- Kernel JSON API (thin wrapper over cJSON)
 *
 * XREF: 02-kernel-core/TODO-20-kernel-libraries.md §6
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

char *json_print(struct cJSON *obj)
{
    return cJSON_PrintUnformatted((cJSON *)obj);
}

void json_free(struct cJSON *obj)
{
    cJSON_Delete((cJSON *)obj);
}
