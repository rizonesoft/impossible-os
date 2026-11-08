/* boot_store_reject_text.h -- the words for a rejected boot entry store.
 *
 * Header-only because two images need the same text and only one links the
 * parser: BOOTX64 carries boot_entries_parser.c, and the kernel carries it only
 * inside its test suite, which the release flavor compiles out. Keeping one copy
 * here is what makes the kernel's log line read the same as the loader's notice.
 * Freestanding: no kernel or UEFI types. */
#ifndef BOOT_STORE_REJECT_TEXT_H
#define BOOT_STORE_REJECT_TEXT_H

#include "boot_entries_parser.h"

#define BSR_TEXT_FN static __attribute__((unused))

/* A stored record's line and column are untrusted u32 values, so a token
 * rendered from NVRAM needs room for the longest name, both labels and two
 * 10-digit numbers: 18 + 9 (" at line ") + 10 + 6 (", col ") + 10 + NUL.
 * test_store_reject_token_bound walks every reject code against the name cap. */
#define BOOT_STORE_REJECT_NAME_MAX  18u
#define BOOT_STORE_REJECT_TOKEN_LEN (BOOT_STORE_REJECT_NAME_MAX + 9u + 10u + 6u + 10u + 1u)

BSR_TEXT_FN const char *bsr_reject_name(int code)
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

BSR_TEXT_FN const char *bsr_reject_cause(int code)
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

BSR_TEXT_FN unsigned int bsr_put_text(char *buf, unsigned int cap, unsigned int at, const char *text)
{
    while (*text && at + 1u < cap) buf[at++] = *text++;
    return at;
}

BSR_TEXT_FN unsigned int bsr_put_number(char *buf, unsigned int cap, unsigned int at, unsigned int value)
{
    char digits[10];
    unsigned int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value);
    while (n && at + 1u < cap) buf[at++] = digits[--n];
    return at;
}

BSR_TEXT_FN void bsr_put_locator(char *buf, unsigned int cap, int code,
                                 unsigned int line, unsigned int column)
{
    if (!buf || cap == 0u)
        return;
    unsigned int at = bsr_put_text(buf, cap, 0u, bsr_reject_name(code));
    if (line != 0u) {
        at = bsr_put_text(buf, cap, at, " at line ");
        at = bsr_put_number(buf, cap, at, line);
        at = bsr_put_text(buf, cap, at, ", col ");
        at = bsr_put_number(buf, cap, at, column);
    }
    buf[at] = '\0';
}

/* Render a stored record for the running OS: `token` gets the same text the
 * notice carried and `*cause` the sentence. The variable lives under a public
 * GUID, so a value no writer would produce returns 0. */
BSR_TEXT_FN int boot_store_reject_describe(const boot_store_reject_record_t *r,
                                           char *token, unsigned int cap, const char **cause)
{
    if (!r || !token || cap == 0u || !cause || r->err_code != BOOT_STORE_REJECT_ERR_CODE)
        return 0;
    if (r->load_status == (unsigned short)BOOT_STORE_LOAD_UNREADABLE) {
        token[bsr_put_text(token, cap, 0u, "STORE_UNREADABLE")] = '\0';
        *cause = "the boot entry store could not be read";
        return 1;
    }
    if (r->load_status == BOOT_STORE_REJECT_LOAD_ALLOC_FAILED) {
        token[bsr_put_text(token, cap, 0u, "POLICY_ALLOC_EXHAUSTED")] = '\0';
        *cause = "the firmware ran out of memory reading boot settings";
        return 1;
    }
    if (r->load_status != (unsigned short)BOOT_STORE_LOAD_OK ||
        r->reject_code == (unsigned short)BOOT_ENTRIES_OK)
        return 0;
    /* Only the lookup's own fall-through is named UNKNOWN. */
    const char *name = bsr_reject_name((int)r->reject_code);
    const char *unknown = "UNKNOWN";
    while (*name && *name == *unknown) { name++; unknown++; }
    if (*name == *unknown)
        return 0;
    bsr_put_locator(token, cap, (int)r->reject_code, r->line, r->column);
    *cause = bsr_reject_cause((int)r->reject_code);
    return 1;
}

#endif /* BOOT_STORE_REJECT_TEXT_H */
