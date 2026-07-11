# Kernel Code Quality -- incident history and rationale

On-demand companion to `SKILL.md` (which auto-loads on every kernel edit and
carries only the executable rules). Read this file when you need the WHY
behind a gate, are triaging a reviewer challenge to one, or are updating a
gate after a new incident.

## Gate 2 (SMP) lessons

- **Lock hold time (2026-04-06):** Codex found the klog ring written without
  its lock; the fix required the snapshot pattern because holding the ring
  lock during `serial_write()` serialized every CPU behind UART I/O.

## Gate 3 (5-layer defense) lessons

- **Non-overlap asserts (2026-04-03):** asserting `CANARY == 0x38` only
  verifies you typed 0x38 -- the AP trampoline canary at 0x38 stomped IDT_PTR
  (0x30 + 10 bytes = 0x3A) and crashed AP boot. Packed data areas with
  variable-size fields (GDT_PTR/IDT_PTR are 10 bytes) need
  `_Static_assert(FIELD_B_OFFSET >= FIELD_A_OFFSET + FIELD_A_SIZE, ...)`.
- **Parallel array sync (2026-04-06):** `s_subsys_names[]` missed `SUBSYS_OB`,
  causing an OOB read in `kernel_subsystem_dump()` during panic. Hence the
  `_Static_assert(sizeof(names)/sizeof(names[0]) == ENUM_COUNT, ...)` rule.
- Reference implementation of all 5 layers: GDT SYSRET ordering in `gdt.h` +
  `gdt.c` + `syscall_fast.c` + `test_nt_types.c` + `CLAUDE.md`; full pattern
  write-up in `TODO-22-kernel-bulletproofing.md`.

## Gate 4 (boot path) lessons

- **thread_create ban + panic-path rules (2026-04-06):** deferred init via
  `thread_create` regressed because `compositor_run()` is an infinite event
  loop that starves kernel threads created before it; panic-path lock/alloc
  rules followed Codex findings the same day.

## Gate 5 (error handling) lessons

- **Serialization escaping (2026-04-06):** `events.jsonl` had unescaped
  message fields that broke `jq` parsing -- hence the structured-output
  escaping rule.

## Gate 6 (bare metal) lessons

