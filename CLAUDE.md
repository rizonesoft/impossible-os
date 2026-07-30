# CLAUDE.md -- Impossible OS

> Claude Code project instructions for the Impossible OS kernel. This file and `.claude/skills/` are the complete Claude Code system -- self-contained, no shared layers.

## Shell Calls -- no `cd <project-dir>` prefix

The working directory PERSISTS between Bash calls, so `cd <project-dir> && <cmd>` is redundant. It is not a
style nit: the prefix defeats permission-allowlist matching -- an entry like `Bash(bash scripts/test.sh:*)`
cannot match a `cd /long/path && bash scripts/test.sh` string, so every prefixed variant becomes a fresh
permission decision. Measured 2026-07-28: **3,187 of 4,729 Bash calls (67%)** carried the prefix; only 4 were a
legitimate `cd <other dir>`. Use `cd` only when you genuinely need a DIFFERENT directory.
`.claude/hooks/cd_prefix_reminder.py` warns (twice per session, never blocks -- the prefixed command is
correct, just costly).

## Build -- Never use raw `make`

```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result -- must show `=== BUILD OK ===`.

Host bootstrap (deps, supported distros, required-tool sentinels, idempotence): [docs/infrastructure/development-tooling.md#host-bootstrap-contract](docs/infrastructure/development-tooling.md#host-bootstrap-contract). Host profiles (support tiers, min versions, devcontainer, unsupported-host policy): [docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments](docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments). Re-check with `bash scripts/setup.sh --verify`; version floors via `bash scripts/setup.sh --versions`.

## Testing -- Category-Based Test Infrastructure

> **TCG works in WSL2** (~10s for build + the full test suite; live counts in [docs/test-coverage/coverage.md](docs/test-coverage/coverage.md)). A post-commit hook runs the full suite after every kernel/source commit. SMEP is disabled globally (boot PML4 has User bit on all pages). WHPX, VirtualBox, and bare metal remain the primary validation platforms.

```bash
bash scripts/test.sh              # all suites (WSL2 TCG, native Windows, bare metal)
bash scripts/test.sh SUITE=mm     # Memory Management only
bash scripts/test.sh SUITE=ob     # Object Manager only
bash scripts/test.sh QUIET=1      # summary only (suppress PASS lines)
make test-mm                      # shorthand for SUITE=mm
make test-fs                      # Filesystem suites
make test-ob                      # Object Manager suites
make test-security                # Security suites
make test-ipc                     # IPC suites (pipe, shmem, semaphore)
make test-sched                   # Scheduler suites
make test-boot                    # Boot init + klog suites
make test-abi                     # PEB/TEB, Registry, env vars/process-ABI suites
make test-storage                 # AHCI, VirtIO suites
make test-exec                    # Binary system suites (exec, EIF, modules)
make test-quota                   # Resource accounting + quota suites
```

Categories: `mm`, `fs`, `sched`, `ob`, `security`, `ipc`, `boot`, `abi`, `storage`, `exec`, `x86`, `desktop`, `ex`, `nls`, `knf`, `except`, `quota` -- the enum in [`include/kernel/test/test.h`](include/kernel/test/test.h) is the source of truth, and every one has a matching `make test-<cat>` target. Configured via `test_suite=` and `test_quiet=` in `boot.conf`. Windows: `scripts/debug/kernel/run-mm-tests.bat` etc.

Register new tests with `test_suite_register_cat("name", fn, TEST_CAT_XX)`. Use `TEST_ASSERT_EQ(a, b, msg)` for value comparisons and `TEST_SKIP(msg)` for hardware-dependent tests.

## Smoke Test -- End-to-End Boot Validation

```bash
bash scripts/test-smoke.sh        # builds, boots in QEMU, checks for Boot complete + C:\>
bash scripts/test-smoke-matrix.sh # the same, across TCG+KVM x 1+2 CPUs (4 legs, ~3 min)
```

The smoke test complements unit tests: unit tests cover individual behaviors against in-memory fixtures, the smoke test proves **the image actually boots to userspace**. It auto-selects KVM if `/dev/kvm` is writable and falls back to TCG otherwise. 30-second timeout, fail-fast on missing markers.

**Boot-validation matrix (2026-07-28).** `test-smoke-matrix.sh` boots the image on **TCG and KVM at 1 and 2 CPUs** and fails if any leg fails. Prefer it over the single-config smoke test wherever a section is being validated; run it under `scripts/overnight/run-artifact.sh` so only the verdict enters context (full logs land in `build/smoke-matrix/`).

Why four legs. Until 2026-07-28 the smoke gate passed **no `-smp` flag at all**, so it had never booted SMP -- and a section changing the TEB <-> `kernel_gs_base` handoff across context switches shipped green through it, then halted on a 2-CPU boot with `[CRIT] ... has TEB but kernel_gs_base=0`. The engines are not redundant either (see the KVM-vs-TCG table below). **A leg that fails on ONE configuration is the finding, not noise** -- never widen an assertion to make it pass; the engine/CPU difference IS the evidence.

**WHPX belongs at the section boundary, not the per-edit loop.** `scripts/machines/run-qemu.ps1 -Accel whpx -Headless` is scriptable from WSL and is the accelerator that caught a halt the Linux matrix could not reproduce, but at ~141s per boot it is a per-section check. It reaps its own QEMU (recorded PIDs only, name-checked against reuse), so a killed wrapper no longer orphans a VM holding the OVMF flash file.

**Triage every leg, and resolve to truth.** Fix a real `[FAIL]`/`[WARN]`/`[CRIT]`/halt/regression; if a line is WRONGLY reported, fix the reporter at source rather than muting it (2026-07-28: 30 "init regressed" warnings were a cumulative-vs-per-step comparison bug -- the fix was correcting the comparison); leave a line alone only when it is genuinely required or normal for the platform, such as absent TPM, absent RDRAND, firmware-table quirks, and deliberate test-path refusals.

**When to run:**

- **After boot-path changes** -- any edit to `src/boot/`, `src/kernel/main/boot_*`, `src/kernel/idt.c|gdt.c|msr.c`, `src/kernel/smp/`, `src/kernel/mm/pmm.c|vmm.c|heap.c`, `src/kernel/drivers/lapic.c|ioapic.c|acpi.c|timer.c`, or any Phase 0/1 init code. Built into `/implement-todo-section` step 16 for exactly these paths.
- **While debugging** a boot crash or init-order bug -- `/debug-session` uses it as the primary reproduction + verification tool. Inspect `build/smoke-test.stripped.log` (ANSI stripped) to grep klog/POST16 output.
- **Before handing the user a build to test on native Windows / bare metal** -- a green smoke test on KVM catches ~90% of the phase-0/1 regressions that used to be caught only by the user's boot.

**When NOT to run:**

- Every edit or every commit -- that's what the post-commit unit-test hook is for. Smoke test is advisory, not a gate.
- Non-kernel changes (user/, src/apps/, src/shell/ alone): unit tests already cover the testable surface.

**KVM vs TCG roles** (both work in WSL2 as of 2026-04-17):

| Platform | Speed | Catches | Misses |
|---|---|---|---|
| **KVM** (default when available) | ~2s boot | real-CPU MSR traps, SMP timing, cache semantics | emulated-device bugs (NVMe, USB); **timing-sensitive races its own speed hides** |
| **TCG** (fallback) | ~10s boot | device emulation bugs, portable x86 behavior; **timing-sensitive lifetime/ordering races** | per-CPU state, real MSR behavior |
| **CI-parity** (`CI_PARITY=1`, distro QEMU + forced TCG) | ~24s full suite | **exactly what CI will report, before you push** | anything needing a real CPU or real hardware |
| **WHPX / VBox / bare metal** (user runs) | varies | the rest | (authoritative) |

Neither WSL runner replaces native Windows or bare metal for shipping. They filter regressions before the user has to boot the image.

**TCG is not only a device-emulation net.** It also catches timing-sensitive races that KVM's speed hides, which is a stronger claim than the original table made and was paid for on 2026-07-27: a `SYS_EXEC` frame handoff depended on a timer tick landing in a window between publishing the new ring-3 frame and returning from the syscall. Under KVM and QEMU 10.2.1 the tick essentially always landed, so the bug was invisible; under the QEMU CI installs it did not, and the process returned to its pre-exec RIP over a freshly overwritten image. It survived **297 commits and a green local suite** that way. When a bug reproduces on one engine and not another, the engine difference IS the evidence -- do not dismiss it as an emulator quirk.

**Run the CI-parity tier before pushing.** `CI_PARITY=1 bash scripts/test.sh` pins the distro apt QEMU (what `.github/workflows/build.yml` installs) and forces TCG (CI has no reliable `/dev/kvm`). Measured 2026-07-27: **23.8s vs 25.6s for the default KVM run** -- indistinguishable, so there is no speed argument for skipping it. It is enforced by `.githooks/pre-push` as a REQUIRED gate (`SKIP_CI_PARITY=1` to override), unlike the opt-in heavy gate beside it: "CI already gates PRs" is precisely the assumption the 297 commits disproved. Version drift is detected rather than assumed -- `scripts/ci-qemu-version.txt` records the version CI installs and the gate warns when the local package stops matching, because a hardcoded pin silently stops tracking the day GitHub moves the runner image.

## Test Code Policy

Tests under `src/kernel/test/test_*.c` MUST NOT call live boot infrastructure (`boot_progress`, `vpd_*`, subsystem `_init`, `panic`, `boot_halt`, etc.) -- pre-commit hook enforces. Full forbidden table, allowed patterns, and `TEST-SIDE-EFFECT-ALLOWED` opt-out: [docs/infrastructure/test-policy.md](docs/infrastructure/test-policy.md). 3 incidents to date; rule was learned the hard way.

## Code Style: ASCII Dashes, No Bare Section Refs

Three enforced policies; full text and rationale at [docs/infrastructure/code-style-policies.md](docs/infrastructure/code-style-policies.md).

- **No Unicode dashes (U+2013/U+2014).** Windows serial and CMD garble them as mojibake. Don't paste `--` as typographic substitute either; rewrite (colon, semicolon, parens). PreToolUse hook blocks edits introducing them.
- **No bare `section`-sign + digit refs in source code comments.** Outside `.md` / `todo/` / `.claude/`, the glyph + digit is only legal when paired with an external-spec qualifier (UEFI, Intel SDM, RFC, ACPI, NVMe, PE/COFF, etc.). `scripts/lint.sh` Check 5 + PreToolUse hook enforce.
- **No hard-wrapped prose in `todo/`.** One paragraph per physical line; the reader's editor wraps it. A fill column makes a section inconsistent with the file around it and turns every later edit into a reflow (measured 2026-07-28 on TODO-21: sections 19-20 averaged 102 chars/line against 209 for the rest of the file). The 250-char cap on `- [ ]`/`- [x]`/`- [/]` LEAD lines still applies, so a long item keeps a short lead and puts its body on an indented continuation line, also unwrapped. `todo_wrap_reminder` warns at authoring time (never blocks); `python3 scripts/todo-reflow.py --diff|--write` repairs existing files and is wired into `validate-todo-file` step 2. The reflow refuses any change that is not purely line breaks, so don't hand-reflow a long TODO.
- **And a 1,000-char cap on the item BODY, split into sub-bullets rather than wrapped.** The lead cap binds only the lead, so bodies grew unbounded: every review round appended to the same continuation paragraph until TODO-04's QEMU-reap item carried eight separate `[high]` findings in one 4,865-char sentence-chain (that file alone held 27 lines over 1,200 chars). Measured 2026-07-30 across all 2,446 continuation lines under `todo/`: p50 74, p90 441, p97 861 -- the giants are anomalies, not the house style. The repair is NOT a fill column (that is the failure above); it is one indented `- ` sub-bullet per idea under a short lead. Sub-bullets are also the only multi-line body shape that survives the tooling: `todo-reflow.py:41` classifies an indented `-` line as `struct` and leaves it alone, whereas consecutive indented PROSE lines are joined back into one long line and a blank-separated indented paragraph is misread as an indented code block (`todo-reflow.py:63`). `todo_wrap_reminder` warns above the cap (never blocks, separate advisory budget from the hard-wrap reminder).

## Freestanding Kernel -- No stdlib

- No `<stdint.h>`, `<string.h>`, etc. -- use `#include "kernel/types.h"`
- No `malloc()`/`printf()` -- use `kmalloc()`, `pmm_alloc_contiguous()`, `printk()`
- `kmalloc()` for ≤ 4 KB only; `pmm_alloc_contiguous()` for everything larger

