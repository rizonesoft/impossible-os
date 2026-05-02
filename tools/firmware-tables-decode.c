/* ============================================================================
 * firmware-tables-decode.c -- Host-side decoder for firmware-tables.json
 *
 * Reads a kernel-published firmware-tables.json (schema_version=1; canonical
 * wire format pinned at docs/boot/firmware-tables-schema.md), validates the
 * schema_version, prints a human-readable inventory to stdout, and re-emits
 * a canonical JSON form for byte-identical round-trip checks.
 *
 * Self-contained: no external JSON library.  The parser handles the subset
 * the kernel writer emits: objects, arrays, strings (with \\n \\r \\t \\" \\\\
 * \\u0000-\\u00ff escapes), integers, doubles, true, false, null.
 *
 * Usage:
 *   firmware-tables-decode <path-to-firmware-tables.json>
 *       Pretty-prints the inventory to stdout.
 *   firmware-tables-decode --canonical <path>
 *       Emits the canonical re-serialized JSON to stdout.
 *   firmware-tables-decode --round-trip <path>
 *       Reads, parses, re-emits canonical, and re-parses; exits 0 iff the
 *       second-pass canonical output equals the first-pass canonical output
 *       byte-for-byte.  The firmware-tables host-decoder round-trip
 *       regression test invokes this mode.
 *
 * Build: HOST_CC -O2 -o firmware-tables-decode tools/firmware-tables-decode.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <stdarg.h>

/* ---- JSON value representation --------------------------------------------- */

typedef enum {
    JV_NULL, JV_BOOL, JV_INT, JV_DOUBLE, JV_STRING, JV_ARRAY, JV_OBJECT
} jv_type;

typedef struct jv {
    jv_type type;
    union {
        int     b;
        long    i;
        double  d;
        char   *s;
        struct {
            struct jv **items;
            size_t count, cap;
        } arr;
        struct {
            char     **keys;
            struct jv **vals;
            size_t count, cap;
        } obj;
    } u;
} jv_t;

static jv_t *jv_new(jv_type t)
{
    jv_t *v = (jv_t *)calloc(1, sizeof(jv_t));
    if (v) v->type = t;
    return v;
}

static void jv_free(jv_t *v)
{
    if (!v) return;
    if (v->type == JV_STRING) free(v->u.s);
    else if (v->type == JV_ARRAY) {
        for (size_t i = 0; i < v->u.arr.count; i++) jv_free(v->u.arr.items[i]);
        free(v->u.arr.items);
    } else if (v->type == JV_OBJECT) {
        for (size_t i = 0; i < v->u.obj.count; i++) {
            free(v->u.obj.keys[i]);
            jv_free(v->u.obj.vals[i]);
        }
        free(v->u.obj.keys);
        free(v->u.obj.vals);
    }
    free(v);
}

static int arr_push(jv_t *a, jv_t *item)
{
    if (a->u.arr.count == a->u.arr.cap) {
        size_t nc = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
        jv_t **n = (jv_t **)realloc(a->u.arr.items, nc * sizeof(jv_t *));
        if (!n) return -1;
        a->u.arr.items = n;
        a->u.arr.cap = nc;
    }
    a->u.arr.items[a->u.arr.count++] = item;
    return 0;
}

static int obj_push(jv_t *o, char *key, jv_t *val)
{
    if (o->u.obj.count == o->u.obj.cap) {
        size_t nc = o->u.obj.cap ? o->u.obj.cap * 2 : 8;
        char **nk = (char **)realloc(o->u.obj.keys, nc * sizeof(char *));
        jv_t **nv = (jv_t **)realloc(o->u.obj.vals, nc * sizeof(jv_t *));
        if (!nk || !nv) { free(nk); free(nv); return -1; }
        o->u.obj.keys = nk;
        o->u.obj.vals = nv;
        o->u.obj.cap = nc;
    }
    o->u.obj.keys[o->u.obj.count] = key;
    o->u.obj.vals[o->u.obj.count] = val;
    o->u.obj.count++;
    return 0;
}

