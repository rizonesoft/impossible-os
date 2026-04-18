# Serial-Log Detectable Policy Violations

> Authoritative list of policy violations the `diagnose-serial-log` skill can
> detect from a kernel serial log. Each detector has Rule, Detection, Severity,
> Strict-only flag, and Remediation. The skill runs non-strict detectors by
> default; strict-only detectors require `--strict` (high false-positive risk).

## How to use this file

When the skill runs the policy-scan pass (step 9a in SKILL.md), it walks each
group below against the log, emits a `POLICY` finding per hit, and cites the
rule name so a developer can look it up here. Remediation tells the fix loop
what shape the patch should take (demote log, rename symbol, restructure
init, etc.). Tune the false-positive rate by promoting / demoting detectors
between default and strict-only.

---

## P1. Text Encoding and Mojibake

### P1.1 No Unicode dashes in serial output
- **Rule:** ASCII only per CLAUDE.md "No Unicode Dashes (en/em)". U+2013 (en dash) and U+2014 (em dash) render as `ΓÇô` / `ΓÇö` under Windows code pages.
- **Detection:** any of these byte/glyph sequences on a line originating from kernel/boot code (ignore pre-kernel firmware banners):
  - UTF-8 bytes: `\xE2\x80\x93` (U+2013), `\xE2\x80\x94` (U+2014)
  - CP437/CP1252 mojibake: `ΓÇô`, `ΓÇö`
  - Raw codepoints U+2013 / U+2014 when log file is UTF-8
- **Severity:** M
- **Strict-only:** no
- **Remediation:** rewrite the source literal. Replace with ASCII `--` or rephrase (colon / parens / two sentences); do NOT patch the log renderer. Grep `src/kernel` and `src/boot` for the mojibake bytes and fix at source.

### P1.2 No section sign in serial / CMD output
- **Rule:** per feedback memory `feedback_no_section_sign`, U+00A7 (section sign) garbles as `┬º` / `Â§` on Windows CP1252 consoles. Fine in markdown docs, forbidden in `printk` / `klog` / `TEST_PENDING`.
- **Detection:**
  - UTF-8 bytes `\xC2\xA7` in a `[BOOT]` / `[subsystem]` / `TEST:` line
  - Mojibake `┬º`, `Â§` in the same lines
- **Severity:** M
- **Strict-only:** no
- **Remediation:** rename at source to `section N` or drop the character entirely. Not a remedy: leave it and tell the user to configure their terminal.

### P1.3 ASCII only in kernel-origin log output
- **Rule:** same as P1.1/P1.2 but broader. Any non-ASCII byte (outside 0x09, 0x0A, 0x0D, 0x20-0x7E) in a kernel/boot log line is a violation.
- **Detection:** per-line byte scan against `[\x09\x0A\x0D\x20-\x7E]`; any line with a stray high byte that is not already caught by P1.1 / P1.2.
- **Severity:** L
- **Strict-only:** yes (high false-positive rate when firmware copies a trademark glyph into a device name)
- **Remediation:** rewrite source literal to ASCII. If the character comes from firmware-provided data (device name, UEFI string), sanitize with a transliteration helper at the kernel boundary.

---

## P2. POST16 Policy

### P2.1 POST16 boot-path only (kernel codes after Phase 3 start)
- **Rule:** CLAUDE.md "POST16 codes are for BOOT-PATH code ONLY." Kernel-range codes (`0x0xxx` Phase 0, `0x1xxx` Phase 1, `0x2xxx` Phase 2, `0x3xxx` Phase 3 entry) must not be emitted after `boot_phase3()` returns. Any emission during scheduler, syscall, ELF load, compositor steady-state is a violation.
- **Detection:** track the last `[BOOT] POST 0x` emission; find the `Boot complete in` marker or first `C:\>` prompt; any POST16 line after that marker whose code is in `0x00xx`-`0x3xxx` range is a violation. Bootloader range (`0xBxxx`) is expected at boot entry only.
- **Severity:** H
- **Strict-only:** no
- **Remediation:** remove the `boot_post_write16()` / `post_code16()` call and replace with a single `klog(LOG_INFO, ...)` at the offending site.

### P2.2 POST16 entry/exit pair mismatch
- **Rule:** CLAUDE.md boot-code-quality Gate 10: "Every boot phase step gets entry/exit POST codes." An entry like `0x0020` (PMM) should be followed by `0x0021` (PMM_OK) within the same phase; a missing exit means the init function hung or silently failed.
- **Detection:** from the POST16 manifest (`build/post16-manifest.env`), pair each `XXX` entry with `XXX+1` exit. For every entry observed in the log, walk forward up to `N` lines for the matching exit. No exit within window (`N=50` non-strict, `N=200` strict) is a violation.
- **Severity:** Critical
- **Strict-only:** no
- **Remediation:** open the subsystem init source, find the early-return or halt path that skipped the exit POST; add the missing `boot_post_write16(POST16_X_OK)` on every success path.

