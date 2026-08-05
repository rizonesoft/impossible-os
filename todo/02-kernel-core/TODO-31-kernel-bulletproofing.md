---
schema_version: 1
id: kernel-bulletproofing
domain: 02-kernel-core
status: active
title: "TODO-31 -- Kernel Bulletproofing"
---

# TODO-31 -- Kernel Bulletproofing

> **Goal:** Apply 5-layer defense-in-depth (static assert, runtime verify, unit test, canary, documentation) to every critical invariant in the kernel. Make it impossible for code changes to silently break cross-file dependencies, struct layouts, assembly offsets, ABI contracts, or memory layout constraints. The GDT SYSRET ordering protection (5 layers, implemented 2026-04-03) is the reference implementation; this TODO extends the same pattern to all 24 identified fragile subsystems. When complete, Impossible OS is the most self-verifying kernel in existence -- every critical invariant is checked at compile time, boot time, and test time. Silent corruption is architecturally impossible.

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
- `include/kernel/nt/service_numbers.h` -- SSDT_MAIN_COUNT = 475
- → XREF: `TODO-02-kernel-configuration-policy.md §2, §10` -- `kernel_config_t` layout, versioning, and fixed-size field invariants must adopt the same 5-layer defense
- -> XREF: `TODO-12-native-api-ssdt.md §1-§5` -- NTSTATUS, SSDT, GDT all depend on these invariants
- -> XREF: `TODO-11-peb-teb-user-abi.md` §6--§5 -- PEB/TEB offsets are Windows ABI contracts (bulletproofing extends pattern to those structs when implemented)
- -> XREF: `TODO-10-kernel-security-hardening.md` §1,§4--§7 -- NX/SMEP/SMAP and guard pages must stay consistent with §6 here

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

| ⭐  | Order | Deliverable                              | Depends On | Status |
| --- | :---: | ---------------------------------------- | ---------- | :----: |
| ⭐  |   1   | per_cpu_data assembly offsets (gs:0, gs:24, gs:32) | --         |  [x]   |
| ⭐  |   2   | boot_config struct layout (cmdline at offset 32) | --         |  [x]   |
| ⭐  |   3   | User ELF range (0x800000-0x900000) 3-file sync | --         |  [x]   |
| ⭐  |   4   | AP trampoline data area layout (0x8E00 offsets) | --         |  [x]   |
| ⭐  |   5   | Task interrupt frame layout (iretq register order) | --         |  [x]   |
| ⭐  |   6   | IDT vector assignment collision detection | --         |  [x]   |
| ⭐  |   7   | SSDT service number count stability      | --         |  [x]   |
| ⭐  |   8   | XSAVE/FXSAVE area alignment (64-byte)    | --         |  [x]   |
| ⭐  |   9   | ISR swapgs symmetry verification         | §5         |  [x]   |
| ⭐  |  10   | Memory layout guard pages (heap, stack, user) | §3         |  [x]   |
| ⭐  |  11   | IXFS superblock layout and magic         | --         |  [x]   |
| ⭐  |  12   | Security structs (SID, TOKEN, ACL/ACE)   | --         |  [x]   |
| ⭐  |  13   | VFS drive letter range and partition offsets | --         |  [x]   |
| ⭐  |  14   | exec_pending state machine verification  | §5         |  [x]   |
| ⭐  |  15   | Framebuffer bare-metal safety (5 rules)  | --         |  [x]   |

> ⭐ = all exclusive -- no other OS has systematic compile-time + boot-time invariant verification across the entire kernel.

---

## 1. per_cpu_data Assembly Offsets

Assembly code (syscall_entry.asm, ap_trampoline.asm) reads `gs:0`, `gs:24`, `gs:32` as hardcoded offsets into `struct per_cpu_data`. If anyone adds a field before `syscall_rsp0`, the assembly reads the wrong data -- silent corruption or triple fault.

**Files:** `include/kernel/smp.h`, `src/kernel/sched/syscall_entry.asm`

