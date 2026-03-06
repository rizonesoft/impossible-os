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
#define HISTORY_MAX 16

/* Arrow key codes (must match kernel keyboard.h) */
#define KEY_UP      0x80
#define KEY_DOWN    0x81
#define KEY_LEFT    0x82
#define KEY_RIGHT   0x83

/* --- Command history ring buffer --- */
static char history[HISTORY_MAX][LINE_MAX];
static int  history_count = 0;     /* total entries stored */
static int  history_idx   = 0;     /* next write slot */

static void history_add(const char *line)
{
    if (strlen(line) == 0)
        return;
    /* Don't add duplicates of the last entry */
    if (history_count > 0) {
        int prev = (history_idx - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (strcmp(history[prev], line) == 0)
            return;
    }
    strcpy(history[history_idx], line);
    history_idx = (history_idx + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX)
        history_count++;
}

/* Clear the current line on screen and replace with new text */
static void line_replace(char *buf, int *pos, const char *newtext)
{
    int i;
    /* Move cursor to start of input and clear */
    for (i = 0; i < *pos; i++)
        putchar('\b');
    for (i = 0; i < *pos; i++)
        putchar(' ');
    for (i = 0; i < *pos; i++)
        putchar('\b');

    /* Copy and display new text */
    strcpy(buf, newtext);
    *pos = (int)strlen(buf);
    printf("%s", buf);
}

/* --- Line editing --- */
static int readline(char *buf, int max)
{
    int pos = 0;
    int hist_pos = history_count;  /* start past the end = "new line" */
    char c;

    while (pos < max - 1) {
        if (sys_read(STDIN_FD, &c, 1) <= 0)
            continue;

        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }

        if (c == '\b' || c == 127) {
            if (pos > 0) {
                pos--;
                printf("\b \b");
            }
            continue;
        }

        if (c == 3) {   /* Ctrl+C */
            printf("^C\n");
            pos = 0;
            break;
        }

        if (c == 4) {   /* Ctrl+D on empty line = EOF */
            if (pos == 0) {
                printf("\n");
                return -1;
            }
            continue;
        }

        /* Arrow key: up = previous history */
        if ((unsigned char)c == KEY_UP) {
            if (hist_pos > 0 && history_count > 0) {
                hist_pos--;
                int real = (history_idx - history_count + hist_pos
                            + HISTORY_MAX) % HISTORY_MAX;
                line_replace(buf, &pos, history[real]);
            }
            continue;
        }

        /* Arrow key: down = next history / clear */
        if ((unsigned char)c == KEY_DOWN) {
            if (hist_pos < history_count - 1) {
                hist_pos++;
                int real = (history_idx - history_count + hist_pos
                            + HISTORY_MAX) % HISTORY_MAX;
                line_replace(buf, &pos, history[real]);
            } else if (hist_pos < history_count) {
                hist_pos = history_count;
                line_replace(buf, &pos, "");
            }
            continue;
        }

        if (isprint(c)) {
            buf[pos++] = c;
            putchar(c);
        }
    }

    buf[pos] = '\0';
    return pos;
}

/* --- Command parsing --- */
static int parse(char *line, char **argv, int max_args)
{
    int argc = 0;
    while (*line && argc < max_args - 1) {
        while (*line && isspace(*line))
            *line++ = '\0';
        if (*line == '\0')
            break;
        argv[argc++] = line;
        while (*line && !isspace(*line))
            line++;
    }
    argv[argc] = NULL;
    return argc;
}

/* --- Built-in commands --- */

static void cmd_help(void)
{
    printf("Impossible OS Shell v0.1.0 - Commands:\n\n");
    printf("  help              Show this message\n");
    printf("  echo [args...]    Print arguments\n");
    printf("  clear             Clear the screen\n");
    printf("  uname             System information\n");
    printf("  version           OS version\n");
    printf("  ls                List files in initrd\n");
    printf("  cat <file>        Print file contents\n");
    printf("  ps                List running processes\n");
    printf("  kill <pid>        Terminate a process\n");
    printf("  uptime            Show system uptime\n");
    printf("  ping <ip>         Send ICMP echo request\n");
    printf("  ifconfig          Show network configuration\n");
    printf("  reboot            Reboot the system\n");
    printf("  shutdown          Power off the system\n");
    printf("  exit [code]       Exit the shell\n");
}

static void cmd_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        printf("%s", argv[i]);
    }
    putchar('\n');
}