### P2.3 POST16 duplicate emission across sources
- **Rule:** POST16 values must be unique across `include/kernel/boot_init.h` + `src/boot/uefi/bootx64.c`. The manifest generator catches static duplicates at build time; log detection catches runtime re-entry where the same code fires from two code paths.
- **Detection:** count `[BOOT] POST 0xNNNN` by code across a single boot. Expected count for each required entry is exactly 1 (or 1 for entry + 1 for exit pair). If a code appears more than once on a single-boot log, flag.
- **Severity:** M
- **Strict-only:** yes (some retry paths legitimately re-emit)
- **Remediation:** check the manifest for a name collision; if names differ, rename the late-adopted one.

---

## P3. Init Ordering

### P3.1 No thread_create for deferred init
- **Rule:** CLAUDE.md "No thread for deferred init." `boot_run_deferred()` must run inline in Phase 3; VirtIO input and VBox mouse drivers broke 2026-04-05 when run on a background thread that the compositor loop starved.
- **Detection:** any `thread_create` / `kthread_create` / `CreateThread` log line emitted BEFORE the first `[COMPOSITOR]` or `C:\>` marker, where the thread name contains `deferred` / `defer_` / `init_` tokens.
- **Severity:** H
- **Strict-only:** no
- **Remediation:** delete the thread, call the function inline from `boot_run_deferred()` on BSP. Revisit only after SEH lands (TODO-10).

### P3.2 No klog before klog_init
- **Rule:** klog writes to the in-memory ring + serial. Before `klog_early_init()` completes (POST16_KLOG_OK = 0x0051), any `klog` output is undefined.
- **Detection:** any line matching `\[(INFO|WARN|ERROR|CRIT)\]` before the first `[BOOT] POST 0x0051` emission.
- **Severity:** M
- **Strict-only:** no
- **Remediation:** replace the early call with `serial_early_print(...)` or defer it to after `klog_early_init()`.

### P3.3 Double-init of a subsystem
- **Rule:** Subsystem init functions (`pmm_init`, `vmm_init`, `heap_init`, `serial_init`, `gdt_init`, `idt_init`, `acpi_init`, `lapic_init`, `ioapic_init`, `timer_hal_init`) must run exactly once on BSP. A second invocation typically crashes (the subsystem was not designed for re-entry).
- **Detection:** per-subsystem, count `[BOOT] POST 0xNNNN` for the entry code of the init step. More than one on a single boot is a violation.
- **Severity:** Critical
- **Strict-only:** no
- **Remediation:** find the caller running it twice, guard with a `kernel_subsystem_ready(SUBSYS_X)` check or a static `initialized` flag.

### P3.4 Phase ordering violation
- **Rule:** Phases execute strictly P0 -> P1 -> P2 -> P3. A P2 activity before P1 completes, or a P0 activity re-fired after P1 started, indicates a re-entry bug.
- **Detection:** track the highest POST16 code seen so far; flag any subsequent code whose phase-prefix digit is strictly lower, unless the code is `0xFF00` (BOOT_OK) or `0xFFFE` (BOOT_FAILED).
- **Severity:** H
- **Strict-only:** no
- **Remediation:** identify the caller; confirm whether the re-entry is a double-init (P3.3) or a misrouted IPI handler firing during init.

### P3.5 Deferred-init completion missing
- **Rule:** Every `[DEFERRED] <name> start +Nms` line must be followed by a matching `[DEFERRED] <name> done +Nms` before boot completes. A start without a done means that driver hung the compositor loop.
- **Detection:** pair up by name. Unpaired starts at end-of-log is a violation.
- **Severity:** H
- **Strict-only:** no
- **Remediation:** open the deferred init function; check for infinite loop, unhandled error, missing done-log call.

---

## P4. Test Code Discipline

### P4.1 No live boot infrastructure calls from tests
- **Rule:** CLAUDE.md "Test Code -- No Live Boot Infrastructure Calls." Tests must NEVER call `boot_progress`, `vpd_stage_*`, `post_display16`, `boot_halt`, `panic`, `KeBugCheckEx`, any subsystem `_init`. 3 incidents to date.
- **Detection:** during test suite output (between `=== TESTS START ===` and `=== TESTS COMPLETE ===`), any `[BOOT] POST 0x` emission that is NOT between a `TEST:` wrapper header and its `[ OK ]` / `[FAIL]`. Also any `VPD:` / `boot_splash:` log line emitted during a test. The pre-commit hook already enforces the static case; this detector catches runtime drift (e.g. a helper that indirectly calls a forbidden function).
- **Severity:** Critical
- **Strict-only:** no
- **Remediation:** identify the test; rewrite to call pure data helpers only. See CLAUDE.md's "Allowed alternatives" list.