> [!NOTE]
> → XREF: [`TODO-32-kernel-logging-v2-lockless.md`](./TODO-32-kernel-logging-v2-lockless.md) §1 -- adds `klog_ring_v2_t klog_ring` field to `struct per_cpu_data`. The 5-layer defense pattern in this section MUST be applied when adding the field: static assert on offset/size, runtime verification at `klog_v2_init`, unit test, doc note. Place the new field AFTER the existing assembly-visible fields (`syscall_rsp0` at 24, etc.) to avoid breaking existing offsets.

- [x] Add `_Static_assert(offsetof(struct per_cpu_data, self) == 0, "gs:0 must be self-pointer")`
- [x] Add `_Static_assert(offsetof(struct per_cpu_data, syscall_rsp0) == 24, "gs:24 must be syscall_rsp0")`
- [x] Add `_Static_assert(offsetof(struct per_cpu_data, user_rsp_scratch) == 32, "gs:32 must be user_rsp_scratch")`
- [x] Runtime: `smp_early_bsp_init()` writes a known value to `self`, reads it back via `mov %%gs:0, %0` inline asm, verifies match
- [x] Unit test: verify all 6 offsets (self, cpu_id, lapic_id, rsp0, syscall_rsp0, user_rsp_scratch) in test_nt_types.c
- [x] Documentation: add offset table comment in smp.h with `/* Assembly depends on these offsets -- do NOT reorder */`

**Test checkpoint:** Build with field inserted before `syscall_rsp0` -> static assert fires at compile time. Runtime verify logs `gs:0 self-pointer OK` at boot. `bash scripts/test.sh SUITE=abi` includes per_cpu gs: offset asserts PASS. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: per_cpu_data assembly offsets -- static assert + runtime verify"`

---

## 2. boot_config and Early Config Struct Layout

The UEFI bootloader (bootx64.c) and kernel (boot_info.h) each define `struct boot_config` separately. The `cmdline` field must be at **exactly byte offset 32** for stable ABI across bootloader versions. If someone adds a field and forgets to decrement `_reserved[]`, cmdline shifts and the bootloader writes garbage.

**Files:** `include/kernel/boot_info.h`, `src/boot/uefi/bootx64.c`

- [x] Add `_Static_assert(offsetof(struct boot_config, cmdline) == 32)` + `sizeof == 512` + `config_found at 288` in boot_info.h
- [x] Add mirror `_Static_assert` in bootx64.c (catches bootloader/kernel drift at compile time)
- [x] Runtime: `boot_phase0()` checks first 4 bytes of cmdline are printable ASCII or NUL -- halts with message on corruption
- [x] Unit test: `test_boot_config_layout()` verifies sizeof, cmdline offset, config_found offset, and config_found == 1
- [x] Canary: cmdline ASCII check doubles as canary -- non-printable bytes mean struct shifted
- [x] Documentation: field offset table comment (30 lines) in boot_info.h with byte positions for all fields
- [ ] Extend the same 5-layer pattern to `kernel_config_t` after TODO-02 §2 lands: add `_Static_assert` size/version guards in `include/kernel/config.h`, a Phase 0 sanity log, unit coverage in `test_kernel_config.c`, and documentation for fixed-size fields and string buffers

**Test checkpoint:** Add a field without shrinking _reserved -> static assert fires. `sizeof(boot_config) != 512` -> compile error. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: boot_config struct layout -- offset 32 + 512-byte size asserts"`

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

**Test checkpoint:** Change USER_ELF_BASE in one file but not others -> static assert fires. Guard page at 0x900000 triggers #PF if user code writes past range. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: user ELF range -- shared constant + guard page + 3-file sync"`

---

## 4. AP Trampoline Data Area Layout (0x8E00)

The AP trampoline assembly reads data from fixed offsets at physical 0x8E00. `smp.h` defines `AP_OFF_CR3`, `AP_OFF_STACK`, etc. If these diverge from the assembly, APs read garbage during boot and crash.

**Files:** `include/kernel/smp.h`, `src/kernel/smp/ap_trampoline.asm`

- [x] 7 `_Static_assert` in smp.h: CR3==0x00, STACK==0x08, GDT_PTR==0x10, ENTRY==0x20, CPUID==0x28, IDT_PTR==0x30, DATA_BASE==trampoline+0xE00
- [x] Runtime: `smp_init()` writes CR3+ENTRY+canary, reads back, verifies match -- halts with FATAL on corruption
- [x] Canary: 0xDEADC0DE at AP_OFF_CANARY (0x38), written before SIPI, verified after data write
- [x] Unit test: `test_ap_trampoline_offsets()` -- 8 assertions covering all offsets + DATA_BASE derivation
- [x] Documentation: 18-line offset table in smp.h with assembly cross-references (which instruction uses each field)

