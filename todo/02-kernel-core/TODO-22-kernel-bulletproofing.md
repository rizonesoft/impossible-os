# TODO-22 -- Kernel Bulletproofing

> **Goal:** Apply 5-layer defense-in-depth (static assert, runtime verify, unit test, canary, documentation) to every critical invariant in the kernel. Make it impossible for code changes to silently break cross-file dependencies, struct layouts, assembly offsets, ABI contracts, or memory layout constraints. The GDT SYSRET ordering protection (5 layers, implemented 2026-04-03) is the reference implementation; this TODO extends the same pattern to all 24 identified fragile subsystems.
>
> When complete, Impossible OS is the most self-verifying kernel in existence -- every critical invariant is checked at compile time, boot time, and test time. Silent corruption is architecturally impossible.

> [!IMPORTANT]
> **Reference implementation:** GDT SYSRET ordering (gdt.h + gdt.c + syscall_fast.c + test_nt_types.c + CLAUDE.md) already has all 5 layers. Use it as a template for every section below.

> [!CAUTION]
> **Memory rule:** Static asserts have zero runtime cost. Runtime verifications in boot_init() add nanoseconds. Unit tests run only when test=1. Canaries add guard pages (~4 KB each). The total overhead is negligible; the safety gain is enormous.

---

## Inputs

- `include/kernel/smp.h` -- per_cpu_data struct (assembly offsets at gs:0, gs:24, gs:32)
- `include/kernel/boot_info.h` -- boot_config struct (cmdline at offset 32)
- `include/kernel/gdt.h` -- GDT selectors (reference: already bulletproofed)
- `include/kernel/idt.h` -- interrupt_frame struct (register order for iretq)
- `src/kernel/smp/ap_trampoline.asm` -- AP data area at 0x8E00
- `src/kernel/isr_stubs.asm` -- swapgs symmetry, interrupt frame push/pop order
- `user/user.ld` -- user ELF load address 0x800000
- `src/kernel/mm/vmm.c` -- USER_PD_INDEX for user-mode pages
- `src/kernel/mm/pmm.c` -- pmm_mark_region_used for user range
- `include/kernel/nt/service_numbers.h` -- SSDT_MAIN_COUNT = 470
- -> XREF: `TODO-05-native-api-ssdt.md §1-§4` -- NTSTATUS, SSDT, GDT all depend on these invariants
- -> XREF: `TODO-04-peb-teb-user-abi.md` -- PEB/TEB offsets are Windows ABI contracts
- -> XREF: `TODO-17-kernel-security-hardening.md` -- guard pages, NX policy depend on memory layout

---

## Outcome

- Every cross-file constant has a `_Static_assert` that prevents silent divergence at compile time.
- Every assembly-referenced struct offset is verified by static assert and unit test.
- Every boot-critical subsystem (GDT, IDT, PMM, VMM, per-CPU, SMP) has runtime verification at init that halts with a clear error message before damage occurs.
- Guard page canaries protect stack boundaries, heap boundaries, and user-mode range.
- CLAUDE.md Bare Metal Gotchas section documents every invariant with rollback instructions.
- Unit test suite covers 100% of invariant assertions (~50 new tests across all sections).

---

## Implementation Order

| Star | Order | Deliverable                                       | Depends On | Status |
| --- | :---: | -------------------------------------------------- | ---------- | :----: |
| ⭐  |   1   | per_cpu_data assembly offsets (gs:0, gs:24, gs:32) | --         |  [x]  |
| ⭐  |   2   | boot_config struct layout (cmdline at offset 32)   | --         |  [x]  |
| ⭐  |   3   | User ELF range (0x800000-0x900000) 3-file sync     | --         |  [x]  |
| ⭐  |   4   | AP trampoline data area layout (0x8E00 offsets)    | --         |  [x]  |
| ⭐  |   5   | Task interrupt frame layout (iretq register order) | --         |  [x]  |
| ⭐  |   6   | IDT vector assignment collision detection          | --         |  [x]  |
| ⭐  |   7   | SSDT service number count stability                | --         |  [x]  |
| ⭐  |   8   | XSAVE/FXSAVE area alignment (64-byte)              | --         |  [x]  |
| ⭐  |   9   | ISR swapgs symmetry verification                   | §5         |  [ ]  |
| ⭐  |  10   | Memory layout guard pages (heap, stack, user)      | §3         |  [ ]  |
| ⭐  |  11   | IXFS superblock layout and magic                   | --         |  [ ]  |
| ⭐  |  12   | Security structs (SID, TOKEN, ACL/ACE)             | --         |  [ ]  |
| ⭐  |  13   | VFS drive letter range and partition offsets       | --         |  [ ]  |
| ⭐  |  14   | exec_pending state machine verification            | §5         |  [ ]  |

