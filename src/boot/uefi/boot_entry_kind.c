/* boot_entry_kind.c -- per-kind payload validators implementation.
 *
 * Pure C, no UEFI types, no dynamic allocation. Compiled in two contexts:
 *   - src/boot/uefi/Makefile -> bootloader, freestanding x86_64-elf
 *   - src/kernel/test (via test_boot_entry_kind.c) -> kernel unit-test build
 *
 * The validators receive the payload byte range as it appears in the raw
 * bootentries.json. The store-level parser already validated overall
 * grammar (object braces match, strings well-formed, no nested arrays
 * past the depth cap); the per-kind walker here scans the flat key-value
 * shape that payload objects use:
 *
 *   "payload": {
 *       "kernel": "\\path",
 *       "initrd": ["\\path", ...],
 *       "cmdline": "...",
 *       "root": "..."
 *   }
 *
 * Strings are extracted with simple escape handling (\" \\ \n \r \t \uXXXX
 * subset); arrays are flat string arrays only. Nested objects in payload
 * are rejected as BOOT_ENTRY_KIND_REJ_FIELD_TYPE.
 */

#include "../../../include/boot/boot_entry_kind.h"

typedef unsigned char  u8;
typedef unsigned int   u32;

/* ============================================================================
 * Mini JSON-flat-object reader
 * ============================================================================ */

typedef struct {
    const u8 *buf;
    u32       len;
    u32       pos;
} jr_t;

static int jr_eof(const jr_t *j) { return j->pos >= j->len; }

