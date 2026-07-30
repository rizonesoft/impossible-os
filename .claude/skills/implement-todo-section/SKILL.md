---
name: implement-todo-section
description: Execute one bounded TODO section, resolve XREF dependencies, run Codex adversarial review, self-review for regressions, validate section, tie up loose ends, and commit. Use when implementing a specific TODO section or a clearly scoped subset of one.
---

# Implement TODO Section

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

> `review-todo-section` owns the full post-commit quality pipeline invoked at step 20. `verify-todo-section` is a thin audit-mode wrapper over review (same workflow, downgrade-only stance). Stamp field rules live in review; both this skill and verify reference them.

## Execution Discipline

> **NO CODE SHIPS WITHOUT BEING EXAMINED FROM EVERY ANGLE.**
>
> You cut corners: skipping Codex, self-reviewing instead of dispatching, accepting "clean" without walking quality gates, skipping the post-implementation review pipeline. Your judgment about which steps to skip has been wrong. Follow every step mechanically.
>
> - **Follow the plan, but do not stop at paper completion.** The checklist items define the planned scope, not the completion boundary. Do not widen aimlessly, but do expand when adjacent work is required for correctness, credible completeness, or obvious polish.
> - **Every Codex dispatch is mandatory.** Step 13 adversarial + step 20 review pipeline. No exceptions.
> - **Every finding gets fixed.** Not noted. Not accepted unless it truly needs missing infrastructure.
> - **Domain code quality gates get walked explicitly.** Not just the hook reminder.
> - **One section = one commit.** Each section is a milestone with a clean commit boundary.
> - **Completion-first rule.** If the section's user-visible outcome still looks obviously incomplete, fragile, poorly wired, or missing the next adjacent capability a real user would hit, either implement that adjacent work now under Branch A rules or file it immediately in the owning TODO with a concrete checklist item and reciprocal XREF.

## Workflow

1. **Read the section** -- full text, notes, test checkpoint, warning boxes.
   One full read per pass is orientation; every LATER read of the TODO or a
   big source in this pass is a slice: `Read(offset, limit)` around the
   section text / IO-table row / function being edited, and after an edit
   re-read only the mutated slice. The Edit freshness gate is satisfied by a
   slice read too (measured waste: the active TODO fully re-read 59x, a
   3216-line source 40x, in one overnight run). If the section has > 10 checklist items, flag it: suggest splitting into **two top-level `##` sections** (new section numbers) before implementing. Never split with `N.M` sublabels (`17.1`, `### 17.2`, `**17.3 Foo**`) -- that pattern is forbidden; see `validate-todo-file`. **Each new section takes the next free number and its body goes LAST in the file** -- do NOT file it next to the section it came from. Physical order must match numeric order (`scripts/lint.sh` Check 22 ERRORs otherwise; repair: `python3 scripts/todo-section-order.py --fix`). Express the relationship in the Implementation Order `Depends On` column, which is what that column is for. A 15-item section is two sections pretending to be one. **P3.1 -- SPLIT-RECOMMENDED needs a STRUCTURED waiver to override.** When `section-manifest.py` (or `section-pack.py`) reports `complexity.verdict = SPLIT-RECOMMENDED` / `waiver_required: true`, you may proceed WITHOUT splitting only by writing a structured waiver -- `{est_files, subsystems, est_tests, context_budget, rationale}` -- and validating it: `python3 scripts/overnight/section-manifest.py waiver-check <waiver.json>` (exit 0 = accepted). A free-form "it's cohesive" is NOT sufficient (an over-large ABI/SSDT section that skipped the split is the upstream half of the context-cap). The estimates make the override an accountable prediction, checkable against what the section actually costs.