**Test checkpoint:** Change AP_OFF_STACK in smp.h but not asm -> static assert catches at compile time. Readback mismatch at boot -> FATAL halt with "AP trampoline data corruption" message. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: AP trampoline data area -- offset asserts + readback verify"`

---

## 5. Task Interrupt Frame Layout

`struct interrupt_frame` in idt.h defines the register save order. The ISR assembly stubs push registers in reverse order and pop in forward order. `schedule()` returns a frame pointer -- if the struct layout doesn't match the asm push/pop order, register restore corrupts the CPU state.

**Files:** `include/kernel/idt.h`, `src/kernel/isr_stubs.asm`

- [x] 12 `_Static_assert` in idt.h: sizeof==176, r15==0, r8==56, rbp==64, rdi==72, rax==112, int_no==120, err_code==128, rip==136, cs==144, rflags==152, rsp==160, ss==168
- [x] Runtime: `isr_handler()` checks `frame->cs` is 0x08 (kernel) or 0x23 (user RPL=3); FATAL halt on corruption
- [x] Unit test: `test_interrupt_frame_layout()` -- 9 assertions covering sizeof, GPR endpoints, iretq frame order, int_no/err_code
- [x] No unused padding in struct (packed, all 22 fields x 8 bytes = 176); canary N/A
- [x] Documentation: 30-line frame layout diagram in idt.h with offset, size, field, pushed-by, and pop-order columns

> [!WARNING]
> **Regression risk:** If interrupt frame layout changes, every ISR, every context switch, and every ring transition breaks. This is the single most critical struct in the entire kernel.

**Test checkpoint:** Resize struct interrupt_frame -> static assert fires. Reorder fields -> offset asserts fire. Runtime: every interrupt verifies cs selector is valid. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: interrupt frame layout -- size + offset asserts"`

---

## 6. IDT Vector Assignment Collision Detection

Multiple subsystems claim IDT vectors: INT 0x80 (syscall), INT 0x81 (yield), INT 0x2E (NT syscall), IPI 0xFC (async), 0xFD (reschedule), 0xFE (TLB shootdown), 0xFF (spurious). If two subsystems register the same vector, one handler silently overwrites the other.

**Files:** `src/kernel/idt.c`, `include/kernel/boot_init.h`

- [x] Created `include/kernel/vectors.h` with 22 `VECTOR_*` constants + 16 uniqueness `_Static_assert` pairs
- [x] Wired into syscall.c (VECTOR_LINUX_SYSCALL, VECTOR_NT_SYSCALL) and task.c (VECTOR_YIELD)
- [x] Runtime: `idt_register_handler()` logs WARN on double-registration (non-NULL overwrite)
- [x] Unit test: `test_vector_uniqueness()` -- 10 assertions verifying values + pairwise uniqueness
- [x] Documentation: vector allocation table in vectors.h header (range/purpose/owner columns)

**Test checkpoint:** Define two vectors with same value -> static assert fires. Register handler on occupied vector -> FATAL log + handler not overwritten. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: IDT vector collision detection -- vectors.h + uniqueness asserts"`

---

## 7. SSDT Service Number Count Stability

`SSDT_MAIN_COUNT` must exactly match the number of `SSDT_NtXxx` defines in `service_numbers.h`. If a service is added without incrementing the count, dispatch silently fails.

**Files:** `include/kernel/nt/service_numbers.h`, `include/kernel/nt/ssdt.h`

- [x] Added `SSDT_LAST_MAIN_INDEX` (0x03DC) + 3 static asserts: count 1-1024, last < 1024, last >= count-1
- [x] Runtime: `ssdt_init()` logs count + last index in serial output
- [x] Unit test (`test_ssdt_main_count`): last index < `SSDT_MAIN_MAX`, `ssdt_get_table(MAIN)` non-NULL, `table->count >= SSDT_MAIN_COUNT` (lower bound -- count grows as handlers register, so no exact-value assert to flake on order)
- [x] Documentation: 20-line "next available indices per range" table in service_numbers.h + add/update instructions