## Assembly -- NASM x86-64 only

- UEFI-era, Long Mode, APIC environment -- no BIOS/VGA/PIC assumptions

## API Surface -- Win32 native

- Win32 is the native API; POSIX via Linux compat layer only
- Canonical paths use Windows style: `C:\Impossible\System32\`

## Product North Star -- Complete, Compatible, Better

Impossible OS is not trying to be a partial Windows clone or a Linux-alike with renamed APIs. The target is broader: Win11 compatibility where that is the right surface, Linux compatibility where that expands reach, and a cleaner, faster, more refined system than either one.

**Completion-first, not checklist-first.** Finishing the listed checklist items is not enough if the feature is still obviously incomplete, fragile, poorly wired, or missing adjacent work that a real user would immediately hit next. When implementation reveals that nearby work is required for credible completeness, the agent must either:

- implement it now if it fits the current subsystem and one-session scope, or
- file it immediately in the owning TODO section or TODO file with a concrete checklist item and reciprocal XREF.

Do not ship happy-path completion with hidden adjacent gaps. "Done for this section" is not the bar; "credible, working, and properly owned" is the bar.

## Development Strategy -- Bare Metal First, SMP From Day One

**SMP-safe by default.** Every new feature must work correctly on multi-CPU systems. Never design single-CPU assumptions into the code -- use per-CPU data, proper locking, and atomic operations from the start. Windows NT was SMP from day one; Linux added it later and paid for it with the BKL for 20 years.

Bare metal is the target platform. VMs (QEMU, VBox) are convenience tools for fast iteration, not validation. Every feature must work on real hardware before it's done. "Verified on QEMU" is necessary but not sufficient -- "Verified on bare metal" is the acceptance criteria.

When writing hardware-touching code, ask: "does this work without a hypervisor?" Emulated hardware (Bochs VGA, forgiving LAPIC, trapped MMIO) hides bugs that crash on real CPUs.

**Never paper over test failures with platform workarounds.** When a test fails on one platform (WHPX, TCG, VBox), the instinct is to widen the assertion: `TEST_ASSERT(val == expected || val == whpx_default)`. This hides the root cause. Instead: diagnose WHY the value differs and fix the underlying code. A test that accepts two answers is a test that verifies nothing. Incident: 2026-04-13 PAT and MSR tests both initially got "accept WHPX default" workarounds; the real bug was per-CPU MSRs not being programmed on APs.

## boot_info ABI

`struct boot_info` at 0x10000 is the handoff contract between BOOTX64.EFI and kernel.exe. An 8-byte header at offset 0 contains magic (`0x49504F53` "IPOS"), version, and size. Phase 0 validates the handoff in two stages before touching it: `boot_info_validate_addr()` confirms the pointer is non-NULL, 8-byte aligned, and within the bootloader's 4 GiB identity map (`BOOT_INFO_EARLY_MAP_END`); `boot_info_validate_header()` then confirms magic, version, and size. Either failure halts with observed vs expected values on serial. **Never ship mismatched BOOTX64.EFI and kernel.exe after a `BOOT_INFO_VERSION` bump** -- the kernel will halt with "boot_info: bad header". Always rebuild both with `bash scripts/build.sh`. The kernel-side ABI lives in [`include/kernel/boot_info.h`](include/kernel/boot_info.h); the bootloader mirror lives in [`src/boot/uefi/boot_info_mirror.h`](src/boot/uefi/boot_info_mirror.h) (consumed by `bootx64.c` via `#include "boot_info_mirror.h"`). Bump `BOOT_INFO_VERSION` in BOTH headers together when adding/removing/reordering fields (not needed for new fields in `_reserved` regions). Drift detection has three layers: (1) compile-time `_Static_assert` blocks pin every load-bearing field offset and the total struct size in both mirror and kernel headers; (2) [`tools/boot-info-manifest/`](tools/boot-info-manifest/) generates a JSON manifest of every field and its offset/size on each build, fails fast on mirror-vs-kernel divergence (`scripts/build.sh` runs `compare.sh` as the first-line gate); (3) `src/kernel/main/boot_version.c` classifies header faults at Phase 0 with structured fault records.

