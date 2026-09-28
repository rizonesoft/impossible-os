# Contributing to Impossible OS

Thank you for your interest in contributing to Impossible OS! This guide covers
everything you need to get started.

---

## 🚀 Getting Started

### Prerequisites

- **Fully supported:** Ubuntu/Debian 24.04+ (native or WSL 2), GitHub Actions
  `ubuntu-latest`, or the committed `.devcontainer` (Ubuntu base image)
- **Best-effort:** Fedora or Arch Linux (needs the LLVM-19 + OVMF shim from the
  Host Profiles doc below)
- **Unsupported:** native Windows (use WSL 2) and macOS
- Git

### Development Environment Setup

```bash
git clone https://github.com/rizonesoft/impossible-os.git
cd impossible-os
bash scripts/setup.sh          # Installs Clang-19, NASM, QEMU, OVMF, mtools, gcc, python3, etc.
bash scripts/build.sh run      # Build + boot in QEMU to verify everything works
```

`setup.sh` handles all dependencies automatically. If you're on an unsupported
distro, run `bash scripts/setup.sh --help` for the required-tool list, then
install manually. The canonical [Host Bootstrap Contract](docs/infrastructure/development-tooling.md#host-bootstrap-contract)
and [Supported Host Profiles](docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments)
document distros, sentinel tools, minimum versions, the devcontainer profile,
idempotence, and scope boundaries. Re-check with `bash scripts/setup.sh --verify`;
see version floors with `bash scripts/setup.sh --versions`.

**Reproducible environment:** open the repo in VS Code with the Dev
Containers extension installed and "Reopen in Container" -- the devcontainer
runs `bash scripts/setup.sh` on first start.

### Enable Git Hooks

One command wires up the always-on hooks (pre-commit lint + post-commit
`COUNT.md`) and tells you the current state:

```bash
bash scripts/install-hooks.sh              # sets core.hooksPath=.githooks (idempotent)
bash scripts/install-hooks.sh --with-pre-push  # same + enables the opt-in pre-push gate
bash scripts/install-hooks.sh --status     # print current state
bash scripts/install-hooks.sh --remove     # undo all of the above
```

Always-on hooks once `core.hooksPath=.githooks` is set:
- [`.githooks/pre-commit`](.githooks/pre-commit) -- lints staged `.c`/`.h`,
  enforces the test-file live-call ban, exits in under a millisecond when
  no C files are staged.
- [`.githooks/post-commit`](.githooks/post-commit) -- regenerates
  [`COUNT.md`](COUNT.md) and amends the commit. Silent on success.

**Opt-in** pre-push gate (`--with-pre-push` or `--enable-pre-push`):
[`.githooks/pre-push`](.githooks/pre-push) delegates to
[`scripts/hooks/pre-push`](scripts/hooks/pre-push) when the
`.git/.impossible-os-prepush` sentinel exists. On push it runs
`bash scripts/build.sh` then `bash scripts/test.sh`; any failure blocks
the push with a pointer at the manual re-run command. Skip it at your
discretion -- GitHub Actions remains the mandatory gate at PR time.
Disable with `bash scripts/install-hooks.sh --disable-pre-push`.

Full lifecycle reference:
[Local CI Hooks](docs/infrastructure/development-tooling.md#local-ci-hooks).

---

## 📐 Code Style

Impossible OS is written in **C** and **x86-64 Assembly (NASM)**. Follow these
conventions to keep the codebase consistent.

### Naming

| Element | Convention | Example |
|---------|-----------|---------|
| Functions | `snake_case` | `pmm_alloc_frame()` |
| Variables | `snake_case` | `frame_count` |
| Macros / Constants | `UPPER_CASE` | `PAGE_SIZE`, `MAX_THREADS` |
| Types / Structs | `snake_case_t` | `task_t`, `vfs_node_t` |

### Formatting

- **Line length:** ≤ 120 characters
- **Function length:** < 50 lines (split into helpers if longer)
- **Include guards:** Use `#pragma once`
- **Indentation:** 4 spaces (no tabs)

### Headers

- One header per source file (e.g., `pmm.c` → `include/mm/pmm.h`)
- All headers live in `include/` -- never in `src/`
- Only freestanding headers allowed: `<stdint.h>`, `<stddef.h>`, `<stdbool.h>`, `<stdarg.h>`
- **Never** include `<stdio.h>`, `<stdlib.h>`, `<string.h>`, or any user-space headers

### Hardware Interactions

Comment all non-obvious port I/O, MMIO, and register manipulation:

```c
// Send EOI to Local APIC -- must be done AFTER reading the ISR,
// otherwise the interrupt may fire again before we handle it
lapic_write(LAPIC_EOI, 0);
```

### Memory Allocation

This is the most common source of bugs. **Read carefully:**

| Allocation | When to Use | Max Size |
|------------|------------|----------|
| `kmalloc()` | Small kernel bookkeeping: VFS nodes, task structs, linked-list nodes | ≤ 4 KB |
| `pmm_alloc_contiguous()` | Everything else: buffers, images, font data, framebuffers | No limit |

> [!CAUTION]
> The kernel heap is only **2 MiB**. Using `kmalloc()` for anything larger than
> a few KB causes silent heap exhaustion. When in doubt, use PMM.

### Documentation Pages

Every roadmap file under `todo/` has a docs page under `docs/`, and a new roadmap file cannot be committed without one. Pages follow the [Documentation Page Contract](docs/contributing/docs-page-contract.md): start from [the template](docs/contributing/_template.md), put the page in its domain's folder, and run `python3 scripts/site/build.py --check` before committing.

---

## 💬 Commit Messages

We use **conventional commits** with scope prefixes:

```
scope: short description
```

### Common Scopes

| Scope | Usage |
|-------|-------|
| `kernel` | Kernel core (memory, scheduler, syscalls) |
| `boot` | UEFI bootloader |
| `desktop` | Window manager, compositor, shell |
| `drivers` | Hardware drivers (AHCI, NIC, keyboard, etc.) |
| `gfx` | Graphics library (rendering, fonts, effects) |
| `fs` | Filesystem (VFS, FAT32, IXFS) |
| `net` | Networking (Ethernet, TCP/IP, DHCP) |
| `build` | Build system, Makefile, scripts |
| `docs` | Documentation, README, TODO updates |
| `agent` | Agent skills, workflows, rules |

### Examples

```
kernel: add seqlock synchronization primitive
drivers: implement RTL8139 receive interrupt handler
desktop: dirty rectangle compositor for drag optimization
build: migrate from GCC to Clang-19 + LLD
docs: professional README with feature table
```

### AI-Assisted Commit Policy (zero trailer)

Impossible OS is a Claude Code-orchestrated project (see [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md) Authority Hierarchy). AI assistance is implicit in the project's identity, not a per-commit attribution concern. Commits in this repo therefore:

- **Do NOT carry `Co-Authored-By: Claude` or similar attribution trailers.** Every commit here is authored by a human operator working with Claude Code; the combined authorship is the repo's default mode of operation, not a special case per commit.
- **Do NOT carry `Assisted-by: TOOL:MODEL` trailers** (the Linux kernel 2025-12 convention from [Documentation/process/coding-assistants.rst](https://docs.kernel.org/process/coding-assistants.html), Sasha Levin / Jonathan Corbet; Fedora adopted a similar policy in October 2025). Impossible OS's divergence from that convention is deliberate, not an oversight. See the stance-change condition below.
- **The screening bar for AI slop is the existing review workflow**, not a commit-trailer check. [`/implement-todo-section`](.claude/skills/implement-todo-section/) steps 13-18 and [`/review-todo-section`](.claude/skills/review-todo-section/) run mandatory Codex adversarial + quality dispatches on every section commit; the scope-gap protocol, domain code-quality gates, and `superpowers:receiving-code-review` discipline catch phantom helpers, pointless refactors "for consistency", and stub-behind-stamp patterns before they reach `main`. If a commit looks like AI slop in review, the review pipeline was skipped; the fix is to re-run it, not to add a trailer.

**Stance-change condition.** This zero-trailer stance stands as long as Impossible OS only accepts contributions from project members working interactively with Claude Code. If the project ever opens to external AI-assisted contributions from non-project-members (e.g. community PRs from contributors using Codex CLI, Aider, Copilot coding-agent, Cursor, etc.), the project adopts the Linux-kernel `Assisted-by: TOOL:MODEL [AGENT_NAME]` trailer grammar at that point. Until then, any commit in this repo carrying such a trailer is the policy bug, not the commit; the trailer gets removed via `git commit --amend` or interactive rebase before merge.

---

## 🔄 Pull Request Process

1. **Fork** the repository
2. **Branch** from `main` using the naming convention below:
   ```bash
   git checkout -b feature/your-feature-name
   ```
3. **Implement** your changes following the code style above
4. **Test** before submitting:
   ```bash
   bash scripts/build.sh             # Must show "=== BUILD OK ===" in build/build.log
   bash scripts/test.sh              # Run all kernel unit tests (headless QEMU)
   bash scripts/build.sh run         # Boot in QEMU to verify visually
   ```
   See [Wrapper Contract](docs/infrastructure/development-tooling.md#wrapper-contract)
   for the full command reference including `--help`, `SUITE=<cat>`, and `QUIET=1` modes.
5. **Commit** with a conventional commit message
6. **Push** and open a Pull Request against `main`

### Branch Naming Convention

| Prefix | Usage | Example |
|--------|-------|---------|
| `feature/*` | New features or capabilities | `feature/tcp-stack` |
| `fix/*` | Bug fixes | `fix/pmm-double-free` |
| `docs/*` | Documentation changes | `docs/contributing-guide` |
| `refactor/*` | Code refactoring (no behavior change) | `refactor/vfs-cleanup` |

### PR Checklist

- [ ] Code compiles without warnings (`-Wall -Wextra -Werror`)
- [ ] Tested in QEMU -- boots and runs correctly
- [ ] No new `kmalloc()` calls for buffers > 4 KB
- [ ] Serial output checked for new warnings/errors
- [ ] Commit message follows `"scope: description"` format

---

## 🗺️ Finding Work Items

All development is tracked in the TODO system:

- **Start here:** [`todo/TODO-00-INDEX.md`](todo/TODO-00-INDEX.md) -- master index of all work
- **Each TODO file** contains detailed sections with implementation prompts
- **Status markers:** `[ ]` uncompleted, `[/]` in progress, `[x]` completed
- **Look for** sections marked with priority: 🔴 P0, 🟠 P1, 🟡 P2, 🟢 P3

### Good First Issues

If you're new to OS development, look for:
- Documentation improvements
- Adding missing kernel log messages
- Small driver enhancements
- Test coverage gaps

---

## ⚠️ Important Rules

1. **Always use `bash scripts/build.sh`** -- never raw `make` commands
2. **APIC-only interrupts** -- do NOT write 8259 PIC routing code
3. **DMA-only storage** -- do NOT use legacy IDE/ATA PIO polling
4. **UEFI GOP framebuffer** -- do NOT write VGA text mode (0xB8000) code
5. **No standard library** -- this is a freestanding kernel, not user-space
6. **Use `printk()`** for output, not `printf()`

---

## 🏷️ Release Tags

We use annotated tags for releases, matching our **CalVer** (`YY.M.D`) scheme:

| Format | Example | Usage |
|--------|---------|-------|
| `v{YY}.{M}.{D}` | `v26.3.18`, `v26.4.1` | Stable releases |
| `v{YY}.{M}.{D}-alpha.{n}` | `v26.3.18-alpha.1` | Early testing |
| `v{YY}.{M}.{D}-beta.{n}` | `v26.3.18-beta.1` | Feature-complete testing |
| `v{YY}.{M}.{D}-rc.{n}` | `v26.3.18-rc.1` | Release candidates |

Tags are always annotated:

```bash
git tag -a v26.3.18 -m "Release v26.3.18"
git push origin v26.3.18
```

Pushing a `v*` tag triggers the automated release CI workflow, which builds
the disk image and publishes a GitHub Release with the auto-generated changelog.

---

## 📄 License

By contributing, you agree that your contributions will be licensed under the
[GPL-3.0-only license](LICENSE).