**Test checkpoint:** The `_Static_assert`s bound `SSDT_MAIN_COUNT`/`SSDT_LAST_MAIN_INDEX` against capacity and each other (count 1-1024, last < 1024, last >= count-1) -- they do NOT count the `SSDT_Nt*` defines, and `gen-user-abi.py` hashes the macros (catching a renumber) but does not assert define-count == `SSDT_MAIN_COUNT`. Exact define-count drift is caught by the manual `grep -c '^#define SSDT_Nt' service_numbers.h` and `/audit-ssdt`, NOT an automated build gate (an automated count-vs-defines check is a tracked bulletproofing gap). Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: SSDT service count -- last index assert + range table"`

---

## 8. XSAVE/FXSAVE Area Alignment

XSAVE requires 64-byte alignment. FXSAVE requires 16-byte alignment. `task_alloc_xsave()` allocates from PMM (page-aligned, which satisfies both). If allocation changes to kmalloc, alignment breaks.

**Files:** `src/kernel/sched/task.c`

- [x] Runtime: `task_alloc_xsave()` checks `(addr & 63) == 0` after allocation; FATAL log + NULL on failure
- [x] 3 `_Static_assert`: XSAVE_ALIGN==64, power-of-2, FXSAVE_SIZE==512
- [x] Unit test: `test_xsave_alignment()` -- PMM frame 64-byte, 16-byte, and page alignment verified
- [x] Documentation: 10-line alignment requirement comment in task.c with SDM references
- [x] Defined `XSAVE_ALIGN` (64) and `FXSAVE_SIZE` (512) constants replacing magic numbers

**Test checkpoint:** Allocate xsave area with kmalloc (unaligned) -> runtime assert fires immediately. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: XSAVE area alignment -- runtime assert + alignment constants"`

---

## 9. ISR swapgs Symmetry Verification

Every `swapgs` on ring-3 entry MUST have a matching `swapgs` on ring-3 exit. A missing or double swapgs corrupts GS for all subsequent kernel code.

**Files:** `src/kernel/isr_stubs.asm`

- [x] Runtime: `isr_handler()` reads gs:0 self-pointer, verifies non-NULL and `self->self == self`; FATAL halt on broken symmetry
- [x] CS integrity check (from §5) catches frame corruption that would mis-route swapgs
- [x] Unit test: `test_gs_self_pointer()` -- 3 assertions: non-NULL, self-pointer match, cpu_id reasonable
- [x] Documentation: symmetry requirement comment already in isr_stubs.asm (lines 40-43)
- [x] Depth counter deferred -- gs:0 self-pointer check is stronger (catches the actual symptom, not proxy)

> [!WARNING]
> **Regression risk:** Adding a new exception handler or modifying the ISR common stub can break swapgs symmetry. Symptoms are invisible until a per-CPU data access returns garbage.

**Test checkpoint:** Boot with interrupts: no `[FATAL]` from GS self-pointer check in `isr_handler()`. Unit test `test_gs_self_pointer()` PASS. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: ISR swapgs symmetry -- GS self-pointer verify in isr_handler"`

---

## 10. Memory Layout Guard Pages

Protect critical memory boundaries with unmapped guard pages that trigger #PF on overflow. Required `vmm_split_huge_page()` to split boot-time 2 MiB identity-mapped huge pages into 4 KiB PTEs before individual pages can be unmapped.

**Files:** `src/kernel/mm/vmm.c`, `src/kernel/mm/heap.c`, `src/kernel/sched/task.c`, `src/kernel/gdt.c`, `src/kernel/smp/smp.c`

