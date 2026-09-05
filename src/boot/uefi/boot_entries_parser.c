/* boot_entries_parser.c -- bootloader boot entry store parser implementation.
 *
 * Pure C, no UEFI types and no allocations. Companion header:
 * include/boot/boot_entries_parser.h. See docs/boot/boot-entry-schema.md for the
 * authoritative spec (CRC algorithm, envelope shape, kind ranges, fallback).
 *
 * Iterative tokenizer + walker with explicit depth cap (BOOT_ENTRIES_MAX_PARSE_DEPTH);
 * adversarial input cannot blow the bootloader stack.
 *
 * Compiled in two contexts:
 *   - src/boot/uefi/Makefile -> bootloader, freestanding x86_64-elf
 *   - src/kernel/test (via test_boot_entry_parser.c) -> kernel unit-test build
 *
 * The translation unit avoids stdlib headers; `unsigned char` / `unsigned int`
 * are exact widths on every Impossible OS target (LP64 / LLP64).
 */

#include "../../../include/boot/boot_entries_parser.h"

typedef unsigned char  u8;
typedef unsigned int   u32;
typedef int            i32;

#define NULL_PTR ((void *)0)

/* ---- IEEE 802.3 CRC-32 (polynomial 0xEDB88320) ----------------------------- */

/* --- Poison-proof lazy initialisation of the CRC table -------------------
 * This object links into BOOTX64.EFI (Makefile), which does NOT zero .bss:
 * firmware pool-poisons it with 0xAF, and a warm reboot can leave the
 * previous boot's bytes at the same address because the image reloads at the
 * same base. The `= 0` initialiser below is DOCUMENTATION, not
 * initialisation, exactly as in bootx64.c.
 *
 * Readiness is therefore a wide exact-match cookie rather than a boolean, and
 * it is `volatile`, both for the same measured reason as the early-diagnostics
 * cookies in bootx64.c. A boolean buys nothing (0xAF is non-zero and so reads
 * as "ready"), and without `volatile` the compiler narrows the wide compare
 * straight back into one: measured 2026-09-03 on the real build object, the
 * previous `static int g_crc32_ready` was emitted at SIZE 1, not 4, because
 * clang may assume nothing outside the program writes an internal-linkage
 * object whose address is never taken. That is the assumption firmware poison
 * violates.
 *
 * What the defect cost: `if (!g_crc32_ready)` skipped the table build, every
 * CRC was then computed over a poisoned table, and boot_entries_parse()
 * rejected a VALID store as CRC_MISMATCH -- sending the loader to its
 * invalid-store fallback, which can select a different kernel than the store
 * asked for.
 *
 * crc32_reset() at the parser entry is the load-bearing half; the cookie is
 * defence in depth, because a constant cookie discriminates poison from
 * ready, never THIS boot from the last one. A stale-but-valid cookie left by
 * a previous boot of the SAME image would in fact leave a correct table (it
 * is a pure function of a compile-time polynomial), but a chainload from a
 * different loader build can leave a table this image never wrote, and the
 * entry reset is what covers that. */
#define BOOT_ENTRIES_CRC32_READY 0x43524332u  /* 'CRC2' */

static u32 g_crc32_table[256];
static volatile u32 g_crc32_ready = 0;

static void crc32_init(void)
{
    u32 i, j, c;
    for (i = 0; i < 256u; i++) {
        c = i;
        for (j = 0; j < 8u; j++)
            c = (c & 1u) ? ((c >> 1) ^ BOOT_ENTRIES_CRC32_POLY) : (c >> 1);
        g_crc32_table[i] = c;
    }
    /* Published LAST, after every table entry is written. */
    g_crc32_ready = BOOT_ENTRIES_CRC32_READY;
}

/* Force the table to be rebuilt from this boot's own code. MUST run as the
 * first statement of boot_entries_parse(), ahead of its early returns, so a
 * store rejected for size or shape still leaves the flag deterministic for a
 * later call. */
static void crc32_reset(void)
{
    g_crc32_ready = 0;
}

/* Computes IEEE 802.3 CRC-32 (init 0xFFFFFFFF, final XOR 0xFFFFFFFF) over raw bytes,
 * with the byte range [zero_off, zero_off+8) treated as 8 ASCII zeros instead of the
 * actual file content. Saves a scratch buffer copy in freestanding-no-heap context.
 */