> ⭐ = all exclusive -- no other OS has systematic compile-time + boot-time invariant verification across the entire kernel.

---

## 1. per_cpu_data Assembly Offsets

Assembly code (syscall_entry.asm, ap_trampoline.asm) reads `gs:0`, `gs:24`, `gs:32` as hardcoded offsets into `struct per_cpu_data`. If anyone adds a field before `syscall_rsp0`, the assembly reads the wrong data -- silent corruption or triple fault.

**Files:** `include/kernel/smp.h`, `src/kernel/sched/syscall_entry.asm`

- [x] Add `_Static_assert(offsetof(struct per_cpu_data, self) == 0, "gs:0 must be self-pointer")`
- [x] Add `_Static_assert(offsetof(struct per_cpu_data, syscall_rsp0) == 24, "gs:24 must be syscall_rsp0")`
- [x] Add `_Static_assert(offsetof(struct per_cpu_data, user_rsp_scratch) == 32, "gs:32 must be user_rsp_scratch")`
- [x] Runtime: `smp_early_bsp_init()` writes a known value to `self`, reads it back via `mov %%gs:0, %0` inline asm, verifies match
- [x] Unit test: verify all 6 offsets (self, cpu_id, lapic_id, rsp0, syscall_rsp0, user_rsp_scratch) in test_nt_types.c
- [x] Documentation: add offset table comment in smp.h with `/* Assembly depends on these offsets -- do NOT reorder */`
- [x] Commit: `"bulletproof: per_cpu_data assembly offsets -- static assert + runtime verify"`

**Test checkpoint:** Build with field inserted before `syscall_rsp0` -> static assert fires at compile time. Runtime verify logs `gs:0 self-pointer OK` at boot. Unit test passes on QEMU WHPX, TCG, VBox.

---

## 2. boot_config Struct Layout

The UEFI bootloader (bootx64.c) and kernel (boot_info.h) each define `struct boot_config` separately. The `cmdline` field must be at **exactly byte offset 32** for stable ABI across bootloader versions. If someone adds a field and forgets to decrement `_reserved[]`, cmdline shifts and the bootloader writes garbage.

**Files:** `include/kernel/boot_info.h`, `src/boot/uefi/bootx64.c`

- [x] Add `_Static_assert(offsetof(struct boot_config, cmdline) == 32)` + `sizeof == 512` + `config_found at 288` in boot_info.h
- [x] Add mirror `_Static_assert` in bootx64.c (catches bootloader/kernel drift at compile time)
- [x] Runtime: `boot_phase0()` checks first 4 bytes of cmdline are printable ASCII or NUL -- halts with message on corruption
- [x] Unit test: `test_boot_config_layout()` verifies sizeof, cmdline offset, config_found offset, and config_found == 1
- [x] Canary: cmdline ASCII check doubles as canary -- non-printable bytes mean struct shifted
- [x] Documentation: field offset table comment (30 lines) in boot_info.h with byte positions for all fields
- [x] Commit: `"bulletproof: boot_config struct layout -- offset 32 + 512-byte size asserts"`

**Test checkpoint:** Add a field without shrinking _reserved -> static assert fires. `sizeof(boot_config) != 512` -> compile error.

---

## 3. User ELF Range (0x800000-0x900000) 3-File Sync

Three files must agree on the user-mode ELF load range: `user.ld` (linker base), `vmm.c` (page table U/S policy), `pmm.c` (mark region used). If these diverge, user binaries load at wrong addresses or overwrite kernel memory.

**Files:** `user/user.ld`, `src/kernel/mm/vmm.c`, `src/kernel/mm/pmm.c`