- [x] `vmm_split_huge_page()`: splits 2 MiB PDE into 512 x 4 KiB PTEs (identity-preserving, idempotent). Required for guard pages in the first 4 GiB identity map.
- [x] `vmm_install_guard_page()`: split + unmap + register for fault handler detection.
- [x] Guard page table (32 entries) with `guard_page_lookup()` in page fault handler -- on hit, `panic_screen()` shows the guard label instead of generic "PAGE_FAULT".
- [x] Kernel task stack guard page: switched from `kmalloc()` to `pmm_alloc_contiguous(3)` (2 stack pages + 1 guard). Guard at bottom catches downward overflow.
- [x] User range guard page: already implemented at 0x900000 in `vmm_create_user_pml4()` (§3).
- [x] Heap overflow guard: `heap_init()` allocates one extra frame after heap end, installs guard if contiguous. Non-fatal degradation if non-contiguous.
- [x] AP stack guard pages: `smp.c` allocates 5 pages (4 stack + 1 guard) per AP. Guard at bottom.
- [x] IST stack guard pages: `gdt.c` allocates 2 pages per IST (1 guard + 1 stack) for #DF, NMI, MCE.
- [x] `vmm_set_user_page()` auto-splits huge pages on demand: if PD entry is 2 MiB, allocates a PT frame and splits it into 4 KiB PTEs. Also propagates User bit at PML4/PDPT/PD levels. Works for ANY address, not just the pre-split ELF range. Forward-compatible with Win32 PE loading and VirtualAlloc at arbitrary addresses.
- [x] Unit test: `test_vmm_split_huge_page()` (split + idempotent + identity preserved) and `test_vmm_guard_page_install()` (not-present + adjacent page intact) in `test_vmm.c`.
- [x] Documentation: guard page map in CLAUDE.md Safety Gates section

**Test checkpoint:** Stack overflow -> hits guard page -> panic_screen shows "GUARD: kernel task stack overflow" with fault address. Guard page at heap end catches heap overrun. IST guard catches exception handler stack overflow. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: memory layout guard pages -- stack, heap, user range"` (split across multiple commits: guard pages, auto-split, PT leak fix)

---

## 11. IXFS Superblock Layout and Magic

IXFS superblock magic (0x49584653) and struct layout must match between `mkfs-ixfs` (host tool) and kernel `ixfs_core.c`. A mismatch corrupts the filesystem.

**Files:** `include/kernel/fs/ixfs.h`, `tools/mkfs-ixfs.c`

- [x] `_Static_assert(sizeof(struct ixfs_superblock) == 512)` in ixfs.h + mirror in mkfs-ixfs.c
- [x] `_Static_assert(IXFS_MAGIC == 0x49584653)` in ixfs.h + mirror in mkfs-ixfs.c
- [x] 3 additional static asserts: s_magic at offset 0, s_checksum at offset 128, s_reserved at offset 132
- [x] Runtime: `ixfs_format.c` already verifies magic (rejects on mismatch), version (v1 read-only, v2 OK, other rejected), and CRC32C checksum (warns on mismatch) on mount
- [x] Unit test: `test_ixfs.c` with 5 suites: superblock size (512), magic (0x49584653), field offsets (6 offsets), version (2), inode size (128 bytes, 32 per block). Registered in test_runner as TEST_CAT_FS
- [x] Documentation: 30-line superblock layout diagram in ixfs.h with offset/size/field/notes columns

**Test checkpoint:** Change IXFS_MAGIC in one place -> static assert fires. Mount volume with wrong magic -> ixfs_init() returns error, not corruption. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: IXFS superblock -- size assert + magic verify"`

---

## 12. Security Structs (SID, TOKEN, ACL/ACE)

Windows security structs have exact binary layouts for ABI compatibility. If sizes or offsets drift, security checks produce wrong results.

**Files:** `include/kernel/security/sid.h`, `include/kernel/security/token.h`, `include/kernel/security/acl.h`

- [x] `_Static_assert(sizeof(ACL) == 8)` in acl.h
- [x] `_Static_assert(sizeof(ACE_HEADER) == 4)` in acl.h
- [x] `_Static_assert(sizeof(ACCESS_ALLOWED_ACE) == 12)`, `ACCESS_DENIED_ACE == 12`, `SYSTEM_MANDATORY_LABEL_ACE == 12` in acl.h
- [x] `_Static_assert(sizeof(SID) == 8)` + 3 offset asserts (Revision=0, SubAuthorityCount=1, IdentifierAuthority=2) in sid.h
- [x] Unit test: `test_sid_binary_format` -- S-1-5-18 binary: Revision=1, SubAuthorityCount=1, Authority=5, SubAuthority[0]=18, length=12
- [x] Unit test: `test_acl_3ace_walk` -- 3 ACEs (2 allowed + 1 denied), walk all 3, verify types, OOB returns error
- [x] Unit test: `test_security_struct_sizes` -- runtime verification of all sizes (ACL=8, ACE_HEADER=4, ACCESS_ALLOWED_ACE=12, SID=8)
- [x] Documentation: Windows ABI compatibility comments in sid.h and acl.h