static jv_t *obj_get(const jv_t *o, const char *k)
{
    if (!o || o->type != JV_OBJECT) return NULL;
    for (size_t i = 0; i < o->u.obj.count; i++)
        if (strcmp(o->u.obj.keys[i], k) == 0) return o->u.obj.vals[i];
    return NULL;
}

/* ---- Parser ---------------------------------------------------------------- */

typedef struct {
    const char *p;
    const char *end;
    char err[128];
} jp_t;

static void jp_skipws(jp_t *jp)
{
    while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r'))
        jp->p++;
}

static int jp_consume(jp_t *jp, char c)
{
    jp_skipws(jp);
    if (jp->p >= jp->end || *jp->p != c) return -1;
    jp->p++;
    return 0;
}

static int jp_match(jp_t *jp, const char *kw)
{
    jp_skipws(jp);
    size_t n = strlen(kw);
    if ((size_t)(jp->end - jp->p) < n) return -1;
    if (memcmp(jp->p, kw, n) != 0) return -1;
    jp->p += n;
    return 0;
}

static char *jp_string(jp_t *jp)
{
    jp_skipws(jp);
    if (jp->p >= jp->end || *jp->p != '"') {
        snprintf(jp->err, sizeof(jp->err), "expected '\"' at offset %ld", jp->p - (jp->p - 1));
        return NULL;
    }
    jp->p++;
    size_t cap = 64, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    while (jp->p < jp->end && *jp->p != '"') {
        char c = *jp->p++;
        if (c == '\\' && jp->p < jp->end) {
            char e = *jp->p++;
            switch (e) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'u': {
                    if (jp->p + 4 > jp->end) { free(out); return NULL; }
                    unsigned code = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = jp->p[k];
                        unsigned d = (h >= '0' && h <= '9') ? (unsigned)(h - '0')
                                   : (h >= 'a' && h <= 'f') ? (unsigned)(h - 'a' + 10)
                                   : (h >= 'A' && h <= 'F') ? (unsigned)(h - 'A' + 10) : 16;
                        if (d == 16) { free(out); return NULL; }
                        code = (code << 4) | d;
                    }
                    jp->p += 4;
                    if (code < 0x80) c = (char)code;
                    else {
                        if (n + 4 >= cap) {
                            cap *= 2;
                            char *n2 = (char *)realloc(out, cap);
                            if (!n2) { free(out); return NULL; }
                            out = n2;
                        }
                        if (code < 0x800) {
                            out[n++] = (char)(0xC0 | (code >> 6));
                            out[n++] = (char)(0x80 | (code & 0x3F));
                        } else {
                            out[n++] = (char)(0xE0 | (code >> 12));
                            out[n++] = (char)(0x80 | ((code >> 6) & 0x3F));
                            out[n++] = (char)(0x80 | (code & 0x3F));
                        }
                        continue;
                    }
                    break;
                }
                default: c = e; break;
            }
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *n2 = (char *)realloc(out, cap);
            if (!n2) { free(out); return NULL; }
            out = n2;
        }
        out[n++] = c;
    }
    if (jp->p >= jp->end || *jp->p != '"') { free(out); return NULL; }
    jp->p++;
    out[n] = '\0';
    return out;
}

static jv_t *jp_value(jp_t *jp);