### P4.2 TEST_ASSERT on deferred-feature sentinel
- **Rule:** per feedback memory `feedback_mandatory_unit_tests` and the settings.json hook: tests asserting `STATUS_NOT_IMPLEMENTED` / `STATUS_NOT_SUPPORTED` / `E_NOTIMPL` / `ENOSYS` as a PASSING result should use `TEST_PENDING`, not `TEST_ASSERT`. TEST_PENDING renders as `[STUB]` and counts in the pending bucket for reserved-but-unimplemented tracking.
- **Detection:** any test line `TEST_ASSERT(...STATUS_NOT_IMPLEMENTED...)` or `TEST_ASSERT_EQ(..., STATUS_NOT_IMPLEMENTED, ...)` appearing in the test output as PASS. Also `[PASS]` lines whose message contains `not implemented` / `not supported`.
- **Severity:** L
- **Strict-only:** no
- **Remediation:** rewrite source as `TEST_PENDING(st == STATUS_NOT_IMPLEMENTED, "NtFooBar: no <subsystem> yet")`.

### P4.3 Expected WARN / ERROR not demoted
- **Rule:** per `diagnose-serial-log` skill's own noise rule: "Preferred fix for persistent noise: demote the log site from `LOG_WARN` or `LOG_ERROR` to `LOG_DEBUG` when the line is expected during tests."
- **Detection:** any `[WARN]` / `[ERROR]` line appearing >= 3 times in the test phase with the same canonicalized key AND nearby `TEST:` test name that owns the error path (matches the Known Test Noise table in SKILL.md).
- **Severity:** L
- **Strict-only:** no
- **Remediation:** edit the emitter to `LOG_DEBUG` or gate on a non-test runtime check.

---

## P5. Kernel Hygiene

### P5.1 No stdlib `printf` in kernel-origin output
- **Rule:** CLAUDE.md "Freestanding Kernel -- No stdlib." `printf()` and friends are not available; `klog`/`printk` are the only output APIs. A `printf:` prefix in serial suggests a test or debug path slipped through.
- **Detection:** any serial line starting with `printf:` or containing ` printf(` as literal function-call output (not inside a string literal).
- **Severity:** M
- **Strict-only:** yes (low signal, occasional legit mention in klog strings)
- **Remediation:** replace with `printk` / `klog`.

### P5.2 Subsystem prefix missing on kernel log lines
- **Rule:** kernel log lines should carry `[subsystem]` or `subsystem:` prefix so the event stream parser can categorize them. Loose log lines without prefix become NOISE or escape classification.
- **Detection:** any kernel-phase line (after P0 complete) that is not blank, not a POST16 emission, not a TEST harness line, and does not start with `[A-Z]` / `[a-z]+:`, and is not a userland process output (`C:\>`, shell banner).
- **Severity:** L
- **Strict-only:** yes (many legitimate exceptions: user-mode, banners, etc.)
- **Remediation:** add a subsystem prefix to the emitter.

---

## P6. SMP and Boot-Info

