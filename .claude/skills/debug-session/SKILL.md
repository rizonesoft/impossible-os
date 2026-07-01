---
name: debug-session
description: Structured kernel debugging session with adversarial review validation. Given a symptom (crash, hang, BSOD, wrong output, test failure), builds a hypothesis, traces to root cause, dispatches Codex adversarial review before fixing, and verifies the fix. Use when the user describes a bug or points at failing behavior.
---

# Debug Session

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Use This Skill When

- The user describes unexpected kernel behavior: crash, hang, panic, wrong output, test failure, regression.
- The user says "this stopped working" or "this crashes on bare metal but not QEMU" or "this test keeps failing."
- You need an independent second opinion on a debugging hypothesis before touching code.
- A previous fix attempt did not resolve the issue.

## Core Principle

> **Observe. Model. Hypothesize. Validate. Fix. Verify.**
>
> The rubber-duck agent exists precisely because debugging tunnel vision is real. You WILL miss things. The rubber-duck sees the same evidence from a fresh angle. Use it BEFORE writing a fix -- not after.

> **Pair with `superpowers:systematic-debugging`.** That skill formalizes the "evidence -> hypothesis -> minimal experiment -> fix" loop in skill-shaped form: it forbids speculation-as-fix, requires a testable hypothesis before every code change, and keeps the agent on the "explain why this fix works given the evidence" rail. Invoke it when about to propose a fix and the hypothesis is not yet bulletproof. The kernel-specific rubber-duck + Codex adversarial pieces below stay; `systematic-debugging` is the cross-domain process layer underneath.

---

## Workflow

### Step 1: Gather Context

Collect everything relevant before forming any hypothesis:

- **Symptom:** exact error message, panic string, register dump, test assertion that failed, or behavioral description
- **Platform:** QEMU TCG / WHPX / VirtualBox / bare metal
- **When it regressed:** last known-good commit (if known)
- **Log evidence:** serial log lines, klog output, POST16 codes, test runner output
- **Files implicated:** source files mentioned in the crash RIP, test file, subsystem

If the user pointed at a log file, read it with `view` before doing anything else.

If the crash includes a RIP:
```bash
llvm-addr2line-19 -e build/kernel.exe -f <RIP>
```

If the bug is a boot-path symptom and there's no fresh log, **reproduce it with the smoke test** before reading code:
```bash
bash scripts/test-smoke.sh                        # KVM if /dev/kvm writable, TCG otherwise
cat build/smoke-test.stripped.log | less          # ANSI-stripped, easiest to grep
```
Smoke test boots the full OS and fails fast (30s timeout) when `Boot complete in` or `C:\>` never appears on serial. Use it as the evidence source whenever the bug is phase-0/1 init, crash before desktop, or "boots fine on WHPX but not on X" where X matches the smoke test target.

### Step 2: Build a Symptom Model

Before reading any source code, write down (internally):

```
symptom: <what the user sees>
platform: <where it fails>
phase: <P0/P1/P2/P3 or unknown>
first_failure_signal: <earliest log/POST16 that indicates something is wrong>
subsystem: <best guess based on the signal>
```

Resist the urge to jump to code immediately. The first visible error is often not the causal one.

### Step 3: Identify the Causal Failure

- Scan the evidence for the **earliest** anomalous signal, not the most dramatic one.
- A panic at `kmalloc` may be caused by heap corruption 500 ms earlier.
- A test `FAIL` may be caused by initialization order, not the test logic itself.
- Look for: NULL dereference indicators, double-free patterns, race conditions, uninitialized state, wrong lock type, missing CPUID guard.

Rank candidates:
```
hypothesis_1: <mechanism> → <symptom> (confidence: high/med/low)
hypothesis_2: <mechanism> → <symptom> (confidence: high/med/low)
```

### Step 4: Trace to Source

For the top hypothesis:

1. **grep/glob** the subsystem files for the relevant function or constant.
2. **Read the source** around the suspected location.
3. Walk the call path: who calls this? what state does it depend on? what could be invalid at this point?
4. Check for the bare-metal gotchas in CLAUDE.md (GS_BASE, MMIO caching, per-CPU MSRs, SMAP/SMEP, etc.).

### Step 5: Dispatch Adversarial Review BEFORE Fixing

> This step is **mandatory** for any bug that is not a trivially obvious typo or off-by-one.

Build the review prompt with:

```
I am debugging a kernel bug in Impossible OS (x86-64, freestanding, UEFI bootloader, clang-19, NASM, APIC, no stdlib).

SYMPTOM:
<exact error or behavior>

PLATFORM:
<QEMU TCG / WHPX / bare metal>

EVIDENCE:
<relevant log lines, POST16 codes, addr2line output>

MY HYPOTHESIS:
<mechanism you believe is the root cause>

PROPOSED FIX:
<what you plan to change and why>

REVIEW ANGLES -- cover ALL of these:
1. Is my hypothesis consistent with the evidence, or am I missing an alternative cause?
2. Does the proposed fix have SMP, interrupt, or memory safety issues?
3. Any bare-metal correctness concerns (MMIO caching, per-CPU MSRs, CPUID guards)?
4. Is there a simpler explanation?
5. Does the fix introduce NULL deref, integer overflow, buffer overread, or resource leak?
6. Does the fix break any existing invariant or ABI contract?
```

Dispatch it:
```bash
bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <prompt above>'
```