static jv_t *jp_number(jp_t *jp)
{
    /* Strict RFC 8259 number grammar:
     *   number = [-] int [frac] [exp]
     *   int    = '0' | digit1-9 *digit
     *   frac   = '.' 1*digit
     *   exp    = ('e'|'E') [+|-] 1*digit
     * Reject: leading zeros (e.g. "01"), missing digits after '.', missing
     * digits after exponent marker, bare '-' or '.' or '1e'. */
    jp_skipws(jp);
    const char *start = jp->p;
    int is_dbl = 0;
    if (jp->p < jp->end && *jp->p == '-') jp->p++;
    /* int part: 1*digit, leading zero allowed only as standalone "0" */
    if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') return NULL;
    if (*jp->p == '0') {
        jp->p++;
    } else {
        while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    }
    if (jp->p < jp->end && *jp->p == '.') {
        is_dbl = 1;
        jp->p++;
        const char *frac_start = jp->p;
        while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
        if (jp->p == frac_start) return NULL;  /* no digit after '.' */
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
        is_dbl = 1;
        jp->p++;
        if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-')) jp->p++;
        const char *exp_start = jp->p;
        while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
        if (jp->p == exp_start) return NULL;   /* no digit after exponent */
    }
    char buf[64];
    size_t len = (size_t)(jp->p - start);
    if (len >= sizeof(buf)) return NULL;
    memcpy(buf, start, len);
    buf[len] = '\0';
    jv_t *v = jv_new(is_dbl ? JV_DOUBLE : JV_INT);
    if (!v) return NULL;
    char *endp = NULL;
    if (is_dbl) {
        v->u.d = strtod(buf, &endp);
        if (endp != buf + len) { jv_free(v); return NULL; }
    } else {
        v->u.i = strtol(buf, &endp, 10);
        if (endp != buf + len) { jv_free(v); return NULL; }
    }
    return v;
}

static jv_t *jp_array(jp_t *jp)
{
    if (jp_consume(jp, '[') != 0) return NULL;
    jv_t *a = jv_new(JV_ARRAY);
    if (!a) return NULL;
    jp_skipws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return a; }
    for (;;) {
        jv_t *item = jp_value(jp);
        if (!item) { jv_free(a); return NULL; }
        if (arr_push(a, item) != 0) { jv_free(item); jv_free(a); return NULL; }
        jp_skipws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        break;
    }
    if (jp_consume(jp, ']') != 0) { jv_free(a); return NULL; }
    return a;
}

static jv_t *jp_object(jp_t *jp)
{
    if (jp_consume(jp, '{') != 0) return NULL;
    jv_t *o = jv_new(JV_OBJECT);
    if (!o) return NULL;
    jp_skipws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return o; }
    for (;;) {
        char *k = jp_string(jp);
        if (!k) { jv_free(o); return NULL; }
        if (jp_consume(jp, ':') != 0) { free(k); jv_free(o); return NULL; }
        jv_t *v = jp_value(jp);
        if (!v) { free(k); jv_free(o); return NULL; }
        if (obj_push(o, k, v) != 0) { free(k); jv_free(v); jv_free(o); return NULL; }
        jp_skipws(jp);
        if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
        break;
    }
    if (jp_consume(jp, '}') != 0) { jv_free(o); return NULL; }
    return o;
}

static jv_t *jp_value(jp_t *jp)
{
    jp_skipws(jp);
    if (jp->p >= jp->end) return NULL;
    char c = *jp->p;
    if (c == '{') return jp_object(jp);
    if (c == '[') return jp_array(jp);
    if (c == '"') {
        char *s = jp_string(jp);
        if (!s) return NULL;
        jv_t *v = jv_new(JV_STRING);
        if (!v) { free(s); return NULL; }
        v->u.s = s;
        return v;
    }
    if (jp_match(jp, "true") == 0)  { jv_t *v = jv_new(JV_BOOL); if (v) v->u.b = 1; return v; }
    if (jp_match(jp, "false") == 0) { jv_t *v = jv_new(JV_BOOL); if (v) v->u.b = 0; return v; }
    if (jp_match(jp, "null") == 0)  { return jv_new(JV_NULL); }
    return jp_number(jp);
}

static jv_t *jv_parse(const char *src, size_t len)
{
    jp_t jp = { .p = src, .end = src + len, .err = {0} };
    jv_t *root = jp_value(&jp);
    if (!root) return NULL;
    /* Reject trailing non-whitespace: a valid object followed by stale-tail
     * bytes must NOT silently parse as the prefix.  This closes the
     * "stale-tail bytes after a valid v1 object" gap on round-trip. */
    jp_skipws(&jp);
    if (jp.p != jp.end) {
        fprintf(stderr, "error: trailing garbage at offset %ld\n",
                (long)(jp.p - src));
        jv_free(root);
        return NULL;
    }
    return root;
}