static u32 crc32_zeroed(const u8 *raw, u32 len, u32 zero_off)
{
    u32 i, crc = 0xFFFFFFFFu;
    /* Exact match, not `!g_crc32_ready`: poison is non-zero and would skip
     * the build, leaving the table itself poisoned. */
    if (g_crc32_ready != BOOT_ENTRIES_CRC32_READY) crc32_init();
    for (i = 0; i < len; i++) {
        u8 b = (i >= zero_off && i < zero_off + 8u) ? (u8)'0' : raw[i];
        crc = (crc >> 8) ^ g_crc32_table[(crc ^ b) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- CRC header value parsing ------------------------------------------- */

/* Parse 8 hex digits at offset; return parsed value or 0 on parse failure.
 * Returns 1 on OK, 0 on failure (invalid hex chars). The caller guarantees
 * off + 8 <= len. */
static int parse_hex8(const u8 *raw, u32 off, u32 *out_val)
{
    u32 v = 0, i;
    for (i = 0; i < 8u; i++) {
        u8 c = raw[off + i];
        u32 nybble;
        if (c >= '0' && c <= '9') nybble = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') nybble = 10u + (u32)(c - 'a');
        else if (c >= 'A' && c <= 'F') nybble = 10u + (u32)(c - 'A');
        else return 0;
        v = (v << 4) | nybble;
    }
    *out_val = v;
    return 1;
}

/* ---- Minimal helpers: strlen, memcmp, copy ----------------------------- */

static u32 s_len(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static int bytes_eq(const u8 *a, const u8 *b, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static void copy_clamped(char *dst, u32 dst_max, const u8 *src, u32 src_len)
{
    u32 n = (src_len < dst_max - 1u) ? src_len : (dst_max - 1u);
    u32 i;
    for (i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[n] = '\0';
}

static void zero_buf(void *p, u32 n)
{
    u8 *b = (u8 *)p;
    u32 i;
    for (i = 0; i < n; i++) b[i] = 0;
}

/* ---- JSON tokenizer (iterative) --------------------------------------- */

typedef enum {
    TOK_EOF = 0, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_COLON, TOK_COMMA, TOK_STRING, TOK_NUMBER, TOK_TRUE, TOK_FALSE, TOK_NULL,
    TOK_ERROR
} tok_t;

typedef struct {
    const u8 *raw;
    u32 raw_len;
    u32 pos;
    /* Current token: */
    tok_t kind;
    u32 start;       /* offset of first byte of token */
    u32 end;         /* offset just past last byte of token */
    /* For TOK_STRING, content_start / content_end span the bytes BETWEEN the
     * surrounding quotes (so the caller can compare the raw inner bytes). */
    u32 content_start;
    u32 content_end;
    /* For TOK_NUMBER, parsed integer value (numbers in this schema are non-
     * negative integers <= 64; we don't support floats / negatives at the
     * envelope layer). */
    u32 number_value;
    int number_valid;       /* 1 if parsed cleanly; 0 if out of range or non-int */
} lexer_t;

static void lex_skip_ws(lexer_t *L)
{
    while (L->pos < L->raw_len) {
        u8 c = L->raw[L->pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { L->pos++; continue; }
        return;
    }
}

/* Skip a JSON string starting at L->pos (must point at the opening quote).
 * Sets content_start / content_end. Handles \\, \", \/, \n, \r, \t, \b, \f, \uHHHH (skip).
 * Returns 1 on OK; 0 on malformed. */
static int lex_string(lexer_t *L)
{
    if (L->pos >= L->raw_len || L->raw[L->pos] != '"') return 0;
    L->start = L->pos;
    L->pos++;
    L->content_start = L->pos;
    while (L->pos < L->raw_len) {
        u8 c = L->raw[L->pos];
        if (c == '"') {
            L->content_end = L->pos;
            L->pos++;
            L->end = L->pos;
            L->kind = TOK_STRING;
            return 1;
        }
        if (c == '\\') {
            if (L->pos + 1u >= L->raw_len) return 0;
            u8 esc = L->raw[L->pos + 1];
            if (esc == 'u') {
                /* \uHHHH -- require exactly 4 hex digits */
                if (L->pos + 5u >= L->raw_len) return 0;
                u32 i;
                for (i = 0; i < 4u; i++) {
                    u8 h = L->raw[L->pos + 2u + i];
                    int ok = (h >= '0' && h <= '9') ||
                             (h >= 'a' && h <= 'f') ||
                             (h >= 'A' && h <= 'F');
                    if (!ok) return 0;
                }
                L->pos += 6u;
                continue;
            }
            /* Whitelist only RFC 8259 single-char escapes. Reject \q etc. */
            if (esc != '"' && esc != '\\' && esc != '/' &&
                esc != 'b' && esc != 'f' && esc != 'n' && esc != 'r' && esc != 't') {
                return 0;
            }
            L->pos += 2u;
            continue;
        }
        if (c < 0x20u) return 0;   /* control char in string -- malformed */
        L->pos++;
    }
    return 0;   /* unterminated string */
}

/* Number tokenizer for non-negative integer values 0..2^32-1. Sufficient for
 * schema_version + numeric kind + timeout_override. Floats / negatives are
 * rejected (number_valid = 0). */
static int lex_number(lexer_t *L)
{
    L->start = L->pos;
    L->number_valid = 1;
    L->number_value = 0;
    if (L->pos >= L->raw_len) return 0;
    if (L->raw[L->pos] == '-') {
        L->number_valid = 0;
        L->pos++;
    }
    if (L->pos >= L->raw_len) return 0;
    /* digits */
    int saw_digit = 0;
    while (L->pos < L->raw_len) {
        u8 c = L->raw[L->pos];
        if (c >= '0' && c <= '9') {
            u32 d = (u32)(c - '0');
            if (L->number_value > (0xFFFFFFFFu - d) / 10u) L->number_valid = 0;
            L->number_value = L->number_value * 10u + d;
            saw_digit = 1;
            L->pos++;
            continue;
        }
        break;
    }
    if (!saw_digit) return 0;
    /* fractional / exponent -> reject (mark invalid but advance past) */
    if (L->pos < L->raw_len && (L->raw[L->pos] == '.' || L->raw[L->pos] == 'e' || L->raw[L->pos] == 'E')) {
        L->number_valid = 0;
        while (L->pos < L->raw_len) {
            u8 c = L->raw[L->pos];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                L->pos++;
                continue;
            }
            break;
        }
    }
    L->end = L->pos;
    L->kind = TOK_NUMBER;
    return 1;
}

static int lex_literal(lexer_t *L, const char *lit, tok_t kind)
{
    u32 n = s_len(lit);
    if (L->pos + n > L->raw_len) return 0;
    if (!bytes_eq(L->raw + L->pos, (const u8 *)lit, n)) return 0;
    L->start = L->pos;
    L->pos += n;
    L->end = L->pos;
    L->kind = kind;
    return 1;
}

/* Advance to the next token. Returns 1 on success (token in L), 0 on parse error
 * (kind set to TOK_ERROR). EOF is success with TOK_EOF. */
static int lex_next(lexer_t *L)
{
    lex_skip_ws(L);
    if (L->pos >= L->raw_len) {
        L->kind = TOK_EOF;
        L->start = L->end = L->pos;
        return 1;
    }
    u8 c = L->raw[L->pos];
    switch (c) {
        case '{': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_LBRACE; return 1;
        case '}': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_RBRACE; return 1;
        case '[': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_LBRACKET; return 1;
        case ']': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_RBRACKET; return 1;
        case ':': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_COLON; return 1;
        case ',': L->start = L->pos; L->pos++; L->end = L->pos; L->kind = TOK_COMMA; return 1;
        case '"': return lex_string(L);
        case 't': return lex_literal(L, "true",  TOK_TRUE);
        case 'f': return lex_literal(L, "false", TOK_FALSE);
        case 'n': return lex_literal(L, "null",  TOK_NULL);
        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return lex_number(L);
        default:
            L->kind = TOK_ERROR;
            return 0;
    }
}

static void lex_init(lexer_t *L, const u8 *raw, u32 len)
{
    L->raw = raw; L->raw_len = len; L->pos = 0;
    L->kind = TOK_EOF; L->start = L->end = 0;
    L->content_start = L->content_end = 0;
    L->number_value = 0; L->number_valid = 0;
}

/* Iterative JSON-value walker. The lexer currently points at the FIRST token of
 * a value. On success, advances past the entire value (including its closing
 * brace / bracket for composites) and leaves the lexer in a state where the
 * caller's next lex_next() returns the FOLLOWING token (',', '}', ']', etc).
 *
 * Validates JSON object/array grammar (delimiter balance is NOT enough; a
 * payload like {"k":} would balance but is malformed -- iterative state
 * machine with explicit container stack is required). Reject codes are
 * mapped by the caller; this returns 1/0.
 *
 * Stack capped at BOOT_ENTRIES_MAX_PARSE_DEPTH; deep nesting -> reject.
 */
typedef enum {
    SKV_OBJ_FIRST_KEY,    /* just consumed '{', expect STRING key or '}' */
    SKV_OBJ_NEXT_KEY,     /* just consumed ',', expect STRING key */
    SKV_OBJ_COMMA_END,    /* just consumed value, expect ',' or '}' */
    SKV_ARR_FIRST_VAL,    /* just consumed '[', expect value or ']' */
    SKV_ARR_NEXT_VAL,     /* just consumed ',', expect value */
    SKV_ARR_COMMA_END,    /* just consumed value, expect ',' or ']' */
} skv_state_t;

/* The unique-key rescan (key_seen_before) skips a value from FURTHER OUT than
 * the authoritative walk does. Skipping the root's `entries` value costs two
 * container levels the main parser never charges to this budget -- it descends
 * the array and the entry object structurally and only calls the skipper on the
 * payload -- so a store the parser ACCEPTS could exhaust a rescan budget of
 * BOOT_ENTRIES_MAX_PARSE_DEPTH. Every prefix the parser accepts must be
 * scannable, or the duplicate check silently stops running on exactly the input
 * an attacker controls. Two extra levels is the exact difference. */
#define BOOT_ENTRIES_RESCAN_EXTRA_DEPTH 2u
#define BOOT_ENTRIES_MAX_SCAN_DEPTH \
    (BOOT_ENTRIES_MAX_PARSE_DEPTH + BOOT_ENTRIES_RESCAN_EXTRA_DEPTH)

static int skip_value_depth(lexer_t *L, u32 max_depth)
{
    tok_t k = L->kind;
    /* Atomic values: nothing else to do. */
    if (k == TOK_STRING || k == TOK_NUMBER || k == TOK_TRUE || k == TOK_FALSE || k == TOK_NULL) {
        return 1;
    }
    if (k != TOK_LBRACE && k != TOK_LBRACKET) return 0;
    if (max_depth > BOOT_ENTRIES_MAX_SCAN_DEPTH) return 0;

    skv_state_t stack[BOOT_ENTRIES_MAX_SCAN_DEPTH];
    u32 depth = 0;
    if (depth >= max_depth) return 0;
    stack[depth++] = (k == TOK_LBRACE) ? SKV_OBJ_FIRST_KEY : SKV_ARR_FIRST_VAL;

    while (depth > 0) {
        if (!lex_next(L)) return 0;
        if (L->kind == TOK_EOF) return 0;
        skv_state_t st = stack[depth - 1];

        if (st == SKV_OBJ_FIRST_KEY || st == SKV_OBJ_NEXT_KEY) {
            if (st == SKV_OBJ_FIRST_KEY && L->kind == TOK_RBRACE) {
                depth--;
                if (depth > 0) stack[depth - 1] = (stack[depth - 1] == SKV_OBJ_FIRST_KEY ||
                                                   stack[depth - 1] == SKV_OBJ_NEXT_KEY ||
                                                   stack[depth - 1] == SKV_OBJ_COMMA_END)
                                                  ? SKV_OBJ_COMMA_END : SKV_ARR_COMMA_END;
                continue;
            }
            if (L->kind != TOK_STRING) return 0;
            /* Expect ':' */
            if (!lex_next(L) || L->kind != TOK_COLON) return 0;
            /* Consume value (recurse via state push or atomic) */
            if (!lex_next(L)) return 0;
            if (L->kind == TOK_LBRACE || L->kind == TOK_LBRACKET) {
                if (depth >= max_depth) return 0;
                /* Mark current state as "after value" before descending */
                stack[depth - 1] = SKV_OBJ_COMMA_END;
                stack[depth++] = (L->kind == TOK_LBRACE) ? SKV_OBJ_FIRST_KEY : SKV_ARR_FIRST_VAL;
            } else if (L->kind == TOK_STRING || L->kind == TOK_NUMBER ||
                       L->kind == TOK_TRUE || L->kind == TOK_FALSE || L->kind == TOK_NULL) {
                stack[depth - 1] = SKV_OBJ_COMMA_END;
            } else {
                return 0;
            }
        }
        else if (st == SKV_OBJ_COMMA_END) {
            if (L->kind == TOK_RBRACE) {
                depth--;
                if (depth > 0) stack[depth - 1] = (stack[depth - 1] == SKV_ARR_FIRST_VAL ||
                                                   stack[depth - 1] == SKV_ARR_NEXT_VAL ||
                                                   stack[depth - 1] == SKV_ARR_COMMA_END)
                                                  ? SKV_ARR_COMMA_END : SKV_OBJ_COMMA_END;
                continue;
            }
            if (L->kind != TOK_COMMA) return 0;
            stack[depth - 1] = SKV_OBJ_NEXT_KEY;
        }
        else if (st == SKV_ARR_FIRST_VAL || st == SKV_ARR_NEXT_VAL) {
            if (st == SKV_ARR_FIRST_VAL && L->kind == TOK_RBRACKET) {
                depth--;
                if (depth > 0) stack[depth - 1] = (stack[depth - 1] == SKV_ARR_FIRST_VAL ||
                                                   stack[depth - 1] == SKV_ARR_NEXT_VAL ||
                                                   stack[depth - 1] == SKV_ARR_COMMA_END)
                                                  ? SKV_ARR_COMMA_END : SKV_OBJ_COMMA_END;
                continue;
            }
            /* Token is the start of a value */
            if (L->kind == TOK_LBRACE || L->kind == TOK_LBRACKET) {
                if (depth >= max_depth) return 0;
                stack[depth - 1] = SKV_ARR_COMMA_END;
                stack[depth++] = (L->kind == TOK_LBRACE) ? SKV_OBJ_FIRST_KEY : SKV_ARR_FIRST_VAL;
            } else if (L->kind == TOK_STRING || L->kind == TOK_NUMBER ||
                       L->kind == TOK_TRUE || L->kind == TOK_FALSE || L->kind == TOK_NULL) {
                stack[depth - 1] = SKV_ARR_COMMA_END;
            } else {
                return 0;
            }
        }
        else { /* SKV_ARR_COMMA_END */
            if (L->kind == TOK_RBRACKET) {
                depth--;
                if (depth > 0) stack[depth - 1] = (stack[depth - 1] == SKV_ARR_FIRST_VAL ||
                                                   stack[depth - 1] == SKV_ARR_NEXT_VAL ||
                                                   stack[depth - 1] == SKV_ARR_COMMA_END)
                                                  ? SKV_ARR_COMMA_END : SKV_OBJ_COMMA_END;
                continue;
            }
            if (L->kind != TOK_COMMA) return 0;
            stack[depth - 1] = SKV_ARR_NEXT_VAL;
        }
    }
    return 1;
}

/* The authoritative walk's skipper: budget starts at the value it is handed. */
static int skip_value_post_token(lexer_t *L)
{
    return skip_value_depth(L, BOOT_ENTRIES_MAX_PARSE_DEPTH);
}

/* ---- Locate the ROOT object's `crc32` member ---------------------------- */

/* Outcome of the CRC-header locate. Tri-state, because a store whose structure
 * is broken BEFORE the header is a JSON problem and not a missing-header one,
 * and the caller maps the two to different reject codes. */
typedef enum {
    CRC_LOC_MALFORMED = -1, /* root-object grammar broke before the header */
    CRC_LOC_ABSENT    = 0,  /* root walked cleanly; no usable crc32 member */
    CRC_LOC_FOUND     = 1,
} crc_loc_t;

/* Find the root object's `"crc32": "0xHHHHHHHH"` member STRUCTURALLY. On
 * CRC_LOC_FOUND, *zero_off is the offset of the 8 hex chars (the bytes the CRC
 * computation zeros, and the same bytes the producer patches in place) and
 * *expected is their value.
 *
 * Only a key at depth 1 of the root object can match. The predecessor scanned
 * the whole file for the seven bytes `"crc32"`, so a store beginning
 * {"note":"crc32", ...} matched inside the note's VALUE and was rejected as a
 * bad CRC field before any key was parsed, while the host validator's regex
 * tie-broke differently -- producer and consumer could disagree about which
 * bytes carry the CRC. A `crc32` member nested inside `payload` was the same
 * shape one level down.
 *
 * This runs BEFORE the CRC is verified, so it walks untrusted bytes: it is
 * bounded by raw_len through the lexer, allocates nothing, and charges every
 * skipped value against BOOT_ENTRIES_MAX_SCAN_DEPTH.
 *
 * The depth budget is the RESCAN budget, not the parse budget, for the reason
 * spelled out at BOOT_ENTRIES_RESCAN_EXTRA_DEPTH: this walk skips the root's
 * `entries` value from two container levels further out than the authoritative
 * walk does (it descends the array and the entry object structurally and only
 * skips the payload). Anything the parser ACCEPTS must be locatable, or the
 * CRC header stops being findable on exactly the deepest legal stores. The
 * root object itself is walked here with lex_next and is never pushed onto the
 * skipper's stack, so it costs no budget.
 */
static crc_loc_t find_crc_field(const u8 *raw, u32 len, u32 *zero_off, u32 *expected)
{
    lexer_t L;
    lex_init(&L, raw, len);
    if (!lex_next(&L) || L.kind != TOK_LBRACE) return CRC_LOC_MALFORMED;

    /* A `}` closes the object legally at the FIRST key position and illegally
     * straight after a comma: `{"schema_version":1,}` is a trailing comma, which
     * the authoritative parse rejects and this walk must not read as "the root
     * simply carries no crc32 member". Without the flag both positions share one
     * RBRACE check and a broken root reports ABSENT. */
    int after_comma = 0;

    for (;;) {
        u32 key_cs, key_ce;
        int is_crc;

        if (!lex_next(&L)) return CRC_LOC_MALFORMED;
        if (L.kind == TOK_RBRACE)
            return after_comma ? CRC_LOC_MALFORMED : CRC_LOC_ABSENT;
        if (L.kind != TOK_STRING) return CRC_LOC_MALFORMED;
        key_cs = L.content_start;
        key_ce = L.content_end;

        if (!lex_next(&L) || L.kind != TOK_COLON) return CRC_LOC_MALFORMED;
        /* lex_next reports end-of-input as a SUCCESSFUL TOK_EOF read, so a store
         * truncated right after the colon would otherwise fall through to the
         * value-shape checks and be classified ABSENT. Truncation is a broken
         * root object, not a missing member. */
        if (!lex_next(&L) || L.kind == TOK_EOF) return CRC_LOC_MALFORMED;

        /* Raw-byte compare, so an escaped spelling of the key is not this key.
         * That matches key_is_literal() below and the schema's literal-key
         * rule: the firmware never decodes escapes in a key name. */
        is_crc = (key_ce - key_cs == 5u) &&
                 bytes_eq(raw + key_cs, (const u8 *)"crc32", 5u);
        if (is_crc) {
            u32 vs, ve;
            /* First occurrence wins. A repeated root key is rejected by the
             * duplicate-key check during the authoritative parse. */
            if (L.kind != TOK_STRING) return CRC_LOC_ABSENT;
            vs = L.content_start;
            ve = L.content_end;
            /* EXACTLY `0x` + 8 hex digits, lowercase `x`. The host validator
             * requires the same (tools/boot-entry-validate/validate.py: the
             * whole-store check rejects any crc32 text that is not 10 chars
             * starting "0x"), and the CRC compute patches a fixed 8-byte span,
             * so a longer or upper-case-prefixed value has no agreed span. */
            if (ve - vs != 10u) return CRC_LOC_ABSENT;
            if (raw[vs] != '0' || raw[vs + 1u] != 'x') return CRC_LOC_ABSENT;
            if (!parse_hex8(raw, vs + 2u, expected)) return CRC_LOC_ABSENT;
            *zero_off = vs + 2u;
            return CRC_LOC_FOUND;
        }

        if (!skip_value_depth(&L, BOOT_ENTRIES_MAX_SCAN_DEPTH)) return CRC_LOC_MALFORMED;

        if (!lex_next(&L)) return CRC_LOC_MALFORMED;
        if (L.kind == TOK_RBRACE) return CRC_LOC_ABSENT;
        if (L.kind != TOK_COMMA) return CRC_LOC_MALFORMED;
        after_comma = 1;
    }
}

/* ---- Unique-key enforcement ------------------------------------------ */

/* A key name must be spelled LITERALLY -- no JSON escape sequences. This is not
 * a restriction invented here: find_crc_field() above compares the root key's
 * RAW bytes against `crc32`, so a store spelling that key with an escape
 * already fails CRC location. Codifying the rule makes raw-byte key
 * comparison exact, which is what the duplicate check below relies on, and it
 * closes the divergence where the host's json.loads decodes an escaped spelling
 * into a key the firmware would treat as unknown.
 */
static int key_is_literal(const u8 *raw, u32 cs, u32 ce)
{
    u32 i;
    for (i = cs; i < ce; i++) if (raw[i] == (u8)'\\') return 0;
    return 1;
}

/* Return 1 if the key spanning key_ptr[0..key_len) already appeared as a key in
 * the object whose body begins at body_pos, at any position BEFORE key_start.
 *
 * A repeated key is a memory-safety problem here, not a style one: the entries
 * branch of boot_entries_parse() keeps its cap counter in a block-scoped local
 * that resets on a second occurrence while out->entry_count does not, so two
 * 64-element `entries` arrays wrote past the 64-slot output array. The same
 * reset defeated the cross-occurrence duplicate-id gate.
 *
 * This RESCANS the already-walked prefix rather than accumulating a seen-key
 * table. That costs O(prefix) per key instead of O(1), but it needs no
 * per-object scratch on the firmware stack (one lexer_t, ~40 bytes, against a
 * fixed table live in every active frame) and -- the reason it is the right
 * shape -- it imposes NO ceiling on how many distinct keys an object may carry.
 * A capped table would have to hard-fail on overflow, turning otherwise-ignored
 * forward-compat extension keys into a whole-store rejection at boot, and the
 * host validator would have to mirror that limit to stay in agreement.
 * The walk is bounded by BOOT_ENTRIES_MAX_TOTAL_BYTES (16 KiB), so the worst
 * case across a whole store is quadratic in a 16 KiB input, once, before
 * ExitBootServices. No wall-clock figure is claimed here: it has not been
 * measured on the slowest supported firmware, and a number nobody measured is
 * worse than no number.
 *
 * FAIL CLOSED. A scan that cannot COMPLETE is not evidence that the key is
 * absent, and conflating the two is what made the first version of this
 * function bypassable: the skipper's depth budget is charged from a different
 * starting level here than in the authoritative walk, so a store the parser
 * accepted could exhaust it, and "could not finish" was read as "no duplicate"
 * -- silently disabling the check on exactly the input an attacker controls.
 * KEY_SCAN_FAILED is therefore a distinct answer and the callers REJECT on it.
 */
#define KEY_SCAN_ABSENT     0
#define KEY_SCAN_DUPLICATE  1
#define KEY_SCAN_FAILED     2

static int key_seen_before(const lexer_t *L, u32 body_pos, u32 key_start,
                           const u8 *key_ptr, u32 key_len)
{
    lexer_t S;
    zero_buf(&S, sizeof(S));
    S.raw = L->raw;
    S.raw_len = L->raw_len;
    S.pos = body_pos;
    while (1) {
        if (!lex_next(&S)) return KEY_SCAN_FAILED;
        if (S.kind == TOK_RBRACE) return KEY_SCAN_ABSENT;  /* object end */
        if (S.kind != TOK_STRING) return KEY_SCAN_FAILED;
        if (S.start >= key_start) return KEY_SCAN_ABSENT;  /* reached the key */
        u32 cs = S.content_start;
        u32 ce = S.content_end;
        if (!lex_next(&S) || S.kind != TOK_COLON) return KEY_SCAN_FAILED;
        if (!lex_next(&S)) return KEY_SCAN_FAILED;
        if (!skip_value_depth(&S, BOOT_ENTRIES_MAX_SCAN_DEPTH)) return KEY_SCAN_FAILED;
        if ((ce - cs) == key_len && bytes_eq(L->raw + cs, key_ptr, key_len))
            return KEY_SCAN_DUPLICATE;
        if (!lex_next(&S)) return KEY_SCAN_FAILED;
        if (S.kind == TOK_RBRACE) return KEY_SCAN_ABSENT;
        if (S.kind != TOK_COMMA) return KEY_SCAN_FAILED;
    }
}

/* Bounded append into the fixed 64-slot output array. Returns 1 on success, 0
 * when the array is full, leaving the array and the count untouched.
 *
 * Defence in depth: the cap inside the entries walk is checked against the
 * PER-OCCURRENCE index, while the write indexes out->entry_count, which spans
 * occurrences -- that gap is what a repeated `entries` key exploited. Rejecting
 * repeated keys is what makes the two agree; this bound means no later refactor
 * of that guard can reopen an out-of-bounds write.
 *
 * It is a separate function so it can be TESTED. Through the parser it is
 * unreachable by construction (the key guard fires first), and a backstop that
 * no test can fail is a backstop that can be deleted without anyone noticing.
 */
static int entries_retain(boot_entries_parse_result_t *out,
                          const boot_entry_envelope_t *e)
{
    if (out->entry_count >= BOOT_ENTRIES_MAX_ENTRIES) return 0;
    out->entries[out->entry_count] = *e;
    out->entry_count++;
    return 1;
}

/* ---- Validation helpers ---------------------------------------------- */

/* RFC 4122 UUID textual form: 8-4-4-4-12 hex digits with dashes. Total 36 chars. */
static int is_uuid_text(const u8 *s, u32 n)
{
    static const u8 GROUPS[5] = { 8u, 4u, 4u, 4u, 12u };
    if (n != 36u) return 0;
    u32 i, off = 0;
    for (i = 0; i < 5u; i++) {
        u32 g = GROUPS[i];
        u32 j;
        for (j = 0; j < g; j++) {
            u8 c = s[off + j];
            int hex = (c >= '0' && c <= '9') ||
                      (c >= 'a' && c <= 'f') ||
                      (c >= 'A' && c <= 'F');
            if (!hex) return 0;
        }
        off += g;
        if (i < 4u) {
            if (s[off] != '-') return 0;
            off++;
        }
    }
    return 1;
}

static int is_kebab_id(const u8 *s, u32 n)
{
    /* ^[a-z0-9]+(-[a-z0-9]+)*$ */
    u32 i;
    int prev_dash = 0;
    if (n == 0u) return 0;
    for (i = 0; i < n; i++) {
        u8 c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            prev_dash = 0;
        } else if (c == '-') {
            if (i == 0u || prev_dash) return 0;
            prev_dash = 1;
        } else {
            return 0;
        }
    }
    if (prev_dash) return 0;   /* trailing dash */
    return 1;
}

static u32 flag_name_to_bit(const u8 *s, u32 n)
{
    static const struct { const char *name; u32 bit; } NAMES[] = {
        { "active",            BOOT_ENTRY_FLAG_ACTIVE },
        { "hidden",            BOOT_ENTRY_FLAG_HIDDEN },
        { "trusted_chainload", BOOT_ENTRY_FLAG_TRUSTED_CHAINLOAD },
        { "hide_when_alone",   BOOT_ENTRY_FLAG_HIDE_WHEN_ALONE },
        { "allow_editor",      BOOT_ENTRY_FLAG_ALLOW_EDITOR },
    };
    u32 i;
    for (i = 0; i < sizeof(NAMES)/sizeof(NAMES[0]); i++) {
        u32 nl = s_len(NAMES[i].name);
        if (nl == n && bytes_eq(s, (const u8 *)NAMES[i].name, n))
            return NAMES[i].bit;
    }
    return 0u;   /* unknown flag -- caller logs warn + drops (forward-compat) */
}

static i32 kind_name_to_num(const u8 *s, u32 n)
{
    static const struct { const char *name; u32 num; } NAMES[] = {
        { "split",       BOOT_ENTRY_KIND_SPLIT },
        { "uki",         BOOT_ENTRY_KIND_UKI },
        { "chainload",   BOOT_ENTRY_KIND_CHAINLOAD },
        { "network",     BOOT_ENTRY_KIND_NETWORK },
        { "resume",      BOOT_ENTRY_KIND_RESUME },
        { "recovery",    BOOT_ENTRY_KIND_RECOVERY },
        { "installer",   BOOT_ENTRY_KIND_INSTALLER },
        { "safe",        BOOT_ENTRY_KIND_SAFE },
        { "diagnostics", BOOT_ENTRY_KIND_DIAGNOSTICS },
        { "test",        BOOT_ENTRY_KIND_TEST },
    };
    u32 i;
    for (i = 0; i < sizeof(NAMES)/sizeof(NAMES[0]); i++) {
        u32 nl = s_len(NAMES[i].name);
        if (nl == n && bytes_eq(s, (const u8 *)NAMES[i].name, n))
            return (i32)NAMES[i].num;
    }
    return -1;
}

/* ---- Reject helpers --------------------------------------------------- */

static void set_reject(boot_entries_parse_result_t *out,
                       boot_entries_reject_code_t code,
                       const char *msg)
{
    out->reject_code = code;
    u32 i, n = s_len(msg);
    if (n >= BOOT_ENTRIES_REJECT_MSG_LEN) n = BOOT_ENTRIES_REJECT_MSG_LEN - 1u;
    for (i = 0; i < n; i++) out->reject_msg[i] = msg[i];
    out->reject_msg[n] = '\0';
}

static void log_reject(boot_entries_log_fn log, const char *msg)
{
    if (log) {
        log("[BOOT] boot-entries: rejected: ");
        log(msg);
        log("\n");
    }
}

/* ---- Parse one entry envelope ---------------------------------------- */

/* L is sitting at the LBRACE that opens this entry. On success, advances past the
 * matching RBRACE; populates `out`. On failure, sets reject in `result`. */
static int parse_entry_object(lexer_t *L, u32 idx,
                              boot_entry_envelope_t *out,
                              boot_entries_parse_result_t *result,
                              int secure_boot_active,
                              boot_entries_log_fn log)
{
    if (L->kind != TOK_LBRACE) {
        set_reject(result, BOOT_ENTRIES_REJECT_NOT_OBJECT, "entry is not a JSON object");
        return 0;
    }

    /* Track which fields we've seen so we can require the mandatory set. The schema
     * makes payload mandatory on every entry; per-entry-kind validation owns the
     * payload contents but the envelope-layer parser requires its presence. */
    int saw_id = 0, saw_title = 0, saw_kind = 0, saw_flags = 0;
    int saw_sort_key = 0, saw_machine_id = 0, saw_policy_tags = 0, saw_payload = 0;

    /* Defaults for output. */
    zero_buf(out, sizeof(*out));
    out->payload_present = 0;
    out->kind_skipped = 0;
    out->kind = BOOT_ENTRY_KIND_SPLIT;   /* placeholder; overwritten below */
    out->timeout_override = BOOT_ENTRIES_TIMEOUT_OVERRIDE_NONE;

    /* Need a reusable buffer to compare entry IDs across iterations */
    (void)idx;   /* unused -- caller tracks index */
    (void)log;   /* per-entry skip-log moved to summary in boot_entries_parse() */

    /* Track separator state so trailing commas (`{..., "x":1, }`) are rejected
     * to match the host validator's strict json.loads grammar (RFC 8259 §5). */
    int after_comma = 0;
    /* Body of THIS entry object -- the `{` is already consumed. key_seen_before()
     * rescans from here to reject a repeated key. Without it the saw_* flags
     * below record presence only, so a second `"id"` silently overwrote the
     * first (last-wins) exactly as the root object's repeated `"entries"` did. */
    u32 entry_body_pos = L->pos;
    while (1) {
        if (!lex_next(L)) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected entry field or '}'");
            return 0;
        }
        if (L->kind == TOK_RBRACE) {
            if (after_comma) {
                set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE,
                           "trailing comma in entry object");
                return 0;
            }
            break;
        }
        if (L->kind != TOK_STRING) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected entry field name (string)");
            return 0;
        }
        /* Save the key range so we can compare BEFORE advancing the lexer */
        u32 key_cs = L->content_start;
        u32 key_ce = L->content_end;
        u32 key_tok_start = L->start;
        if (!key_is_literal(L->raw, key_cs, key_ce)) {
            set_reject(result, BOOT_ENTRIES_REJECT_ESCAPED_KEY,
                       "entry key name must not use JSON escapes");
            return 0;
        }
        int entry_scan = key_seen_before(L, entry_body_pos, key_tok_start,
                                         L->raw + key_cs, key_ce - key_cs);
        if (entry_scan == KEY_SCAN_DUPLICATE) {
            set_reject(result, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                       "repeated key in entry object");
            return 0;
        }
        if (entry_scan == KEY_SCAN_FAILED) {
            set_reject(result, BOOT_ENTRIES_REJECT_DEPTH_LIMIT,
                       "entry unique-key rescan could not complete");
            return 0;
        }
        /* Expect colon */
        if (!lex_next(L) || L->kind != TOK_COLON) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ':' after field name");
            return 0;
        }
        /* Advance to value */
        if (!lex_next(L)) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected entry field value");
            return 0;
        }
        u32 key_len = key_ce - key_cs;
        const u8 *key_ptr = L->raw + key_cs;

        /* Match key */
        if (key_len == 2u && bytes_eq(key_ptr, (const u8 *)"id", 2u)) {
            if (L->kind != TOK_STRING) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ID, "id must be a string");
                return 0;
            }
            u32 vlen = L->content_end - L->content_start;
            if (vlen < 1u || vlen > BOOT_ENTRIES_MAX_ID_LEN) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ID, "id length out of range");
                return 0;
            }
            if (!is_kebab_id(L->raw + L->content_start, vlen)) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ID, "id is not kebab-case");
                return 0;
            }
            copy_clamped(out->id, sizeof(out->id), L->raw + L->content_start, vlen);
            saw_id = 1;
        }
        else if (key_len == 5u && bytes_eq(key_ptr, (const u8 *)"title", 5u)) {
            if (L->kind != TOK_STRING) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_TITLE, "title must be a string");
                return 0;
            }
            u32 vlen = L->content_end - L->content_start;
            if (vlen < 1u || vlen > BOOT_ENTRIES_MAX_TITLE_LEN) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_TITLE, "title length out of range");
                return 0;
            }
            copy_clamped(out->title, sizeof(out->title), L->raw + L->content_start, vlen);
            saw_title = 1;
        }
        else if (key_len == 4u && bytes_eq(key_ptr, (const u8 *)"kind", 4u)) {
            if (L->kind == TOK_STRING) {
                u32 vlen = L->content_end - L->content_start;
                i32 num = kind_name_to_num(L->raw + L->content_start, vlen);
                if (num >= 0) {
                    out->kind = (u32)num;
                } else {
                    /* Unknown string kind -- skip-with-warn (forward-compat) */
                    out->kind_skipped = 1;
                    out->kind = 0xFFFFFFFFu;   /* sentinel */
                }
            } else if (L->kind == TOK_NUMBER) {
                if (!L->number_valid) {
                    set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD, "kind number invalid");
                    return 0;
                }
                u32 v = L->number_value;
                /* Stable range 0..99: reject as malformed if not a known name. */
                if (v < 100u) {
                    /* Reverse-lookup: only known stable kinds are 0..9 */
                    if (v > 9u) {
                        set_reject(result, BOOT_ENTRIES_REJECT_UNKNOWN_KIND_RANGE,
                                   "kind in stable range but not a known value");
                        return 0;
                    }
                    out->kind = v;
                } else if (v <= 199u) {
                    /* Vendor range -- skip with warn */
                    out->kind_skipped = 1;
                    out->kind = v;
                } else {
                    /* Reserved future -- skip with warn */
                    out->kind_skipped = 1;
                    out->kind = v;
                }
            } else {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                           "kind must be string or integer");
                return 0;
            }
            saw_kind = 1;
        }
        else if (key_len == 5u && bytes_eq(key_ptr, (const u8 *)"flags", 5u)) {
            if (L->kind != TOK_LBRACKET) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_FLAGS, "flags must be an array");
                return 0;
            }
            /* Strict element walk: alternates between "expect string-or-]" and
             * "expect ,-or-]". Catches `[,]`, `[a,,b]`, `[a b]` (missing comma),
             * trailing comma `[a,]`. Forward-compat: unknown flag names drop
             * with warn (bit==0). The flag is security-relevant
             * (trusted_chainload feeds the chainload Secure Boot gate) so
             * grammar must be tight. */
            u32 bits = 0;
            int expect_value = 1;   /* 1 = string or `]`; 0 = `,` or `]` */
            int saw_any = 0;
            while (1) {
                if (!lex_next(L)) {
                    set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "flags array malformed");
                    return 0;
                }
                if (L->kind == TOK_RBRACKET) {
                    /* trailing-comma reject: if we just consumed `,` we're in
                     * expect_value=1; an empty array (saw_any==0) is OK. */
                    if (saw_any && expect_value) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_FLAGS, "trailing comma in flags");
                        return 0;
                    }
                    break;
                }
                if (expect_value) {
                    if (L->kind != TOK_STRING) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_FLAGS, "flags element not a string");
                        return 0;
                    }
                    u32 flag_len = L->content_end - L->content_start;
                    u32 bit = flag_name_to_bit(L->raw + L->content_start, flag_len);
                    bits |= bit;
                    expect_value = 0;
                    saw_any = 1;
                } else {
                    if (L->kind != TOK_COMMA) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_FLAGS, "expected ',' or ']' in flags");
                        return 0;
                    }
                    expect_value = 1;
                }
            }
            out->flags = bits;
            saw_flags = 1;
        }
        else if (key_len == 8u && bytes_eq(key_ptr, (const u8 *)"sort_key", 8u)) {
            if (L->kind != TOK_STRING) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD, "sort_key must be a string");
                return 0;
            }
            u32 vlen = L->content_end - L->content_start;
            if (vlen > BOOT_ENTRIES_MAX_SORT_KEY_LEN) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                           "sort_key length out of range");
                return 0;
            }
            copy_clamped(out->sort_key, sizeof(out->sort_key), L->raw + L->content_start, vlen);
            saw_sort_key = 1;
        }
        else if (key_len == 10u && bytes_eq(key_ptr, (const u8 *)"machine_id", 10u)) {
            if (L->kind != TOK_STRING) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD, "machine_id must be a string");
                return 0;
            }
            u32 vlen = L->content_end - L->content_start;
            /* Empty machine_id is the explicit "match any machine" wildcard
             * (the boot-policy filter treats e->machine_id[0]=='\0' as
             * universal). Non-empty values must be RFC 4122 textual UUID
             * form; the policy filter compares exact 36-char strings. */
            if (vlen > 0u && !is_uuid_text(L->raw + L->content_start, vlen)) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                           "machine_id must be empty or RFC 4122 UUID textual form");
                return 0;
            }
            copy_clamped(out->machine_id, sizeof(out->machine_id), L->raw + L->content_start, vlen);
            saw_machine_id = 1;
        }
        else if (key_len == 11u && bytes_eq(key_ptr, (const u8 *)"policy_tags", 11u)) {
            if (L->kind != TOK_LBRACKET) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD, "policy_tags must be an array");
                return 0;
            }
            /* Strict element walk: each element must be a string; alternating
             * expect_value / expect_comma states catch [a,], [a,,b], [a b]. */
            int expect_value = 1;
            int saw_any = 0;
            while (1) {
                if (!lex_next(L)) {
                    set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "policy_tags array malformed");
                    return 0;
                }
                if (L->kind == TOK_RBRACKET) {
                    if (saw_any && expect_value) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "trailing comma in policy_tags");
                        return 0;
                    }
                    break;
                }
                if (expect_value) {
                    if (L->kind != TOK_STRING) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "policy_tags element must be a string");
                        return 0;
                    }
                    u32 tlen = L->content_end - L->content_start;
                    if (tlen > BOOT_ENTRIES_MAX_POLICY_TAG_LEN) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "policy_tags element exceeds 23-char cap");
                        return 0;
                    }
                    if (out->policy_tag_count < BOOT_ENTRIES_MAX_POLICY_TAGS) {
                        copy_clamped(out->policy_tags[out->policy_tag_count],
                                     BOOT_ENTRIES_MAX_POLICY_TAG_LEN + 1u,
                                     L->raw + L->content_start, tlen);
                        out->policy_tag_count++;
                    } else {
                        out->policy_tag_overflow = 1;
                    }
                    expect_value = 0;
                    saw_any = 1;
                } else {
                    if (L->kind != TOK_COMMA) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "expected ',' or ']' in policy_tags");
                        return 0;
                    }
                    expect_value = 1;
                }
            }
            saw_policy_tags = 1;
        }
        else if (key_len == 19u && bytes_eq(key_ptr, (const u8 *)"health_check_subset", 19u)) {
            /* Optional per-entry health-gate override. Array of
             * up to BOOT_ENTRIES_HEALTH_SUBSET_MAX_NAMES strings naming
             * health checks to run. Empty / absent -> kernel runs the
             * full default check set. Cap-exceeded -> hard reject (no
             * silent truncation; the user's intent could be a
             * lock-out-everything-else override). */
            if (L->kind != TOK_LBRACKET) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                           "health_check_subset must be an array");
                return 0;
            }
            int expect_value = 1;
            int saw_any = 0;
            while (1) {
                if (!lex_next(L)) {
                    set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE,
                               "health_check_subset array malformed");
                    return 0;
                }
                if (L->kind == TOK_RBRACKET) {
                    if (saw_any && expect_value) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "trailing comma in health_check_subset");
                        return 0;
                    }
                    break;
                }
                if (expect_value) {
                    if (L->kind != TOK_STRING) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "health_check_subset element must be a string");
                        return 0;
                    }
                    u32 nlen = L->content_end - L->content_start;
                    if (nlen == 0u
                        || nlen >= BOOT_ENTRIES_HEALTH_SUBSET_NAME_LEN) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "health_check_subset element length out of range");
                        return 0;
                    }
                    /* Printable-ASCII grammar (matches validate.py) so
                     * the kernel can pass names straight to the
                     * registry without re-sanitising. Reject backslash
                     * + double-quote (would have been escape sequences
                     * the lexer already decoded; their unescaped form
                     * is forbidden in registry names). */
                    {
                        const u8 *vp = L->raw + L->content_start;
                        for (u32 vi = 0; vi < nlen; vi++) {
                            u8 c = vp[vi];
                            if (c < 0x20u || c > 0x7Eu
                                || c == (u8)'\\' || c == (u8)'"') {
                                set_reject(result,
                                    BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                    "health_check_subset element must be printable ASCII without backslash or quote");
                                return 0;
                            }
                        }
                    }
                    if (out->health_check_subset_count
                        >= BOOT_ENTRIES_HEALTH_SUBSET_MAX_NAMES) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "health_check_subset exceeds 8-name cap");
                        return 0;
                    }
                    copy_clamped(out->health_check_subset[out->health_check_subset_count],
                                 BOOT_ENTRIES_HEALTH_SUBSET_NAME_LEN,
                                 L->raw + L->content_start, nlen);
                    out->health_check_subset_count++;
                    expect_value = 0;
                    saw_any = 1;
                } else {
                    if (L->kind != TOK_COMMA) {
                        set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                                   "expected ',' or ']' in health_check_subset");
                        return 0;
                    }
                    expect_value = 1;
                }
            }
        }
        else if (key_len == 7u && bytes_eq(key_ptr, (const u8 *)"payload", 7u)) {
            if (L->kind != TOK_LBRACE) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD, "payload must be an object");
                return 0;
            }
            out->payload_offset = L->start;
            if (!skip_value_post_token(L)) {
                set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "payload object malformed or too deep");
                return 0;
            }
            out->payload_length = L->end - out->payload_offset;
            out->payload_present = 1;
            saw_payload = 1;
        }
        else if (key_len == 16u && bytes_eq(key_ptr, (const u8 *)"timeout_override", 16u)) {
            if (L->kind != TOK_NUMBER || !L->number_valid || L->number_value > 600u) {
                set_reject(result, BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD,
                           "timeout_override must be int in 0..600");
                return 0;
            }
            out->timeout_override = (u32)L->number_value;
        }
        else {
            /* Unknown envelope key -- skip the value (forward-compat) */
            if (!skip_value_post_token(L)) {
                set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "unknown field value malformed");
                return 0;
            }
        }

        /* After the value, expect ',' or '}' */
        if (!lex_next(L)) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ',' or '}' after entry value");
            return 0;
        }
        if (L->kind == TOK_RBRACE) break;
        if (L->kind != TOK_COMMA) {
            set_reject(result, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ',' between entry fields");
            return 0;
        }
        after_comma = 1;
    }

    /* Required fields. payload is mandatory in the schema even though per-entry-kind
     * validation is deferred -- a missing payload means the per-kind handler has no
     * data to operate on, which is a malformed entry, not an "optional field". */
    if (!(saw_id && saw_title && saw_kind && saw_flags && saw_sort_key && saw_machine_id
          && saw_policy_tags && saw_payload)) {
        set_reject(result, BOOT_ENTRIES_REJECT_MISSING_FIELD, "entry missing required envelope fields");
        return 0;
    }

    /* Path-escape policy moved to the boot-policy filter (per-entry demote
     * instead of whole-store reject). The parser passes untrusted chainload
     * entries through; boot_policy_decide() records BOOT_REJECT_REASON_PATH_ESCAPE
     * per entry under Secure Boot so other viable entries still get a chance.
     * Suppresses an unused-parameter warning when the conditional is gone: */
    (void)secure_boot_active;

    /* Per-skipped-entry logging is deferred to a summary line in
     * boot_entries_parse() so a hostile 64-skipped store cannot dominate
     * the boot serial path with thousands of bytes (perf review finding).
     */
    return 1;
}