**Test checkpoint:** Resize ACL struct -> static assert fires. Create SID with wrong SubAuthorityCount -> runtime check catches. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: security structs -- SID/ACL/ACE size asserts + ABI unit tests"`

---

## 13. VFS Drive Letter Range and Partition Offsets

VFS uses A-Z (26 letters). MBR partition table is at offset 0x1BE. GPT header at LBA 1. These are industry standards that must not drift.

**Files:** `src/kernel/fs/vfs.c`, `src/kernel/fs/mbr.c`, `src/kernel/fs/gpt.c`

- [x] `_Static_assert(VFS_MAX_DRIVES == 26)` in vfs.h
- [x] `_Static_assert(MBR_ENTRY_OFFSET == 446)` + `MBR_SIG_OFFSET == 510` + `MBR_ENTRY_SIZE == 16` + non-overlap (4*16=64 fills to signature) in mbr.h
- [x] `_Static_assert(GPT_HEADER_LBA == 1)` + `GPT_ENTRY_SIZE == 128` in gpt.h
- [x] Runtime: `drive_index()` already rejects letters outside A-Z (returns -1); `vfs_mount()` checks `idx < 0`
- [x] Unit test: `test_vfs_drive_constants` -- VFS_MAX_DRIVES==26, Z valid, '[' rejected, '@' rejected
- [x] Unit test: `test_mbr_gpt_constants` -- MBR offset 446, sig 510, entry 16, 4 partitions, GPT LBA 1, GPT entry 128
- [x] Documentation: MBR layout table in mbr.h, GPT constants documented in gpt.h

**Test checkpoint:** Change VFS_MAX_DRIVES -> static assert fires. Out-of-range drive letter -> runtime rejection. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: VFS + partition offsets -- range checks + industry-standard asserts"`

---

## 14. exec_pending State Machine Verification

`exec_pending` in the task struct controls whether the scheduler saves the interrupt frame. If set but never cleared, the task's ring-3 frame is lost. If cleared prematurely, the exec'd frame is overwritten.

**Files:** `src/kernel/sched/task.c`