static void jr_skip_ws(jr_t *j)
{
    while (!jr_eof(j)) {
        u8 c = j->buf[j->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            j->pos++;
        else
            break;
    }
}

static int jr_match(jr_t *j, char c)
{
    jr_skip_ws(j);
    if (jr_eof(j) || j->buf[j->pos] != (u8)c) return 0;
    j->pos++;
    return 1;
}

/* Read a JSON string into `out` (NUL-terminated, capped at `out_cap-1`
 * bytes). Returns 1 on success, 0 on shape error or overflow. Recognizes
 * \" \\ \n \r \t escapes; treats \uXXXX as a single byte by taking the
 * low byte (the parser already validated full escape correctness;
 * payload values are operationally ASCII so \uXXXX reduction is safe). */
static int jr_read_string(jr_t *j, char *out, u32 out_cap)
{
    jr_skip_ws(j);
    if (jr_eof(j) || j->buf[j->pos] != '"') return 0;
    j->pos++;

    u32 wp = 0;
    while (!jr_eof(j)) {
        u8 c = j->buf[j->pos++];
        if (c == '"') {
            if (wp >= out_cap) return 0;
            out[wp] = 0;
            return 1;
        }
        if (c == '\\') {
            if (jr_eof(j)) return 0;
            u8 esc = j->buf[j->pos++];
            u8 mapped;
            switch (esc) {
                case '"':  mapped = '"'; break;
                case '\\': mapped = '\\'; break;
                case '/':  mapped = '/'; break;
                case 'n':  mapped = '\n'; break;
                case 'r':  mapped = '\r'; break;
                case 't':  mapped = '\t'; break;
                case 'b':  mapped = '\b'; break;
                case 'f':  mapped = '\f'; break;
                case 'u': {
                    if (j->pos + 4 > j->len) return 0;
                    /* Take low byte of the 4 hex digits. Parser already
                     * validated hex grammar. */
                    u32 v = 0;
                    for (int k = 0; k < 4; k++) {
                        u8 hd = j->buf[j->pos++];
                        v <<= 4;
                        if (hd >= '0' && hd <= '9') v |= (u32)(hd - '0');
                        else if (hd >= 'a' && hd <= 'f') v |= (u32)(hd - 'a' + 10);
                        else if (hd >= 'A' && hd <= 'F') v |= (u32)(hd - 'A' + 10);
                        else return 0;
                    }
                    mapped = (u8)(v & 0xFFu);
                    break;
                }
                default: return 0;
            }
            if (wp + 1 >= out_cap) return 0;
            out[wp++] = (char)mapped;
        } else if (c < 0x20) {
            return 0; /* unescaped control char */
        } else {
            if (wp + 1 >= out_cap) return 0;
            out[wp++] = (char)c;
        }
    }
    return 0; /* unterminated string */
}

/* Skip a JSON value (string / array / number / true / false / null /
 * object). Used when the walker encounters a key it doesn't recognize
 * for the current kind. Conservative: rejects nested objects (payload
 * is required to be flat). */
static int jr_skip_value(jr_t *j, int allow_object)
{
    jr_skip_ws(j);
    if (jr_eof(j)) return 0;
    u8 c = j->buf[j->pos];
    if (c == '"') {
        char dump[BOOT_ENTRIES_MAX_PATH_LEN + 1u];
        return jr_read_string(j, dump, sizeof(dump));
    }
    if (c == '[') {
        j->pos++;
        jr_skip_ws(j);
        if (!jr_eof(j) && j->buf[j->pos] == ']') { j->pos++; return 1; }
        while (!jr_eof(j)) {
            if (!jr_skip_value(j, 0)) return 0;
            jr_skip_ws(j);
            if (jr_eof(j)) return 0;
            u8 nx = j->buf[j->pos];
            if (nx == ',') { j->pos++; continue; }
            if (nx == ']') { j->pos++; return 1; }
            return 0;
        }
        return 0;
    }
    if (c == '{') {
        if (!allow_object) return 0;
        /* For now payload values are flat; the only nested object would
         * be future-spec extension. Reject conservatively. */
        return 0;
    }
    if (c == 't') {
        if (j->pos + 4 > j->len) return 0;
        if (j->buf[j->pos+1]!='r'||j->buf[j->pos+2]!='u'||j->buf[j->pos+3]!='e') return 0;
        j->pos += 4; return 1;
    }
    if (c == 'f') {
        if (j->pos + 5 > j->len) return 0;
        if (j->buf[j->pos+1]!='a'||j->buf[j->pos+2]!='l'||j->buf[j->pos+3]!='s'||j->buf[j->pos+4]!='e') return 0;
        j->pos += 5; return 1;
    }
    if (c == 'n') {
        if (j->pos + 4 > j->len) return 0;
        if (j->buf[j->pos+1]!='u'||j->buf[j->pos+2]!='l'||j->buf[j->pos+3]!='l') return 0;
        j->pos += 4; return 1;
    }
    /* number: skip until terminator */
    if (c == '-' || (c >= '0' && c <= '9')) {
        while (!jr_eof(j)) {
            u8 nc = j->buf[j->pos];
            if ((nc >= '0' && nc <= '9') || nc == '.' || nc == 'e' || nc == 'E' ||
                nc == '+' || nc == '-') {
                j->pos++;
            } else break;
        }
        return 1;
    }
    return 0;
}

/* Read a flat string array into `out` (each element NUL-terminated),
 * capping at `max_count` strings. Returns 1 on success. */
static int jr_read_string_array(jr_t *j,
                                 char (*out)[BOOT_ENTRIES_MAX_PATH_LEN + 1u],
                                 u32 max_count,
                                 u32 *out_count)
{
    jr_skip_ws(j);
    if (jr_eof(j) || j->buf[j->pos] != '[') return 0;
    j->pos++;
    *out_count = 0;
    jr_skip_ws(j);
    if (!jr_eof(j) && j->buf[j->pos] == ']') { j->pos++; return 1; }

    while (!jr_eof(j)) {
        if (*out_count >= max_count) return 0; /* over cap */
        if (!jr_read_string(j, out[*out_count], BOOT_ENTRIES_MAX_PATH_LEN + 1u)) return 0;
        (*out_count)++;
        jr_skip_ws(j);
        if (jr_eof(j)) return 0;
        u8 nc = j->buf[j->pos];
        if (nc == ',') { j->pos++; continue; }
        if (nc == ']') { j->pos++; return 1; }
        return 0;
    }
    return 0;
}

/* ============================================================================
 * String helpers
 * ============================================================================ */

static int str_is_ascii(const char *s)
{
    while (*s) {
        u8 c = (u8)*s++;
        if (c < 0x20 || c >= 0x7F) return 0;
    }
    return 1;
}

static int str_starts_with(const char *s, const char *p)
{
    while (*p) {
        if (*s != *p) return 0;
        s++; p++;
    }
    return 1;
}

static int path_is_under_allowed_prefix(const char *path)
{
    /* Allowed prefixes for split-kernel and UKI paths. Mirrors the
     * file-format Allowed Path Prefixes section. Network URLs and
     * chainload binaries land in deferred validators. */
    if (str_starts_with(path, "\\EFI\\ImpossibleOS\\")) return 1;
    if (str_starts_with(path, "\\EFI\\Linux\\")) return 1;
    if (str_starts_with(path, "\\boot\\")) return 1;
    /* Bare "\\<file>" allowed for split fallback (the bootloader's
     * existing kernel_paths search includes "\\kernel.exe"). Restricted
     * to a SINGLE segment: a second backslash anywhere after the
     * leading one means a nested path outside the allowed roots
     * (`\Windows\kernel.exe`, `\vendor\boot.exe`, etc.) and is
     * rejected. Without this constraint the bare-backslash branch
     * accepts arbitrary ESP paths -- defeating the allowlist. */
    if (path[0] == '\\' && path[1] != 0) {
        const char *p = path + 1;
        while (*p) {
            if (*p == '\\') return 0;
            p++;
        }
        return 1;
    }
    return 0;
}

static int path_has_dotdot(const char *path)
{
    /* Reject ".." anywhere in the path -- prevents directory traversal. */
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') return 1;
        p++;
    }
    return 0;
}