/* ---- Canonical emitter ----------------------------------------------------- */

typedef struct { char *buf; size_t pos, cap; } jw_t;

static void jw_putc(jw_t *w, char c)
{
    if (w->pos + 1 >= w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 256;
        char *n = (char *)realloc(w->buf, nc);
        if (!n) return;
        w->buf = n; w->cap = nc;
    }
    w->buf[w->pos++] = c;
}
static void jw_puts(jw_t *w, const char *s) { while (*s) jw_putc(w, *s++); }

static void jw_str(jw_t *w, const char *s)
{
    jw_putc(w, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  jw_puts(w, "\\\""); break;
            case '\\': jw_puts(w, "\\\\"); break;
            case '\n': jw_puts(w, "\\n");  break;
            case '\r': jw_puts(w, "\\r");  break;
            case '\t': jw_puts(w, "\\t");  break;
            case '\b': jw_puts(w, "\\b");  break;
            case '\f': jw_puts(w, "\\f");  break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    jw_puts(w, esc);
                } else {
                    jw_putc(w, (char)c);
                }
        }
    }
    jw_putc(w, '"');
}

static void jw_value(jw_t *w, const jv_t *v);

static void jw_array(jw_t *w, const jv_t *a)
{
    jw_putc(w, '[');
    for (size_t i = 0; i < a->u.arr.count; i++) {
        if (i) jw_putc(w, ',');
        jw_value(w, a->u.arr.items[i]);
    }
    jw_putc(w, ']');
}

static void jw_object(jw_t *w, const jv_t *o)
{
    jw_putc(w, '{');
    for (size_t i = 0; i < o->u.obj.count; i++) {
        if (i) jw_putc(w, ',');
        jw_str(w, o->u.obj.keys[i]);
        jw_putc(w, ':');
        jw_value(w, o->u.obj.vals[i]);
    }
    jw_putc(w, '}');
}

static void jw_value(jw_t *w, const jv_t *v)
{
    if (!v) { jw_puts(w, "null"); return; }
    char tmp[64];
    switch (v->type) {
        case JV_NULL:   jw_puts(w, "null"); break;
        case JV_BOOL:   jw_puts(w, v->u.b ? "true" : "false"); break;
        case JV_INT:    snprintf(tmp, sizeof(tmp), "%ld", v->u.i); jw_puts(w, tmp); break;
        case JV_DOUBLE: snprintf(tmp, sizeof(tmp), "%g", v->u.d);  jw_puts(w, tmp); break;
        case JV_STRING: jw_str(w, v->u.s); break;
        case JV_ARRAY:  jw_array(w, v); break;
        case JV_OBJECT: jw_object(w, v); break;
    }
}

static char *jv_emit(const jv_t *root)
{
    jw_t w = { .buf = NULL, .pos = 0, .cap = 0 };
    jw_value(&w, root);
    jw_putc(&w, '\0');
    return w.buf;
}

/* ---- File I/O -------------------------------------------------------------- */

static char *slurp_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

/* ---- Pretty-printer -------------------------------------------------------- */

static void print_kv(const char *key, const jv_t *v, int indent)
{
    for (int i = 0; i < indent; i++) putchar(' ');
    if (v->type == JV_STRING)      printf("%-22s %s\n", key, v->u.s);
    else if (v->type == JV_INT)    printf("%-22s %ld\n", key, v->u.i);
    else if (v->type == JV_DOUBLE) printf("%-22s %g\n", key, v->u.d);
    else if (v->type == JV_BOOL)   printf("%-22s %s\n", key, v->u.b ? "true" : "false");
    else if (v->type == JV_NULL)   printf("%-22s null\n", key);
}