## Bare Metal Gotchas

Hard-won rules from real hardware debugging. Violating any of these crashes on bare metal while looking fine in VMs. Full incident histories with dates, commit refs, and full rationale: [docs/infrastructure/bare-metal-gotchas.md](docs/infrastructure/bare-metal-gotchas.md). **Read that doc before touching kernel/boot code; the hooks do NOT catch most of these (they are code-pattern issues, not file-edit issues).**

Short-form list (group / rule):

- **ISR / IDT:** no LAPIC TPR writes in ISR path; no CLAC/STAC without SMAP CPUID; GS_BASE before any interrupt fires.
- **MMU:** no SMEP/SMAP until per-process page tables; no MMIO through WB pages (use `vmm_map_mmio_uc()`); CR3 reloads on WHPX can reset per-vCPU MSRs (re-program PAT after TLB flush); user-mode ELF range 0x800000-0x900000 is single source of truth.
- **SMP startup:** no Init Level De-Assert IPI (deprecated since P6, hangs WHPX); per-AP INIT->de-assert->SIPI is fine and required.
- **Devices:** NVMe I/O unreliable on QEMU WHPX (test on TCG); SATA on WHPX fine.
- **FPU/SIMD:** FXSAVE/XSAVE buffer needs FCW=0x037F + MXCSR=0x1F80; CR0.TS must be cleared before ALL FPU instructions; never compile SSE2 fallback code with `-mavx2` (split into separate translation units).
- **MSR probe:** `msr_try_read()` is no-crash guarantee, NOT existence check (gate on CPUID first); requires kernel IDT loaded (no MSR probes in boot_phase0).
- **Init ordering:** no `thread_create` for deferred init (compositor starves threads); per-thread fault isolation needs SEH (TODO-10).
- **Disk-sourced config:** dynamic buffers + hard-fail overflow, never silent truncate (boot.conf 2026-04-21 incident).

## Safety Gates

- **`include/kernel/mm/memmap.h` is the single source of truth for the virtual address-space layout.** Every top-level window base/extent (user, HHDM direct map, MMIO/fixmap, per-CPU, kernel image) is defined there and pinned by `_Static_assert`; never invent an address-space constant elsewhere. A sub-region carved inside a window may live with its owner (e.g. `user_range.h`) but must carry a containment assert against its window. Two rules the header exists to enforce: (1) **the HHDM and kernel-image phys<->virt relations are NOT interchangeable** -- a linker symbol is not a direct-map address; use `mm_image_virt_to_phys()` / `mm_hhdm_to_phys()`, which reject each other's inputs, never a blended `v - OFFSET`; (2) **never write an exclusive upper bound for the kernel image window** -- it ends at `UINT64_MAX`, so `base + size` wraps to 0 and turns `v < end` into a constant false that rejects every valid address. Use the inclusive `MM_KERNEL_IMAGE_LAST`. clang does NOT diagnose that wrap (not even under `-Weverything`); the gcc host gate `tools/memmap-check/check.sh` is the only automated net for it. Full layout + rationale: [docs/infrastructure/kernel-address-space.md](docs/infrastructure/kernel-address-space.md).
- **GDT user segment order is SYSRET-critical.** `GDT_USER_DATA` (0x18) MUST be before `GDT_USER_CODE` (0x20). SYSRET computes CS=STAR[63:48]+16 and SS=STAR[63:48]+8 with fixed offsets. Swapping these selectors corrupts every ring-3 return. Enforced by `_Static_assert` in `gdt.h`, runtime verification in `gdt_init()`, and unit test. Never reorder without understanding the SYSRET constraint.
- **User-mode pages need User bit at all 4 levels.** `vmm_set_user_page()` auto-splits 2 MiB huge pages on demand and propagates User at PML4, PDPT, PD, and PT levels. It works for ANY address now, not just the pre-split user ELF range. However, all per-process user pages currently share the kernel's physical frames (identity-mapped). True per-process isolation requires allocating unique physical pages per process -- planned for the full Win32 PE loader / VirtualAlloc implementation.
- **Guard pages protect all stack and heap boundaries.** `vmm_install_guard_page()` splits 2 MiB huge pages and clears the PTE, causing #PF on access. Page fault handler checks a 32-entry guard table and shows the label (e.g. "GUARD: kernel task stack overflow") instead of generic PAGE_FAULT. Guard pages installed at: kernel task stacks (bottom), AP stacks (bottom), IST stacks (DF/NMI/MCE, bottom), heap end, user ELF range end (0x900000). When allocating new stacks, always allocate N+1 pages and guard the bottom one.
- **Scheduler is flat-cyclic round-robin across ALL runnable threads.** `find_next_task()` walks every `(task, thread)` slot in a single global cyclic order starting one slot after the current one; the first-reached thread at the highest priority wins. Same-priority threads in different tasks are NOT favored over same-task siblings (or vice versa) -- everyone gets a fair turn. Code can rely on this: a `while (!cond) yield()` loop will not starve sibling kthreads. Counter-example fixed 2026-04-15: the old task-first nested scan starved a newly-created kthread when its caller cooperatively yielded (instead of `thread_join`-blocking) -- ALPC handshake tests timed out 5s waiting for a worker that the scheduler never picked.

