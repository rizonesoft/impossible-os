# CLAUDE.md -- Impossible OS

> Claude Code project instructions for the Impossible OS kernel. This file and `.claude/skills/` are the complete Claude Code system -- self-contained, no shared layers.

## Build -- Never use raw `make`

```bash
bash scripts/build.sh           # incremental
bash scripts/build.sh clean     # full clean
bash scripts/build.sh run       # build + QEMU
```
Check `tail -1 build/build.log` for result -- must show `=== BUILD OK ===`.

Host bootstrap (deps, supported distros, required-tool sentinels, idempotence): [docs/infrastructure/development-tooling.md#host-bootstrap-contract](docs/infrastructure/development-tooling.md#host-bootstrap-contract). Host profiles (support tiers, min versions, devcontainer, unsupported-host policy): [docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments](docs/infrastructure/development-tooling.md#supported-host-profiles-and-reproducible-environments). Re-check with `bash scripts/setup.sh --verify`; version floors via `bash scripts/setup.sh --versions`.

## Testing -- Category-Based Test Infrastructure

> **TCG works in WSL2** (~10s for build + 1493 tests). A post-commit hook runs the full suite after every kernel/source commit. SMEP is disabled globally (boot PML4 has User bit on all pages). WHPX, VirtualBox, and bare metal remain the primary validation platforms.

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
make test-abi                     # PEB/TEB + Registry suites
make test-storage                 # AHCI, VirtIO suites
make test-exec                    # Binary system suites (exec, EIF, modules)
```

Categories: `mm`, `fs`, `sched`, `ob`, `security`, `ipc`, `boot`, `abi`, `storage`, `exec`. Configured via `test_suite=` and `test_quiet=` in `boot.conf`. Windows: `scripts/debug/kernel/run-mm-tests.bat` etc.

Register new tests with `test_suite_register_cat("name", fn, TEST_CAT_XX)`. Use `TEST_ASSERT_EQ(a, b, msg)` for value comparisons and `TEST_SKIP(msg)` for hardware-dependent tests.

## Smoke Test -- End-to-End Boot Validation

```bash
bash scripts/test-smoke.sh        # builds, boots in QEMU, checks for Boot complete + C:\>
```

The smoke test complements unit tests: unit tests cover individual behaviors against in-memory fixtures, the smoke test proves **the image actually boots to userspace**. It auto-selects KVM if `/dev/kvm` is writable and falls back to TCG otherwise. 30-second timeout, fail-fast on missing markers.

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
| **KVM** (default when available) | ~2s boot | real-CPU MSR traps, SMP timing, cache semantics | emulated-device bugs (NVMe, USB) |
| **TCG** (fallback) | ~10s boot | device emulation bugs, portable x86 behavior | per-CPU state, real MSR behavior |
| **WHPX / VBox / bare metal** (user runs) | varies | the rest | (authoritative) |

Neither WSL runner replaces native Windows or bare metal for shipping. They filter regressions before the user has to boot the image.

## Test Code Policy

Tests under `src/kernel/test/test_*.c` MUST NOT call live boot infrastructure (`boot_progress`, `vpd_*`, subsystem `_init`, `panic`, `boot_halt`, etc.) -- pre-commit hook enforces. Full forbidden table, allowed patterns, and `TEST-SIDE-EFFECT-ALLOWED` opt-out: [docs/infrastructure/test-policy.md](docs/infrastructure/test-policy.md). 3 incidents to date; rule was learned the hard way.

## Code Style: ASCII Dashes, No Bare Section Refs

Two enforced policies; full text and rationale at [docs/infrastructure/code-style-policies.md](docs/infrastructure/code-style-policies.md).

- **No Unicode dashes (U+2013/U+2014).** Windows serial and CMD garble them as mojibake. Don't paste `--` as typographic substitute either; rewrite (colon, semicolon, parens). PreToolUse hook blocks edits introducing them.
- **No bare `section`-sign + digit refs in source code comments.** Outside `.md` / `todo/` / `.claude/`, the glyph + digit is only legal when paired with an external-spec qualifier (UEFI, Intel SDM, RFC, ACPI, NVMe, PE/COFF, etc.). `scripts/lint.sh` Check 5 + PreToolUse hook enforce.

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

`struct boot_info` at 0x10000 is the handoff contract between BOOTX64.EFI and kernel.exe. An 8-byte header at offset 0 contains magic (`0x49504F53` "IPOS"), version, and size. Phase 0 validates the handoff in two stages before touching it: `boot_info_validate_addr()` confirms the pointer is non-NULL, 8-byte aligned, and within the bootloader's 4 GiB identity map (`BOOT_INFO_EARLY_MAP_END`); `boot_info_validate_header()` then confirms magic, version, and size. Either failure halts with observed vs expected values on serial. **Never ship mismatched BOOTX64.EFI and kernel.exe after a `BOOT_INFO_VERSION` bump** -- the kernel will halt with "boot_info: bad header". Always rebuild both with `bash scripts/build.sh`. Bump `BOOT_INFO_VERSION` in both `include/kernel/boot_info.h` and `src/boot/uefi/bootx64.c` when adding/removing/reordering fields (not needed for new fields in `_reserved` regions). Five per-field offset asserts on the mirror structs catch same-size reorders between kernel and bootloader images.

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

Impossible OS does NOT accept autonomous coding-agent PRs (Copilot cloud-agent, Devin, Cognition, or equivalent tools that run tasks in sandboxes and open PRs without per-step human authorship). Every commit is authored by a human operator working interactively with Claude Code. The repo deliberately does NOT ship `.github/workflows/copilot-setup-steps.yml`, `.github/agents/`, `.github/chatmodes/`, or `.github/instructions/` -- their absence is policy, not omission. `.github/copilot-instructions.md`, `AGENTS.md`, and `CLAUDE.md` exist for reviewer-mode / cross-tool-pointer / doctrine-source purposes and are NOT autonomous-agent enablement. GitHub-side cloud-agent enablement (org/repo Settings -> Copilot -> Access policies) is a procedural guard; the regression pack cannot detect it. Full reasoning, MCP-server corollary, and stance-change condition: [docs/infrastructure/ai-system.md "Autonomous-Agent Boundary Policy"](docs/infrastructure/ai-system.md#autonomous-agent-boundary-policy).

## Git Hooks

One command configures all repo-tracked hooks:

```bash
bash scripts/install-hooks.sh              # always-on: pre-commit lint + post-commit COUNT.md
bash scripts/install-hooks.sh --with-pre-push  # same + opt-in build/test pre-push gate
bash scripts/install-hooks.sh --status     # what is active now
bash scripts/install-hooks.sh --remove     # undo
```

Canonical path is `.githooks/` (activated by `core.hooksPath`). Manual edits under `.git/hooks/` are inert once configured. Mandatory gates are the pre-commit lint (fast, staged-file only) and the post-commit `COUNT.md` refresh; the pre-push build+test gate is opt-in because CI already gates PRs. Full lifecycle in [docs/infrastructure/development-tooling.md#local-ci-hooks](docs/infrastructure/development-tooling.md#local-ci-hooks). Claude Code harness hooks (post-commit unit-test + boot-smoke advisories) live in [.claude/settings.json](.claude/settings.json) -- a separate system, not git hooks.

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

Use the strongest available Opus-class model for implementation and judgment-heavy work such as `implement-todo-section`, `review-todo-section`, `create-todo`, `gap-analysis-todo`, `quality-review-section`, and design-heavy roadmap shaping. Keep Codex and GPT-5.4 style external review passes for adversarial review, consistency audit, and performance review.

External review does not replace self-review. The agent must always perform its own integration-level check for completeness, polish, regressions, parity gaps, and ownerless adjacent work.

### Codex Invocation Policy

When Claude invokes Codex from any path (the `codex-*` skills, the Codex plugin slash commands, raw `node ...codex-companion.mjs ...`, or `codex exec` / `codex review` / `codex task`), Claude does NOT pass `--model` / `--effort` / `-m` / `-e` / `-c model=...` / `-c model_reasoning_effort=...` / `-c model_provider=...`. The user controls those centrally via `~/.codex/config.toml` (currently `model = "gpt-5.5"`, `model_reasoning_effort = "medium"`).

Enforcement: PreToolUse hook [`.claude/hooks/codex_model_flag_block.py`](.claude/hooks/codex_model_flag_block.py) blocks via `shlex` tokenization + segment-by-control-operator + bypass-shape walk (env-prefix, wrappers, `&&` chains, global options before subcommand, heredoc-on-Codex). 29-case sub-test in [`scripts/test-tooling.sh`](scripts/test-tooling.sh). Full detection mechanics + bypass shapes + absolute-path note: [docs/infrastructure/codex-invocation-policy.md](docs/infrastructure/codex-invocation-policy.md). Opt-out: `CODEX_FLAG_OVERRIDE=1` per call.

## Skills

Claude Code skills live in `.claude/skills/`. They auto-load when Claude judges them relevant based on the `description` field. Each skill is self-contained. Impossible OS is Claude Code-only as of 2026-04-18; Cursor was removed because the parallel skill set created clutter without a corresponding productivity win. The full ownership matrix (Claude master, Codex/Copilot subordinate reviewers, edit-here-not-there rules) lives at [docs/infrastructure/ai-system.md](docs/infrastructure/ai-system.md).

| Skill | Description |
|---|---|
| `/implement-todo-section` | Implement one TODO section end-to-end |
| `/implement-ssdt-range` | Implement + wire a range of SSDT entries, mark Done [x] |
| `/complete-todo-file` | Finalize / loose-end sweep an active TODO; runs unit tests + verification + commits |
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
| `/gap-analysis-todo` | Gap analysis vs Win11/Linux parity + cross-TODO overlap |
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

### Plugin skills -- usage notes

The four installed Claude Code plugins (`superpowers`, `feature-dev`, `firecrawl`, `explanatory-output-style`) ship their own skills + commands + agents that are NOT in the table above. Treat them as helpers, not replacements:

- **`superpowers@claude-plugins-official`** -- load-bearing. `superpowers:receiving-code-review` is wired into all 9 codex-* skills + a hook gate; `superpowers:verification-before-completion` is wired into `review-todo-section` step 15.5 and `complete-todo-file` Execution Discipline. Other useful skills (referenced in specific in-repo skills): `systematic-debugging`, `test-driven-development`, `dispatching-parallel-agents`, `requesting-code-review`. Never disable the plugin.
- **`feature-dev@claude-plugins-official`** -- agents (`code-architect`, `code-explorer`, `code-reviewer`) are usable as helpers. The `/feature-dev` slash command is **NOT the primary path** for feature work in this repo: it skips the Codex adversarial-review pipeline + the section-commit gate that `/implement-todo-section` enforces. Treat `/feature-dev` as a brainstorming aid, not a shipping path. `feature-dev:code-explorer` IS referenced as an Explore-agent alternative in `implement-todo-section` step 3 for very-large unfamiliar codebases.
- **`firecrawl@claude-plugins-official`** -- preferred over built-in `WebSearch` / `WebFetch` for Win11/Linux parity research (`gap-analysis-todo` Phase 2, `create-todo` step 2) and as the canonical bootstrap recipe for new skills derived from external docs (`firecrawl:skill-gen`, see [skill-authoring.md](docs/infrastructure/skill-authoring.md) step 1).
- **`explanatory-output-style@claude-plugins-official`** -- SessionStart hook only; injects "explanatory mode" instructions (the `★ Insight ─` blocks). Not a skill, no triggers; benign but adds ~400 tokens per session.

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
| Implementing a TODO section | `implement-todo-section` | Steps 13-18 (Codex review, fix loop, self-review, build, validate, tie up loose ends) are mandatory before commit. |
| Commit + push of a TODO section-ship (`git commit` flips `\| \[x\] \|` in IO table) | `review-todo-section` | Next tool call after `git push` MUST be `Skill(review-todo-section, ...)`. PreToolUse hook BLOCKs Edit/Write/non-review-Skill/non-script-Bash if HEAD has `[x]` flip without `**Verified:**` stamp. Step 5 (adversarial) and step 8 (quality consistency + perf) are separate dispatches. Opt-out for revert / stamp-only: `SKIP_REVIEW_HOOK=1`. |
| Test file in `src/kernel/test/` | `implement-unit-tests` | Wire to `test_runner.c`, correct `TEST_CAT_*`, concrete assertions. |
| Test asserts `STATUS_NOT_IMPLEMENTED` / `E_NOTIMPL` / `ENOSYS` as expected | `implement-unit-tests` "TEST_PENDING vs TEST_ASSERT" | Use `TEST_PENDING(cond, msg)`. Counts in `pending` bucket, renders `[STUB]` in boot log. Never pair with runtime `klog(LOG_WARN, ...)` (duplicate signal). |
| Writing `Accepted:` / `Deferred:` XREF in a TODO stamp | `review-todo-section` step 15 | Every XREF must name a concrete `[ ]` item in the target (`(item: "NAME" at line N)` or named helper). Bare `section`-refs and paraphrase parentheticals rejected. PostToolUse reminder + git-commit BLOCK enforce. |

If a skill is listed above, "I forgot" is not a valid excuse. The hooks will remind you; act on the reminder.

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