**Do not write code until the adversarial review responds.**

### Step 6: Evaluate Adversarial Review Feedback

Read the feedback carefully. For each finding:

- **Adopt** if it identifies an alternative root cause, a missing invariant, or a safety hazard.
- **Reject with justification** if the finding does not apply given the specific evidence.
- Revise the hypothesis if the rubber-duck identified a blind spot.

If the rubber-duck identified a completely different root cause, go back to Step 3 with the new hypothesis.

- **Stuck ladder -- Claude (2) -> Codex (1):** after ~3 failed hypothesis cycles on the
  same symptom, escalate to `Skill(codex:codex-rescue)` (up to 2 rounds) for an
  independent diagnosis; validate its synthesis before applying (it is a lead, not a
  verdict). If still stuck after that, file the symptom as a blocked-with-XREF item
  rather than thrashing further.

### Step 7: Implement the Fix

Apply the fix following the kernel-code-quality gates:

- No stdlib headers
- SMP-safe (spinlock, atomic, or per-CPU for any new mutable state)
- No MMIO through WB pages
- No inline asm in neutral code
- Guard every allocation return value
- No FIXME/TODO in the fix -- fix it fully or document a real scope gap

```bash
bash scripts/build.sh
```

Must show `=== BUILD OK ===`.

### Step 8: Verify the Symptom is Gone

**Do not stop at "the fix looks right."**

- If a unit test was failing: run the relevant test category and confirm it passes.
- If the bug was a boot-path symptom: re-run `bash scripts/test-smoke.sh` and confirm `SMOKE TEST PASSED` + `Boot complete in`. The smoke test is the single best KVM/TCG verification for init-order, IDT/GDT, phase sequencing, and "boots to `C:\>`" class bugs -- faster and more deterministic than full unit runs.
- If the symptom was platform-specific: note which platforms can be verified in WSL vs. native Windows vs. bare metal. KVM catches real-CPU behavior (traps, MSR access) that WHPX hides; TCG catches device-emulation issues (NVMe, USB). Pick the platform that would have caught the original symptom.
- If the fix is a NULL guard or bounds check: verify the guard is actually exercised by an existing or new test.
- If it was a regression: identify the commit that introduced it and confirm the fix is minimal.

Add or strengthen a test if none exists that would have caught this bug:

```c
TEST_ASSERT_EQ(actual, expected, "description of what this catches");
```

Register with: `test_suite_register_cat("name", fn, TEST_CAT_XX)`

### Step 9: Document the Lesson (if bare-metal or SMP)

If the bug was a hard-won lesson (bare-metal crash, SMP race, UEFI table corruption), add it to:

- `CLAUDE.md` "Bare Metal Gotchas" or "Safety Gates" section
- The relevant skill (kernel-code-quality Gate 6 or Gate 2)

### Step 10: Commit

Use a descriptive commit message:

```
fix: <subsystem> -- <one-line description of what was wrong>

<optional body: what the bug was, why it happened, what platforms it affected>
```

(Per CLAUDE.md "Commits -- zero AI-attribution trailers": no `Co-Authored-By` / `Assisted-by` lines on Impossible OS commits, including from Codex review.)

---

## Quick Reference

| Situation | Action |
|---|---|
| Have RIP from crash | `llvm-addr2line-19 -e build/kernel.exe -f <RIP>` |
| Need boot-path repro (no fresh log) | `bash scripts/test-smoke.sh`; inspect `build/smoke-test.stripped.log` |
| "Works on WHPX, crashes on KVM" | Trap behavior divergence -- KVM raises real #GP on unvirtualized MSRs, WHPX silently returns 0 (see CLAUDE.md msr_try_read notes) |
| "Works on QEMU, crashes on bare metal" | Check MMIO caching, per-CPU MSRs, CPUID guards, SMEP/SMAP |
| "Crashes intermittently" | SMP race -- check spinlocks, atomics, per-CPU state |
| "Only fails in test mode" | Check for test code calling live boot infra (`boot_progress`, `vpd_*`, `_init`) |
| "NULL dereference in klog/printk" | Check GS_BASE is set; check spinlock held during serial output |
| "FPU fault (#MF or #XM) on first FP op" | Check FCW (offset 0, `0x037F`) and MXCSR (offset 24, `0x1F80`) in XSAVE buffer |
| "Interrupt crashes on bare metal/TCG but not WHPX" | Check for CLAC/STAC without SMAP CPUID; check LAPIC TPR writes |
| "AP hangs during SMP init" | Check Init Level De-Assert IPI removed; check GS_BASE on AP; check per-AP MSR programming |
| "test_*.c causes boot freeze" | Test is calling a forbidden live boot function -- wrap in pure helper instead |

## Rubber-Duck Prompt Template (copy-paste into a Codex dispatch or any external reviewer chat)

```
I am debugging a kernel bug in Impossible OS (x86-64, freestanding, UEFI bootloader, clang-19, NASM, APIC, no stdlib).

SYMPTOM:


PLATFORM (QEMU TCG / WHPX / VirtualBox / bare metal):


LOG EVIDENCE:


ADDR2LINE OUTPUT (if crash):


MY HYPOTHESIS:


PROPOSED FIX:


QUESTIONS:
1. Is my hypothesis consistent with the evidence?
2. Does my fix have SMP, interrupt, or memory safety issues?
3. Any bare-metal correctness concerns?
4. Simpler explanation?
```