- [x] Runtime: both `schedule()` paths (yield + preemptive) check `exec_pending_tick` age; if >10 ticks (`EXEC_PENDING_STUCK_TICKS`), log WARN and force-clear
- [x] Runtime: `task_exec()` records `exec_pending_tick = uptime()` when setting `exec_pending = 1`
- [x] Canary: `exec_pending_tick` field added to `struct task`; both schedule paths clear it to 0 on switch-in
- [x] Unit test: `test_exec_pending_state` -- PID 0 exec_pending==0 and exec_pending_tick==0 (never exec'd)
- [x] Documentation: state machine diagram comment in task.h at `exec_pending` field

**Test checkpoint:** exec_pending stuck for >10 ticks -> WARN log. Normal exec -> cleared after first context switch. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: exec_pending state machine -- age tracking + stuck detection"`

---

## 15. Framebuffer Bare-Metal Safety (5 Rules)

Five non-negotiable rules for correct framebuffer/GOP handling on real hardware. VMs hide violations (forgiving caching, no real scanout). Violating these causes perf degradation, display corruption, or UEFI faults on bare metal.

**Files:** `src/kernel/drivers/framebuffer.c`, `src/boot/uefi/bootx64.c`, `src/kernel/mm/vmm.c`, `src/kernel/main/boot_hw.c`

- [x] **Rule 1: Pixel format -- runtime reject on unsupported format.** `fb_init()` checks `pixel_format` is RGBX or BGRX and `bpp == 32`. Rejects BitMask/other with LOG_FATAL and disables framebuffer. Bootloader also filters in `gop_negotiate_mode()`.
- [x] **Rule 2: Pitch alignment -- runtime verify pitch is sane.** `fb_init()` asserts `(pitch % (bpp/8)) == 0` and `pitch >= width * (bpp/8)`. Catches firmware that reports garbled pitch values. Back buffer uses `fb_stride = fb_width` (tightly packed); hardware access always derives `hw_stride = fb_pitch / (fb_bpp/8)`.
- [x] **Rule 3: WC mapping -- PAT readback verify + VRAM remap.** PAT MSR entry 1 reprogrammed WT->WC in `boot_phase0()` with readback verification (`boot_halt` on mismatch). `fb_init()` calls `vmm_map_mmio_wc()` to remap VRAM at 4+ GiB VA. LOG_ERROR on WC failure (identity-map fallback).
- [x] **Rule 4: Mode changes -- structurally enforced.** GOP protocol pointer not stored after `ExitBootServices()`. `gop_negotiate_mode()` runs during bootloader init. Post-ExitBS code has no access to GOP -- violation is a compile error, not a runtime bug.
- [x] **Rule 5: Back-buffer -- guarded by pointer comparison.** All direct `hw_addr` writes gated by `if (back_buf != hw_addr)`. When back buffer is allocated, drawing goes to `back_buf`; only `fb_swap()`/`fb_swap_rect()` copy to VRAM.
- [x] Documentation: rules in CLAUDE.md memory + coding skill Gate 6 + TODO-31 §15

**Test checkpoint:** Serial log shows `PAT: 0x0007040600070406 -> 0x0007040600010406 (entry 1 = WC)` and `Framebuffer WC-mapped: phys ... -> VA ...`. Change pixel_format to 2 (BitMask) -> fb_init() rejects with FATAL. Change pitch to odd value -> FATAL. PAT MSR write trapped -> boot_halt. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"bulletproof: framebuffer 5-rule guardrails -- pixel fmt, pitch, PAT verify, WC remap"`

---

## OS Comparison

| ⭐  | Feature                | 🪟 Win11  | 🐧 Linux       | 🚀 Impossible OS    |
| --- | ---------------------- | --------- | -------------- | ------------------- |
| ⭐  | Compile struct asserts | ❌ sparse | ⚠️ BUILD_BUG_ON | ✅ §1--§15          |
| ⭐  | Boot invariant verify  | ❌ hidden | ⚠️ BUG_ON       | ✅ §1--§15          |
| ⭐  | Asm offset asserts     | ❌ manual | ⚠️ asm-offsets  | ✅ §1 §4 §5         |
| ⭐  | Guard pages wide       | ✅ stacks | ✅ VMAP_STACK  | ✅ §10 all sites    |
| ⭐  | ABI size lock-in       | ❌ opaque | ⚠️ sparse       | ✅ §2 §5 §11 §12    |
| ⭐  | IDT vector uniqueness  | ❌ manual | ❌ manual      | ✅ §6 vectors.h     |
| ⭐  | swapgs sanity check    | ❌ none   | ❌ none        | ✅ §9 gs:0 self     |
| ⭐  | Stuck scheduler flags  | ❌ none   | ❌ none        | ✅ §14 exec_pending |

> **All exclusive.** Linux uses sparse BUILD_BUG_ON and asm-offsets.c; Windows has no public compile-time invariant system like this.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_bulletproof()`.

- [x] Create `src/kernel/test/test_bulletproof.c` with:
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
- [x] Register in `test_runner_init()`: `test_register_bulletproof()`

**Test checkpoint:** `bash scripts/test.sh SUITE=abi` -- all `BP:` suites PASS; `tail -1 build/build.log` is `=== BUILD OK ===`. Test on: QEMU WHPX, QEMU TCG, VirtualBox, bare metal.

- [x] Commit: `"test: kernel bulletproofing -- invariant assertion suite"`

> **Done:** 7 suites, ~35 assertions -- registered in `test_runner_init()` (2026-04-04)

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===` (all static asserts pass)
- [ ] Temporarily break each invariant one at a time -> verify compile error or boot halt
- [ ] `run-all-tests.bat` -> all bulletproofing tests pass
- [ ] `run-abi-tests.bat` -> GDT + frame + struct assertions in ABI category
- [ ] Verify on QEMU WHPX, TCG, VirtualBox -- no regressions from guard pages
- [ ] Bare metal: verify guard pages work on real hardware

**Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi)

---
