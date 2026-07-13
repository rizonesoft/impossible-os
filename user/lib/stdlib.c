/* ============================================================================
 * stdlib.c -- Standard library functions
 * ============================================================================ */

#include "stdlib.h"
#include "syscall.h"

/* --- String-to-integer --- */

int atoi(const char *s)
{
    return (int)atol(s);
}

long atol(const char *s)
{
    long result = 0;
    int sign = 1;

    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;

    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return result * sign;
}

/* --- Integer-to-string --- */

static void reverse(char *s, int len)
{
    int i = 0, j = len - 1;
    char tmp;
    while (i < j) {
        tmp = s[i];
        s[i] = s[j];
        s[j] = tmp;
        i++;
        j--;
    }
}

char *itoa(int value, char *buf, int base)
{
    return ltoa((long)value, buf, base);
}

char *ltoa(long value, char *buf, int base)
{
    int i = 0;
    int negative = 0;
    unsigned long uval;

    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    if (value < 0 && base == 10) {
        negative = 1;
        uval = (unsigned long)(-(value + 1)) + 1;
    } else {
        uval = (unsigned long)value;
    }

    while (uval > 0) {
        unsigned long rem = uval % (unsigned long)base;
        buf[i++] = (rem < 10) ? (char)('0' + rem) : (char)('a' + rem - 10);
        uval /= (unsigned long)base;
    }

    if (negative)
        buf[i++] = '-';

    buf[i] = '\0';
    reverse(buf, i);
    return buf;
}

char *utoa(unsigned int value, char *buf, int base)
{
    int i = 0;

    if (base < 2 || base > 36) {
        buf[0] = '\0';
        return buf;
    }

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }

    while (value > 0) {
        unsigned int rem = value % (unsigned int)base;
        buf[i++] = (rem < 10) ? (char)('0' + rem) : (char)('a' + rem - 10);
        value /= (unsigned int)base;
    }

    buf[i] = '\0';
    reverse(buf, i);
    return buf;
}

/* --- Absolute value --- */

int abs(int n)
{
    return n < 0 ? -n : n;
}

long labs(long n)
{
    return n < 0 ? -n : n;
}

/* --- Memory allocation ---
 * Simple bump allocator with a static 64 KiB arena.
 * free() is a no-op (no reclamation). Sufficient for simple programs. */

#define HEAP_SIZE (64 * 1024)   /* 64 KiB */

static char heap_arena[HEAP_SIZE];
static size_t heap_offset = 0;

void *malloc(size_t size)
{
    size_t aligned;
    void *ptr;

    if (size == 0)
        return NULL;

    /* Align to 16 bytes */
    aligned = (size + 15) & ~15UL;

    if (heap_offset + aligned > HEAP_SIZE)
        return NULL;

    ptr = &heap_arena[heap_offset];
    heap_offset += aligned;

    return ptr;
}

void free(void *ptr)
{
    /* No-op: bump allocator doesn't reclaim memory */
    (void)ptr;
}

/* --- Command-line tokenizer (Windows quoting) --- */

int cmd_tokenize(char *line, char **argv, int max_argc)
{
    int argc = 0;
    char *r = line;   /* read cursor */

    if (!line || !argv || max_argc < 1) {
        if (argv && max_argc >= 1)
            argv[0] = (char *)0;
        return 0;
    }

    for (;;) {
        char *w;      /* write cursor for the compacted token */
        int in_quotes = 0;

        /* Skip inter-token whitespace. */
        while (*r == ' ' || *r == '\t')
            r++;
        if (*r == '\0')
            break;

        /* Cap: leave room for the NULL terminator. Tokens beyond the cap are
         * dropped; the caller warns. */
        if (argc >= max_argc - 1)
            break;

        w = r;
        argv[argc++] = w;

        while (*r) {
            if ((*r == ' ' || *r == '\t') && !in_quotes)
                break;

            if (*r == '\\') {
                /* Count the backslash run and peek at what follows it. */
                int nbs = 0;
                while (*r == '\\') { nbs++; r++; }
                if (*r == '"') {
                    int k;
                    for (k = 0; k < nbs / 2; k++) *w++ = '\\';
                    if (nbs & 1) {
                        *w++ = '"';          /* 2n+1: escaped literal quote */
                        r++;
                    } else {
                        in_quotes = !in_quotes;  /* 2n: quote toggles */
                        r++;
                    }
                } else {
                    int k;
                    for (k = 0; k < nbs; k++) *w++ = '\\';
                }
                continue;
            }

            if (*r == '"') {
                if (in_quotes && r[1] == '"') {
                    *w++ = '"';              /* "" inside quotes -> literal quote */
                    r += 2;
                } else {
                    in_quotes = !in_quotes;
                    r++;
                }
                continue;
            }

            *w++ = *r++;
        }

        /* NUL-terminate the compacted token. If the token ended on whitespace,
         * advance past it; if on end-of-string, leave r at the NUL. */
        if (*r) {
            r++;         /* consume the separating whitespace */
            *w = '\0';
        } else {
            *w = '\0';
        }
    }

    argv[argc] = (char *)0;
    return argc;
}

/* --- Process control --- */

void exit(int status)
{
    sys_exit(status);
}
