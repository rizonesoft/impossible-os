# WSL 2 Sandbox Environment

Impossible OS is developed inside a **WSL 2** (Windows Subsystem for Linux) sandbox.
This ensures complete isolation from the Windows host — the agent and build tools
cannot modify the host filesystem, registry, or hardware.

## Host Configuration

| Property | Value |
|----------|-------|
| WSL Version | **2** (not 1 — required for full Linux kernel) |
| Distro | Ubuntu 24.04.4 LTS |
| Kernel | 6.6.87.2-microsoft-standard-WSL2 |
| Shell | bash (`/dev/pts`) |

## Verification

```bash
# Confirm WSL 2
wsl --list --verbose
# Should show: Ubuntu  Running  2

# Confirm kernel
uname -r
# 6.6.87.2-microsoft-standard-WSL2
```

## Workspace

| Property | Value |
|----------|-------|
| Path | `/home/derickpayne/impossible-os/` |
| Filesystem | ext4 (native Linux) |
| NOT on | `/mnt/c/` (Windows mount — too slow, wrong permissions) |

> **Important:** The project must live on the Linux filesystem (`~/`), not on a
> Windows-mounted path. WSL 2's ext4 filesystem is dramatically faster for
> compilation and avoids permission issues.

## Safety Boundaries

The following restrictions are enforced via `rules.md` (loaded by the IDE):

| Rule | Reason |
|------|--------|
| No access to `/mnt/c/` or Windows paths | Prevents accidental host modification |
| No `dd`, `mkfs` on real devices (`/dev/sda`, `/dev/nvme*`) | Prevents disk destruction |
| No system-wide package installs without approval | Prevents environment pollution |
| No external network requests unless instructed | Prevents data leakage |
| All output stays in `~/impossible-os/` | Workspace isolation |

## IDE Connection

The IDE (Antigravity / VS Code) connects to WSL via the **WSL extension**:

1. Install the **WSL** extension
2. `F1` → **WSL: Connect to WSL**
3. Open folder: `/home/derickpayne/impossible-os`
4. The integrated terminal runs bash inside WSL 2

All file editing, terminal commands, and builds happen inside the WSL 2 container.
