/* ============================================================================
 * sysinfo.c -- system inventory CLI ("sysinfo.exe")
 *
 * Today's only subcommand is `firmware-updates` (a read-only advisor that
 * renders X:\Diag\firmware-advisor.json as a table).  The binary is laid
 * out as a subcommand dispatcher so future inventory views (memory, cpu,
 * disk topology, network ifconfig parity) can land here without renaming.
 *
 * Closing line on `firmware-updates` is the canonical refusal:
 *   "Apply via the vendor's BIOS update tool; Impossible OS does not write firmware."
 * Required by policy -- never write firmware from this binary.
 *
 * Cache + JSON contract: the kernel publishes X:\Diag\firmware-advisor.json
 * during firmware_advisor_init().  This binary parses the pinned
 * schema_version=1 shape directly with a hand-rolled scanner -- pulling in
 * cJSON would require a user-mode allocator port that this section does
 * not need.  If the schema ever changes, both ends move together.
 * ============================================================================ */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

#define ADVISOR_PATH       "X:\\Diag\\firmware-advisor.json"
#define ADVISOR_BUF_BYTES  (32 * 1024)
#define MAX_COMPONENTS     16

/* ---- Per-component view extracted from JSON ---- */
struct component_view {
    char     fw_class[64];
    char     fw_type_name[16];
    uint32_t current;
    uint32_t latest;
    char     status[24];
    char     severity[16];
    char     vendor_update_url[160];
    char     release_notes_url[160];
    char     cve_id[24];
    char     last_attempt_status_name[40];
    uint32_t last_attempt_status;
    uint32_t rollback_floor_ok;
};

struct top_view {
    int      schema_version;
    char     cache_state[16];
    uint64_t cache_fetched_unix_time;
    int      n_components;
    struct component_view components[MAX_COMPONENTS];
};

/* ---- JSON scanner (purpose-built for the pinned schema) ---- *
 * Not a general parser.  It walks the buffer once, tracks brace depth,
 * extracts string and number values for known keys at depth 1 (top-level
 * fields) and depth 2 (per-component fields when inside the `components`
 * array).  Anything we don't recognize is skipped.  Returns 0 on success,
 * -1 on a structural error (unbalanced braces, runaway string, etc.). */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* Read a JSON string literal starting at *p (which points at the opening
 * '"').  Copy the unescaped contents into out (capped at out_cap-1).
 * Returns the cursor past the closing '"', or NULL on parse error. */
static const char *read_string(const char *p, const char *end,
                               char *out, size_t out_cap)
{
    if (p >= end || *p != '"') return (const char *)0;
    p++;
    size_t out_len = 0;
    while (p < end && *p != '"') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char esc = *p++;
            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'n':  c = '\n'; break;
            case 't':  c = '\t'; break;
            case 'r':  c = '\r'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'u':
                /* \uXXXX -- skip the 4 hex digits; we render the whole
                 * payload to a Windows console where multi-byte UTF-16
                 * isn't useful in this surface anyway. */
                if (p + 4 <= end) p += 4;
                c = '?';
                break;
            default:
                /* Unknown escape; pass the literal char through. */
                c = esc;
            }
        }
        if (out_len + 1 < out_cap)
            out[out_len++] = c;
    }
    if (p >= end || *p != '"') return (const char *)0;
    out[out_len] = '\0';
    return p + 1;
}

/* Read a JSON unsigned-integer literal at *p.  Stops at the first non-digit.
 * Returns the cursor past the digits and stores the value in *out. */
static const char *read_uint(const char *p, const char *end, uint64_t *out)
{
    uint64_t v = 0;
    int saw_digit = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10u + (uint64_t)(*p - '0');
        saw_digit = 1;
        p++;
    }
    if (!saw_digit) return (const char *)0;
    *out = v;
    return p;
}

/* Skip whatever JSON value is at p (string / number / bool / null / object /
 * array).  Returns the cursor past the value or NULL on parse error. */
