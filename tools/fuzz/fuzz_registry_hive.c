/* libFuzzer harness: registry hive payload records.
 *
 * Input is the payload of a .hive file (what follows the 4096-byte header),
 * read from disk at boot. hive_parse_payload (src/kernel/registry_hive_parse.c)
 * is the registry's one reader of it: run first with no visitor, as the
 * validation pass, then with a visitor, as the load pass. Contract: no input
 * reads outside the buffer or recurses past HIVE_MAX_PARSE_DEPTH, the two
 * passes agree, and every name and value handed to the visitor is in bounds.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "registry.h"

static uint32_t g_values;

static void *on_key(void *ctx, void *parent, const char *name)
{
    (void)ctx;
    if (strlen(name) > REG_MAX_KEY_NAME)
        abort();
    return parent;   /* any non-NULL handle: the tree itself is not modelled */
}

static int on_value(void *ctx, void *key, const char *name, uint32_t type,
                    const uint8_t *data, uint32_t size)
{
    static uint8_t sink[REG_MAX_VALUE_SIZE];

    (void)ctx; (void)key; (void)type;
    if (strlen(name) > REG_MAX_VALUE_NAME || size > REG_MAX_VALUE_SIZE)
        abort();
    memcpy(sink, data, size);   /* ASan: the span is inside the input */
    g_values++;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t root;
    hive_visitor_t vis = { on_key, on_value, (void *)0 };
    uint32_t keys, values, keys2, values2;

    if (size > HIVE_MAX_DATA_SIZE)
        return 0;
    if (size >= sizeof(hive_header_t))
        (void)hive_header_fields_ok((const hive_header_t *)data);

    if (hive_parse_payload(data, (uint32_t)size, (void *)0, (const hive_visitor_t *)0,
                           &keys, &values) != 0)
        return 0;
    if (hive_validate_payload(data, (uint32_t)size, keys, values, (uint32_t *)0,
                              (uint32_t *)0) != 0)
        abort();
    g_values = 0;
    if (hive_parse_payload(data, (uint32_t)size, &root, &vis, &keys2, &values2) != 0 ||
        keys2 != keys || values2 != values || g_values != values)
        abort();
    return 0;
}