2. **Resolve dependencies** -- follow every `-> XREF:` line. **When the section carries 3+ cross-TODO XREFs, dispatch `Agent(subagent_type="xref-dependency-mapper", ...)` BY DEFAULT** -- it reads each target section + the referenced helpers and returns the dependency-status brief (READY / BLOCKED-ON + provides-surface at file:line) without pulling several large TODO files into this context; verify load-bearing dependencies at their file:line before building on them. (Your own section's spec text stays a direct read -- never digest it.) Stop and ask if a prerequisite is incomplete or scope conflicts with reality. If part is implementable and part is blocked, implement only the unblocked subset; keep blocked items `[ ]` or `[/]` with explicit blocker notes.
3. **Explore the codebase -- DETERMINISTIC FIRST, then agent for ambiguity only.**
   **Run `python3 scripts/overnight/section-pack.py <todo-path> <N>` FIRST.** In ~2s it produces the whole orientation with no model: the section manifest (open items, XREFs, likely files, tests, required gates, complexity), each backticked symbol's DEFINITION + reference count (clangd via `compile_commands.json` when present, else ripgrep), registration/ABI/syscall touchpoints, and the evidence-bundle structural views -- and it writes a content-bound receipt. Read the pack's `pack_path` JSON. **A fresh pack SATISFIES the `agent_dispatch_required` gate** (it IS the deterministic equivalent of the discovery agent), so you do NOT need to dispatch an explorer just to clear the gate. **D2 discipline -- do NOT hand-grep to orient BEFORE the pack/explorer.** A "where does helper X live / is there a Y primitive / do we have a Z allocator" question is answered by the pack's symbol DEFINITION resolution (or ONE `kernel-explorer` dispatch), never a 3-6-round inline grep hunt; for an ABI-impacting kernel/boot section, run the pack (and dispatch the explorer if the pack leaves the integration surface ambiguous) FIRST, before any inline grep/read. Drifting into inline fan-out trips `inline_churn_monitor` (25-op reminder) and, if you grep a file an in-flight explorer is already mapping, `inflight_race_guard` (D1).
   Dispatch a Sonnet agent ONLY for what the pack leaves genuinely ambiguous -- `unresolved_symbols`, behavioral/lock-order context the pack cannot derive, or a new-domain surface with no clear edit targets. When you do:
   - `src/kernel/`, `include/kernel/`, `src/boot/` -> `Agent(subagent_type="kernel-explorer", ...)` -- kernel-tuned: callers, lock order, init-phase placement, SMP/guard-rail context.
   - Everything else -> `Agent(subagent_type="section-context-mapper", ...)` -- returns the bounded section package: current behavior, relevant source+tests, callers/callees, shared state, XREF status, ranked edit targets, all at file:line. (`feature-dev:code-explorer` stays the fallback for non-kernel new-domain pioneering.)
   - SMP-heavy sections (locks/atomics/ISR/refcounts/teardown in scope) -> ALSO `Agent(subagent_type="concurrency-evidence-mapper", ...)` for the concurrency inventory (no verdicts; feeds your design and the later kernel-quality-auditor pass).

   The pack and any explorer/mapper are advisory: you still read the top "verify-first" hits yourself (slice reads) and do all edits + decisions. Identical re-dispatches over unchanged content are served from the agent-result cache automatically -- reuse the cached report; to force a fresh run, change the prompt. **For external-spec facts (UEFI, ACPI, NVMe, xHCI, Intel SDM, PE/COFF, TPM, RFCs), dispatch `Agent(subagent_type="spec-research-analyst", ...)` instead of fetching spec pages into this context** -- it returns quoted normative excerpts with spec-section citations; cross-check load-bearing claims. For toolchain/emulator/host questions (clang-19 codegen, QEMU/WHPX quirks, WSL2, GitHub Actions), dispatch `Agent(subagent_type="web-research-analyst", ...)` the same way.
   - **Completion radar (MANDATORY, pre-code):** before writing code, answer these six questions against the section and integration surface:
     1. **Correctness:** what normal-path and error-path behavior must exist for this to be real?
     2. **Completeness:** what nearby capability would still be obviously missing if I stopped at the listed items?
     3. **Wiring:** what tables, exports, registrations, docs, tests, or TODO/XREF sync must land with the code?
     4. **Parity:** what do Win11 and Linux already do here that this section still would not?
     5. **Superiority:** is there a cleaner, faster, safer, or more refined design we can afford now?
     6. **Ownership:** if any adjacent work is too large for this session, exactly which TODO item owns it?
   - If the radar finds small same-subsystem adjacent work, plan to ship it in this section. If it finds larger adjacent work, pre-plan the Branch B/C/D TODO update instead of discovering that at the end.
4. **Codex design review** -- dispatch a design review via the Codex plugin BEFORE writing code. The `design_review_required.py` PreToolUse hook is the default-on backstop; it BLOCKs every Edit/Write on code targets after this skill is invoked until a design dispatch fires. Follow the `codex-design-review` skill, including its **Size Discipline** (integration surface = names only, max 4 questions, prompt under 80 lines -- bloated prompts hit Bash's 10-min wall and get truncated). Legitimate skip cases are listed in the `codex-design-review` "Skip When" section (docs-only, stamp-only, pure constant additions, test-only registration, single-function plumbing); to use them, set `SKIP_DESIGN_REVIEW_HOOK=1` on the next tool call AND state in chat which criterion applies. "Host-side, no SMP risk" is NOT a skip criterion.
    ```bash
    bash scripts/codex-dispatch.sh '[review-kind: design] <todo-path> <design review prompt>'
    ```

    The PostToolUse hook fires `receiving-code-review` reminder; follow it on every design finding with the same Fix / Reject / Accept rigor as adversarial: verify at file:line, then Fix (adopt + note why) / Reject (with code evidence, named in chat) / Accept (domain-qualified XREF to a concrete `[ ]` item). Per-finding decision detail, the measured design-review false-positive shapes, and the Notes-recording rule: [references/review-triage.md](references/review-triage.md).
5. **Run the domain-appropriate code quality skill** -- walk the gates BEFORE writing code. The hook auto-selects based on path:
   - `src/boot/` -> `boot-code-quality` (UEFI error handling, EBS boundary, table safety, fallbacks)
   - `src/kernel/`, `include/kernel/` -> `kernel-code-quality` (SMP safety, memory rules, bare-metal)
   - `src/desktop/` -> `desktop-code-quality` (WC mapping, compositor loop, pixel format)
   - `src/shell/` -> `shell-code-quality` (Win32 console API, path conventions)
   - `user/`, `src/apps/` -> `userland-code-quality` (syscall interface, no kernel headers)
   Do not skip. Past incidents: SMP race, memory leak, wrong test assertion, FrameBufferBase crash -- all caused by writing code before checking quality gates.
6. **Implement** -- bounded scope only.
   - Freestanding kernel: `#include "kernel/types.h"` -- no `<stdint.h>`, `<string.h>`.
   - No `malloc()`/`printf()` -- use `kmalloc()` (<=4 KB), `pmm_alloc_contiguous()` (larger), `printk()`.
   - Assembly: NASM x86-64 only. UEFI-era, Long Mode, APIC -- no BIOS/VGA/PIC.
   - API surface: Win32 native. Windows-style canonical paths (`C:\Impossible\System32\`).
   - **POST16 codes are for BOOT-PATH code ONLY** -- Phase 0/1/2 boot and hardware init, where a triple fault can beat klog to the serial line. Never in scheduler / syscall / file I/O / exec / network code that runs after Phase 3; a `klog(LOG_INFO, ...)` line is more useful there. Full rule: [references/implementation-rules.md](references/implementation-rules.md).
   - **Scope-gap protocol (MANDATORY).** If while writing code you find yourself about to add a `// TODO`/`// FIXME`/`// HACK`/`// for now`/`// placeholder` comment, a `STATUS_NOT_IMPLEMENTED` return, a stub function body, or a conditional that narrows supported input below what the section's user-visible behavior promises -- STOP. You have hit a scope gap. Walk the decision tree in [scope-gap-protocol.md](scope-gap-protocol.md): Branch A (inline expansion under 1000 lines with `[x]` checklist item + NOTE callout), Branch B (new section in same TODO), Branch C (new TODO via `/create-todo` after dedup sweep), or Branch D (add to existing TODO found during dedup). Gate 10 of `kernel-code-quality` forbids the TODO comment; the scope-gap protocol is how you resolve the gap without breaking Gate 10. Report the branch you took in chat for clear cases; ASK the user which branch if sizing is borderline (900-1100 lines, unclear subsystem boundary).
   - **STATUS_NOT_IMPLEMENTED completion-first policy.** Standalone and under 1000 lines -> implement it fully (Branch A). Needs significant new infrastructure -> Branch B/C/D with a tracked item and domain-qualified XREF. Never leave a stub without a tracked follow-up. **Stale TODO patterns** (a checklist demanding POST16 for non-boot-path code, or tautological constant tests) get DELETED from the TODO, not satisfied. Both in [references/implementation-rules.md](references/implementation-rules.md).
7. **Build** -- `bash scripts/build.sh`, confirm `tail -1 build/build.log` shows `=== BUILD OK ===`.
8. **Wire unit tests** -- before writing any test code, read the TODO file's **Unit Tests** section (if one exists) to find:
   - The expected test file name (e.g. `test_exec.c`)
   - The expected registration function (e.g. `test_register_exec()`)
   - The expected test category (e.g. `TEST_CAT_EXEC`)
   - Which specific assertions are required for this section
   
   Create the test file / registration function if it doesn't exist yet. Do NOT piggy-back tests onto an unrelated test file just because it's convenient. If the subsystem doesn't fit existing `TEST_CAT_*` categories, create a new one (enum in `test.h`, names/labels in `test_runner.c`, `make test-*` target in Makefile, `bootx64.c` test_suite parser, AND the matching bat runner under the **right subdir** for the test layer -- see the bullet below). Then add/update assertions and confirm build passes.
   
   > **CRITICAL (incident 2026-04-12): Every section MUST have at least one unit test wired.** If the section has no testable surface (pure bootloader UEFI code with no kernel-side fields), document why in the TODO section with `**Note:** No kernel test surface -- validation via serial log on WHPX.`
   
   - **After wiring tests, add a one-line `> **Test runner:**` note to the TODO section** (after the Test checkpoint paragraph, before the stamps) naming the bat to run and expected results, and create the bat if it is missing. The four note shapes, the per-layer bat subdir rules, the bat template, and the aggregate-runner rule: [references/test-wiring.md](references/test-wiring.md).
   - **No tautological constant tests** (an assertion echoing a `#define` literal verifies only that you typed it; the compiler already enforces that) and **HARD BAN: tests must NEVER call live boot infrastructure** in `src/kernel/test/test_*.c` -- `boot_progress(`, `vpd_*(`, `panic(`, any subsystem `_init(`, etc. The pre-commit hook enforces the ban; the full forbidden table and the allowed alternatives are in [references/test-wiring.md](references/test-wiring.md) and [docs/infrastructure/test-policy.md](../../../docs/infrastructure/test-policy.md).
9. **Codex test coverage analysis** -- after wiring tests, dispatch a test coverage gap analysis to catch missing assertions before the adversarial review finds them. Follow the `codex-test-coverage` skill: list public functions, existing tests, and ask Codex to find untested error paths, boundaries, and negative cases. **If Codex recommends testing a forbidden function, REJECT with code evidence** -- Codex doesn't know WSL constraints. Test the underlying pure helper, or accept the gap with a `**Note:**` line in the TODO's Unit Tests section. Add any other missing tests found. Skip for trivial sections (< 3 test assertions).
    ```bash
    bash scripts/codex-dispatch.sh '[review-kind: test-coverage] <todo-path> <test coverage prompt>'
    ```
10. **Update TODO section** -- mark items with evidence:
    - **Done proof gate:** `[x]` only when implemented + wired + functional on normal path. Never `[x]` for stubs or `STATUS_NOT_IMPLEMENTED` placeholders.
    - **No false completeness:** if the section's named feature still lacks an obvious adjacent piece required to feel real, do not stamp the section as fully done until that work is either shipped or filed in the owner TODO with a concrete unchecked item and reciprocal XREF.
    - **Rewrite checklist items to match shipped reality, not original draft.** When step 4 (design review) or step 13 (adversarial review) changed the approach, the checklist item must describe **what shipped**, not what the original draft proposed. Example from §8: original "Per-LSP `threading.Lock` inside `LspSubprocess`" became, when marked `[x]`: "Per-LSP `threading.Lock`s inside `LspSubprocess` -- `_io_lock` (stdin write), `_pending_lock`, `_next_id_lock`, `_open_uris_lock`. All per-instance; different LSPs run in parallel. Shipped in §1 + §7." A `[x]` item is a CANONICAL RECORD of shipped code; preserving stale draft wording on a checked-off item is drift -- a future reader greps the `[x]` line and gets a description of the proposed approach, not the actual implementation. Cite filenames + key symbol names + brief one-line rationale; the full per-finding evidence trail lives in the commit message.
    - SSDT claims: verify service number <-> function <-> `ssdt_register()` <-> master table row consistency. If SSDT handlers were implemented, update `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c` for any Win32 wrapper names.
    - Blocked items: keep `[ ]` or `[/]`, add missing prerequisite/ownership items with owner scope and `-> XREF`.
    - **Deferred-item resolution:** scan earlier sections in the SAME TODO for items marked "deferred to section N" where N is this section. If the work was done, mark them `[x]`. Deferred items are promises.
    - **Cross-TODO sync:** when this section references or satisfies external TODO requirements, update those TODOs in the same run.
    - Preserve existing formatting (table headers, icons, column structure).
    - **No N.M subnumbering:** never add `### N.M`, `**N.M ...**`, or extra heading levels that carve one `## N.` into sub-chapters. Keep **one continuous** `- [ ]` list under each `## N.`; put grouping in bullet wording. Need more structure -- add a new `##` section with the next number, not `17.1`/`17.2`.
    - **Write a `> **Notes:**` block** (MANDATORY for sections marked `[x]` or `[/]` that shipped any artifact). Placement: immediately after the pre-stamp `> **Test runner:**` line, before the `> **Verified:**` stamp. **HARD RULE: 3-6 bullets, ONE LINE EACH**, in the canonical order (what shipped / how it integrates / downstream effects / canonical doc / scope boundary). No sub-bullets, no per-finding adoption sub-blocks -- that evidence belongs in the commit message. `notes_bloat_check.py` BLOCKs violations. Canonical shape, forbidden patterns, and the rationale: [references/todo-bookkeeping.md](references/todo-bookkeeping.md).
11. **Update Implementation Order table** -- `[x]` (fully done) or `[/]` (in progress).
12. **Update OS Comparison table** -- replace placeholders with concrete descriptions. `Planned` -> `Done` or `Partial`. **If the section added a capability not yet represented in the table, ADD a row** (do not just update existing rows). Scan the Implementation Order table and confirm every `[x]` row has a corresponding row in OS Comparison; a missing row is as much drift as a stale one. Also update the post-table summary sentences if they cap out before the section just shipped. **Do not** add or extend `<!-- Sources: ... -->` URL comment blocks; cite new research in the PR or chat only.
13. **Codex adversarial review** (MANDATORY -- NO EXCEPTIONS) -- dispatch to Codex plugin. If Codex responds with "no diff available", re-dispatch with actual file content (read 100-200 relevant lines). A shallow response requires re-prompting.
    ```bash
    bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <prompt with ALL mandatory angles>'
    ```
    **Mandatory angles (include ALL):** integer overflow, buffer overread, NULL deref, SMP races, resource leaks, ABI mismatch, bounds on untrusted data.

    After Codex responds, the PostToolUse hook fires `receiving-code-review` reminder; follow it on every finding. **Adversarial false-positive watch:** Codex reads code without runtime context and frequently flags "missing lock" when the caller already holds it, "race" on paths single-threaded by construction, or "buffer overflow" on buffers static-asserted larger than the access. Verify at file:line; fix valid Critical/High at the root cause; reject wrong findings with code evidence; accept out-of-scope with domain-qualified XREF (not lazy deferral).
14. **Self-review BEFORE fixing** -- catches what Codex misses at the integration level:
    - Regressions: did any existing functionality break?
    - Race conditions: any new shared mutable state without synchronization?
    - Edge cases: off-by-one, null pointer paths, boundary values?
    - Resource leaks on error paths?
    - Industry standards: no hacks, no TODO/FIXME in new code.
    - **Checklist item-by-item:** is each item `[x]`, `[/]`, or `[ ]` with explicit justification?
    - Deferred items from earlier sections resolved?
    - **Completion radar again:** does the feature now look correct, complete, wired, parity-aware, and properly owned, or did implementation reveal adjacent work that still needs to ship or be tracked?
15. **Fix loop** (1 round mandatory; up to 3 if needed):
    - Fix all valid Critical and High from BOTH Codex (step 13) and self-review (step 14).
    - When a build/test/smoke failure produces a large log, dispatch `Agent(subagent_type="diagnostic-digester", <log path>)` first and validate its hypotheses at `file:line` before editing -- do not read the whole log into this context.
    - Fix valid Medium unless explicitly accepted.
    - **Rebuild after fixes -- via `bash scripts/overnight/run-artifact.sh fixloop -- bash scripts/build.sh` for every loop round** (deterministic JSON envelope: verdict + every error line + artifact path; a green run costs ZERO model tokens, and a failing one hands you the bounded error set instead of the whole log -- measured on TODO-12 section 8, 21 in-context raw builds burned ~90% of the section's context). `diagnostic-digester` is for FAILURES with large artifacts only, never green runs.
    - On loop rounds after the first, if the fix targets moved to new files, re-dispatch `Agent(subagent_type="review-evidence-mapper", ...)` for the fresh file:line edit map instead of re-reading whole files; use lsp-bridge (`definition`/`references`) for repeated symbol lookups, not inline `bash grep`.
    - **Pre-dispatch self-diff gate FIRST** (canonical rule: review-todo-section step 6): before ANY re-dispatch, read the fix diff against this round's + prior rounds' findings and self-check the fix-then-regress shapes (scope creep, an op reordered before its precondition, `==` where a bit-flag test is needed, sentinel/boundary handling, and re-opening a prior finding). Only then: re-dispatch Codex if fixes are STRUCTURAL; a localized fix that passes the gate is surgical -- self-verify and proceed without a full round.
    - **Test-only / cosmetic fix deltas take the lighter path (B2, canonical rule: review-todo-section step 6).** A fix diff confined to test files or cosmetic edits does not change shipped behavior -- self-verify (assertion still correct + suite green) and proceed WITHOUT a full re-adversarial; at most ONE scoped confirming round if the test changes WHAT is asserted. And when a finding asks you to harden a test, do NOT reach for a fragile allocator-/layout-dependent bound (a magic `delta <= N` tied to heap internals draws a new finding every round -- the §11 7-review spiral); use a structural invariant (set-then-delete prewarm net-zero, exact-count, or the `test_harness.c` pattern) that converges in one round.
    - Unresolved Critical/High after round 3: do not mark section complete.
16. **2nd final build** -- `bash scripts/build.sh`, confirm `=== BUILD OK ===`. **When this step also needs the smoke test or a full test suite (long output), run them through `bash scripts/overnight/run-artifact.sh <label> -- <cmd>`** (deterministic envelope, NO model tokens on green) and then quote the on-disk artifact tail (`build/build.log`, `build/smoke-test.stripped.log`, suite summary) YOURSELF -- the wrapper spares this context the output flood; dispatch `diagnostic-digester` only on a FAIL envelope, and your verification-before-completion quote comes from the artifact. This catches anything broken by the fix loop or self-review changes. **If the section touched boot-path code** (`src/boot/`, `src/kernel/main/boot_*`, `src/kernel/idt.c`, `src/kernel/gdt.c`, `src/kernel/msr.c`, `src/kernel/smp/`, `src/kernel/mm/pmm.c|vmm.c`, `src/kernel/drivers/lapic.c|ioapic.c|acpi.c`, or any Phase 0/1 init), also run `bash scripts/test-smoke.sh` and confirm `SMOKE TEST PASSED`. The smoke test boots the full OS in QEMU (KVM if `/dev/kvm` is writable, TCG otherwise) and checks for `Boot complete in` + `C:\>` on serial -- catches boot regressions that unit tests cannot (phase ordering, #GP from wrong IDT state, init-order crashes). Non-boot changes: skip the smoke test, the post-commit unit-test hook is enough.

    **Boot-validation matrix (2026-07-28).** Where this step calls for the smoke test, run `bash scripts/test-smoke-matrix.sh` instead of a single `scripts/test-smoke.sh` -- it boots the image on TCG and KVM at 1 and 2 CPUs (four legs, ~3 min) and fails if ANY leg fails. Always under the artifact wrapper: `bash scripts/overnight/run-artifact.sh smoke-matrix -- bash scripts/test-smoke-matrix.sh`. Full logs stay on disk at `build/smoke-matrix/`; only the verdict enters context, which is what makes four boots affordable when per-turn context is ~77% of the bill.
    **Why four and not one.** Until 2026-07-28 the smoke gate passed no `-smp` flag at all, so it had never booted SMP -- and a section that changed the TEB <-> `kernel_gs_base` handoff across context switches shipped green through it, then halted on a 2-CPU boot. The engines are not redundant either: CLAUDE.md records that KVM catches real-CPU MSR and SMP-timing behaviour while TCG catches "timing-sensitive races that KVM's speed hides", one of which survived 297 commits behind a green local suite.
    **A leg that fails on ONE configuration is the finding, not noise.** Do not widen an assertion or retry until it passes; the engine/CPU difference IS the evidence. Diagnose why that configuration differs.
    **At the SECTION boundary (not the per-edit loop), add the WHPX leg:** `timeout 220 powershell.exe -ExecutionPolicy Bypass -File scripts/machines/run-qemu.ps1 -Accel whpx -Headless`, captured to a file and grepped for the same fatal patterns. WHPX is the accelerator that caught the halt the Linux matrix could not reproduce, and at ~141s per boot it belongs once per section rather than once per edit. It reaps its own QEMU, so a timeout no longer orphans a VM.
    **Triage every leg's output, and resolve to truth.** For each `[FAIL]` / `[WARN]` / `[CRIT]` / halt / regression: fix it if real; if it is WRONGLY REPORTED, fix the reporter at source rather than muting the line (2026-07-28: 30 "init regressed" warnings were a cumulative-vs-per-step comparison bug, and the fix was correcting the comparison, not silencing it); leave it only when the line is genuinely required or normal for the platform -- absent TPM, absent RDRAND, firmware-table quirks, and deliberate test-path refusals are expected output, not defects.
17. **Validate section against code-truth** (MANDATORY) -- conservative pre-commit evidence pass. This catches stale or optimistic status claims without burning a full Codex dispatch (that's step 20's job). Walk these checks:
    - **Evidence plan.** For each `[x]` you are about to commit, note the proof: `claim -> file:line + symbol + reason`. Grep/Read to confirm; no hand-waving.
    - **Build confirm.** `tail -1 build/build.log` shows `=== BUILD OK ===` (step 16 already did this; confirm it still holds after any last-minute edits).
    - **Plan-quality check.** Section has a `- [ ] Commit:` line. `**Test checkpoint:**` block has concrete pass/fail criteria, not "should work". Checklist items name specific functions/types/files, not vague "implement X". Symbols referenced are defined in Inputs or created by a prior section -- no orphaned references. If the section leaves obvious adjacent work ownerless when marked done, flag the plan as incomplete.
    - **Conservative classification.** Mark `[x]` only when implemented + wired + functional on normal path. **Done-proof gate:** `*_stub` handlers and normal-path `STATUS_NOT_IMPLEMENTED` placeholders never get `[x]`. **False-completeness gate:** do not leave `[x]` on a section that technically landed but still lacks obvious adjacent wiring, parity-critical behavior, or a concrete owner item for the remaining gap. **SSDT check:** service number <-> Nt function <-> `ssdt_register()` <-> master-table row consistency.
    - **Cross-TODO sync.** If this section's status change affects referenced TODO sections, patch reciprocal dependency/status notes in the same commit. Auto-close a referenced target item ONLY when explicit `ID`/`SATISFIES` mapping exists AND evidence proves full target acceptance-criteria coverage; otherwise keep target open and add a concrete missing-work item.
    - **Record what changed.** In the commit message (or a chat note before committing), name what was checked, what passed, what failed, and why any section state changed.
18. **Tie up loose ends** (MANDATORY) -- scan the ENTIRE TODO file and any XREF'd TODO files for:
    - **N.M structure drift:** if any `## N.` uses forbidden `### N.M` or `**N.M ...**`, merge to one checklist per section per `validate-todo-file`.
    - Deferred items pointing to this section that weren't resolved in step 10.
    - Stale warning boxes that should be updated to NOTE (resolved).
    - Implementation Order rows that need status updates.
    - Cross-TODO dependency notes that are now satisfied.
    - **Owner-side stranded sweep (MANDATORY, P6.1) -- the section you just shipped is what unblocks other TODOs' parked items.** Run `python3 scripts/overnight/stranded_deferrals.py --owner <this-todo-basename> --section <N>`. It lists every `[/]` item in ANOTHER TODO whose XREF names this section as its owner. Verify at file:line that the capability the item waits on really shipped (item text is often stale -- a 2026-07-27 sample found items reading "BLOCKED: X unimplemented" whose X had long since shipped), then act on the ones this section's work actually freed.
      **HOW to re-open one -- do NOT just flip `- [/]` to `- [ ]` in place.** The section oracle classifies on the Implementation Order ROW status plus SECTION stamps, never on checklist items, so an item flipped in place inside a `[x]` + Verified + Quality-reviewed section stays invisible to the runner -- AND it drops out of the stranded audit (which matches `- [/]` only), so it is then invisible to BOTH nets. **File the freed work as a concrete `- [ ]` item in a section the oracle can still see** -- an existing open section of the owning TODO, or a new `##` section via scope-gap protocol Branch B/C/D -- with a reciprocal XREF, and leave the original `[/]` item pointing at it. That is the only shape the runner will actually come back to.
    - **Inbound `> **Accepted:**` + `> **Deferred:**` sweep (MANDATORY).** Any `[x]` item this section just closed is almost certainly the target of inbound Accepted/Deferred stamps. **Fast path:** `python3 scripts/todo-graph/query.py deferred-by <id>` enumerates every one of them; empty result means the sweep is done, rows mean walk each reference and delete / rewrite / flag it. Resolution rules and the reason the step exists: [references/todo-bookkeeping.md](references/todo-bookkeeping.md).
    - Any checklist items in other sections affected by this implementation.
    - **PE export table sync:** if this section implemented new public APIs callable from user-mode, verify they're in `s_kernel32_exports[]`/`s_ntdll_exports[]` in `pe.c`.
    - **SSDT audit trigger:** if this section implemented or modified SSDT handlers, recommend running `/audit-ssdt` after commit to verify master table consistency.
    - **Filed-in-owner check** and **Accepted-XREF concreteness check (MANDATORY):** every follow-up `[ ]` that names an owner must ALSO exist as a checklist item in that owner section with a reciprocal XREF, and every "Accepted with XREF" finding from step 13 must point at a **concrete `[ ]` item** (a section title or prose mention is NOT concrete) -- create it now if missing, via scope-gap protocol Branch C/D if no owner section fits. Both rules in full: [references/todo-bookkeeping.md](references/todo-bookkeeping.md).
    - **Completion radar, post-implementation (MANDATORY):** ask one last time whether the feature is merely section-complete or genuinely credible. If obvious adjacent work remains and fits one-session same-subsystem scope, implement it now. If it does not fit, create or extend the owner TODO now before committing.
19. **Commit and push** -- only after steps 13-18 are ALL complete.
    - Use the section's `Commit:` line as the commit message.
    - Stage all changed source files, headers, the updated TODO file(s), and test changes together.
    - Push to `origin/main` immediately after successful commit.
    - Mark the section's `- [ ] Commit: "..."` item `[x]`.
    - **Section-commit gate enforcement:** the new `.claude/hooks/section_commit_gate.py` hook BLOCKS this commit (exit 2) if the staged diff carries the section-commit signature (source change + Implementation Order `[x]` flip) but the evidence is missing -- a stale build, a Codex review whose `trigger_files` don't cover the staged source, or a review never received through `superpowers:receiving-code-review`. Steps 13-17 produce that evidence; skipping any of them means the gate refuses the commit, not "reminder-then-proceed". The gate runs at BOTH harness PreToolUse AND `.githooks/pre-commit` (the latter catches `git add foo && git commit -m bar` chains the harness layer cannot see). Opt-out requires BOTH `SKIP_REVIEW_HOOK=1` AND `SKIP_REVIEW_HOOK_REASON="<text >= 12 chars>"`; the reason is logged to `.claude/state/skip-log.jsonl` AND `last-codex-review.json` is reset so the skip cannot be reused.

## Phase 2: Post-Implementation Review (MANDATORY -- NO EXCEPTIONS)

> After the implementation commit, invoke the review pipeline to verify and quality-review the freshly implemented section. This catches issues that implementation-time Codex missed because it reviewed the diff in isolation.
>
> **This step is NEVER skippable.** Not for "straightforward plumbing." Not for "Codex already ran in step 13." Not for "same pattern as the previous section." Step 13 reviews the diff in isolation during implementation; step 20 reviews the committed code in full context -- they catch different classes of bugs. The pattern of "looks simple, I'll skip the review" is the exact signal to NOT skip. (Incident 2026-04-12: §2 review skipped, user had to ask for it.)

20. **Invoke `/review-todo-section`** on the section just committed. **This is the LITERAL next tool call after `git push` succeeds** -- not "soon after", not "if the code feels risky", not "after checking whether it is needed". Invocation format:

    ```
    Skill(name="review-todo-section", args="<todo path> §<N> <title>")
    ```

    e.g. `Skill(name="review-todo-section", args="todo/01-boot-platform/TODO-01-boot-protocol-abi-handoff.md §11 Capability Negotiation and Degraded-Feature Flags")`.

    The pipeline runs:
    - Phase 1: evidence mapping + scope-gap audit + test checkpoint verification + build
    - Phase 2: MANDATORY Codex adversarial dispatch (separate from step 13; reviews the committed code in full context)
    - Phase 3: domain code-quality gates walked explicitly + MANDATORY Codex quality dispatch (consistency + performance) + test coverage check + self-review
    - Phase 4: stamp with Verified + Quality reviewed, commit review fixes, push.

    **Enforcement:** A PreToolUse hook in `.claude/settings.json` watches `git log HEAD` after a section-ship commit (diff contains a `[x]` flip in the Implementation Order table, commit message does NOT start with `review:`/`todo:`/`docs:`, same commit did NOT add a `**Verified:**` stamp). When those conditions hold, the hook BLOCKS every Edit / Write / non-review Skill / non-script Bash call with exit 2 and a message telling the agent to invoke this skill next. Read / Grep / Glob / script Bash stay free so the review pipeline itself can run. Opt-out for legitimate false positives (revert commits, stamp-only edits, etc.): `SKIP_REVIEW_HOOK=1` env var on the blocked tool call.

    The hook is the enforcement layer; this skill prose is the explanation. The pattern "I already ran Codex during step 13, the review is paperwork" IS the failure mode -- step 13 and step 20 are different pipelines with different scopes.

## HARD GATE: Steps 13-18 are MANDATORY before step 19

> **You MUST NOT `git commit` or `git push` until steps 13 through 18 have all been completed.**
>
> This is not optional. This is not skippable for "simple" changes. Every section goes through this pipeline. No exceptions.
>
> If you find yourself about to run `git add` and you have not yet:
> - (13) Dispatched Codex adversarial review and received severity-labeled findings
> - (14) Fixed all Critical/High findings with build evidence
> - (15) Done final self-review for regressions, races, bugs, completeness
> - (16) Confirmed 2nd final build passes
> - (17) Reconciled checklist status against code-truth (evidence plan + done-proof + false-completeness + SSDT + cross-TODO sync)
> - (18) Scanned for and tied up loose ends in current and XREF'd TODO files
>
> then **STOP and go back to step 13**.
>
> **Pattern to watch for:** "Build passes, looks straightforward, I'll just commit." That thought is the signal to STOP. The straightforward changes are exactly the ones where review catches the bug you didn't think about.

## Relationship to review-todo-section and verify-todo-section

`review-todo-section` owns the post-commit quality pipeline (adversarial + quality Codex + domain gates + stamps). Step 20 of this skill invokes it. `verify-todo-section` is a thin audit-mode wrapper over review for auditing already-committed work (downgrade-only; never marks new `[x]`). All three skills share [scope-gap-protocol.md](scope-gap-protocol.md) and [build-evidence.md](build-evidence.md); stamp field rules live in review.

## Guardrails

- **Commit after each section.** Never batch multiple sections into one commit.
- **Steps 13-18 are blocking prerequisites for step 19.** This guardrail exists because it was violated multiple times -- the pattern of "build passes, skip review, commit" must be broken.
- Do not mark items `[x]` from intent, partial progress, or stubs. Require implementation + wiring evidence.
- For SSDT sections, verify service number <-> function <-> registration <-> master table consistency before editing status.
- If prerequisite work is missing, keep items open and add explicit prerequisite ownership items.
- Do not auto-close referenced TODO items without `ID`/`SATISFIES` mapping plus full acceptance proof.
- Do not create new TODO files here.
- Do not turn this into a broad file-wide cleanup pass.
- Never introduce N.M subnumbering in TODO markdown (`17.1`, `### 3.2`, bold `**4.1**` pseudo-headings). Full-file rule: `validate-todo-file`.
- Do not silently widen scope when requirements conflict with reality. If the section is missing work the user-visible behavior requires, invoke the scope-gap protocol (step 6) -- never patch with a `// TODO` comment or a `STATUS_NOT_IMPLEMENTED` stub.

## Additional Resources

Read on demand -- the step that needs one names it inline.

- [build-evidence.md](build-evidence.md) -- shared with verify-todo-section (single source of truth)
- [scope-gap-protocol.md](scope-gap-protocol.md) -- shared with verify-todo-section (Branches A/B/C/D decision tree for scope gaps)
- [references/review-triage.md](references/review-triage.md) -- steps 4 + 13: per-finding Fix/Reject/Accept detail and the measured Codex false-positive shapes
- [references/test-wiring.md](references/test-wiring.md) -- step 8: Test-runner note shapes, bat layout + template, no-tautological-constants, the live-boot HARD BAN table
- [references/implementation-rules.md](references/implementation-rules.md) -- step 6: POST16 boot-path-only rule, STATUS_NOT_IMPLEMENTED policy, stale-TODO-pattern deletion
- [references/todo-bookkeeping.md](references/todo-bookkeeping.md) -- steps 10 + 18: Notes-block canonical shape, inbound Accepted/Deferred sweep, Accepted-XREF concreteness

## TODO-08 §10 step-state telemetry

The skill-step-observer hook records each step on its real tool call; the skill-step-block hook BLOCKs commit if any required terminal step's evidence is missing. There is no "I did it inline" shortcut -- the hook does not see narration. The hook fires on `Bash(git commit:*)` and `Skill(review-todo-section)`. Required terminal steps for this skill are listed in `.claude/hooks/skill_step_map.py`. Opt-out (legitimate revert / stamp-only flows): `SKIP_SKILL_STEP_BLOCK=1 SKIP_SKILL_STEP_BLOCK_REASON="<text >= 12 chars>"`.