- [x] Created `include/kernel/mm/user_range.h` with USER_ELF_BASE, USER_ELF_SIZE, USER_ELF_END, USER_PD_INDEX + 5 static asserts
- [x] Wired into vmm.c, pmm.c, task.c -- replaced all hardcoded 0x800000/0x900000 with shared constants
- [x] Runtime: `pmm_init()` verifies first+last frames of user range are set in bitmap after reservation
- [x] Unit test: `test_user_elf_range()` verifies all constants match expected values + single PD entry check
- [x] Canary: guard page at USER_ELF_END -- PT entry cleared (not-present), catches both user and kernel overflow
- [x] Documentation: CLAUDE.md gotcha updated to reference user_range.h as single source of truth
- [x] Commit: `"bulletproof: user ELF range -- shared constant + guard page + 3-file sync"`

**Test checkpoint:** Change USER_ELF_BASE in one file but not others -> static assert fires. Guard page at 0x900000 triggers #PF if user code writes past range.

---

## 4. AP Trampoline Data Area Layout (0x8E00)

The AP trampoline assembly reads data from fixed offsets at physical 0x8E00. `smp.h` defines `AP_OFF_CR3`, `AP_OFF_STACK`, etc. If these diverge from the assembly, APs read garbage during boot and crash.

**Files:** `include/kernel/smp.h`, `src/kernel/smp/ap_trampoline.asm`

- [x] 7 `_Static_assert` in smp.h: CR3==0x00, STACK==0x08, GDT_PTR==0x10, ENTRY==0x20, CPUID==0x28, IDT_PTR==0x30, DATA_BASE==trampoline+0xE00
- [x] Runtime: `smp_init()` writes CR3+ENTRY+canary, reads back, verifies match -- halts with FATAL on corruption
- [x] Canary: 0xDEADC0DE at AP_OFF_CANARY (0x38), written before SIPI, verified after data write
- [x] Unit test: `test_ap_trampoline_offsets()` -- 8 assertions covering all offsets + DATA_BASE derivation
- [x] Documentation: 18-line offset table in smp.h with assembly cross-references (which instruction uses each field)
- [x] Commit: `"bulletproof: AP trampoline data area -- offset asserts + readback verify"`

**Test checkpoint:** Change AP_OFF_STACK in smp.h but not asm -> static assert catches at compile time. Readback mismatch at boot -> FATAL halt with "AP trampoline data corruption" message.

---

## 5. Task Interrupt Frame Layout

`struct interrupt_frame` in idt.h defines the register save order. The ISR assembly stubs push registers in reverse order and pop in forward order. `schedule()` returns a frame pointer -- if the struct layout doesn't match the asm push/pop order, register restore corrupts the CPU state.

**Files:** `include/kernel/idt.h`, `src/kernel/isr_stubs.asm`

- [x] 12 `_Static_assert` in idt.h: sizeof==176, r15==0, r8==56, rbp==64, rdi==72, rax==112, int_no==120, err_code==128, rip==136, cs==144, rflags==152, rsp==160, ss==168
- [x] Runtime: `isr_handler()` checks `frame->cs` is 0x08 (kernel) or 0x23 (user RPL=3); FATAL halt on corruption
- [x] Unit test: `test_interrupt_frame_layout()` -- 9 assertions covering sizeof, GPR endpoints, iretq frame order, int_no/err_code
- [x] No unused padding in struct (packed, all 22 fields x 8 bytes = 176); canary N/A
- [x] Documentation: 30-line frame layout diagram in idt.h with offset, size, field, pushed-by, and pop-order columns
- [x] Commit: `"bulletproof: interrupt frame layout -- size + offset asserts"`

> [!WARNING]
> **Regression risk:** If interrupt frame layout changes, every ISR, every context switch, and every ring transition breaks. This is the single most critical struct in the entire kernel.

**Test checkpoint:** Resize struct interrupt_frame -> static assert fires. Reorder fields -> offset asserts fire. Runtime: every interrupt verifies cs selector is valid.

---

## 6. IDT Vector Assignment Collision Detection

Multiple subsystems claim IDT vectors: INT 0x80 (syscall), INT 0x81 (yield), INT 0x2E (NT syscall), IPI 0xFC (async), 0xFD (reschedule), 0xFE (TLB shootdown), 0xFF (spurious). If two subsystems register the same vector, one handler silently overwrites the other.

**Files:** `src/kernel/idt.c`, `include/kernel/boot_init.h`