/* ============================================================================
 * Per-kind validators
 * ============================================================================ */

static void zero_decoded(boot_entry_decoded_t *out)
{
    u8 *p = (u8 *)out;
    for (unsigned int i = 0; i < (unsigned int)sizeof(*out); i++) p[i] = 0;
}

static int validate_split(unsigned int flags,
                          int secure_boot_active,
                          const u8 *payload_bytes,
                          u32 payload_len,
                          boot_entry_decoded_t *out)
{
    (void)flags;
    (void)secure_boot_active;
    if (!payload_bytes || payload_len == 0)
        return BOOT_ENTRY_KIND_REJ_PAYLOAD_MISSING;

    jr_t j; j.buf = payload_bytes; j.len = payload_len; j.pos = 0;
    if (!jr_match(&j, '{')) return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;

    jr_skip_ws(&j);
    /* Empty payload object -- still missing required kernel field. */
    if (!jr_eof(&j) && j.buf[j.pos] == '}') {
        j.pos++;
        return BOOT_ENTRY_KIND_REJ_FIELD_MISSING;
    }

    while (!jr_eof(&j)) {
        char key[64];
        if (!jr_read_string(&j, key, sizeof(key)))
            return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
        if (!jr_match(&j, ':'))
            return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;

        if (key[0] == 'k' && key[1] == 'e' && key[2] == 'r' && key[3] == 'n' &&
            key[4] == 'e' && key[5] == 'l' && key[6] == 0) {
            if (!jr_read_string(&j, out->u.split.kernel, sizeof(out->u.split.kernel)))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            if (out->u.split.kernel[0] == 0) return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            if (path_has_dotdot(out->u.split.kernel)) return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            if (!path_is_under_allowed_prefix(out->u.split.kernel))
                return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            if (!str_is_ascii(out->u.split.kernel)) return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            out->u.split.has_kernel = 1;
        } else if (key[0] == 'c' && key[1] == 'm' && key[2] == 'd' &&
                   key[3] == 'l' && key[4] == 'i' && key[5] == 'n' &&
                   key[6] == 'e' && key[7] == 0) {
            if (!jr_read_string(&j, out->u.split.cmdline, sizeof(out->u.split.cmdline)))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            if (!str_is_ascii(out->u.split.cmdline))
                return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            out->u.split.has_cmdline = 1;
        } else if (key[0] == 'r' && key[1] == 'o' && key[2] == 'o' &&
                   key[3] == 't' && key[4] == 0) {
            if (!jr_read_string(&j, out->u.split.root, sizeof(out->u.split.root)))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            if (!str_is_ascii(out->u.split.root))
                return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            if (path_has_dotdot(out->u.split.root))
                return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            out->u.split.has_root = 1;
        } else if (key[0] == 'i' && key[1] == 'n' && key[2] == 'i' &&
                   key[3] == 't' && key[4] == 'r' && key[5] == 'd' &&
                   key[6] == 0) {
            if (!jr_read_string_array(&j, out->u.split.initrd,
                                      BOOT_ENTRY_DECODED_SPLIT_INITRD_MAX,
                                      &out->u.split.initrd_count))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            for (u32 i = 0; i < out->u.split.initrd_count; i++) {
                if (out->u.split.initrd[i][0] == 0) return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
                if (path_has_dotdot(out->u.split.initrd[i]))
                    return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
                if (!path_is_under_allowed_prefix(out->u.split.initrd[i]))
                    return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
                if (!str_is_ascii(out->u.split.initrd[i]))
                    return BOOT_ENTRY_KIND_REJ_FIELD_VALUE;
            }
        } else {
            /* Unknown key: skip the value and continue. Forward-compat
             * for vendor / future fields. */
            if (!jr_skip_value(&j, 0))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
        }

        jr_skip_ws(&j);
        if (jr_eof(&j)) return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
        u8 nx = j.buf[j.pos];
        if (nx == ',') { j.pos++; continue; }
        if (nx == '}') { j.pos++; break; }
        return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
    }

    if (!out->u.split.has_kernel)
        return BOOT_ENTRY_KIND_REJ_FIELD_MISSING;

    return BOOT_ENTRY_KIND_OK;
}

