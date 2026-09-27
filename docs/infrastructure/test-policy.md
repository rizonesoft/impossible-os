<!-- docs: covers=todo/00-infrastructure/TODO-03-kernel-test-harness.md -->
# Test Code Policy -- No Live Boot Infrastructure Calls

> WSL has no working QEMU (CLAUDE memory `feedback_no_qemu_wsl`). I cannot validate runtime behavior here -- the user has to boot on native Windows or bare metal. Tests that mutate live boot state can freeze the kernel between the time they're committed and the time the user notices. **3 incidents to date** (2026-04-07 was a unit test calling `boot_progress("VERIFY_TEST", 0xCAFE)` from `test_boot_init.c`; froze WHPX boot after `BOOT_STEP clears ready on BOOT_DEFERRED`).

## Forbidden in tests

Tests under `src/kernel/test/test_*.c` MUST NOT call any function in this list. The PreToolUse hook `.claude/hooks/test_side_effect_ban.py` (wired in `.claude/settings.json`) enforces EXACTLY this table on every Edit/Write to a test file, and nothing beyond it: a side effect the table does not name (for example `pmm_free_frame()` on a frame the test did not allocate, which is fine on a test-owned frame and wrong on a live one) is a review judgment, not a hook, and a review that rejects such a test must say so on its own evidence rather than cite this page as the enforcer. The hook's `--selftest` pins the table's refusal direction and is run by `scripts/test-tooling.sh`.

| Forbidden in tests | Why |
|---|---|
| `boot_progress(`, `boot_post_write16(`, `boot_post_nvram_write16(`, `post_display16(` | Live VPD / framebuffer / I/O port / NVRAM side effects |
| `vpd_stage_begin(`, `vpd_stage_done(`, `vpd_stage_fail(`, `vpd_init(` | VPD state machine -- mutates row counters and renders to fb |
| `boot_splash_init(`, `boot_splash_status(`, `boot_splash_finish(`, `boot_splash_start_animation(` | Splash screen state |
| `boot_halt(`, `panic(`, `KeBugCheckEx(` | Halts the running kernel |
| `serial_init(`, `pmm_init(`, `vmm_init(`, `heap_init(`, `klog_early_init(`, `klog_disk_enable(` | Re-initializes a live subsystem |
| `acpi_init(`, `lapic_init(`, `ioapic_init(`, `timer_hal_init(`, `gdt_init(`, `idt_init(` | Brings up real hardware -- crashes if called twice |
| `boot_seed_consume(` | One-shot consumer of the boot entropy seed: frees its LIVE frames and wipes the payload, so every later consumer of that boot starves (added 2026-09-03 after the early-entropy seed-consumer capability-gate section rejected a test seam over it twice) |

## Allowed test patterns

- Pure constant checks (`TEST_ASSERT_EQ(POST16_FOO, 0x1020, ...)`)
- Save/restore wrappers around `kernel_subsystem_set_ready` / `kernel_subsystem_ready` on a specific slot (`SUBSYS_PMM` is fine -- existing tests use it)
- Direct calls to pure data helpers (`boot_timing_record_step()` is OK -- it just appends to an in-memory buffer; `boot_progress()` is NOT OK because it ALSO updates VPD/framebuffer)
- BOOT_REQUIRE / BOOT_STEP via wrapper functions that return the expected `boot_result_t`
- Read-only oracle queries (`kernel_subsystem_ready()`, `boot_timing_get_steps()`)

## Codex test-coverage findings

If a Codex test-coverage finding recommends testing a forbidden function, REJECT it with code evidence and document the gap. Codex doesn't know about WSL constraints. Test the underlying pure helper instead, or accept the gap and add a `**Note:**` in the TODO's Unit Tests section.

## Opt-out

For legitimate test cases (panic recovery testing in a controlled context, hardware fault simulation), add `/* TEST-SIDE-EFFECT-ALLOWED: <one-line reason> */` in the test function body.
