/* ============================================================================
 * shell.c — Impossible OS Command-Line Shell
 *
 * User-mode REPL with line editing, command parsing, and built-in commands.
 * Linked against libc.a.
 * ============================================================================ */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "ctype.h"
#include "syscall.h"

/* --- Constants --- */
#define LINE_MAX    256
#define ARGV_MAX    32
#define PROMPT      "impossible> "

/* --- ANSI escape sequences --- */
#define ESC_CLEAR   "\033[2J\033[H"    /* Clear screen + cursor home */

/* --- Line editing ---
 * Reads a line from stdin with backspace support.
 * Returns the number of characters in the line (excluding '\0'). */
static int readline(char *buf, int max)
{
    int pos = 0;
    char c;

    while (pos < max - 1) {
        /* Blocking read: one character at a time */
        if (sys_read(STDIN_FD, &c, 1) <= 0)
            continue;

        if (c == '\n' || c == '\r') {
            /* Enter: submit line */
            putchar('\n');
            break;
        }

        if (c == '\b' || c == 127) {
            /* Backspace: erase last character */
            if (pos > 0) {
                pos--;
                /* Move cursor back, overwrite with space, move back again */
                printf("\b \b");
            }
            continue;
        }

        if (c == 3) {
            /* Ctrl+C: cancel current line */
            printf("^C\n");
            pos = 0;
            break;
        }

        if (c == 4) {
            /* Ctrl+D on empty line: EOF */
            if (pos == 0) {
                printf("\n");
                return -1;  /* signal EOF */
            }
            continue;
        }

        /* Only accept printable characters */
        if (isprint(c)) {
            buf[pos++] = c;
            putchar(c);  /* echo */
        }
    }

    buf[pos] = '\0';
    return pos;
}

/* --- Command parsing ---
 * Splits a line into argv[] by whitespace.
 * Returns argc (number of arguments). */
static int parse(char *line, char **argv, int max_args)
{
    int argc = 0;

    while (*line && argc < max_args - 1) {
        /* Skip leading whitespace */
        while (*line && isspace(*line))
            *line++ = '\0';

        if (*line == '\0')
            break;

        /* Found an argument */
        argv[argc++] = line;

        /* Skip to next whitespace */
        while (*line && !isspace(*line))
            line++;
    }

    argv[argc] = NULL;
    return argc;
}

/* --- Built-in commands --- */

static void cmd_help(void)
{
    printf("Impossible OS Shell - Built-in commands:\n");
    printf("  help          Show this help message\n");
    printf("  echo [args]   Print arguments to stdout\n");
    printf("  clear         Clear the screen\n");
    printf("  uname         Show system information\n");
    printf("  exit [code]   Exit the shell\n");
    printf("  version       Show OS version\n");
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1)
            putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
}

static void cmd_clear(void)
{
    printf(ESC_CLEAR);
}

static void cmd_uname(void)
{
    printf("Impossible OS x86_64\n");
}

static void cmd_version(void)
{
    printf("Impossible OS v0.1.0\n");
    printf("  Architecture: x86-64 (Long Mode)\n");
    printf("  Shell:        built-in REPL\n");
    printf("  Libc:         minimal freestanding\n");
}

/* --- Command dispatch --- */
static int dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
        cmd_help();
    } else if (strcmp(argv[0], "echo") == 0) {
        cmd_echo(argc, argv);
    } else if (strcmp(argv[0], "clear") == 0) {
        cmd_clear();
    } else if (strcmp(argv[0], "uname") == 0) {
        cmd_uname();
    } else if (strcmp(argv[0], "version") == 0) {
        cmd_version();
    } else if (strcmp(argv[0], "exit") == 0) {
        int code = 0;
        if (argc > 1)
            code = atoi(argv[1]);
        return code - 256;  /* negative = exit signal */
    } else {
        printf("unknown command: %s\n", argv[0]);
        printf("Type 'help' for a list of commands.\n");
    }

    return 0;
}

/* --- Main REPL --- */
int main(void)
{
    char line[LINE_MAX];
    char *argv[ARGV_MAX];
    int argc, len, result;

    printf("\n");
    printf("  ====================================\n");
    printf("  |   Impossible OS Shell v0.1.0     |\n");
    printf("  |   Type 'help' for commands.      |\n");
    printf("  ====================================\n");
    printf("\n");

    for (;;) {
        printf(PROMPT);

        len = readline(line, LINE_MAX);
        if (len < 0) {
            /* EOF (Ctrl+D) */
            printf("exit\n");
            break;
        }
        if (len == 0)
            continue;

        argc = parse(line, argv, ARGV_MAX);
        result = dispatch(argc, argv);

        if (result < 0) {
            /* Exit was requested */
            return result + 256;
        }
    }

    return 0;
}