static void print_table_entry(const jv_t *entry, int idx)
{
    const jv_t *name      = obj_get(entry, "name");
    const jv_t *guid      = obj_get(entry, "guid");
    const jv_t *source    = obj_get(entry, "source");
    const jv_t *phys_addr = obj_get(entry, "phys_addr");
    const jv_t *size      = obj_get(entry, "size");
    const jv_t *checksum  = obj_get(entry, "checksum");
    const jv_t *cs_status = obj_get(entry, "checksum_status");
    const jv_t *vr        = obj_get(entry, "validation_reason");

    printf("  [%2d] %-12s %-16s %-18s sz=%-6ld cs=%-3ld %-12s",
           idx,
           name && name->type == JV_STRING ? name->u.s : "?",
           source && source->type == JV_STRING ? source->u.s : "?",
           phys_addr && phys_addr->type == JV_STRING ? phys_addr->u.s : "?",
           size && size->type == JV_INT ? size->u.i : 0L,
           checksum && checksum->type == JV_INT ? checksum->u.i : 0L,
           cs_status && cs_status->type == JV_STRING ? cs_status->u.s : "?");
    if (guid && guid->type == JV_STRING) printf(" %s", guid->u.s);
    if (vr && vr->type == JV_STRING)     printf(" reason=%s", vr->u.s);
    putchar('\n');
}

static void pretty_print(const jv_t *root)
{
    const jv_t *sv = obj_get(root, "schema_version");
    const jv_t *ts = obj_get(root, "generated_at_utc");
    const jv_t *fp = obj_get(root, "firmware_platform");

    printf("=== firmware-tables.json ===\n");
    if (sv) print_kv("schema_version",   sv, 0);
    if (ts) print_kv("generated_at_utc", ts, 0);
    /* Wire format: firmware_platform is a string ("ACPI"/"DTB"/"HYBRID"/
     * "UNKNOWN"). Older drafts wrapped it in an object; tolerate both. */
    if (fp && fp->type == JV_STRING) {
        print_kv("firmware_platform", fp, 0);
    } else if (fp && fp->type == JV_OBJECT) {
        const jv_t *cls = obj_get(fp, "class");
        if (cls) print_kv("firmware_platform", cls, 0);
    }
    const jv_t *cp = obj_get(root, "conformance_profile");
    if (cp && cp->type == JV_OBJECT) {
        const jv_t *level = obj_get(cp, "level");
        const jv_t *name  = obj_get(cp, "name");
        if (level) print_kv("conformance_level", level, 0);
        if (name)  print_kv("conformance_name",  name,  0);
    }

    const jv_t *tables = obj_get(root, "tables");
    if (tables && tables->type == JV_ARRAY) {
        printf("\n-- tables[] (%zu entries) --\n", tables->u.arr.count);
        for (size_t i = 0; i < tables->u.arr.count; i++) {
            const jv_t *e = tables->u.arr.items[i];
            if (e->type == JV_OBJECT) print_table_entry(e, (int)i);
        }
    }

    const jv_t *deg = obj_get(root, "degraded");
    if (deg && deg->type == JV_ARRAY) {
        printf("\n-- degraded[] (%zu entries) --\n", deg->u.arr.count);
        for (size_t i = 0; i < deg->u.arr.count; i++) {
            const jv_t *e = deg->u.arr.items[i];
            if (e->type == JV_INT) printf("  index=%ld\n", e->u.i);
        }
    }

    const jv_t *qa = obj_get(root, "quirks_active");
    if (qa && qa->type == JV_ARRAY) {
        printf("\n-- quirks_active[] (%zu entries) --\n", qa->u.arr.count);
        for (size_t i = 0; i < qa->u.arr.count; i++) {
            const jv_t *e = qa->u.arr.items[i];
            if (e->type == JV_STRING) printf("  %s\n", e->u.s);
        }
    }

    static const char *blocks[] = {
        "acpi", "smbios", "mat", "rt_properties", "esrt", "apei", "dbg2", "wsmt"
    };
    for (size_t b = 0; b < sizeof(blocks)/sizeof(blocks[0]); b++) {
        const jv_t *blk = obj_get(root, blocks[b]);
        if (!blk || blk->type != JV_OBJECT) continue;
        printf("\n-- %s --\n", blocks[b]);
        for (size_t k = 0; k < blk->u.obj.count; k++) {
            const jv_t *child = blk->u.obj.vals[k];
            const char *key   = blk->u.obj.keys[k];
            if (child->type == JV_OBJECT) printf("  %-22s <object>\n", key);
            else if (child->type == JV_ARRAY) printf("  %-22s <array of %zu>\n", key, child->u.arr.count);
            else print_kv(key, child, 2);
        }
    }
}