- **msr_try_read is not an existence probe (2026-04-13):**
  `test_msr_try_read(0xFFFFFFFF)` returned success on WHPX because the
  hypervisor absorbs unknown-MSR reads (returns 0, no #GP).
- **Per-CPU MSRs (2026-04-13):** PAT entry 1 was WC on the BSP but WT (Intel
  default) on APs; `vmm_map_mmio_wc()` pages silently got WT on APs. Fixed by
  adding `cpu_configure_pat()` to `cpu_harden()` (the per-AP hook).
- **CR3 reload resets per-vCPU MSRs on WHPX (2026-04-13):**
  `vmm_promote_to_1g()` flushed the TLB via `write_cr3(read_cr3())` and PAT
  reverted to WT on the VMEXIT; fixed by re-programming PAT after all page
  table changes.
- **VEX encoding crash (2026-04-13):** `memops.c` SSE2 fallback + dispatch
  compiled with `-mavx2` emitted `vmovdqu`, #UD on TCG `qemu64`.
- **Framebuffer 5-rule checklist (2026-04-04):** added after the WC remap
  implementation.

## Gate 8 (unit tests) lessons

- **Test side-effect ban (2026-04-07, 3rd recurring incident):**
  `test_boot_progress_records_step` froze WHPX boot by driving the live VPD
  state machine. Tests use pure helpers, never live boot infrastructure;
  `boot_timing_record_step()` is OK (in-memory append), `boot_progress()` is
  NOT (also updates VPD/framebuffer). The
  `feedback_test_no_live_boot_calls` memory documents the pattern; the
  `test_side_effect_ban` hook + docs/infrastructure/test-policy.md enforce.

## Gate 10 (production quality) lessons

- **Never widen assertions per-platform (2026-04-13):** the PAT test and MSR
  test both initially got "accept WHPX default" workarounds; the real bug was
  per-CPU MSRs not programmed on APs. A test that accepts two answers
  verifies nothing.

## Gate 11a (Win32 surface) incident

**2026-04-27 (TODO-02 section 2):** Codex flagged legacy SSDT 0x00D2/0x00D3
(`NtQuerySystemEnvironmentValue` / `NtSetSystemEnvironmentValue`) handlers as
broken: they re-routed to the Ex variant via stack args 5/6, but the SSDT
dispatcher only forwards 4 args, so the attrs/GUID arg silently zeroed. First
instinct was removing the registrations to fix the H2 ABI mismatch. The user
pushed back: "we are working towards a complete Win32 API. Removing
implementations that could potentially break Win32 API support is not a good
idea. Implement it correctly." The right fix was the canonical UNICODE_STRING
legacy ABI with implicit `EFI_GLOBAL_VARIABLE_GUID`, not removal. The user's
review gate (`feedback_no_simplify`, `feedback_no_substandard_code`) treats
removal-as-fix as a corner-cutting failure mode.

## Gate 11 (spec compliance) incident

**2026-04-12:** the UEFI device-path walk used `Type == 0x7F` instead of
`Type == 0x7F && SubType == 0xFF` for END_ENTIRE, and HardDrive DP extraction
checked MBRType but not SignatureType. Both diverged from the UEFI spec;
Claude accepted them as "forward-reserve" / "low risk" instead of fixing
immediately, and the user had to ask.

## Five-layer defense -- full code examples

**Layer 1 -- Static assert:**
```c
_Static_assert(__builtin_offsetof(struct my_struct, field) == EXPECTED,
    "why this offset matters");
_Static_assert(sizeof(struct my_struct) == EXPECTED_SIZE, "size contract");
_Static_assert(MY_CONSTANT == EXPECTED_VALUE, "value contract");
/* Packed data areas with variable-size fields: assert NON-OVERLAP. */
_Static_assert(FIELD_B_OFFSET >= FIELD_A_OFFSET + FIELD_A_SIZE,
    "field B must not overlap field A");
```
Use `__builtin_offsetof` (not `offsetof`) -- freestanding.

**Layer 2 -- Runtime verification at init:**
```c
if (actual != expected) {
    klog(LOG_FATAL, "subsys", "invariant violated: expected %u, got %u",
         (uint64_t)expected, (uint64_t)actual);
    boot_halt("subsystem invariant violated");
}
```

**Layer 3 -- Unit test:**
```c
static void test_my_invariant(void) {
    TEST_ASSERT_EQ(actual, expected, "description");
}
/* test_suite_register_cat("name", fn, TEST_CAT_XX); */
```

**Layer 4 -- Canary/guard:** guard pages (not-present PTE) at boundaries;
magic values / CRC32 verified on use; readback verification (write GS_BASE,
read via `mov %%gs:0`, compare).

**Layer 5 -- Documentation:** offset-table comment in the header; CLAUDE.md
Bare Metal Gotchas / Safety Gates entry for hard-won lessons;
`/* WARNING: Assembly depends on this -- do NOT reorder */`.

**Parallel array/table sync:**
```c
_Static_assert(sizeof(s_names) / sizeof(s_names[0]) == ENUM_COUNT,
    "name table must match enum count");
```

## Self-update protocol

This skill evolves as the kernel grows. Update `SKILL.md` when: a new
bare-metal gotcha is discovered (Gate 6); a new cross-file invariant pattern
emerges (Gate 3); a new test category is added (Gate 8); a gate rule
repeatedly false-alarms (refine it); a bug slips through that a gate should
have caught (add the check). Put the RULE in SKILL.md and the incident STORY
here -- SKILL.md is injected on every kernel edit and must stay lean. Note
gate updates here with the date and the incident.