/* ---- Top-level parse -------------------------------------------------- */

int boot_entries_parse(const unsigned char *raw, unsigned int raw_len,
                       int secure_boot_active,
                       boot_entries_log_fn log,
                       boot_entries_parse_result_t *out)
{
    /* FIRST statement: the CRC readiness flag lives in unzeroed .bss (see the
     * cookie comment above) and each early return below would otherwise leave
     * it holding firmware poison for the next call. */
    crc32_reset();

    /* Initialize */
    zero_buf(out, sizeof(*out));
    out->reject_code = BOOT_ENTRIES_OK;

    if (!raw || raw_len == 0u) {
        set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "empty or NULL input");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    /* Size cap */
    if (raw_len > BOOT_ENTRIES_MAX_TOTAL_BYTES) {
        set_reject(out, BOOT_ENTRIES_REJECT_FILE_TOO_LARGE, "file size exceeds 16 KiB cap");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    /* Locate the root object's crc32 member early -- needed for verification
     * BEFORE the structure is trusted. The locate is structural, so it tells a
     * broken root object apart from a store that simply carries no usable
     * header, and the two get different reject codes. */
    u32 zero_off, expected_crc;
    crc_loc_t loc = find_crc_field(raw, raw_len, &zero_off, &expected_crc);
    if (loc == CRC_LOC_MALFORMED) {
        set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "malformed JSON before the crc32 header");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }
    if (loc != CRC_LOC_FOUND) {
        set_reject(out, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD,
                   "no root crc32 member of the form \"0xHHHHHHHH\"");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }
    out->header_crc = expected_crc;
    out->computed_crc = crc32_zeroed(raw, raw_len, zero_off);
    if (out->computed_crc != out->header_crc) {
        set_reject(out, BOOT_ENTRIES_REJECT_CRC_MISMATCH,
                   "CRC mismatch (file bytes diverged from stored CRC)");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    /* Now parse the structure. */
    lexer_t L;
    lex_init(&L, raw, raw_len);

    /* Top-level: { schema_version, crc32, entries } -- walk fields */
    if (!lex_next(&L) || L.kind != TOK_LBRACE) {
        set_reject(out, BOOT_ENTRIES_REJECT_NOT_OBJECT, "top-level not an object");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    int saw_sv = 0, saw_crc = 0, saw_entries = 0;
    /* Reject trailing comma at root (`{..., "entries":[...], }`) for parity
     * with the host validator's strict json.loads grammar (RFC 8259 §5). */
    int top_after_comma = 0;
    /* Body of the root object -- the `{` is already consumed. */
    u32 root_body_pos = L.pos;
    while (1) {
        if (!lex_next(&L)) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected top-level field or '}'");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        if (L.kind == TOK_RBRACE) {
            if (top_after_comma) {
                set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE,
                           "trailing comma at top-level object");
                log_reject(log, out->reject_msg);
                return out->reject_code;
            }
            break;
        }
        if (L.kind != TOK_STRING) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected top-level field name");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        u32 key_cs = L.content_start, key_ce = L.content_end;
        u32 key_tok_start = L.start;
        if (!key_is_literal(L.raw, key_cs, key_ce)) {
            set_reject(out, BOOT_ENTRIES_REJECT_ESCAPED_KEY,
                       "top-level key name must not use JSON escapes");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        /* Reject a REPEATED top-level key. `entries` is the one that corrupts
         * memory (see key_seen_before), but the rule is applied to every key
         * because the block-scoped reset also re-armed the duplicate-id gate and
         * schema_version / crc32 were equally re-assignable. */
        int root_scan = key_seen_before(&L, root_body_pos, key_tok_start,
                                        L.raw + key_cs, key_ce - key_cs);
        if (root_scan == KEY_SCAN_DUPLICATE) {
            set_reject(out, BOOT_ENTRIES_REJECT_DUPLICATE_KEY,
                       "repeated top-level key");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        if (root_scan == KEY_SCAN_FAILED) {
            set_reject(out, BOOT_ENTRIES_REJECT_DEPTH_LIMIT,
                       "top-level unique-key rescan could not complete");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        if (!lex_next(&L) || L.kind != TOK_COLON) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ':' after top-level field");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        if (!lex_next(&L)) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected top-level field value");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        u32 key_len = key_ce - key_cs;
        const u8 *key_ptr = L.raw + key_cs;

        if (key_len == 14u && bytes_eq(key_ptr, (const u8 *)"schema_version", 14u)) {
            if (L.kind != TOK_NUMBER || !L.number_valid || L.number_value != BOOT_ENTRIES_SCHEMA_VERSION) {
                set_reject(out, BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION,
                           "schema_version must equal 1");
                log_reject(log, out->reject_msg);
                return out->reject_code;
            }
            saw_sv = 1;
        }
        else if (key_len == 5u && bytes_eq(key_ptr, (const u8 *)"crc32", 5u)) {
            if (L.kind != TOK_STRING) {
                set_reject(out, BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD, "crc32 must be a string");
                log_reject(log, out->reject_msg);
                return out->reject_code;
            }
            saw_crc = 1;
        }
        else if (key_len == 7u && bytes_eq(key_ptr, (const u8 *)"entries", 7u)) {
            if (L.kind != TOK_LBRACKET) {
                set_reject(out, BOOT_ENTRIES_REJECT_NOT_ARRAY, "entries must be an array");
                log_reject(log, out->reject_msg);
                return out->reject_code;
            }
            /* Strict alternating state machine for entries-array separators:
             * after `[`, expect object or `]` (empty array OK); after object,
             * expect `,` or `]`; after `,`, expect object. Catches `[,]`,
             * `[a,,b]`, missing commas, trailing commas. */
            int entries_expect_value = 1;
            int entries_saw_any = 0;
            u32 idx = 0;
            /* Track ALL parsed IDs (retained + skipped) so duplicate detection
             * matches the host validator's seen_ids semantics. Width is the
             * id-grammar cap + NUL; cached lengths avoid an O(n) rescan per
             * compare. Frame stays under one 4 KiB page including the live
             * envelope, lexer, and parser locals. */
            char all_ids[BOOT_ENTRIES_MAX_ENTRIES][BOOT_ENTRIES_MAX_ID_LEN + 1u];
            u8 all_id_lens[BOOT_ENTRIES_MAX_ENTRIES];
            u32 all_ids_count = 0;
            while (1) {
                if (!lex_next(&L)) {
                    set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "entries array malformed");
                    log_reject(log, out->reject_msg);
                    return out->reject_code;
                }
                if (L.kind == TOK_RBRACKET) {
                    if (entries_saw_any && entries_expect_value) {
                        set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE,
                                   "trailing comma in entries");
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    break;
                }
                if (entries_expect_value) {
                    if (L.kind != TOK_LBRACE) {
                        set_reject(out, BOOT_ENTRIES_REJECT_NOT_OBJECT,
                                   "entries element not an object");
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    if (idx >= BOOT_ENTRIES_MAX_ENTRIES) {
                        set_reject(out, BOOT_ENTRIES_REJECT_TOO_MANY_ENTRIES,
                                   "entries array exceeds 64-entry cap");
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    boot_entry_envelope_t tmp;
                    if (!parse_entry_object(&L, idx, &tmp, out, secure_boot_active, log)) {
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    /* Duplicate-id check covers BOTH retained and skipped entries. */
                    u32 j, tlen = s_len(tmp.id);
                    for (j = 0; j < all_ids_count; j++) {
                        if ((u32)all_id_lens[j] == tlen &&
                            bytes_eq((const u8 *)all_ids[j], (const u8 *)tmp.id, tlen)) {
                            set_reject(out, BOOT_ENTRIES_REJECT_DUPLICATE_ID,
                                       "duplicate entry id");
                            log_reject(log, out->reject_msg);
                            return out->reject_code;
                        }
                    }
                    if (all_ids_count < BOOT_ENTRIES_MAX_ENTRIES) {
                        u32 k, n = tlen < BOOT_ENTRIES_MAX_ID_LEN ? tlen : BOOT_ENTRIES_MAX_ID_LEN;
                        for (k = 0; k < n; k++) all_ids[all_ids_count][k] = tmp.id[k];
                        all_ids[all_ids_count][n] = '\0';
                        all_id_lens[all_ids_count] = (u8)n;
                        all_ids_count++;
                    }
                    if (tmp.kind_skipped) {
                        out->skipped_count++;
                    } else if (!entries_retain(out, &tmp)) {
                        set_reject(out, BOOT_ENTRIES_REJECT_TOO_MANY_ENTRIES,
                                   "retained entries exceed 64-entry cap");
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    idx++;
                    entries_expect_value = 0;
                    entries_saw_any = 1;
                } else {
                    if (L.kind != TOK_COMMA) {
                        set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE,
                                   "expected ',' or ']' in entries");
                        log_reject(log, out->reject_msg);
                        return out->reject_code;
                    }
                    entries_expect_value = 1;
                }
            }
            saw_entries = 1;
        }
        else {
            /* Unknown top-level key -- skip the value */
            if (!skip_value_post_token(&L)) {
                set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "unknown top-level value malformed");
                log_reject(log, out->reject_msg);
                return out->reject_code;
            }
        }

        /* After value, expect ',' or '}' */
        if (!lex_next(&L)) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ',' or '}' at top level");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        if (L.kind == TOK_RBRACE) break;
        if (L.kind != TOK_COMMA) {
            set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE, "expected ',' between top-level fields");
            log_reject(log, out->reject_msg);
            return out->reject_code;
        }
        top_after_comma = 1;
    }

    if (!(saw_sv && saw_crc && saw_entries)) {
        set_reject(out, BOOT_ENTRIES_REJECT_MISSING_FIELD,
                   "missing schema_version / crc32 / entries");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    /* After the root `}` we must see only whitespace then EOF. CRC-correct files
     * with trailing garbage (e.g. a second top-level object) are malformed. */
    if (!lex_next(&L) || L.kind != TOK_EOF) {
        set_reject(out, BOOT_ENTRIES_REJECT_JSON_PARSE,
                   "trailing content after root object");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    if (out->entry_count == 0u && out->skipped_count == 0u) {
        set_reject(out, BOOT_ENTRIES_REJECT_NO_ENTRIES, "entries array is empty");
        log_reject(log, out->reject_msg);
        return out->reject_code;
    }

    if (log) {
        /* One bounded summary line: "[BOOT] boot-entries: parsed N entries (M skipped)\n"
         * where N <= 64 and M <= 64. Replaces the prior per-skipped log to keep
         * boot serial output bounded under hostile input (perf-review fix). */
        char buf[80];
        u32 i, n = 0;
        const char *prefix = "[BOOT] boot-entries: parsed ";
        for (i = 0; prefix[i]; i++) buf[n++] = prefix[i];
        u32 ec = out->entry_count;
        if (ec >= 10u) { buf[n++] = (char)('0' + ec / 10u); ec %= 10u; }
        buf[n++] = (char)('0' + ec);
        const char *mid = " entries (";
        for (i = 0; mid[i]; i++) buf[n++] = mid[i];
        u32 sc = out->skipped_count;
        if (sc >= 10u) { buf[n++] = (char)('0' + sc / 10u); sc %= 10u; }
        buf[n++] = (char)('0' + sc);
        const char *suffix = " skipped)\n";
        for (i = 0; suffix[i]; i++) buf[n++] = suffix[i];
        buf[n] = '\0';
        log(buf);
    }

    return BOOT_ENTRIES_OK;
}

/* ---- Fallback synth --------------------------------------------------- */

void boot_entries_synthesize_fallback(int uki_mode, boot_entry_envelope_t *out)
{
    zero_buf(out, sizeof(*out));
    /* id = "fallback" */
    {
        const char *id = "fallback";
        u32 i;
        for (i = 0; id[i] && i < sizeof(out->id) - 1u; i++) out->id[i] = id[i];
        out->id[i] = '\0';
    }
    /* title = "Impossible OS (fallback)" */
    {
        const char *t = "Impossible OS (fallback)";
        u32 i;
        for (i = 0; t[i] && i < sizeof(out->title) - 1u; i++) out->title[i] = t[i];
        out->title[i] = '\0';
    }
    out->kind = uki_mode ? BOOT_ENTRY_KIND_UKI : BOOT_ENTRY_KIND_SPLIT;
    out->flags = BOOT_ENTRY_FLAG_ACTIVE;
    out->payload_present = 0;
    out->kind_skipped = 0;
}

/* ---- BLS display order ------------------------------------------------ */

/* NUL-terminated string compare (no <string.h> in this TU). Returns <0/0/>0
 * like strcmp. Local to the BLS comparator; the parser's own scans use
 * length-bounded byte compares instead. */
static int boot_entry_str_cmp(const char *a, const char *b)
{
    u32 k = 0;
    while (a[k] == b[k] && a[k] != 0) k++;
    return (int)(unsigned char)a[k] - (int)(unsigned char)b[k];
}

int boot_entry_bls_less(const boot_entry_envelope_t *a,
                        const boot_entry_envelope_t *b)
{
    int c = boot_entry_str_cmp(a->sort_key, b->sort_key);
    if (c != 0) return c < 0;
    c = boot_entry_str_cmp(a->machine_id, b->machine_id);
    if (c != 0) return c < 0;
    return boot_entry_str_cmp(a->id, b->id) < 0;
}

void boot_entries_bls_sort(const boot_entries_parse_result_t *parse,
                           unsigned int *idx, unsigned int n)
{
    for (unsigned int i = 1; i < n; i++) {
        unsigned int v = idx[i];
        int j = (int)i - 1;
        while (j >= 0 &&
               boot_entry_bls_less(&parse->entries[v],
                                   &parse->entries[idx[j]])) {
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = v;
    }
}

/* ---- Reject vocabulary ------------------------------------------------
 * Two views of the same boot_entries_reject_code_t: a stable token for
 * serial/log correlation, and a one-line human cause for the on-screen
 * rejected-store notice in the bootloader error-recovery roadmap. Kept
 * beside the enum they
 * describe so a new reject code cannot be added without the compiler's
 * -Wswitch pointing here; the default arms exist for a code arriving from
 * an out-of-contract caller, not to excuse an unhandled enumerator.
 */
const char *boot_entries_reject_name(int code)
{
    switch ((boot_entries_reject_code_t)code) {
        case BOOT_ENTRIES_OK:                        return "OK";
        case BOOT_ENTRIES_REJECT_FILE_TOO_LARGE:     return "FILE_TOO_LARGE";
        case BOOT_ENTRIES_REJECT_JSON_PARSE:         return "JSON_PARSE";
        case BOOT_ENTRIES_REJECT_DEPTH_LIMIT:        return "DEPTH_LIMIT";
        case BOOT_ENTRIES_REJECT_NOT_OBJECT:         return "NOT_OBJECT";
        case BOOT_ENTRIES_REJECT_MISSING_FIELD:      return "MISSING_FIELD";
        case BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION: return "BAD_SCHEMA_VERSION";
        case BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD:    return "BAD_CRC32_FIELD";
        case BOOT_ENTRIES_REJECT_CRC_MISMATCH:       return "CRC_MISMATCH";
        case BOOT_ENTRIES_REJECT_NOT_ARRAY:          return "NOT_ARRAY";
        case BOOT_ENTRIES_REJECT_NO_ENTRIES:         return "NO_ENTRIES";
        case BOOT_ENTRIES_REJECT_TOO_MANY_ENTRIES:   return "TOO_MANY_ENTRIES";
        case BOOT_ENTRIES_REJECT_DUPLICATE_ID:       return "DUPLICATE_ID";
        case BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD: return "BAD_ENVELOPE_FIELD";
        case BOOT_ENTRIES_REJECT_BAD_ID:             return "BAD_ID";
        case BOOT_ENTRIES_REJECT_BAD_TITLE:          return "BAD_TITLE";
        case BOOT_ENTRIES_REJECT_BAD_FLAGS:          return "BAD_FLAGS";
        case BOOT_ENTRIES_REJECT_UNKNOWN_KIND_RANGE: return "UNKNOWN_KIND_RANGE";
        case BOOT_ENTRIES_REJECT_PATH_ESCAPE:        return "PATH_ESCAPE";
        case BOOT_ENTRIES_REJECT_INTERNAL:           return "INTERNAL";
        case BOOT_ENTRIES_REJECT_DUPLICATE_KEY:      return "DUPLICATE_KEY";
        case BOOT_ENTRIES_REJECT_ESCAPED_KEY:        return "ESCAPED_KEY";
    }
    return "UNKNOWN";
}

const char *boot_entries_reject_cause(int code)
{
    switch ((boot_entries_reject_code_t)code) {
        case BOOT_ENTRIES_OK:
            return "the store was accepted";
        case BOOT_ENTRIES_REJECT_FILE_TOO_LARGE:
            return "the store file is larger than the 16 KiB limit";
        case BOOT_ENTRIES_REJECT_JSON_PARSE:
            return "the store is not valid JSON";
        case BOOT_ENTRIES_REJECT_DEPTH_LIMIT:
            return "the store nests objects too deeply";
        case BOOT_ENTRIES_REJECT_NOT_OBJECT:
            return "the store's top level is not a JSON object";
        case BOOT_ENTRIES_REJECT_MISSING_FIELD:
            return "a required field is missing from the store";
        case BOOT_ENTRIES_REJECT_BAD_SCHEMA_VERSION:
            return "the store's schema version is not supported";
        case BOOT_ENTRIES_REJECT_BAD_CRC32_FIELD:
            return "the store's checksum field is malformed";
        case BOOT_ENTRIES_REJECT_CRC_MISMATCH:
            return "the store's checksum does not match its contents";
        case BOOT_ENTRIES_REJECT_NOT_ARRAY:
            return "the store's entry list is not a JSON array";
        case BOOT_ENTRIES_REJECT_NO_ENTRIES:
            return "the store contains no boot entries";
        case BOOT_ENTRIES_REJECT_TOO_MANY_ENTRIES:
            return "the store contains more entries than the limit allows";
        case BOOT_ENTRIES_REJECT_DUPLICATE_ID:
            return "two boot entries share the same id";
        case BOOT_ENTRIES_REJECT_BAD_ENVELOPE_FIELD:
            return "a boot entry field has the wrong type";
        case BOOT_ENTRIES_REJECT_BAD_ID:
            return "a boot entry id is empty or malformed";
        case BOOT_ENTRIES_REJECT_BAD_TITLE:
            return "a boot entry title is empty or malformed";
        case BOOT_ENTRIES_REJECT_BAD_FLAGS:
            return "a boot entry flags value is out of range";
        case BOOT_ENTRIES_REJECT_UNKNOWN_KIND_RANGE:
            return "a boot entry uses an unrecognised entry kind";
        case BOOT_ENTRIES_REJECT_PATH_ESCAPE:
            return "a boot entry path points outside the boot partition";
        case BOOT_ENTRIES_REJECT_INTERNAL:
            return "the store parser hit an internal limit";
        case BOOT_ENTRIES_REJECT_DUPLICATE_KEY:
            return "a JSON object in the store repeats a key";
        case BOOT_ENTRIES_REJECT_ESCAPED_KEY:
            return "a JSON key in the store is spelled with an escape";
    }
    return "the store was rejected for an unrecognised reason";
}

int boot_entries_store_notice_warranted(int load_status, int reject_code)
{
    switch ((boot_store_load_status_t)load_status) {
        case BOOT_STORE_LOAD_ABSENT:
            /* No store on this machine. Normal on a fresh install and on
             * every dev/smoke image, so it is never a notice -- this arm
             * is the whole reason the load status is tri-state rather than
             * a bare "did the parse succeed" boolean. */
            return 0;
        case BOOT_STORE_LOAD_UNREADABLE:
            /* A store is there (or the volume itself failed) and we could
             * not use it. The parser never ran, so reject_code carries
             * only the caller's synthesized placeholder and is not
             * consulted here. */
            return 1;
        case BOOT_STORE_LOAD_OK:
            /* The bytes were read, so the parser's verdict decides. */
            return reject_code != (int)BOOT_ENTRIES_OK;
    }
    /* Out-of-contract status: stay quiet rather than cry wolf on a healthy
     * machine. The serial line still records the raw values. */
    return 0;
}