static void cmd_clear(void)
{
    printf("\033[2J\033[H");
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

static void cmd_ls(void)
{
    char name[256];
    unsigned int i;

    printf("  Files in initrd:\n");
    for (i = 0; ; i++) {
        if (sys_readdir(name, sizeof(name), i) < 0)
            break;
        printf("    %s\n", name);
    }
}

static void cmd_cat(int argc, char **argv)
{
    char buf[4096];
    long n;

    if (argc < 2) {
        printf("Usage: cat <filename>\n");
        return;
    }

    n = sys_readfile(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("cat: %s: file not found\n", argv[1]);
        return;
    }

    buf[n] = '\0';
    printf("%s", buf);
    /* Add newline if file doesn't end with one */
    if (n > 0 && buf[n - 1] != '\n')
        putchar('\n');
}

static const char *state_name(unsigned int state)
{
    switch (state) {
    case TASK_READY:   return "READY";
    case TASK_RUNNING: return "RUNNING";
    case TASK_DEAD:    return "DEAD";
    case TASK_WAITING: return "WAITING";
    default:           return "?";
    }
}

static void cmd_ps(void)
{
    struct proc_info procs[32];
    long count;
    int i;

    count = sys_getprocs(procs, sizeof(procs));
    if (count < 0) {
        printf("ps: failed to get process list\n");
        return;
    }

    printf("  PID  STATE      NAME\n");
    printf("  ---  --------   ----\n");
    for (i = 0; i < count && i < 32; i++) {
        printf("  %-4u %-10s %s\n",
               procs[i].pid,
               state_name(procs[i].state),
               procs[i].name[0] ? procs[i].name : "(unnamed)");
    }
}

static void cmd_kill(int argc, char **argv)
{
    int pid;
    if (argc < 2) {
        printf("Usage: kill <pid>\n");
        return;
    }
    pid = atoi(argv[1]);
    if (pid <= 0) {
        printf("kill: invalid PID\n");
        return;
    }
    if (sys_kill(pid) < 0) {
        printf("kill: failed to kill PID %d\n", pid);
    } else {
        printf("kill: sent to PID %d\n", pid);
    }
}

static void cmd_uptime(void)
{
    long secs = sys_uptime();
    long hours = secs / 3600;
    long mins = (secs % 3600) / 60;
    long s = secs % 60;

    printf("  Uptime: ");
    if (hours > 0)
        printf("%ldh ", hours);
    if (mins > 0 || hours > 0)
        printf("%ldm ", mins);
    printf("%lds\n", s);
}

static void cmd_reboot(void)
{
    printf("Rebooting...\n");
    sys_reboot();
}

static void cmd_shutdown(void)
{
    printf("Shutting down...\n");
    sys_shutdown();
}

/* Parse "a.b.c.d" → big-endian IP (network byte order) */
static unsigned int parse_ip(const char *s)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int i, field = 0;
    unsigned int cur = 0;

    for (i = 0; s[i]; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            cur = cur * 10 + (unsigned int)(s[i] - '0');
        } else if (s[i] == '.') {
            if (field == 0) a = cur;
            else if (field == 1) b = cur;
            else if (field == 2) c = cur;
            cur = 0;
            field++;
        }
    }
    d = cur;
    /* Return in big-endian (network byte order for x86 little-endian) */
    return a | (b << 8) | (c << 16) | (d << 24);
}

static void print_ip(unsigned int ip)
{
    printf("%u.%u.%u.%u",
           ip & 0xFF, (ip >> 8) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
}

static void cmd_ping(int argc, char **argv)
{
    unsigned int ip;
    int i;

    if (argc < 2) {
        printf("Usage: ping <ip_address>\n");
        return;
    }

    ip = parse_ip(argv[1]);
    printf("PING ");
    print_ip(ip);
    printf("\n");

    for (i = 1; i <= 4; i++) {
        sys_ping(ip, i);
        printf("  Sent echo request seq=%d\n", i);

        /* Wait a bit for reply (simple busy-wait) */
        {
            int j;
            for (j = 0; j < 500000; j++)
                sys_yield();
        }
    }
}

static void cmd_ifconfig(void)
{
    struct user_net_config cfg;
    sys_netinfo(&cfg, sizeof(cfg));

    printf("  eth0:\n");
    printf("    MAC:     %x:%x:%x:%x:%x:%x\n",
           (unsigned int)cfg.mac[0], (unsigned int)cfg.mac[1],
           (unsigned int)cfg.mac[2], (unsigned int)cfg.mac[3],
           (unsigned int)cfg.mac[4], (unsigned int)cfg.mac[5]);

    if (cfg.configured) {
        printf("    IP:      ");
        print_ip(cfg.ip);
        printf("\n    Subnet:  ");
        print_ip(cfg.subnet);
        printf("\n    Gateway: ");
        print_ip(cfg.gateway);
        printf("\n    DNS:     ");
        print_ip(cfg.dns);
        printf("\n");
    } else {
        printf("    IP:      (not configured, DHCP pending)\n");
    }
}

/* --- Command dispatch --- */
static int dispatch(int argc, char **argv)
{
    if (argc == 0)
        return 0;

    if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0)
        cmd_help();
    else if (strcmp(argv[0], "echo") == 0)
        cmd_echo(argc, argv);
    else if (strcmp(argv[0], "clear") == 0 || strcmp(argv[0], "cls") == 0)
        cmd_clear();
    else if (strcmp(argv[0], "uname") == 0)
        cmd_uname();
    else if (strcmp(argv[0], "version") == 0 || strcmp(argv[0], "ver") == 0)
        cmd_version();
    else if (strcmp(argv[0], "ls") == 0 || strcmp(argv[0], "dir") == 0)
        cmd_ls();
    else if (strcmp(argv[0], "cat") == 0 || strcmp(argv[0], "type") == 0)
        cmd_cat(argc, argv);
    else if (strcmp(argv[0], "ps") == 0)
        cmd_ps();
    else if (strcmp(argv[0], "kill") == 0)
        cmd_kill(argc, argv);
    else if (strcmp(argv[0], "uptime") == 0)
        cmd_uptime();
    else if (strcmp(argv[0], "ping") == 0)
        cmd_ping(argc, argv);
    else if (strcmp(argv[0], "ifconfig") == 0 || strcmp(argv[0], "ipconfig") == 0)
        cmd_ifconfig();
    else if (strcmp(argv[0], "reboot") == 0)
        cmd_reboot();
    else if (strcmp(argv[0], "shutdown") == 0)
        cmd_shutdown();
    else if (strcmp(argv[0], "exit") == 0) {
        int code = argc > 1 ? atoi(argv[1]) : 0;
        return code - 256;
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
            printf("exit\n");
            break;
        }
        if (len == 0)
            continue;

        /* Add to history before parsing (parse modifies the string) */
        history_add(line);

        argc = parse(line, argv, ARGV_MAX);
        result = dispatch(argc, argv);

        if (result < 0)
            return result + 256;
    }

    return 0;
}
