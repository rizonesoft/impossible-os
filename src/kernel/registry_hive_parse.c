/* ============================================================================
 * registry_hive_parse.c -- hive file header and payload decoding
 *
 * A hive is a 4096-byte header followed by depth-first key and value records:
 *
 *   Key record:   [name_len:u16][name:N][value_count:u16][child_count:u16]
 *   Value record: [name_len:u16][name:N][type:u32][data_size:u32][data:N]
 *
 * Every byte comes from disk. This file is the only reader of that layout:
 * hive_header_fields_ok checks the header, and hive_parse_payload walks the
 * records once, handing each key and value to a visitor. hive_load validates
 * with no visitor (nothing written), then walks again with the visitor that
 * builds the tree, so the rules that accept a file and the code that loads it
 * cannot drift apart. No allocation and no I/O here, so tools/fuzz runs it on
 * the host under AddressSanitizer.
 * ============================================================================ */

#include "registry.h"
#include "kernel/le_bytes.h"

typedef struct {
    const uint8_t *buf;
    uint32_t       pos;
    uint32_t       cap;
} hive_rd_t;

static int rd_u16(hive_rd_t *r, uint16_t *v)
{
    if (r->pos > r->cap || 2 > r->cap - r->pos) return -1;
    *v = le16_at(r->buf + r->pos);
    r->pos += 2;
    return 0;
}

static int rd_u32(hive_rd_t *r, uint32_t *v)
{
    if (r->pos > r->cap || 4 > r->cap - r->pos) return -1;
    *v = le32_at(r->buf + r->pos);
    r->pos += 4;
    return 0;
}

/* Hand out LEN bytes in place; the caller copies what it keeps. */
static int rd_span(hive_rd_t *r, uint32_t len, const uint8_t **out)
{
    if (r->pos > r->cap || len > r->cap - r->pos) return -1;
    *out = r->buf + r->pos;
    r->pos += len;
    return 0;
}

/* A name record: bounded length, copied out and terminated. */
static int rd_name(hive_rd_t *r, uint32_t max, char *out)
{
    uint16_t len;
    const uint8_t *p;
    uint32_t i;

    if (rd_u16(r, &len) < 0 || len > max || rd_span(r, len, &p) < 0)
        return -1;
    for (i = 0; i < len; i++)
        out[i] = (char)p[i];
    out[len] = '\0';
    return 0;
}

typedef struct {
    hive_rd_t              rd;
    const hive_visitor_t  *vis;
    uint32_t               keys;
    uint32_t               values;
} hive_walk_t;

static int walk_value(hive_walk_t *w, void *key)
{
    char vname[REG_MAX_VALUE_NAME + 1];
    uint32_t vtype, vsize;
    const uint8_t *vdata;

    if (rd_name(&w->rd, REG_MAX_VALUE_NAME, vname) < 0) return -1;
    if (rd_u32(&w->rd, &vtype) < 0) return -1;
    if (rd_u32(&w->rd, &vsize) < 0) return -1;
    if (vsize > REG_MAX_VALUE_SIZE) return -1;
    if (rd_span(&w->rd, vsize, &vdata) < 0) return -1;
    w->values++;
    if (w->vis && w->vis->value(w->vis->ctx, key, vname, vtype, vdata, vsize) < 0)
        return -1;
    return 0;
}

/* Each key costs a fixed frame here and in the visitor; HIVE_MAX_PARSE_DEPTH
 * bounds the recursion so a file cannot exhaust the kernel stack. */
static int walk_key(hive_walk_t *w, void *parent, uint32_t depth)
{
    char name[REG_MAX_KEY_NAME + 1];
    uint16_t val_count, child_count, i;
    void *key = (void *)0;

    if (depth == 0 || depth > HIVE_MAX_PARSE_DEPTH)
        return -1;
    if (rd_name(&w->rd, REG_MAX_KEY_NAME, name) < 0) return -1;
    if (rd_u16(&w->rd, &val_count) < 0) return -1;
    if (rd_u16(&w->rd, &child_count) < 0) return -1;
    w->keys++;

    if (w->vis) {
        key = w->vis->key(w->vis->ctx, parent, name);
        if (!key) return -1;
    }
    for (i = 0; i < val_count; i++)
        if (walk_value(w, key) < 0) return -1;
    for (i = 0; i < child_count; i++)
        if (walk_key(w, key, depth + 1) < 0) return -1;
    return 0;
}

int hive_parse_payload(const uint8_t *data, uint32_t size, void *root,
                       const hive_visitor_t *vis, uint32_t *keys_out,
                       uint32_t *values_out)
{
    hive_walk_t w;
    char root_name[REG_MAX_KEY_NAME + 1];
    uint16_t root_vals, root_children, i;

    if (!data || size == 0)
        return -1;
    w.rd.buf = data;
    w.rd.pos = 0;
    w.rd.cap = size;
    w.vis = vis;
    w.keys = 0;
    w.values = 0;

    /* The root record names the hive's own root key, which the caller already
     * holds: its name is read and bounded but not looked up. It carries the
     * full key name (hive_serialize_key emits up to REG_MAX_KEY_NAME); the
     * header's root_name[64] is a separate truncated quick-id field. */
    if (rd_name(&w.rd, REG_MAX_KEY_NAME, root_name) < 0) return -1;
    if (rd_u16(&w.rd, &root_vals) < 0) return -1;
    if (rd_u16(&w.rd, &root_children) < 0) return -1;
    w.keys++;

    for (i = 0; i < root_vals; i++)
        if (walk_value(&w, root) < 0) return -1;
    for (i = 0; i < root_children; i++)
        if (walk_key(&w, root, 1) < 0) return -1;

    /* Trailing bytes mean the counts and the records disagree. */
    if (w.rd.pos != w.rd.cap)
        return -1;
    if (keys_out) *keys_out = w.keys;
    if (values_out) *values_out = w.values;
    return 0;
}

int hive_validate_payload(const uint8_t *data, uint32_t size,
                          uint32_t expected_keys, uint32_t expected_values,
                          uint32_t *keys_out, uint32_t *values_out)
{
    uint32_t keys, values;

    if (hive_parse_payload(data, size, (void *)0, (const hive_visitor_t *)0,
                           &keys, &values) < 0)
        return -1;
    if (keys != expected_keys || values != expected_values)
        return -1;
    if (keys_out) *keys_out = keys;
    if (values_out) *values_out = values;
    return 0;
}

int hive_header_fields_ok(const hive_header_t *hdr)
{
    if (hdr->magic != HIVE_MAGIC) return 0;
    if (hdr->version != HIVE_VERSION) return 0;
    if (hdr->data_offset != HIVE_HEADER_SIZE) return 0;
    if (hdr->data_size == 0 || hdr->data_size > HIVE_MAX_DATA_SIZE) return 0;
    return 1;
}