static int validate_uki(unsigned int flags,
                        int secure_boot_active,
                        const u8 *payload_bytes,
                        u32 payload_len,
                        boot_entry_decoded_t *out)
{
    (void)flags;
    (void)secure_boot_active;
    (void)out;
    /* UKI payload schema: uki_path (string, required by canonical
     * doc but the running BOOTX64.UKI.efi already located the kernel,
     * so payload-absence is also accepted at runtime), profile
     * (integer, optional). Disk-side `kernel` / `cmdline` / `initrd`
     * keys ARE NOT permitted -- those would be smuggled overrides on
     * a Secure-Boot-signed UKI. The validator allowlists known UKI
     * keys and rejects anything else. */
    if (!payload_bytes || payload_len == 0)
        return BOOT_ENTRY_KIND_OK;

    jr_t j; j.buf = payload_bytes; j.len = payload_len; j.pos = 0;
    if (!jr_match(&j, '{')) return BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN;
    jr_skip_ws(&j);
    if (!jr_eof(&j) && j.buf[j.pos] == '}') { j.pos++; return BOOT_ENTRY_KIND_OK; }

    while (!jr_eof(&j)) {
        char key[64];
        if (!jr_read_string(&j, key, sizeof(key)))
            return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
        if (!jr_match(&j, ':'))
            return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;

        /* Allowed UKI keys: uki_path (advisory string) + profile
         * (integer 0..15 per UAPI UKI multi-profile selector).
         * Anything else is a forbidden override on a signed UKI. */
        int allowed = 0;
        if (key[0] == 'u' && key[1] == 'k' && key[2] == 'i' &&
            key[3] == '_' && key[4] == 'p' && key[5] == 'a' &&
            key[6] == 't' && key[7] == 'h' && key[8] == 0) {
            char dump[BOOT_ENTRIES_MAX_PATH_LEN + 1u];
            if (!jr_read_string(&j, dump, sizeof(dump)))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            allowed = 1;
        } else if (key[0] == 'p' && key[1] == 'r' && key[2] == 'o' &&
                   key[3] == 'f' && key[4] == 'i' && key[5] == 'l' &&
                   key[6] == 'e' && key[7] == 0) {
            /* Profile is a JSON number 0..15. The skip-value walker
             * accepts any number; the parser already validated the
             * integer grammar. */
            if (!jr_skip_value(&j, 0))
                return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
            allowed = 1;
        } else {
            /* Reject smuggled overrides (kernel, cmdline, initrd, root,
             * arbitrary vendor keys that could redirect the load). */
            return BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN;
        }
        (void)allowed;

        jr_skip_ws(&j);
        if (jr_eof(&j)) return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
        u8 nx = j.buf[j.pos];
        if (nx == ',') { j.pos++; continue; }
        if (nx == '}') { j.pos++; return BOOT_ENTRY_KIND_OK; }
        return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
    }

    return BOOT_ENTRY_KIND_REJ_FIELD_TYPE;
}