### P6.1 AP count mismatch
- **Rule:** the bootloader reports `boot_info.cpu_count` (expected AP count). After SMP init, the kernel should log `SMP: N CPUs up` where N matches. A mismatch indicates an AP failed to start (stuck in INIT/SIPI, bad trampoline, etc.).
- **Detection:** parse `boot_info.cpu_count=N` from preamble if present; find `SMP: M CPUs up` in log; flag when `M < N`.
- **Severity:** Critical if M == 1 (SMP didn't start); H if M < N (some APs failed).
- **Strict-only:** no
- **Remediation:** grep AP-start path; check INIT/SIPI sequence, trampoline, GDT visibility from AP.

### P6.2 boot_info magic / version mismatch
- **Rule:** CLAUDE.md "boot_info ABI" says `BOOT_INFO_VERSION` must match between BOOTX64.EFI and kernel.exe. A mismatch halts with `boot_info: bad header`.
- **Detection:** any `boot_info: bad header` or `boot_info: magic mismatch` line in log (will be followed by a halt).
- **Severity:** Critical
- **Strict-only:** no
- **Remediation:** rebuild with `bash scripts/build.sh` (rebuilds both images from current source).

### P6.3 CLAC/STAC #UD on CPUs without SMAP
- **Rule:** CLAUDE.md Bare Metal Gotchas: `clac` / `stac` cause #UD on CPUs without SMAP in CPUID. Commit must not add CLAC/STAC until SMAP is actually enabled.
- **Detection:** `#UD` exception at an RIP that `llvm-addr2line-19` resolves to a function containing `clac` or `stac` instructions.
- **Severity:** Critical
- **Strict-only:** no (requires addr2line to verify; if unavailable, still flag at M severity on #UD alone)
- **Remediation:** remove the CLAC/STAC, gate on `cpu_has(FEATURE_SMAP)`.

---

## P7. Crash / Fault Policy

### P7.1 #NM fault after Phase 0 MSR init
- **Rule:** CLAUDE.md "FXSAVE/XSAVE buffer: set BOTH FCW and MXCSR defaults." CR0.TS must be cleared; FCW=0x037F, MXCSR=0x1F80. A #NM fault after Phase 0 completes means the initial FPU state was not seeded.
- **Detection:** `#NM` (vector 7) exception AFTER `[PHASE0]` markers complete.
- **Severity:** Critical
- **Strict-only:** no
- **Remediation:** verify `task_alloc_xsave()` seeds FCW + MXCSR after zeroing the buffer.

### P7.2 Guard-page fault without guard label
- **Rule:** CLAUDE.md Safety Gates: "Guard pages protect all stack and heap boundaries. Page fault handler checks a 32-entry guard table and shows the label."
- **Detection:** `PAGE_FAULT` at an address in a known guard-page range but WITHOUT the `GUARD:` prefix in the panic message means the guard-table entry was never registered.
- **Severity:** H
- **Strict-only:** no
- **Remediation:** find the stack/heap allocation, ensure `vmm_install_guard_page()` + `guard_table_register()` runs alongside the allocation.

### P7.3 Triple fault without POST16 last-good
- **Rule:** A triple fault / silent hang must have a preceding `[BOOT] POST 0x` emission that localizes the hang; if the last POST16 is missing, POST16 coverage policy (P2.1) was violated.
- **Detection:** log ends mid-boot, no `Boot complete in` marker, and the last `[BOOT] POST 0x` was more than `N=20` log lines before the end.
- **Severity:** H
- **Strict-only:** no
- **Remediation:** add POST16 entry/exit codes to every boot function touched on the code path between last-good and end-of-log.

---

## P8. Performance Gates

### P8.1 Boot time exceeds platform budget
- **Rule:** project performance budgets. KVM clean boot should be under ~3s; TCG under ~15s; WHPX under ~6s; VirtualBox under ~10s; bare metal under ~5s. Anything more than 2x the median on a clean boot is a regression.
- **Detection:** parse `Boot complete in Ns` + platform tag; compare against hard-coded budget table. If >2x budget = H; >4x = Critical.
- **Severity:** scaled (see above)
- **Strict-only:** no
- **Remediation:** open `build/boot-timing.log` (if present) and find the longest individual phase; investigate.

### P8.2 Individual phase time exceeds section budget
- **Rule:** Per-phase budgets (from CLAUDE.md + observed historical median): PMM < 100ms, VMM < 200ms, SMP < 500ms, ACPI < 100ms, FS mount < 1000ms.
- **Detection:** parse per-phase timing from boot_timing.log or the `[DEFERRED]` / `+Nms` markers. Flag phases beyond 2x budget.
- **Severity:** M
- **Strict-only:** yes (budgets are soft; noise is common)
- **Remediation:** profile the slow phase.

### P8.3 Repeat-log rate exceeds threshold
- **Rule:** a kernel log emitting the same canonicalized message more than 100 times in a single boot indicates a hot-path log bug (logging from an ISR, from a retry loop without backoff, etc.).
- **Detection:** after canonicalization (addresses, IDs, durations collapsed), any dedup group with `repeats > 100`.
- **Severity:** M
- **Strict-only:** no
- **Remediation:** rate-limit or demote to `LOG_DEBUG`; if from an ISR, move to a tasklet.

---

## Policy Counts

- **Total detectors:** 25 (across 8 groups)
- **Default (non-strict):** 20
- **Strict-only:** 5 (P1.3, P2.3, P5.1, P5.2, P8.2)

## Adding new detectors

When a new CLAUDE.md policy becomes log-observable:
1. Pick the right group (P1-P8) or add a new P9+.
2. Fill the Rule / Detection / Severity / Strict-only / Remediation template.
3. Reference the source CLAUDE.md section or memory entry in the Rule line.
4. Do NOT add detectors that require reading source outside the boot log (those belong to `validate-todo-file` or domain code-quality skills).