/* ---- Round-trip + main ----------------------------------------------------- */

static int validate_schema_version(const jv_t *root);

static int round_trip(const char *path)
{
    size_t n = 0;
    char *input = slurp_file(path, &n);
    if (!input) return 2;

    jv_t *root1 = jv_parse(input, n);
    free(input);
    if (!root1) { fprintf(stderr, "round-trip: first parse failed\n"); return 3; }

    /* Schema-version gate before re-emit -- a schema_version=99 or
     * missing-schema file must not "succeed" round-trip just because the
     * canonical re-emit is byte-stable. */
    if (validate_schema_version(root1) != 0) { jv_free(root1); return 8; }

    char *first = jv_emit(root1);
    jv_free(root1);
    if (!first) { fprintf(stderr, "round-trip: first emit failed\n"); return 4; }

    jv_t *root2 = jv_parse(first, strlen(first));
    if (!root2) { fprintf(stderr, "round-trip: second parse failed\n"); free(first); return 5; }

    char *second = jv_emit(root2);
    jv_free(root2);
    if (!second) { fprintf(stderr, "round-trip: second emit failed\n"); free(first); return 6; }

    int diff = strcmp(first, second);
    if (diff != 0) {
        fprintf(stderr, "round-trip: byte mismatch\n");
        fprintf(stderr, "  first  (%zu bytes): %.80s%s\n",
                strlen(first),  first,  strlen(first)  > 80 ? "..." : "");
        fprintf(stderr, "  second (%zu bytes): %.80s%s\n",
                strlen(second), second, strlen(second) > 80 ? "..." : "");
    }

    free(first);
    free(second);
    return diff == 0 ? 0 : 7;
}

static int validate_schema_version(const jv_t *root)
{
    const jv_t *sv = obj_get(root, "schema_version");
    if (!sv || sv->type != JV_INT) {
        fprintf(stderr, "error: schema_version missing or not an integer\n");
        return -1;
    }
    if (sv->u.i != 1) {
        fprintf(stderr, "error: unsupported schema_version=%ld (decoder supports v1)\n", sv->u.i);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s [--canonical|--round-trip] <path>\n", argv[0]);
        return 1;
    }

    int mode_canonical = 0;
    int mode_roundtrip = 0;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--canonical") == 0)       mode_canonical = 1;
        else if (strcmp(argv[i], "--round-trip") == 0) mode_roundtrip = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "error: unknown flag %s\n", argv[i]);
            return 1;
        } else {
            path = argv[i];
        }
    }

    if (!path) { fprintf(stderr, "error: missing path argument\n"); return 1; }

    if (mode_roundtrip) return round_trip(path);

    size_t n = 0;
    char *input = slurp_file(path, &n);
    if (!input) return 2;

    jv_t *root = jv_parse(input, n);
    free(input);
    if (!root) { fprintf(stderr, "error: parse failed\n"); return 3; }

    if (validate_schema_version(root) != 0) { jv_free(root); return 4; }

    if (mode_canonical) {
        char *out = jv_emit(root);
        if (out) { printf("%s\n", out); free(out); }
    } else {
        pretty_print(root);
    }

    jv_free(root);
    return 0;
}
