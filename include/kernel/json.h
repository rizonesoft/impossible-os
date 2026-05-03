/* ============================================================================
 * json.h -- Kernel JSON API (thin wrapper over cJSON)
 *
 * Provides a simple interface for parsing, querying, and generating JSON
 * documents in the kernel. Used for theme files, config, update manifests.
 *
 * Must call json_init() in Phase 2 after heap is ready.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Forward declaration -- avoids exposing cJSON internals to all includers */
struct cJSON;

/* Initialize JSON subsystem (redirects cJSON allocator to kmalloc/kfree).
 * Call once in Phase 2 after heap is ready. */
void json_init(void);

/* Parse a JSON string. Returns root node, or NULL on error.
 * Caller must call json_free() when done. */
struct cJSON *json_parse(const char *text);

/* Get an object member by key. Returns NULL if not found. */
struct cJSON *json_get(struct cJSON *obj, const char *key);

/* Extract a string value. Returns NULL if item is not a string. */
const char *json_str(struct cJSON *item);

/* Extract an integer value. Returns 0 if item is not a number. */
int json_int(struct cJSON *item);

/* Extract an unsigned 32-bit value via cJSON's valuedouble (the unsaturated
 * representation; valueint clamps at INT_MAX, which silently truncates JSON
 * numbers >= 0x80000000). Returns 0 if item is not a number, is negative,
 * is non-integral, or exceeds UINT32_MAX. *valid (if non-NULL) reports
 * whether the value was a clean u32 -- callers that want to distinguish
 * "absent / malformed / out-of-range" from a literal 0 should consult it. */
uint32_t json_u32(struct cJSON *item, int *valid);

/* Extract a double value. Returns 0.0 if item is not a number. */
double json_double(struct cJSON *item);

/* Check if item is true (cJSON_True). */
int json_bool(struct cJSON *item);

/* Array helpers.  json_array_size returns 0 for non-arrays.  json_array_get
 * returns NULL for OOR or non-array.  Use uint32_t to match the rest of the
 * kernel surface; a JSON array carrying > 4G entries would never fit in our
 * heap anyway. */
uint32_t json_array_size(struct cJSON *arr);
struct cJSON *json_array_get(struct cJSON *arr, uint32_t index);

/* Print JSON to a string (unformatted). Caller must kfree() the result. */
char *json_print(struct cJSON *obj);

/* Free a parsed JSON tree. */
void json_free(struct cJSON *obj);