Stop and ask before: security-sensitive changes, destructive operations, ABI changes, dependency additions, large refactors.

## Doc Sync

When you change code or conventions, update `CLAUDE.md`, `.claude/skills/`, and affected TODO files in the same task.

## Commits -- zero AI-attribution trailers

Impossible OS commits do NOT carry `Co-Authored-By: Claude`, `Assisted-by: TOOL:MODEL`, or similar AI-attribution trailers. AI assistance is implicit in the project's identity (Claude Code-orchestrated by design; see Authority Hierarchy in [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md#authority-hierarchy-read-this-first)), not a per-commit concern. This is a deliberate divergence from the Linux kernel 2025-12 `Assisted-by:` convention ([Documentation/process/coding-assistants.rst](https://docs.kernel.org/process/coding-assistants.html)) and Fedora's October 2025 adoption; the stance-change condition and the AI-slop screening bar are documented in [CONTRIBUTING.md "AI-Assisted Commit Policy"](CONTRIBUTING.md#ai-assisted-commit-policy-zero-trailer). If a commit in this repo carries such a trailer, the trailer is the bug; amend or rebase before merge.

## Autonomous-agent boundary -- interactive only

Impossible OS does NOT accept autonomous coding-agent PRs (Copilot cloud-agent, Devin, Cognition, or equivalent tools that run tasks in sandboxes and open PRs without per-step human authorship). Every commit is authored by a human operator working interactively with Claude Code. The repo deliberately does NOT ship `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`, or `.github/instructions/` -- their absence is policy, not omission. `AGENTS.md` and `CLAUDE.md` exist for cross-tool-pointer / doctrine-source purposes and are NOT autonomous-agent enablement. (`.github/copilot-instructions.md` previously sat alongside them as a Copilot-CLI reviewer-mode instruction file; both that file and `scripts/copilot-review.sh` were retired wholesale 2026-04-28 when the Copilot-CLI subordinate-reviewer role was dropped in favor of Codex review mode.) GitHub-side cloud-agent enablement (org/repo Settings -> Copilot -> Access policies) is a procedural guard; the regression pack cannot detect it. Full reasoning, MCP-server corollary, and stance-change condition: [docs/infrastructure/ai-system.md "Autonomous-Agent Boundary Policy"](docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy).

## Git Hooks

One command configures all repo-tracked hooks:

```bash
bash scripts/install-hooks.sh              # always-on: pre-commit lint + post-commit COUNT.md
bash scripts/install-hooks.sh --with-pre-push  # same + opt-in build/test pre-push gate
bash scripts/install-hooks.sh --status     # what is active now
bash scripts/install-hooks.sh --remove     # undo
```

Canonical path is `.githooks/` (activated by `core.hooksPath`). Manual edits under `.git/hooks/` are inert once configured. Mandatory gates are the pre-commit lint (fast, staged-file only) and the post-commit `COUNT.md` refresh; the pre-push build+test gate is opt-in because CI already gates PRs. Additionally, an always-on **overnight-runner control-plane gate** runs `scripts/overnight/tests/run-all.sh` at BOTH commit and push whenever the change touches [`scripts/overnight/control-plane-manifest.txt`](scripts/overnight/control-plane-manifest.txt) (the arm/launch/guard/breaker/review-dispatch code) -- a broken control-plane change can no longer reach an unattended night run with green hooks (`SKIP_RUNNER_SUITE=1` to override). The suite gate covers the FULL manifest; the separate ATTENDED-canary requirement (arm-sequencer.sh, guardrail Layer 4) was tiered 2026-07-12 so it re-arms only on FLOW-CRITICAL changes -- [`scripts/overnight/control-plane-deterministic.txt`](scripts/overnight/control-plane-deterministic.txt) is a strict allowlist of test-backed metrics/receipts/reporting files that are proven by the deterministic suite instead of a watched run; everything unlisted stays flow-critical (default-conservative). A second always-on conditional gate runs [`scripts/audit-hooks.sh`](scripts/audit-hooks.sh) (0.5s) whenever a commit stages `.claude/hooks/**` or `.claude/settings.json`, so a new hook cannot land without its [`.claude/hooks/MANIFEST.md`](.claude/hooks/MANIFEST.md) row (`SKIP_HOOK_AUDIT=1` to override). That audit already existed but its only caller was `scripts/test-tooling.sh`, which runs solely in the `Build` workflow on pushes to `main` and PRs against it -- unreachable from a long-lived feature branch, which is how two hooks shipped undocumented across 291 commits. Full lifecycle in [docs/infrastructure/development-tooling.md#local-ci-hooks](docs/infrastructure/development-tooling.md#local-ci-hooks); the six-layer guardrail system is documented in [todo/TODO-Claude-Overnight-Runner.md](todo/TODO-Claude-Overnight-Runner.md) "Guardrails against re-breaking the runner". Claude Code harness hooks (post-commit unit-test + boot-smoke advisories) live in [.claude/settings.json](.claude/settings.json) -- a separate system, not git hooks.

## Toolchain

- Compiler: `clang-19 --target=x86_64-elf`
- Assembler: `nasm`
- Linker: `ld.lld-19`
- Crash debug: `llvm-addr2line-19 -e build/kernel.exe -f <RIP>`

### Kernel Binary Format -- ELF, Permanently

The kernel image is ELF and stays ELF. This is a pinned decision, not a defer-till-later. Rationale: the kernel's file format is strictly internal (the UEFI bootloader at [src/boot/uefi/bootx64.c](src/boot/uefi/bootx64.c) is the only code that parses it); nothing user-visible or Win32-ABI-visible depends on it. The common intuition that "Windows compatibility requires a PE32+ kernel" is correlation, not causation -- Linux kernel + `.ko` modules are both ELF and Windows kernel + `.sys` drivers are both PE because each ecosystem standardized on one format, not because technical alignment was required. An ELF kernel can load PE32+ drivers, EIF native binaries, and ELF Linux binaries regardless; format choices are independent at every layer.

Switch condition (what would unpin this): ONLY if Impossible OS needs to run under a hypervisor or firmware that rejects ELF kernel images (niche -- QEMU KVM/TCG, WHPX, VirtualBox, and UEFI bare metal all accept ELF via our bootloader). Driver ecosystem reach, Win32 API compat, `.sys` loading, `ntoskrnl`-parity claims -- none of these require a PE32+ kernel image.

Driver and module formats are tracked separately in the [kernel module system TODO](todo/04-drivers-hardware/TODO-05-kernel-module-system.md) (kernel module loader) and the [EIF full implementation TODO](todo/02-kernel-core/TODO-20-eif-full-implementation.md) (EIF native binary format). Both accept multiple formats (EIF native + PE32+ for drivers) without influencing the kernel's own format.

## Model Roles

Use the strongest available Opus-class model for Claude implementation and judgment-heavy work such as `implement-todo-section`, `review-todo-section`, `create-todo`, `gap-audit-todo`, `quality-review-section`, and design-heavy roadmap shaping. Keep Codex review passes for adversarial review, consistency audit, and performance review.

External review does not replace self-review. The agent must always perform its own integration-level check for completeness, polish, regressions, parity gaps, and ownerless adjacent work.

**Codex reviewers are read-only; Codex write is interactive-rescue-only (Option A, 2026-07-11).** The two Codex roles are separated by SUBCOMMAND at the sandbox layer, not by trust: `codex-companion.mjs adversarial-review` (every review skill + the review broker) is pinned to a `read-only` sandbox in code and cannot mutate the tree; the WRITE surface is `codex … task --write` (workspace-write), reachable ONLY from an interactive operator session where you review Codex's diff and author the commit yourself (per-step human authorship -> the autonomous-agent boundary holds). Two guards enforce the separation: (1) `run_phase_guard.py` BLOCKs any write dispatch (`--write` / `--sandbox workspace-write|danger-full-access` / `--full-auto` / `--yolo`) in the unattended headless run (`[SEQ-CODEX-WRITE]`); (2) `codex_review_completed.py` firewalls a write dispatch OUT of `last-codex-review.json`, so an interactive rescue can never satisfy or queue a review gate it never performed. `runner_bash_guard.py` already blocks the entire Codex surface from any subagent. Net: reviewers can never write, and a writing Codex can never masquerade as a reviewer. Invoke rescue via `codex … task --write` or `Skill(codex:rescue)` interactively; codes: [docs/infrastructure/hook-codes.md](docs/infrastructure/hook-codes.md#seq-codex-write).

### Codex Invocation Policy

When Claude invokes Codex from any path (the `codex-*` skills, the Codex plugin slash commands, raw `node ...codex-companion.mjs ...`, or `codex exec` / `codex review` / `codex task`), Claude does NOT pass `--model` / `--effort` / `-m` / `-e` / `-c model=...` / `-c model_reasoning_effort=...` / `-c model_provider=...`. The user controls those centrally via `~/.codex/config.toml` (currently `model = "gpt-5.6-sol"`; the `model_reasoning_effort` value is a user dial -- low / medium / high are all legitimate and Claude must NEVER override or audit it).

Enforcement: PreToolUse hook [`.claude/hooks/codex_model_flag_block.py`](.claude/hooks/codex_model_flag_block.py) blocks via `shlex` tokenization + segment-by-control-operator + bypass-shape walk (env-prefix, wrappers, `&&` chains, global options before subcommand, heredoc-on-Codex). 29-case sub-test in [`scripts/test-tooling.sh`](scripts/test-tooling.sh). Full detection mechanics + bypass shapes + absolute-path note: [docs/infrastructure/codex-invocation-policy.md](docs/infrastructure/codex-invocation-policy.md). Opt-out: `CODEX_FLAG_OVERRIDE=1` per call.

### Codex Prompt Argument Escaping

Every Codex dispatch prompt MUST be **single-quoted** so bash never evaluates the prompt body. The canonical dispatch shape is the wrapper:

```bash
bash scripts/codex-dispatch.sh '[review-kind: <kind>] <todo-path> <body>'
```

The wrapper `exec`s into `node "$HOME/.../codex-companion.mjs" adversarial-review "$1"` after enforcing `argc == 1`. Hook recognition (`_review_kind.detect_review_kind_from_cmd`, `codex_review_completed.py _is_codex_bash_trigger`) accepts both the wrapper shape and the legacy direct-node shape so existing logs and ad-hoc invocations remain attributed.

Enforcement: `scripts/lint.sh` Check 12 scans `.claude/skills/`, `scripts/`, `docs/` for documented dispatch examples and WARNs when the prompt body is in DOUBLE quotes containing `$(...)` / `${...}` / unescaped `<` (bash would expand or redirect on those). The lint check is the load-bearing safety: argv-time validation in the wrapper cannot detect post-expansion damage. Single-quote at the source.

Opt-out: `SKIP_LINT_PROMPT_ESCAPING=1` per call.

## MCP Usage

Two MCP servers are wired in [`.mcp.json`](.mcp.json) at project scope: `todo-graph` (read-only TODO-graph queries) and `lsp-bridge` (LSP-backed code intelligence).

1. Prefer `mcp__lsp-bridge__definition` / `references` over `grep` for symbol queries -- the LSP returns the canonical answer; grep returns N partial matches that need disambiguation.
2. Prefer `mcp__todo-graph__ready` / `backlinks` / `code` over manual TODO walks -- one MCP call returns the structured answer that a multi-step grep+read traversal approximates.
3. **MCP-first is INTERACTIVE-first; scope it to "when the `mcp__` tools are actually loaded this session."** Measured 2026-07-03: headless `claude -p` connects project MCP servers asynchronously (~0.7s each) and does NOT block session init on the connection (no knob changes this -- `MCP_TIMEOUT` has no effect). On a cold start (every overnight launcher spawn + watchdog relaunch) the init tool-list snapshot catches the servers still `pending` ~2/3 of the time, so the headless session begins with **zero `mcp__` tools in its schema** and cannot call them. When `mcp__todo-graph__*` / `mcp__lsp-bridge__*` are absent, do NOT emulate them with manual `grep -n "^## N\."` section scans or `sed -n 'X,Yp'` reads -- use the DETERMINISTIC, always-available equivalents the MCP servers merely wrap: `python3 scripts/todo-graph/query.py <verb>` and `scripts/todo-graph/resolve_symbol.py` (via Bash), plus the **Grep / Glob tools** (never Bash `grep`/`sed`) and slice `Read(offset, limit)`. The Grep-tool-over-Bash-grep floor is MCP-independent and always applies.
4. Full doctrine (12 todo-graph + 15 lsp-bridge tools, when-to-use / when-not-to-use, cross-tool parity story, the headless-availability caveat) at [docs/infrastructure/mcp-usage.md](docs/infrastructure/mcp-usage.md).

## Skills

Claude Code skills live in `.claude/skills/`. They auto-load when Claude judges them relevant based on the `description` field. Each skill is self-contained. Impossible OS is Claude-primary for interactive work; Cursor was removed because the parallel skill set created clutter without a corresponding productivity win. The Copilot CLI subordinate-reviewer wrapper was retired wholesale 2026-04-28. Codex is used only in review mode: finding-only, received through the existing review discipline. The full ownership matrix lives at [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md).

| Skill | Description |
|---|---|
| `/implement-todo-section` | Implement one TODO section end-to-end |
| `/implement-todo-item` | Implement a single `[ ]` (or close a `[/]`) item; auto-promotes to section-ship when it closes the last `[ ]` |
| `/implement-ssdt-range` | Implement + wire a range of SSDT entries, mark Done [x] |
| `/complete-todo-file` | Finalize / loose-end sweep an active TODO; runs unit tests + verification + commits |
| `/overnight-sequencer` | Unattended whole-repo completion driver (headless, armed via `arm-sequencer.sh`). Fixpoint loop over every `todo/` file: triage -> validate -> gap-audit -> per-section implement/review -> close -> advance, hard-enforced by `run_phase_guard.py` (no deviation, no asking, no stop before fixpoint). Follows `todo/TODO-Claude-Overnight-Runner.md`. **Arm overnight runs ONLY via `bash .claude/skills/overnight-sequencer/arm-sequencer.sh` (`--with-browser` for gh-pages). NEVER arm via the plugin `overnight-runner:schedule` / `overnight-runner:start` -- that path engages no repo guard, so the headless run dies at the first background-review wait and thrashes on relaunch; the `overnight_plugin_skill_block` hook blocks it.** |
| `/overnight-todo-runner` | (Superseded by `/overnight-sequencer`; retained as interactive fallback pending retirement after first live sequencer run.) Drive ONE TODO file to completion interactively; Stop hook blocks final-answer between section ships. |
| `/create-todo` | Create a new TODO file |
| `/validate-todo-file` | Validate a TODO for structural gaps |
| `/verify-todo-section` | Audit-mode wrapper over `/review-todo-section` (downgrade-only) |
| `/quality-review-section` | Deep quality review (Win11/Linux parity, optimization) |
| `/review-todo-section` | Post-implementation review with mandatory adversarial + consistency + perf Codex |
| `/implement-unit-tests` | Implement a TODO's Unit Tests section |
| `/audit-ssdt` | Audit SSDT registration vs master tables |
| `/diagnose-serial-log` | Full serial-log audit (crashes / races / leaks / perf / drift) |
| `/debug-session` | Structured debug session with rubber-duck validation |
| `/todo-pipeline` | 3-stage TODO prep (validate -> gap analysis -> validate) |
| `/gap-audit-todo` | Gap audit vs Win11/Linux parity + cross-TODO overlap (mandatory secondary Codex pass via `codex-gap-audit`) |
| `/codex-gap-audit` | Codex red-team of a TODO gap-audit inventory (mandatory in `gap-audit-todo` Phase 3.5) |
| `/codex-design-review` | Codex pre-implementation design review |
| `/codex-review-todo` | Codex adversarial review of all implemented sections in a TODO |
| `/codex-adversarial-review-section` | Codex adversarial review for a single section (up to 3 rounds) |
| `/codex-fix-review` | Fix Codex findings, re-review until resolved |
| `/codex-test-coverage` | Codex test coverage gap analysis |
| `/codex-impact-analysis` | Codex dependency impact analysis |
| `/codex-consistency-audit` | Codex cross-file consistency audit |
| `/codex-perf-review` | Codex performance hot-path review |
| `/boot-code-quality` | Pre-flight checklist for UEFI boot code |
| `/kernel-code-quality` | Auto-load checklist for kernel C (SMP, memory, bare-metal) |
| `/desktop-code-quality` | Pre-flight checklist for desktop compositor code (placeholder) |
| `/shell-code-quality` | Pre-flight checklist for command shell code (placeholder) |
| `/userland-code-quality` | Pre-flight checklist for user-mode applications (placeholder) |

> Adding, editing, or retiring a skill: see [docs/infrastructure/skill-authoring.md](docs/infrastructure/skill-authoring.md) for the lifecycle, template, and catalog hygiene rules. Every live skill has a row here AND in [.claude/skills/README.md](.claude/skills/README.md); a directory without matching rows is drift.

### Specialist agents -- advisory, read-only

Twenty-one subagents in [`.claude/agents/`](.claude/agents/) that the pipeline skills delegate work to, in two capability-bounded classes (enforced by `scripts/lint.sh` Check 14):

- **Analysts (sensors):** read-only (`Read`/`Grep`/`Glob`, plus `WebSearch`/`WebFetch` for the researchers). They return findings as text and never edit, build, commit, dispatch Codex, or invoke skills.
- **Runners (constrained executors, `<!-- agent-class: runner -->`):** add `Bash` for named idempotent commands only -- verification scripts (checks-runner), read-only git archaeology (git-historian), read-only GitHub queries (gh-query-runner). They never edit files, never run git/gh mutations, never push, and NEVER touch Codex (a runner-issued dispatch would corrupt review-receipt state). Verification-before-completion quotes come from the main session's own read of the on-disk artifact, not the runner's report.

In both classes the main session remains the sole mutator/committer/Codex-dispatcher, so the section-commit gate, phase guard, and evidence binding are untouched. Design + rationale: the 2026-06-20 specialist-agents design doc, retired from the tree 2026-07-03 (git history: `docs/superpowers/specs/2026-06-20-overnight-specialist-agents-design.md`; original five-agent roster, superseded note inside).

| Agent | Model | Dispatched by |
|---|---|---|
| `kernel-explorer` | sonnet | `implement-todo-section` step 3 (kernel/boot exploration) + `debug-session` step 4 (call-path walk) |
| `section-context-mapper` | sonnet | `implement-todo-section` step 3 default for non-kernel surfaces + any interactive task crossing the offload threshold (bounded section package: behavior, source+tests, callers/callees, shared state, XREF status, ranked edit targets) |
| `concurrency-evidence-mapper` | sonnet | `review-todo-section` step 7 (parallel with kernel-quality-auditor) + `implement-todo-section` step 3 on SMP-heavy sections (lock/atomic/IRQ/ownership/refcount/teardown INVENTORY, no verdicts -- the Opus auditor stays the authority) |
| `kernel-quality-auditor` | opus | `review-todo-section` step 7 (SMP/bare-metal gate walk) |
| `boot-quality-auditor` | sonnet | `review-todo-section` step 7 (UEFI gate walk) |
| `parity-research-analyst` | sonnet | `gap-audit-todo` Phase 2-3 + `review-todo-section` steps 9-12 + `create-todo` research (research-scale parity questions only; 1-row OS-table syncs stay main-session) |
| `review-evidence-mapper` | sonnet | `review-todo-section` Phase 1 + `quality-review-section` step 1 (pre-Codex evidence map; supplements, does not replace, the main session's >=2 gate reads) |
| `diagnostic-digester` | sonnet | `implement-todo-section` fix loop + `review-todo-section` build-fail (failure-log digest; hypotheses validated by main session before fixing) |
| `todo-hygiene-auditor` | sonnet | `complete-todo-file` close-out (fuzzy-residue hygiene punch-list; script-verified before applied) |
| `ssdt-auditor` | sonnet | `audit-ssdt` scan + `implement-ssdt-range` pre-read (registration-vs-master-table mismatch report / range worklist) |
| `serial-log-auditor` | sonnet | `diagnose-serial-log` step 2 + `debug-session` multi-log evidence (full-set anomaly timeline; mechanical detectors stay main-session) |
| `test-coverage-mapper` | sonnet | `implement-unit-tests` step 1 (spec status, quoted signatures, runner wiring points) |
| `doc-sync-auditor` | sonnet | `complete-todo-file` close-out (CLAUDE.md/skills/docs drift vs shipped work) |
| `spec-research-analyst` | sonnet | `implement-todo-section` step 3 + `debug-session` (external-spec normative facts with citations) |
| `web-research-analyst` | sonnet | `implement-todo-section` step 3 + `debug-session` (toolchain/emulator/host/CI research; parity + spec stay with their specialists) |
| `xref-dependency-mapper` | sonnet | `implement-todo-section` step 2 (3+ cross-TODO XREFs: dependency-status brief) |
| `overnight-log-explorer` | sonnet | ad-hoc + `overnight-runner-improvements` backlog work (run-transcript cost/efficiency digest: tool-call accounting, wait/poll waste, churn; serial logs stay with serial-log-auditor) |
| `todo-validation-mapper` | sonnet | `validate-todo-file` / `todo-pipeline` legwork + ad-hoc "is this TODO sound?" (structural evidence map: sections vs IO table, stamps, XREF existence, Inputs paths) |
| `checks-runner` (runner) | sonnet | **DEPRECATED for green mechanics (2026-07-11): a passing build/test/CI query spends no model.** Build/suite/smoke now run through `scripts/overnight/run-artifact.sh` (deterministic JSON envelope); CI status through a direct `gh run list --json` query; only a FAIL envelope routes to `diagnostic-digester`. Runner retained solely for a genuinely output-flooding verbatim run the wrapper cannot bound. |
| `git-historian` (runner) | sonnet | `debug-session` + `diagnose-serial-log` regression pass (read-only git archaeology: when did X change/break) |
| `gh-query-runner` (runner) | sonnet | `complete-todo-file` post-push CI check + ad-hoc GitHub state queries (read-only gh: runs/PRs/issues/api GET) |

Model doctrine (updated with the Claude 5 family): **sonnet is the default for every read-only analyst** -- the trust contract (main session verifies findings at file:line before acting) plus the downstream nets (Codex red-team, build/test gates, bare-metal validation) are the backstop. Opus is reserved for the thin/sole nets; today that is only `kernel-quality-auditor` (sole SMP/lock-order/bare-metal specialist net for the repo's most expensive bug class). The read-only tool allowlist is enforced by `scripts/lint.sh` Check 14.

**Interactive offload -- default-on, not pipeline-only.** The fleet applies to interactive sessions the same as to skill pipelines; the main session orchestrates and judges, agents do the legwork. **Routing threshold: a task that would take 3+ search/read rounds or ingest more than ~50 KB goes to an agent; 1-2-read tasks stay inline** (below that line, agent startup + summarization costs more than it saves). Executor split (HARD -- green mechanics NEVER spend a model): waiting / triage / graph checks / green builds+tests / CI status -> deterministic scripts (`run-artifact.sh`, `preflight.py`, `sequencer_triage.py`, `gh run list --json`); FAILURE digests / exploration / call-path mapping / logs / XREFs / coverage / research / hygiene -> Sonnet agents; design decisions, edits, security/ABI/SMP judgment, Codex-finding triage -> the main session. The decision ladder for any offloadable task is ORDERED: deterministic script -> cached report (`agent_result_cache`) -> Sonnet analyst -> Opus judgment; stop at the first that answers. Default routes: TODO structural validation -> `scripts/todo-graph/*` + `todo-validation-mapper` for the fuzzy residue only; build/test/lint/smoke runs -> `run-artifact.sh` (deterministic; `diagnostic-digester` on FAIL only); kernel/boot exploration or call-path tracing -> `kernel-explorer` (any other code surface -> `section-context-mapper`; generic repo search -> `Explore`); concurrency inventory (locks/atomics/IRQ/ownership/refcounts/teardown -- the heap-ownership class extends THIS mapper, never a new agent) -> `concurrency-evidence-mapper`; "when did X break/change" -> `git-historian`; CI status -> a direct `gh run list --json` query (deterministic; `gh-query-runner` ONLY for paginated `--log-failed` slices on a confirmed-red run); serial/boot logs -> `serial-log-auditor`; overnight-run transcripts -> `overnight-log-explorer`; coverage gaps -> `test-coverage-mapper`; doc drift -> `doc-sync-auditor`; parity/spec/toolchain research -> the three researchers. An agent dispatch must REPLACE expected main-session reads, never add a layer on top of doing them anyway. Identical dispatches over unchanged content are auto-served from the content-addressed agent-result cache (`agent_result_cache` hook) -- reuse the returned report; change the prompt to force a fresh run. Never delegate edits, commits, Codex dispatch, finding classification, or security/ABI decisions to an agent. The `interactive_offload_router.py` UserPromptSubmit hook injects a hint when a prompt matches one of these shapes (warning-only, never blocks); load-bearing findings are still verified at file:line by the main session (trust contract), and verification-before-completion quotes still come from the main session's own read of the artifact.

**Typed evidence envelopes (Fable-style typed-result discipline).** The 11 FINDINGS-shaped analysts (the auditors + mappers) emit the `review-result-v1` JSON envelope; the 6 NARRATIVE analysts (the three researchers, `diagnostic-digester`, `serial-log-auditor`, `overnight-log-explorer`) deliberately do NOT -- forcing a citation narrative or an anomaly timeline into a findings array degrades it. Where an analyst returns structured findings, it emits the `review-result-v1` JSON envelope (`{schema, scope_digest, coverage[], findings[{severity,file,line,summary}], unknowns[], confidence, artifact?}`) validated mechanically by `scripts/overnight/evidence-schema.py` -- so the main session receives compact typed facts, not prose transcripts, and a malformed envelope is REJECTED by a script (no model spent judging completeness). The command wrappers already do this: `run-artifact.sh` (build/test), `review-envelope.py` (combined Codex legs), `preflight.py` (preflight verdict).

**Cost is the hard constraint (2026-07-11).** Every routing change is cost-neutral or cost-negative: green build/test/CI/triage/graph mechanics spend NO model (deterministic scripts); Sonnet is used only to digest a FAILURE or replace substantial Opus reading, and a dispatch that would merely supplement reading Opus does anyway is forbidden; the content-addressed agent cache (now hashing untracked file CONTENTS, not just names) makes repeat dispatches free and logs `cache-hit` token-savings estimates. Full doctrine + the net-savings scorecard: [todo/TODO-Claude-Overnight-Runner.md](todo/TODO-Claude-Overnight-Runner.md) "HARD COST CONSTRAINT".

**Runner lineage.** The overnight-runner architecture was co-evolved with the external `~/runner-kit` (lineage: impossible-os -> Systific -> kit -> retrofitted here 2026-07-03); the kit was retired 2026-07-04 and this repo is now the sole authority for its runner. Load-bearing specializations that stay: the build-graph triage oracle (todo-cache.json + dual stamps), `PHASE_ALLOWED_SKILLS` hard skill-sequence enforcement, the phase-transition CLI, and the sole-mutator doctrine (no mutating executor agents by design).

### Plugin skills -- usage notes

The three installed Claude Code plugins (`superpowers`, `feature-dev`, `explanatory-output-style`) ship their own skills + commands + agents that are NOT in the table above. Treat them as helpers, not replacements:

- **`superpowers@claude-plugins-official`** -- load-bearing. `superpowers:receiving-code-review` is wired into all 9 codex-* skills + a hook gate; `superpowers:verification-before-completion` is wired into `review-todo-section` step 15.5 and `complete-todo-file` Execution Discipline. Other useful skills (referenced in specific in-repo skills): `systematic-debugging`, `test-driven-development`, `dispatching-parallel-agents`, `requesting-code-review`. Never disable the plugin.
- **`feature-dev@claude-plugins-official`** -- agents (`code-architect`, `code-explorer`, `code-reviewer`) are usable as helpers. The `/feature-dev` slash command is **NOT the primary path** for feature work in this repo: it skips the Codex adversarial-review pipeline + the section-commit gate that `/implement-todo-section` enforces. Treat `/feature-dev` as a brainstorming aid, not a shipping path. `feature-dev:code-explorer` IS referenced as an Explore-agent alternative in `implement-todo-section` step 3 for very-large unfamiliar codebases.
- **`explanatory-output-style@claude-plugins-official`** -- SessionStart hook only; injects "explanatory mode" instructions (the `★ Insight ─` blocks). Not a skill, no triggers; benign but adds ~400 tokens per session.

> **Removed:** `firecrawl@claude-plugins-official` was uninstalled 2026-04-27. The plugin required `FIRECRAWL_API_KEY` + an external CLI binary that weren't installed on the dev host, so every dispatch silently fell back to WebSearch / WebFetch. The cost-benefit didn't favor keeping the inactive plugin around.

## Mandatory Skill Triggers

These are non-negotiable. Auto-loading by description is unreliable -- the rules below apply regardless of whether the skill description "felt relevant." Hooks in `.claude/settings.json` enforce the first two by injecting reminders into the conversation.

| Trigger | Skill | Why |
|---|---|---|
| Edit/Write on `src/boot/` | `boot-code-quality` | UEFI error handling, EBS boundary, table safety, fallback chains. |
| Edit/Write on `src/kernel/` or `include/kernel/` | `kernel-code-quality` | SMP, memory rules, bare-metal, POST16 boot-path only. |
| Edit/Write on `src/desktop/` | `desktop-code-quality` | WC framebuffer mapping, compositor loop, pixel format. |
| Edit/Write on `src/shell/` | `shell-code-quality` | Win32 console API, Windows paths. |
| Edit/Write on `user/` or `src/apps/` | `userland-code-quality` | user libc, syscall interface, no kernel headers. |
| Codex skill or `adversarial-review` returns findings | `superpowers:receiving-code-review` | Reviewer not authority: verify each finding at file:line, classify Fix/Reject/Accept with evidence; no blind-implement, no performative agreement. |
| Implementing a TODO section | `implement-todo-section` | Steps 13-18 (Codex review, fix loop, self-review, build, validate, tie up loose ends) are mandatory before commit. Every section runs the full pipeline: design + adversarial + consistency + perf + conditional re-adversarial + fix loop. Spiral-check fires a `systemMessage` reminder at 2x and 4x the uniform 60-min wall-clock budget. |
| Commit + push of a TODO section-ship (`git commit` flips `\| \[x\] \|` in IO table) | `review-todo-section` | Next tool call after `git push` MUST be `Skill(review-todo-section, ...)`. PreToolUse hook BLOCKs Edit/Write/non-review-Skill/non-script-Bash if HEAD has `[x]` flip without `**Verified:**` stamp. Step 5 (adversarial) and step 8 (quality consistency + perf) are separate dispatches. Opt-out (`SKIP_REVIEW_HOOK=1`) for: revert; stamp-only; or a post-review fix implementing the reviewer's exact recommendation, re-verified by full suite+smoke. |
| Test file in `src/kernel/test/` | `implement-unit-tests` | Wire to `test_runner.c`, correct `TEST_CAT_*`, concrete assertions. |
| Test asserts `STATUS_NOT_IMPLEMENTED` / `E_NOTIMPL` / `ENOSYS` as expected | `implement-unit-tests` "TEST_PENDING vs TEST_ASSERT" | Use `TEST_PENDING(cond, msg)`. Counts in `pending` bucket, renders `[STUB]` in boot log. Never pair with runtime `klog(LOG_WARN, ...)` (duplicate signal). |
| Writing `Accepted:` / `Deferred:` XREF in a TODO stamp | `review-todo-section` step 15 | Every XREF must name a concrete `[ ]` item in the target (`(item: "NAME" at line N)` or named helper). Bare `section`-refs and paraphrase parentheticals rejected. PostToolUse reminder + git-commit BLOCK enforce. |
| About to claim work is complete, fixed, or passing (before commit / PR / Verified stamp) | `superpowers:verification-before-completion` | Re-run the relevant build / test / lint commands and quote the actual output BEFORE asserting success. Wired into `review-todo-section` step 15.5 and `complete-todo-file` Execution Discipline. Closes the `feedback_never_skip_review` failure mode where claims were made without re-running. |
| Bug, test failure, or unexpected behavior, before proposing a fix | `superpowers:systematic-debugging` | Build a hypothesis from observed evidence, identify root cause, validate before patching. Repo's `debug-session` cross-references it. Bypassing this step into "I'll just try changing X" is the bug-whack-a-mole pattern. |

If a skill is listed above, "I forgot" is not a valid excuse. The hooks will remind you; act on the reminder. Per-skill verdicts and the suppression rules for the rest of the superpowers catalog (e.g. `subagent-driven-development` forbidden on kernel/boot paths) live at [docs/infrastructure/superpowers-policy.md](docs/infrastructure/superpowers-policy.md).

## Repository Layout

```
src/
├── boot/uefi/      Custom UEFI bootloader
├── kernel/         Kernel core (PMM, VMM, scheduler, VFS, drivers)
├── desktop/        Compositing desktop shell
├── shell/          Command-line shell
└── libc/           Minimal kernel libc
include/            All headers (mirrors src/)
resources/          Fonts, icons, wallpapers
todo/               Development roadmap (14 domains, 86 TODO files)
.claude/            Claude Code AI system (skills + settings + hooks)
```