- [x] Created `include/kernel/vectors.h` with 22 `VECTOR_*` constants + 16 uniqueness `_Static_assert` pairs
- [x] Wired into syscall.c (VECTOR_LINUX_SYSCALL, VECTOR_NT_SYSCALL) and task.c (VECTOR_YIELD)
- [x] Runtime: `idt_register_handler()` logs WARN on double-registration (non-NULL overwrite)
- [x] Unit test: `test_vector_uniqueness()` -- 10 assertions verifying values + pairwise uniqueness
- [x] Documentation: vector allocation table in vectors.h header (range/purpose/owner columns)
- [x] Commit: `"bulletproof: IDT vector collision detection -- vectors.h + uniqueness asserts"`

**Test checkpoint:** Define two vectors with same value -> static assert fires. Register handler on occupied vector -> FATAL log + handler not overwritten.

---

## 7. SSDT Service Number Count Stability

`SSDT_MAIN_COUNT` must exactly match the number of `SSDT_NtXxx` defines in `service_numbers.h`. If a service is added without incrementing the count, dispatch silently fails.

**Files:** `include/kernel/nt/service_numbers.h`, `include/kernel/nt/ssdt.h`

- [x] Added `SSDT_LAST_MAIN_INDEX` (0x03D7) + 3 static asserts: count 1-1024, last < 1024, last >= count-1
- [x] Runtime: `ssdt_init()` logs count + last index in serial output
- [x] Unit test: 5 assertions -- count==470, last==0x03D7, last < MAX, table pointer valid, table->count matches
- [x] Documentation: 20-line "next available indices per range" table in service_numbers.h + add/update instructions
- [x] Commit: `"bulletproof: SSDT service count -- last index assert + range table"`

**Test checkpoint:** Add a service number without updating count -> static assert fires.

---

## 8. XSAVE/FXSAVE Area Alignment

XSAVE requires 64-byte alignment. FXSAVE requires 16-byte alignment. `task_alloc_xsave()` allocates from PMM (page-aligned, which satisfies both). If allocation changes to kmalloc, alignment breaks.

**Files:** `src/kernel/sched/task.c`

- [x] Runtime: `task_alloc_xsave()` checks `(addr & 63) == 0` after allocation; FATAL log + NULL on failure
- [x] 3 `_Static_assert`: XSAVE_ALIGN==64, power-of-2, FXSAVE_SIZE==512
- [x] Unit test: `test_xsave_alignment()` -- PMM frame 64-byte, 16-byte, and page alignment verified
- [x] Documentation: 10-line alignment requirement comment in task.c with SDM references
- [x] Defined `XSAVE_ALIGN` (64) and `FXSAVE_SIZE` (512) constants replacing magic numbers
- [x] Commit: `"bulletproof: XSAVE area alignment -- runtime assert + alignment constants"`

**Test checkpoint:** Allocate xsave area with kmalloc (unaligned) -> runtime assert fires immediately.

---

## 9. ISR swapgs Symmetry Verification

Every `swapgs` on ring-3 entry MUST have a matching `swapgs` on ring-3 exit. A missing or double swapgs corrupts GS for all subsequent kernel code.

**Files:** `src/kernel/isr_stubs.asm`

- [ ] Canary: per-CPU `swapgs_depth` counter -- increment on entry swapgs, decrement on exit swapgs. Must be 0 after every interrupt return.
- [ ] Runtime: `isr_handler()` can verify GS points to per-CPU data (read gs:0 self-pointer, compare to known address)
- [ ] Unit test: trigger interrupt from ring-3, verify per-CPU data accessible in handler, verify TEB accessible after return
- [ ] Documentation: symmetry requirement comment already excellent in isr_stubs.asm
- [ ] Commit: `"bulletproof: ISR swapgs symmetry -- depth canary + self-pointer verify"`

> [!WARNING]
> **Regression risk:** Adding a new exception handler or modifying the ISR common stub can break swapgs symmetry. Symptoms are invisible until a per-CPU data access returns garbage.

**Test checkpoint:** Artificially skip exit swapgs -> canary fires (depth != 0). Normal operation -> depth always 0 after interrupt.

---

## 10. Memory Layout Guard Pages

Protect critical memory boundaries with unmapped guard pages that trigger #PF on overflow.

**Files:** `src/kernel/mm/pmm.c`, `src/kernel/mm/vmm.c`, `src/kernel/sched/task.c`

