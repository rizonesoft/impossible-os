# Command-Line Shell

The shell is a user-mode program (`user/shell.c`) that provides an interactive
command-line interface. It runs as a regular process, using the libc and syscall
interface to interact with the kernel.

## Key Files

| File | Purpose |
|------|---------|
| `user/shell.c` | Shell REPL, command parsing, built-in commands |
| `user/include/syscall.h` | Syscall wrappers used by the shell |

## REPL Loop

```
┌─────────────────────────────┐
│  Print prompt "C:\> "       │
│           │                 │
│  Read line (with editing)   │
│           │                 │
│  Parse command + arguments  │
│           │                 │
│  Execute built-in or fork   │
│           │                 │
│  Loop ────┘                 │
└─────────────────────────────┘
```

## Line Editing

| Key | Action |
|-----|--------|
| Printable chars | Insert at cursor |
| Backspace | Delete previous character |
| Enter | Submit command |
| Ctrl+C | Cancel current line |
| Ctrl+D | Exit shell (EOF) |
| Up arrow | Previous command (history) |
| Down arrow | Next command (history) |

## Built-In Commands

### File Operations

| Command | Alias | Description |
|---------|-------|-------------|
| `cat <file>` | `type` | Print file contents |
| `ls` | `dir` | List files in current directory |

### System Information

| Command | Alias | Description |
|---------|-------|-------------|
| `uname` | `version`, `ver` | OS name, version, architecture |
| `uptime` | — | Time since boot |
| `ps` | — | List running processes (PID, state, name) |

### Process Control

| Command | Description |
|---------|-------------|
| `kill <pid>` | Terminate a process by PID |
| `exit [code]` | Exit the shell with optional exit code |

### Power Management

| Command | Description |
|---------|-------------|
| `reboot` | ACPI reboot (via `SYS_REBOOT`) |
| `shutdown` | ACPI power-off (via `SYS_SHUTDOWN`) |

### Networking

| Command | Description |
|---------|-------------|
| `ping <ip>` | Send ICMP echo request |
| `ifconfig` / `ipconfig` | Show network configuration |

### Utilities

| Command | Description |
|---------|-------------|
| `echo <text>` | Print text to stdout |
| `clear` / `cls` | Clear the screen |
| `help` | List all available commands |

## Command History

The shell maintains a history buffer of recent commands. Arrow keys navigate
through previous entries, allowing re-execution without retyping.

## Process Execution

For external commands (not built-in), the shell:
1. Calls `fork()` to create a child process
2. Calls `exec(filename)` in the child to load the program
3. Calls `waitpid(child_pid)` in the parent to wait for completion
4. Prints the exit code if non-zero