static const char *skip_value(const char *p, const char *end)
{
    p = skip_ws(p, end);
    if (p >= end) return (const char *)0;
    if (*p == '"') {
        char tmp[2];
        return read_string(p, end, tmp, sizeof(tmp));
    }
    if ((*p >= '0' && *p <= '9') || *p == '-') {
        if (*p == '-') p++;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
            p++;
        return p;
    }
    if (*p == 't' && (end - p) >= 4 && p[1] == 'r' && p[2] == 'u' && p[3] == 'e')
        return p + 4;
    if (*p == 'f' && (end - p) >= 5 && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e')
        return p + 5;
    if (*p == 'n' && (end - p) >= 4 && p[1] == 'u' && p[2] == 'l' && p[3] == 'l')
        return p + 4;
    if (*p == '{' || *p == '[') {
        char open  = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (p < end && depth > 0) {
            if (*p == '"') {
                char tmp[2];
                p = read_string(p, end, tmp, sizeof(tmp));
                if (!p) return (const char *)0;
            } else if (*p == open) {
                depth++; p++;
            } else if (*p == close) {
                depth--; p++;
            } else {
                p++;
            }
        }
        return p;
    }
    return (const char *)0;
}

/* Parse one component object.  p points just past the opening '{'.  Returns
 * the cursor past the matching '}'.  Unknown keys are skipped. */
static const char *parse_component(const char *p, const char *end,
                                   struct component_view *cv)
{
    /* Defaults for unspecified fields */
    cv->fw_class[0] = '\0';
    cv->fw_type_name[0] = '\0';
    cv->current = 0;
    cv->latest = 0;
    cv->status[0] = '\0';
    cv->severity[0] = '\0';
    cv->vendor_update_url[0] = '\0';
    cv->release_notes_url[0] = '\0';
    cv->cve_id[0] = '\0';
    cv->last_attempt_status_name[0] = '\0';
    cv->last_attempt_status = 0;
    cv->rollback_floor_ok = 0;

    p = skip_ws(p, end);
    while (p < end && *p != '}') {
        char key[64];
        p = read_string(p, end, key, sizeof(key));
        if (!p) return (const char *)0;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return (const char *)0;
        p = skip_ws(p + 1, end);

        if (strcmp(key, "fw_class") == 0)
            p = read_string(p, end, cv->fw_class, sizeof(cv->fw_class));
        else if (strcmp(key, "fw_type_name") == 0)
            p = read_string(p, end, cv->fw_type_name, sizeof(cv->fw_type_name));
        else if (strcmp(key, "status") == 0)
            p = read_string(p, end, cv->status, sizeof(cv->status));
        else if (strcmp(key, "severity") == 0)
            p = read_string(p, end, cv->severity, sizeof(cv->severity));
        else if (strcmp(key, "vendor_update_url") == 0)
            p = read_string(p, end, cv->vendor_update_url, sizeof(cv->vendor_update_url));
        else if (strcmp(key, "release_notes_url") == 0)
            p = read_string(p, end, cv->release_notes_url, sizeof(cv->release_notes_url));
        else if (strcmp(key, "cve_id") == 0)
            p = read_string(p, end, cv->cve_id, sizeof(cv->cve_id));
        else if (strcmp(key, "last_attempt_status_name") == 0)
            p = read_string(p, end, cv->last_attempt_status_name, sizeof(cv->last_attempt_status_name));
        else if (strcmp(key, "current") == 0) {
            uint64_t v; p = read_uint(p, end, &v); cv->current = (uint32_t)v;
        } else if (strcmp(key, "latest") == 0) {
            uint64_t v; p = read_uint(p, end, &v); cv->latest = (uint32_t)v;
        } else if (strcmp(key, "last_attempt_status") == 0) {
            uint64_t v; p = read_uint(p, end, &v); cv->last_attempt_status = (uint32_t)v;
        } else if (strcmp(key, "rollback_floor_ok") == 0) {
            uint64_t v; p = read_uint(p, end, &v); cv->rollback_floor_ok = (uint32_t)v;
        } else {
            p = skip_value(p, end);
        }
        if (!p) return (const char *)0;

        p = skip_ws(p, end);
        if (p < end && *p == ',') { p++; p = skip_ws(p, end); }
    }
    if (p >= end || *p != '}') return (const char *)0;
    return p + 1;
}

/* Parse the top-level advisor JSON. */
static int parse_advisor(const char *buf, size_t len, struct top_view *tv)
{
    const char *p   = buf;
    const char *end = buf + len;

    tv->schema_version = 0;
    tv->cache_state[0] = '\0';
    tv->cache_fetched_unix_time = 0;
    tv->n_components = 0;

    p = skip_ws(p, end);
    if (p >= end || *p != '{') return -1;
    p++;
    p = skip_ws(p, end);

    while (p < end && *p != '}') {
        char key[64];
        p = read_string(p, end, key, sizeof(key));
        if (!p) return -1;
        p = skip_ws(p, end);
        if (p >= end || *p != ':') return -1;
        p = skip_ws(p + 1, end);

        if (strcmp(key, "schema_version") == 0) {
            uint64_t v; p = read_uint(p, end, &v);
            tv->schema_version = (int)v;
        } else if (strcmp(key, "cache_state") == 0) {
            p = read_string(p, end, tv->cache_state, sizeof(tv->cache_state));
        } else if (strcmp(key, "cache_fetched_unix_time") == 0) {
            uint64_t v; p = read_uint(p, end, &v);
            tv->cache_fetched_unix_time = v;
        } else if (strcmp(key, "components") == 0) {
            if (p >= end || *p != '[') return -1;
            p++;
            p = skip_ws(p, end);
            while (p < end && *p != ']') {
                if (*p != '{') return -1;
                if (tv->n_components >= MAX_COMPONENTS) {
                    /* Too many components for our buffer; skip the tail. */
                    p = skip_value(p, end);
                } else {
                    p = parse_component(p + 1, end, &tv->components[tv->n_components]);
                    if (!p) return -1;
                    tv->n_components++;
                }
                p = skip_ws(p, end);
                if (p < end && *p == ',') { p++; p = skip_ws(p, end); }
            }
            if (p >= end || *p != ']') return -1;
            p++;
        } else {
            p = skip_value(p, end);
        }
        if (!p) return -1;

        p = skip_ws(p, end);
        if (p < end && *p == ',') { p++; p = skip_ws(p, end); }
    }
    return 0;
}

/* ---- Renderer ---- */

static void render_firmware_updates(const struct top_view *tv)
{
    printf("Firmware Update Advisor (read-only)\n");
    printf("  Cache state:   %s\n", tv->cache_state);
    if (tv->cache_fetched_unix_time)
        printf("  Cache fetched: unix=%llu\n",
               (unsigned long long)tv->cache_fetched_unix_time);
    printf("\n");

    if (tv->n_components == 0) {
        printf("No firmware components reported by ESRT (firmware does not advertise updatable resources).\n\n");
        printf("Apply via the vendor's BIOS update tool; Impossible OS does not write firmware.\n");
        return;
    }

    /* Component-by-component summary.  Window-style stacked layout (avoids
     * fixed column widths that would clip long URLs). */
    for (int i = 0; i < tv->n_components; i++) {
        const struct component_view *c = &tv->components[i];
        printf("[%d] %s  (%s)\n",
               i + 1,
               c->fw_class[0] ? c->fw_class : "(no FwClass)",
               c->fw_type_name[0] ? c->fw_type_name : "Unknown");
        printf("    Current:  0x%x   Latest: 0x%x\n", c->current, c->latest);
        printf("    Status:   %s%s%s\n",
               c->status[0] ? c->status : "unknown",
               c->severity[0] ? "   Severity: " : "",
               c->severity[0] ? c->severity : "");
        if (c->cve_id[0])
            printf("    CVE:      %s\n", c->cve_id);
        if (c->last_attempt_status_name[0] && c->last_attempt_status != 0)
            printf("    Last attempt: %s (code %u)\n",
                   c->last_attempt_status_name, c->last_attempt_status);
        if (c->vendor_update_url[0])
            printf("    Vendor:   %s\n", c->vendor_update_url);
        if (c->release_notes_url[0])
            printf("    Notes:    %s\n", c->release_notes_url);
        if (!c->rollback_floor_ok)
            printf("    Note:     vendor blocks downgrade below current FwVersion\n");
        printf("\n");
    }

    printf("Apply via the vendor's BIOS update tool; Impossible OS does not write firmware.\n");
}

/* ---- Main ---- */

static int cmd_firmware_updates(void)
{
    /* Open + read the advisor JSON. */
    HANDLE fh = sys_openfile(ADVISOR_PATH, 1 /* read */);
    if (fh == INVALID_HANDLE_VALUE) {
        printf("sysinfo: could not open %s\n", ADVISOR_PATH);
        printf("  (firmware advisor not initialized -- "
               "this build did not run firmware_advisor_init() before sysinfo.exe started)\n");
        return 2;
    }

    static char buf[ADVISOR_BUF_BYTES];
    long n = sys_readhandle(fh, buf, ADVISOR_BUF_BYTES - 1);
    sys_closehandle(fh);
    if (n <= 0) {
        printf("sysinfo: read failure on %s (rc=%ld)\n", ADVISOR_PATH, n);
        return 3;
    }
    buf[n] = '\0';

    static struct top_view tv;
    if (parse_advisor(buf, (size_t)n, &tv) != 0) {
        printf("sysinfo: %s parse error\n", ADVISOR_PATH);
        return 4;
    }
    if (tv.schema_version != 1) {
        printf("sysinfo: %s schema_version=%d (expected 1)\n",
               ADVISOR_PATH, tv.schema_version);
        return 5;
    }

    render_firmware_updates(&tv);
    return 0;
}

static void usage(void)
{
    printf("Usage: sysinfo <subcommand>\n");
    printf("\n");
    printf("Subcommands:\n");
    printf("  firmware-updates    Read-only firmware-update advisor\n");
}

int main(int argc, char **argv)
{
    /* Default subcommand: firmware-updates.  cmd.exe's sysinfo builtin
     * spawns us via SYS_EXEC, which doesn't currently propagate argv,
     * so a bare `sysinfo` at the C:\> prompt should still be useful.
     * If argv extension lands later, the explicit subcommand path
     * below stays the canonical contract. */
    if (argc < 2)
        return cmd_firmware_updates();
    if (strcmp(argv[1], "firmware-updates") == 0)
        return cmd_firmware_updates();
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0
        || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }
    printf("sysinfo: unknown subcommand: %s\n", argv[1]);
    usage();
    return 1;
}