- [ ] Kernel stack guard page: unmap page immediately below each kernel stack allocation
- [ ] User range guard page: unmap 0x900000 (first page after user ELF range)
- [ ] Heap overflow guard: unmap page after heap end
- [ ] AP stack guard pages: unmap page below each AP stack
- [ ] IST stack guard pages: unmap page below each IST stack (DF, NMI, MCE)
- [ ] Unit test: write to guard page address -> verify #PF is triggered (not a crash, just a clean fault)
- [ ] Documentation: guard page map in CLAUDE.md
- [ ] Commit: `"bulletproof: memory layout guard pages -- stack, heap, user range"`

**Test checkpoint:** Overflow kernel stack -> hits guard page -> clean #PF with "stack overflow detected" message instead of silent corruption.

---

## 11. IXFS Superblock Layout and Magic

IXFS superblock magic (0x49584653) and struct layout must match between `mkfs-ixfs` (host tool) and kernel `ixfs_core.c`. A mismatch corrupts the filesystem.

**Files:** `include/kernel/fs/ixfs.h`, `tools/mkfs-ixfs.c`

- [ ] `_Static_assert(sizeof(struct ixfs_superblock) == 512, "IXFS superblock must be 512 bytes")`
- [ ] `_Static_assert(IXFS_MAGIC == 0x49584653, "IXFS magic must be 'IXFS'")`
- [ ] Runtime: `ixfs_init()` verifies magic, version, and checksum on mount
- [ ] Unit test: format volume with mkfs-ixfs, mount in kernel, verify superblock fields match
- [ ] Documentation: superblock layout diagram in ixfs.h
- [ ] Commit: `"bulletproof: IXFS superblock -- size assert + magic verify"`

**Test checkpoint:** Change IXFS_MAGIC in one place -> static assert fires. Mount volume with wrong magic -> ixfs_init() returns error, not corruption.

---

## 12. Security Structs (SID, TOKEN, ACL/ACE)

Windows security structs have exact binary layouts for ABI compatibility. If sizes or offsets drift, security checks produce wrong results.

**Files:** `include/kernel/security/sid.h`, `include/kernel/security/token.h`, `include/kernel/security/acl.h`

- [ ] `_Static_assert(sizeof(ACL) == 8, "ACL header must be 8 bytes (Windows ABI)")`
- [ ] `_Static_assert(sizeof(ACE_HEADER) == 4, "ACE_HEADER must be 4 bytes")`
- [ ] `_Static_assert(sizeof(ACCESS_ALLOWED_ACE) >= 8, "ACCESS_ALLOWED_ACE minimum size")`
- [ ] `_Static_assert` on SID field offsets matching Windows layout
- [ ] Unit test: create well-known SID (S-1-5-18), verify binary format matches Windows
- [ ] Unit test: create ACL with 3 ACEs, walk ACL, verify all ACEs found at correct offsets
- [ ] Documentation: Windows ABI compatibility notes in each struct header
- [ ] Commit: `"bulletproof: security structs -- SID/ACL/ACE size asserts + ABI unit tests"`

**Test checkpoint:** Resize ACL struct -> static assert fires. Create SID with wrong SubAuthorityCount -> runtime check catches.

---

## 13. VFS Drive Letter Range and Partition Offsets

VFS uses A-Z (26 letters). MBR partition table is at offset 0x1BE. GPT header at LBA 1. These are industry standards that must not drift.

**Files:** `src/kernel/fs/vfs.c`, `src/kernel/fs/mbr.c`, `src/kernel/fs/gpt.c`

- [ ] `_Static_assert(VFS_MAX_DRIVES == 26, "VFS drive letters A-Z")`
- [ ] `_Static_assert(MBR_PARTITION_TABLE_OFFSET == 0x1BE, "MBR partition table at 446")`
- [ ] Runtime: `vfs_mount()` rejects drive letters outside A-Z range
- [ ] Unit test: mount at 'Z' succeeds, mount at '[' (after Z) fails
- [ ] Unit test: read MBR from sample disk, verify partition entries at 0x1BE
- [ ] Documentation: partition offset table in mbr.c and gpt.c
- [ ] Commit: `"bulletproof: VFS + partition offsets -- range checks + industry-standard asserts"`

