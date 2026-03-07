# 54 — Shell Commands

## Overview

Expand the terminal shell with essential file management and system commands.

---

## Current commands
| Command | Status |
|---------|--------|
| `help` | ✅ |
| `ls` | ✅ |
| `clear` | ✅ |
| `ping` | ✅ |
| `ifconfig`/`ipconfig` | ✅ |

## Missing essential commands

### File operations
| Command | Syntax | Purpose |
|---------|--------|---------|
| `cd` | `cd <dir>` | Change directory |
| `pwd` | `pwd` | Print working directory |
| `mkdir` | `mkdir <name>` | Create directory |
| `rmdir` | `rmdir <name>` | Remove empty directory |
| `cp` | `cp <src> <dst>` | Copy file |
| `mv` | `mv <src> <dst>` | Move/rename file |
| `rm` | `rm <file>` | Delete file (to trash) |
| `cat` | `cat <file>` | Print file contents |
| `touch` | `touch <file>` | Create empty file |
| `echo` | `echo <text>` | Print text (+ `> file` redirect) |

### System commands
| Command | Syntax | Purpose |
|---------|--------|---------|
| `whoami` | `whoami` | Current user name |
| `uname` | `uname -a` | OS version + kernel info |
| `uptime` | `uptime` | System uptime |
| `date` | `date` | Current date and time |
| `free` | `free` | RAM usage |
| `ps` | `ps` | Process list |
| `kill` | `kill <pid>` | Kill process |
| `shutdown` | `shutdown` | Power off |
| `reboot` | `reboot` | Restart |

### Network commands
| Command | Syntax | Purpose |
|---------|--------|---------|
| `wget` | `wget <url>` | Download file (after HTTP) |
| `nslookup` | `nslookup <host>` | DNS resolve |

### Features
| Feature | Description |
|---------|-------------|
| Tab completion | Complete file/command names |
| Command history | Up/down arrows |
| Pipe `\|` | `cat file \| grep text` (future) |
| Redirect `>` | `echo hello > file.txt` |
| `&&` chaining | `mkdir foo && cd foo` |

## Files: `user/shell.c` (expand existing, +500 lines)
## Implementation: 1-2 weeks