static int validate_safe(unsigned int flags,
                         int secure_boot_active,
                         const u8 *payload_bytes,
                         u32 payload_len,
                         boot_entry_decoded_t *out)
{
    /* SAFE shares SPLIT's payload shape (kernel + cmdline + root +
     * initrd[]). The boot_mode flag is set post-policy by the caller
     * via the ladder's reason; this validator only checks payload
     * shape. */
    return validate_split(flags, secure_boot_active, payload_bytes, payload_len, out);
}

static int validate_stub(unsigned int flags,
                         int secure_boot_active,
                         const u8 *payload_bytes,
                         u32 payload_len,
                         boot_entry_decoded_t *out)
{
    (void)flags;
    (void)secure_boot_active;
    (void)payload_bytes;
    (void)payload_len;
    (void)out;
    /* CHAINLOAD / NETWORK / RESUME / RECOVERY / INSTALLER / DIAGNOSTICS
     * / TEST: validators not yet wired. The supported_kinds_mask gate
     * already filters these at admission, but the validator here
     * provides defense-in-depth: even if the mask widens before the
     * per-kind walker is ready, the validator says "not supported" and
     * the entry gets demoted via KIND_UNAVAILABLE. */
    return BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int boot_entry_kind_validate(unsigned int kind,
                             unsigned int flags,
                             int secure_boot_active,
                             const unsigned char *payload_bytes,
                             unsigned int payload_len,
                             boot_entry_decoded_t *out)
{
    if (!out) return BOOT_ENTRY_KIND_REJ_INTERNAL;
    zero_decoded(out);
    out->kind = kind;

    int rc;
    switch (kind) {
        case BOOT_ENTRY_KIND_SPLIT:
            rc = validate_split(flags, secure_boot_active, payload_bytes, payload_len, out);
            break;
        case BOOT_ENTRY_KIND_UKI:
            rc = validate_uki(flags, secure_boot_active, payload_bytes, payload_len, out);
            break;
        case BOOT_ENTRY_KIND_SAFE:
            rc = validate_safe(flags, secure_boot_active, payload_bytes, payload_len, out);
            break;
        case BOOT_ENTRY_KIND_CHAINLOAD:
        case BOOT_ENTRY_KIND_NETWORK:
        case BOOT_ENTRY_KIND_RESUME:
        case BOOT_ENTRY_KIND_RECOVERY:
        case BOOT_ENTRY_KIND_INSTALLER:
        case BOOT_ENTRY_KIND_DIAGNOSTICS:
        case BOOT_ENTRY_KIND_TEST:
            rc = validate_stub(flags, secure_boot_active, payload_bytes, payload_len, out);
            break;
        default:
            rc = BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED;
            break;
    }

    out->valid = (rc == BOOT_ENTRY_KIND_OK) ? 1 : 0;
    return rc;
}

const char *boot_entry_kind_reject_name(int code)
{
    switch (code) {
        case BOOT_ENTRY_KIND_OK:                  return "OK";
        case BOOT_ENTRY_KIND_REJ_PAYLOAD_MISSING: return "REJ_PAYLOAD_MISSING";
        case BOOT_ENTRY_KIND_REJ_PAYLOAD_FORBIDDEN: return "REJ_PAYLOAD_FORBIDDEN";
        case BOOT_ENTRY_KIND_REJ_FIELD_MISSING:   return "REJ_FIELD_MISSING";
        case BOOT_ENTRY_KIND_REJ_FIELD_TYPE:      return "REJ_FIELD_TYPE";
        case BOOT_ENTRY_KIND_REJ_FIELD_VALUE:     return "REJ_FIELD_VALUE";
        case BOOT_ENTRY_KIND_REJ_NOT_SUPPORTED:   return "REJ_NOT_SUPPORTED";
        case BOOT_ENTRY_KIND_REJ_INTERNAL:        return "REJ_INTERNAL";
        default:                                  return "UNKNOWN";
    }
}