**Test checkpoint:** Change VFS_MAX_DRIVES -> static assert fires. Out-of-range drive letter -> runtime rejection.

---

## 14. exec_pending State Machine Verification

`exec_pending` in the task struct controls whether the scheduler saves the interrupt frame. If set but never cleared, the task's ring-3 frame is lost. If cleared prematurely, the exec'd frame is overwritten.

**Files:** `src/kernel/sched/task.c`

- [ ] Runtime: `schedule()` logs WARN if exec_pending has been set for more than 2 quanta (stuck)
- [ ] Runtime: `task_exec()` records the tick count when exec_pending was set; schedule can check age
- [ ] Unit test: call task_exec(), verify exec_pending is set; yield -> verify exec_pending is cleared after switch-in
- [ ] Canary: task struct gets `exec_pending_tick` field; if > 10 ticks old, force-clear and log error
- [ ] Documentation: state machine diagram in task.c (set in task_exec, cleared in schedule on switch-in)
- [ ] Commit: `"bulletproof: exec_pending state machine -- age tracking + stuck detection"`

**Test checkpoint:** exec_pending stuck for >10 ticks -> WARN log. Normal exec -> cleared after first context switch.

---

## OS Comparison

| ⭐ | Feature                     | Win11                  | Linux                    | Impossible OS               |
|----|-----------------------------|------------------------|--------------------------|-----------------------------|
| ⭐ | Compile-time struct asserts | ❌ Not systematic      | ⚠️ BUILD_BUG_ON sparse  | ⬜ §1-§14 all subsystems   |
| ⭐ | Boot-time invariant verify  | ❌ Not exposed         | ⚠️ BUG_ON at init       | ⬜ §1-§14 every init       |
| ⭐ | Assembly offset asserts     | ❌ Manual sync         | ⚠️ asm-offsets.c        | ⬜ §1,§4,§5 static assert  |
| ⭐ | Guard pages everywhere      | ✅ Stack guard pages   | ✅ VMAP_STACK guard     | ⬜ §10 heap+stack+user     |
| ⭐ | ABI struct size asserts     | ❌ Undocumented        | ⚠️ Sparse checks        | ⬜ §2,§5,§11,§12 all ABIs  |
| ⭐ | Vector collision detection  | ❌ Manual              | ❌ Manual               | ⬜ §6 compile-time unique  |
| ⭐ | swapgs symmetry canary      | ❌ Not verified        | ❌ Not verified         | ⬜ §9 depth counter        |
| ⭐ | Stuck flag detection        | ❌ Not tracked         | ❌ Not tracked          | ⬜ §14 exec_pending age    |

> **All exclusive.** No other OS systematically applies 5-layer defense to every critical kernel invariant. Linux has sparse `BUILD_BUG_ON` checks and `asm-offsets.c` for assembly offset generation, but nothing approaching comprehensive coverage. Windows has no public compile-time invariant system.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bulletproof()`.

- [ ] Create `src/kernel/test/test_bulletproof.c` with:
  - per_cpu_data offset assertions (3 offsets: self, syscall_rsp0, user_rsp_scratch)
  - boot_config size == 512 and cmdline offset == 32
  - User ELF range constants match across defines
  - AP trampoline offset constants match expected values
  - interrupt_frame size and critical field offsets
  - IDT vector uniqueness (all assigned vectors are distinct)
  - SSDT_MAIN_COUNT matches generated count
  - XSAVE area allocation is 64-byte aligned
  - GDT SYSRET ordering (already in test_nt_types.c -- cross-reference)
  - IXFS_MAGIC == 0x49584653 and superblock size == 512
  - ACL size == 8, ACE_HEADER size == 4
  - VFS_MAX_DRIVES == 26
- [ ] Register in `test_runner_init()`: `test_register_bulletproof()`
- [ ] Commit: `"test: kernel bulletproofing -- invariant assertion suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===` (all static asserts pass)
- [ ] Temporarily break each invariant one at a time -> verify compile error or boot halt
- [ ] `run-all-tests.bat` -> all bulletproofing tests pass
- [ ] `run-abi-tests.bat` -> GDT + frame + struct assertions in ABI category
- [ ] Verify on QEMU WHPX, TCG, VirtualBox -- no regressions from guard pages
- [ ] Bare metal: verify guard pages work on real hardware
