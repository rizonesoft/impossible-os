# TODO-17 — Kernel Security Hardening

> **Goal:** Activate every CPU security feature that `cpuid.c` already
> detects but currently never enables: NX (EFER.NXE + PTE NX bits), SMEP, SMAP, KPTI (separate user/kernel page tables + PCID), Spectre mitigations (IBRS/retpoline/IBPB), CET shadow stack, CET indirect branch tracking, and KASLR. Couple this with kernel-side hardening that does not require CPU support: heap magic cookies, redzone detection, stack canaries (`-fstack-protector-strong` + RDRAND-seeded `__stack_chk_guard`), and guard pages below kernel stacks. Without these, Impossible OS is exploitable via any 2018-era hardware vulnerability and trivially attackable by user-mode code — unacceptable for a production OS in 2026.

> [!IMPORTANT]
> **Current state:** `cpuid.c` detects and logs SMEP, SMAP, NX, CET_SS,
> CET_IBT, IBRS, PCID, INVPCID (`CPU_FEATURE_*` flags) but **nothing activates them**. CR4 is only written in `gfx_simd.c` (for SSE) and `ap_trampoline.asm` (for PAE/PSE). IA32_EFER.NXE is never set — no PTE has the NX bit. No per-process user page table exists (KPTI absent). No MSR_IA32_SPEC_CTRL write anywhere. No heap cookies, no stack canaries in the build flags, no guard pages below kernel stacks.

---

## Inputs

- `src/kernel/cpuid.c` — `cpu_has(CPU_FEATURE_*)` detection already complete
- `src/kernel/mm/vmm.c` — `write_cr3`, `vmm_flush_tlb`, PTE format
- `src/kernel/smp/smp.c` — `wrmsr`/`rdmsr` helpers already present
- `src/kernel/smp/ap_trampoline.asm` — AP startup; must enable same CR4/MSR features on every CPU, not only the BSP
- `src/kernel/mm/heap.c` — `kmalloc`/`kfree` (heap hardening §8)
- `include/kernel/idt.h` — `struct interrupt_frame` (syscall/exception entry)
- `scripts/build.sh` / `Makefile` — compiler flag changes for §9 (canaries)
- → XREF: `TODO-04-peb-teb-user-abi.md §3` — `swapgs` on INT 0x80 entry/exit path; must also emit IBRS save and STAC/CLAC inline ASM for SMAP compliance (§3 of this TODO)
- → XREF: `TODO-05-native-api-ssdt.md §2` — SYSCALL/SYSRET fast path is where the CR3 swap for KPTI (§4) is inserted and IBRS enable (§5) happens on kernel entry; §3 (INT 0x2E path) also needs the same CR3 swap
- → XREF: `TODO-11-security-reference-monitor.md §6` — MIC Low-IL processes are the primary beneficiaries of SMEP/SMAP (user code cannot exec kernel pages or read kernel memory)
- → XREF: `TODO-16-crash-dump-generation.md §1` — `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE` is the stop code emitted by §9 (`__stack_chk_fail`) and §8 (cookie mismatch)
- → XREF: `TODO-05-native-api-ssdt.md §4` — SSDT indices 0x01F0–0x01F4 and 0x02A2–0x02A4 reserved for Enclave and signing-level syscalls

---

## Outcome

- NX bit is set on all non-code PTE entries; IA32_EFER.NXE is enabled on all CPUs; attempting to execute data pages faults immediately.
- SMEP and SMAP are enabled on all CPUs; user-space execution and data access from kernel mode fault unless explicitly bracketed by `CLAC`/`STAC`.
- KPTI gives every process a shadow user-page-table with no kernel text mapped; Meltdown becomes unexploitable.
- PCID allows KPTI CR3 switches without full global TLB flushes.
- IBRS is written on kernel entry; retpoline replaces all indirect branches; IBPB fires at context switches; Spectre v1/v2 mitigated.
- CET shadow stack enforces return-address integrity for the kernel; CET IBT enforces `ENDBR64` at every indirect call target.
- `kmalloc` allocations carry magic-cookie headers and redzones; corruption is caught immediately at `kfree`.
- Kernel stacks have an unmapped guard page; stack overflow raises a clean `#PF` instead of silently overwriting memory.
- KASLR randomizes the kernel load address at boot using RDRAND.

---

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                   | Status |
| --- | :---: | -------------------------------------------------- | ---------------------- | :----: |
| 💎  |   1   | NX bit: EFER.NXE + PTE NX on all non-code mappings | —                      |  [x]   |
| 💎  |   2   | SMEP & SMAP: CR4 activation + CLAC/STAC wrappers   | §1                     |  [x]   |
| 💎  |   3   | KPTI: per-process user page table + CR3 swap       | §1, T04 §3, T05 §2     |  [ ]   |
| 💎  |   4   | PCID: TLB tagging for KPTI (no-flush CR3 switch)   | §3                     |  [ ]   |
| 💎  |   5   | Spectre: IBRS/IBPB MSR + retpoline build flag      | T05 §2                 |  [ ]   |
| 💎  |   6   | CET shadow stack (kernel ring 0)                   | §1, §2                 |  [ ]   |
| 💎  |   7   | CET indirect branch tracking (IBT / ENDBR64)       | §6                     |  [ ]   |
| 💎  |   8   | Kernel heap hardening (cookies, redzone)           | T16 §1                 |  [ ]   |
| 💎  |   9   | Stack canaries (`-fstack-protector-strong`)        | T16 §1                 |  [ ]   |
| 💎  |  10   | Kernel stack guard pages                           | §1                     |  [ ]   |
| ⭐  |  11   | KASLR (RDRAND kernel load address)                 | §1, §3                 |  [ ]   |
| 💎  |  12   | Enclave and signing syscalls wired to SSDT         | §11, TODO-05 §4        |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

---

## 1. NX Bit: EFER.NXE + PTE NX on All Non-Code Mappings

### 1.1 Enable IA32_EFER.NXE

- [x] EFER bit definitions (`EFER_SCE`, `EFER_LME`, `EFER_LMA`, `EFER_NXE`) added to `kernel/msr.h`
- [x] `cpu_enable_nx()` in `cpu_security.c`: checks `cpu_has(CPU_FEATURE_NX)`, sets `EFER_NXE` via `msr_write()`
- [x] Called on BSP in Phase 0 after `cpuid_init()` via `cpu_harden()`; called on each AP in `ap_entry()`
- [x] `ap_trampoline.asm`: EFER set to LME + NXE + SCE before paging is enabled — no privilege gap between long mode entry and `cpu_harden()`

### 1.2 PTE NX bit definition

- [x] `VMM_FLAG_NX (1ULL << 63)` already defined in `vmm.h`
- [x] `vmm_map_page()` applies flags to leaf PTE only; intermediates get `PRESENT | WRITABLE` (correct per x86-64)
- [x] NX gated: `vmm_map_page()` strips `VMM_FLAG_NX` if `!cpu_has(CPU_FEATURE_NX)` — safe on older hardware

### 1.3 Apply NX to all existing mappings

- [x] `vmm_apply_nx_policy()`: walks PML4→PDPT→PD→PT, sets NX on all non-text pages (2 MiB huge + 4 KiB entries); skips pages overlapping `__text_start`..`__text_end` (linker symbols added)
- [x] `vmm_flush_tlb_all()` called after NX application; logs count of NX'd pages + text range
- [ ] Verification test (execute heap pointer -> #PF) -- deferred to boot test suite (debug=1)

### 1.4 Commit

- [x] Committed across multiple patches: EFER.NXE activation, AP trampoline NXE+SCE, PTE NX gate, vmm_apply_nx_policy()

---

## 2. SMEP & SMAP: CR4 Activation + CLAC/STAC Wrappers

### 2.1 CR4 helpers

- [x] `read_cr4()`/`write_cr4()` + `CR4_SMEP`/`CR4_SMAP` in `cpu_security.c`
- [x] `cpu_enable_smep()` + `cpu_enable_smap()` with platform gate (`hv_supports_cr4_smep_smap()` -- skips on Hyper-V/WHPX, enables on KVM/VMware/VBox/bare metal)
- [x] Called on BSP via `cpu_harden()` in Phase 0 and on each AP in `ap_entry()`

### 2.2 CLAC/STAC inline wrappers

- [x] `stac()`/`clac()` inlines + `KERNEL_ACCESS_USER_BEGIN()`/`KERNEL_ACCESS_USER_END()` macros in `cpu_security.h`; no-op if `!cpu_has(CPU_FEATURE_SMAP)`
- [x] `copy_from_user()` / `copy_to_user()` added in `cpu_security.c` with SMAP brackets
- [ ] Migrate existing syscall argument dereferences to use `copy_from_user` — deferred to syscall TODO
- [ ] `ProbeForRead` / `ProbeForWrite` — deferred to TODO-10 (SEH)
- [x] IDT `isr_common_stub` emits `clac` on kernel entry — AC=0 guaranteed at top of every interrupt/exception handler

### 2.3 Commit

- [x] Committed across multiple patches: CR4 SMEP/SMAP with Hyper-V gate, CLAC/STAC macros, copy_from/to_user, IDT clac

---

## 3. KPTI: Per-Process User Page Table + CR3 Swap `[Opus]`

### 3.1 Dual page-table design

- [ ] Each process gets two `pml4_t` roots:
  - **Kernel CR3** (`task->kernel_cr3`): existing full mapping (kernel + user VA); used for all kernel-mode execution
  - **User CR3** (`task->user_cr3`): sparse mapping; contains only:
    - The process's own user-space pages (`[0, 0x800000000000)`)
    - The syscall entry trampoline stub page (one 4 KiB page containing the `syscall` entry `SWAPGS` + CR3 swap code; must be mapped NX=0 at the same VA in both tables)
    - The GDT/TSS page (CPU needs these in both tables)
  - Kernel text, data, heap, stacks, and other processes are **absent** from `user_cr3` — this is the Meltdown fix

### 3.2 User CR3 construction

- [ ] `vmm_create_user_cr3(task)`:
  1. `pmm_alloc_contiguous(1)` — allocate a fresh PML4 page, zero it
  2. Copy user-space PML4 entries (`pml4[0..255]`) from the kernel CR3 into the new PML4 (user half of VA space)
  3. Map only the KPTI trampoline stub and GDT/TSS into the upper-half entries that the user CR3 needs; all other kernel PML4 entries remain absent
- [ ] `vmm_sync_user_cr3(task)` — called whenever a user-space page is mapped/unmapped: sync the corresponding PML4 entry in `task->user_cr3`

### 3.3 CR3 swap at ring transitions

- [ ] Syscall entry (→ XREF `TODO-05-native-api-ssdt.md §2`): immediately after `SWAPGS`:
  ```asm
  mov rax, [gs:pcpu_kernel_cr3]   ; load per-CPU saved kernel CR3
  mov cr3, rax                    ; switch to kernel CR3
  ```
- [ ] Syscall return: before `SYSRETQ`:
  ```asm
  mov rax, [gs:pcpu_user_cr3]     ; load current task's user CR3
  mov cr3, rax                    ; switch back to user CR3
  ```
- [ ] Interrupt/exception entry (all 256 IDT stubs): same CR3 swap pattern; the KPTI trampoline stub is the first code that runs in user CR3 context and immediately swaps to the kernel CR3
- [ ] Per-CPU `pcpu_kernel_cr3` and `pcpu_user_cr3` fields in `cpu_data` struct (→ XREF `src/kernel/smp/smp.c` — `cpu_data` per-CPU struct, already implemented); updated on every scheduler context switch

### 3.4 Commit

- [ ] Commit: `"kernel/security: KPTI dual page tables, user/kernel CR3 swap at ring transitions"`

---

## 4. PCID: TLB Tagging for No-Flush CR3 Switch `[Opus]`

### 4.1 PCID assignment

- [ ] Enable `CR4.PCIDE (bit 17)` if `cpu_has(CPU_FEATURE_PCID)`: `cpu_set_cr4_bit(CR4_PCIDE)` during `cpu_enable_smep_smap()`
- [ ] Assign a 12-bit PCID to each process; stored in `task->pcid`:
  - PCID 0 = reserved for initial boot / no-PCID fallback
  - PCID 1..4094 = per-process; allocated from a monotone counter with wrap; on wrap, issue a global `INVPCID` (type 2 = global) to flush all stale TLB entries
  - `user_cr3` physical address gets `PCID` in bits [11:0]: `cr3_val = task->user_cr3_phys | task->pcid`
  - `kernel_cr3` uses `PCID + 0x800` convention (top bit set = kernel tag)

### 4.2 No-flush CR3 write

- [ ] When `CR4.PCIDE=1`, writing CR3 with bit 63 set (`NOFLUSH=1`) skips the TLB invalidation for that PCID; only entries with a different PCID are retained:
  ```asm
  ; Switch to kernel CR3 with NOFLUSH (bit 63 set in the value)
  mov rax, [gs:pcpu_kernel_cr3]
  bts rax, 63                     ; set NOFLUSH bit
  mov cr3, rax
  ```
- [ ] Use NOFLUSH on all hot-path CR3 writes (syscall entry/return, timer interrupt); use full-flush CR3 write only when `INVPCID` is needed (e.g., on `munmap` of a user page)
- [ ] `INVPCID` type 1 (`INVPCID_ADDR`) for single-VA invalidation: `invpcid [pcid, va]` in `vmm_flush_tlb(va)` when PCID is active

### 4.3 Commit

- [ ] Commit: `"kernel/security: PCID TLB tagging, NOFLUSH CR3 writes, INVPCID for targeted flush"`

---

## 5. Spectre Mitigations: IBRS/IBPB + Retpoline `[Opus]`

### 5.1 IBRS on kernel entry/exit

- [ ] `MSR_IA32_SPEC_CTRL = 0x48`; `SPEC_CTRL_IBRS = 1`:
  ```c
  void cpu_spec_ctrl_enter_kernel(void) {
      if (cpu_has(CPU_FEATURE_IBRS))
          wrmsr(MSR_IA32_SPEC_CTRL, SPEC_CTRL_IBRS);
  }
  void cpu_spec_ctrl_exit_kernel(void) {
      if (cpu_has(CPU_FEATURE_IBRS))
          wrmsr(MSR_IA32_SPEC_CTRL, 0);
  }
  ```
- [ ] Insert `cpu_spec_ctrl_enter_kernel()` at kernel entry (syscall entry stub and every ISR common stub) and `cpu_spec_ctrl_exit_kernel()` at kernel exit (SYSRETQ / IRETQ); the MSR writes have ~20 cycle overhead — acceptable for syscall paths
- [ ] If `cpu_has(CPU_FEATURE_ENHANCED_IBRS)` (CPUID leaf 7 EDX bit 29): set IBRS once at boot and never clear it (Enhanced IBRS is always-on and has no exit overhead)

### 5.2 IBPB at context switch

- [ ] `MSR_IA32_PRED_CMD = 0x49`; `PRED_CMD_IBPB = 1`
- [ ] `cpu_issue_ibpb()` — write 1 to `MSR_IA32_PRED_CMD` to flush the branch predictor on context switch; call in `sched_switch_task()` when switching between processes with different security domains (UIDs / token user SIDs differ); skip if same UID to reduce overhead

### 5.3 Retpoline

- [ ] Add `-mindirect-branch=thunk-extern` (GCC) or `-mretpoline` (Clang 19) to `CFLAGS` in `Makefile`; Clang 19 (`clang-19`) is already the compiler, so use `-mretpoline -mretpoline-external-thunk`
- [ ] Provide the retpoline thunk in `src/kernel/retpoline.asm` (one thunk per scratch register `rax`–`r15`; Clang emits `call __x86_indirect_thunk_rax` instead of `jmp rax`):
  ```asm
  __x86_indirect_thunk_rax:
      call .set_up_target
  .capture_spec:
      pause
      lfence
      jmp  .capture_spec
  .set_up_target:
      mov [rsp], rax
      ret
  ```
- [ ] Verify no `jmp *reg` or `call *reg` remains in kernel assembly after the build: `objdump -d build/kernel.elf | grep -E "jmp.*%r|call.*%r"` — must be empty

### 5.4 Commit

- [ ] Commit: `"kernel/security: IBRS on kernel entry/exit, IBPB at context switch, retpoline build flag"`

---

## 6. CET Shadow Stack (Kernel Ring 0) `[Opus]`

### 6.1 CET MSRs and CR4

- [ ] MSR definitions:
  ```c
  #define MSR_IA32_S_CET           0x6A2   /* supervisor shadow stack control */
  #define MSR_IA32_PL0_SSP         0x6A4   /* ring-0 shadow stack pointer */
  #define MSR_IA32_INTERRUPT_SSP_TABLE 0x6A8 /* IST shadow stack table */
  #define S_CET_SH_STK_EN          (1ULL << 0)
  #define S_CET_WR_SHSTK_EN        (1ULL << 1)  /* WRSS instruction enable */
  #define S_CET_ENDBR_EN           (1ULL << 2)  /* IBT control (§7) */
  ```
- [ ] `cpu_enable_cet_ss()`:
  1. `cpu_set_cr4_bit(CR4_CET)` — enable CET in CR4
  2. `wrmsr(MSR_IA32_S_CET, S_CET_SH_STK_EN | S_CET_WR_SHSTK_EN)`
  3. Ensure `MSR_IA32_PL0_SSP` is set to the initial kernel shadow stack page's last 8 bytes (top of the shadow stack)

### 6.2 Shadow stack page allocation

- [ ] Each kernel thread needs a shadow stack: one 4 KiB page per thread, marked `PTE_USER=0`, `PTE_NX=1`, and the special **supervisor shadow stack token** format (bit 1 of the 8-byte token set — indicates this is the bottom of the shadow stack)
- [ ] `cet_alloc_shadow_stack(thread)` — `pmm_alloc_contiguous(1)`; write token at the end of the page; `vmm_map_page(shadow_stack_va, pa, PTE_SUPERVISOR_SHADOW_STACK)` — PTE bit 5 = 1 marks shadow-stack pages; processor enforces SHSTK semantics (only `RSTORSSP`/`SAVEPREVSSP` can write)
- [ ] On kernel thread creation in `src/kernel/sched/task.c` (`task_create()`): allocate shadow stack; set `task->shadow_stack_top`
- [ ] Context switch: save/restore `MSR_IA32_PL0_SSP` per thread

### 6.3 Exception / interrupt shadow stacks (IST)

- [ ] Windows uses IST entries in the TSS for critical exceptions (#DF, #PF, NMI); each IST entry must also get a shadow stack in the `MSR_IA32_INTERRUPT_SSP_TABLE` (8-entry table, one VA per IST slot)
- [ ] `cet_init_interrupt_ssp_table()` — allocate 8 shadow stack pages; write their top VAs into the 64-byte `INTERRUPT_SSP_TABLE` structure; write the table physical address to `MSR_IA32_INTERRUPT_SSP_TABLE`

### 6.4 Commit

- [ ] Commit: `"kernel/security: CET shadow stack — CR4.CET, S_CET MSR, per-thread SSP allocation"`

---

## 7. CET Indirect Branch Tracking (IBT / ENDBR64) `[Opus]`

### 7.1 Build system: `-fcf-protection=branch`

- [ ] Add `-fcf-protection=branch` to kernel `CFLAGS` (Clang 19 supports this); the compiler emits `ENDBR64` at the start of every function and every valid indirect call/jump target
- [ ] Verify: `objdump -d build/kernel.elf | grep endbr64 | wc -l` — must be > 0 (non-zero); count should match approximate function count
- [ ] Assembly files (`src/kernel/smp/ap_trampoline.asm`, ISR stubs, IDT stubs): manually add `endbr64` at each entry point that is reached via an indirect branch; NASM opcode: `db 0xF3, 0x0F, 0x1E, 0xFA`

### 7.2 Enable IBT in S_CET

- [ ] `cpu_enable_cet_ibt()` — add to `cpu_enable_cet_ss()` call sequence: `wrmsr(MSR_IA32_S_CET, rdmsr(MSR_IA32_S_CET) | S_CET_ENDBR_EN)`
- [ ] Legacy code mode: if a code region is loaded that does not have `ENDBR64` instructions (e.g., a legacy driver), temporarily disable IBT via `MSR_IA32_S_CET.NO_TRACK_EN` for that execution context; re-enable after (requires driver annotation `MODULE_FLAG_NO_IBT`)
- [ ] `ENDBR_EN` should be enabled after all kernel code is loaded and verified; setting it before loading a module without ENDBR64 would immediately fault

### 7.3 Commit

- [ ] Commit: `"kernel/security: CET IBT — ENDBR64 in kernel build, S_CET.ENDBR_EN activation"`

---

## 8. Kernel Heap Hardening: Cookies & Redzones `[Sonnet]`

### 8.1 Allocation header

- [ ] Extend the `kmalloc` block header in `src/kernel/mm/heap.c`:
  ```c
  #define HEAP_COOKIE_MAGIC  0xDEADBEEFC0FFEE01ULL  /* XOR'd with alloc address */

  typedef struct {
      uint64_t  cookie;        /* HEAP_COOKIE_MAGIC ^ (uint64_t)block_ptr */
      uint32_t  size;          /* requested allocation size */
      uint32_t  redzone_front; /* 0xFEFEFEFE — detect underflow */
      /* user data follows */
      /* uint8_t redzone_back[8] after user data — detect overflow */
  } kmalloc_header_t;
  ```
- [ ] `kmalloc(size)`:
  - Allocate `sizeof(kmalloc_header_t) + size + 8` bytes from the heap
  - Write `cookie = HEAP_COOKIE_MAGIC ^ (uint64_t)header_ptr`
  - Write `redzone_front = 0xFEFEFEFEFEFEFEFEULL`
  - Write redzone_back 8 bytes after user data = `0xBDBDBDBDBDBDBDBDULL`
  - Return `(header + 1)` — pointer to user data portion
- [ ] `kfree(ptr)`:
  - Recover header = `(kmalloc_header_t *)ptr - 1`
  - Validate `cookie == HEAP_COOKIE_MAGIC ^ (uint64_t)header`: if mismatch → `KeBugCheckEx(BUGCHECK_HEAP_CORRUPTION, ...)` (→ XREF `TODO-16-crash-dump-generation.md §1`)
  - Validate `redzone_front == 0xFEFEFEFEFEFEFEFEULL` — detect underflow
  - Validate redzone_back == `0xBDBDBDBDBDBDBDBDULL` — detect overflow
  - Zero the user data before returning to pool (`explicit_bzero`)
- [ ] `kmalloc_zeroed(size)` — like `kmalloc` but zeroes the user region immediately (for security-sensitive allocations like token structs, security descriptors)

### 8.2 POOL_TAG tagging (debug)

- [ ] Extend `kmalloc_tagged(size, tag)` where `tag` is a 4-byte ASCII pool tag (e.g., `'TOKN'`, `'ALPC'`) stored in a 5th header field; `kfree_tagged(ptr, tag)` verifies the tag matches to catch mixed-pool use-after-free patterns; tag field is present only in debug builds (`KERNEL_DEBUG` defined)
- [ ] Existing callers that use plain `kmalloc` get implicit tag `'\0\0\0\0'`

### 8.3 Commit

- [ ] Commit: `"kernel/mm: kmalloc cookie + redzone hardening, kmalloc_zeroed, POOL_TAG debug"`

---

## 9. Stack Canaries (`-fstack-protector-strong`) `[Sonnet]`

### 9.1 Build system change

- [ ] Remove `-fno-stack-protector` from `CFLAGS` in `Makefile`
- [ ] Add `-fstack-protector-strong` — protects functions that have:
  - local arrays or structs
  - address-taken local variables
  - calls to `alloca` Clang 19 supports this exactly
- [ ] Verify no `__stack_chk_guard` linker error before §9.2 provides the symbol

### 9.2 `__stack_chk_guard` initialization

- [ ] `src/kernel/security/stack_canary.c`:
  ```c
  uintptr_t __stack_chk_guard = 0;  /* set by canary_init() */

  void canary_init(void) {
      uint64_t rand = 0;
      if (cpu_has(CPU_FEATURE_RDRAND)) {
          /* Three RDRAND retries per Intel spec */
          for (int i = 0; i < 3; i++) {
              uint8_t ok;
              __asm__ volatile(
                  "rdrand %0\n setc %1"
                  : "=r"(rand), "=r"(ok) : : "cc"
              );
              if (ok) break;
          }
      }
      if (!rand) {
          /* RDRAND unavailable: use RDTSC + ACPI timer XOR'd with kernel base */
          uint64_t tsc; __asm__ volatile("rdtsc" : "=A"(tsc));
          rand = tsc ^ (uint64_t)(uintptr_t)canary_init ^ 0xDEADC0DE00000000ULL;
      }
      /* Ensure canary never matches 0 or 0x00XXXXXXXX00 (null-terminator guard) */
      rand |= 0xFF00000000000000ULL;
      rand &= ~0x00000000000000FFULL;
      __stack_chk_guard = (uintptr_t)rand;
  }
  ```
- [ ] Call `canary_init()` very early in Phase 1 kernel init, before any stack-protected function is called (→ XREF `TODO-01-kernel-init-sequencing.md §3`)

### 9.3 `__stack_chk_fail` handler

- [ ] Add `__stack_chk_fail` handler to `src/kernel/security/stack_canary.c`:
  ```c
  __attribute__((noreturn)) void __stack_chk_fail(void) {
      KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE,
                   0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */, 0, 0, 0);
      __builtin_unreachable();
  }
  ```
- [ ] The `KeBugCheckEx` call triggers `panic_screen()` with the security stop code, generates a crash dump (→ XREF `TODO-16-crash-dump-generation.md §5`), and halts

### 9.4 Commit

- [ ] Commit: `"kernel/security: -fstack-protector-strong, RDRAND canary init, __stack_chk_fail"`

---

## 10. Kernel Stack Guard Pages `[Sonnet]`

### 10.1 Guard page below each kernel stack

- [ ] Every kernel thread stack is allocated as `STACK_SIZE + PAGE_SIZE` pages; the first page (bottom of the stack, lowest address) is mapped with `PTE_PRESENT=0` — an unmapped guard page:
  ```c
  void stack_alloc_with_guard(task_t *t) {
      uintptr_t pa = pmm_alloc_contiguous(KERNEL_STACK_PAGES + 1);
      /* Map guard page as not-present */
      vmm_map_page(t->stack_guard_va, pa, 0 /* not present */);
      /* Map stack pages above guard */
      for (int i = 1; i <= KERNEL_STACK_PAGES; i++)
          vmm_map_page(t->stack_base_va + i * PAGE_SIZE,
                       pa + i * PAGE_SIZE, PTE_KERNEL_RW | PTE_NX);
      t->rsp0 = t->stack_base_va + (KERNEL_STACK_PAGES + 1) * PAGE_SIZE;
  }
  ```
- [ ] On `#PF` with fault address in the guard page VA range: trigger `KeBugCheckEx(BUGCHECK_KERNEL_STACK_INPAGE_ERROR, ...)` rather than a generic page fault BSOD; this distinguishes stack overflow from null- pointer dereferences

### 10.2 IST stacks also need guard pages

- [ ] Each IST stack (`ist1`–`ist7` in the TSS) must also have a guard page below it; allocate with the same `stack_alloc_with_guard` helper; prevents nested-exception stack overflow from silently corrupting memory

### 10.3 Commit

- [ ] Commit: `"kernel/security: guard pages below kernel stacks and IST stacks"`

---

## 11. KASLR: RDRAND Kernel Load Address `[Opus]`

### 11.1 Boot-time address randomization

- [ ] The bootloader (`src/boot/uefi/bootx64.c`) currently loads the kernel ELF at its linked virtual base. For KASLR:
  1. Use UEFI `GetRNG` protocol (or `RDRAND` instruction) to generate a random 9-bit slide value `s ∈ [0, 511]`
  2. Add `s * 2 MiB` to the kernel's linked virtual base: `kernel_slide = s * 0x200000`
  3. Load kernel ELF sections at `linked_va + kernel_slide` instead of `linked_va`
  4. Pass `kernel_slide` to the kernel in `boot_info.kaslr_slide`

### 11.2 ELF relocation at load time

- [ ] The kernel ELF must be built as a position-independent executable or carry a `.rela.text` / `.rela.data` relocation table
- [ ] Build flag: add `-pie -fPIE` to kernel CFLAGS, or use `-mcmodel=kernel` with explicit relocation entries; verify with `readelf -r build/kernel.elf` that `R_X86_64_64` entries are present
- [ ] Bootloader applies relocations: for each `R_X86_64_64` entry, add `kernel_slide` to the stored address at the given offset
- [ ] Linker map (`kernel.map`): add `kernel_slide` to all symbol addresses before writing `kernel.sym` (→ XREF `TODO-16-crash-dump-generation.md §3`) so `symtab_resolve` remains accurate after KASLR

### 11.3 Kernel-side KASLR awareness

- [ ] In `kernel_main()`: read `boot_info.kaslr_slide`; store in `g_kaslr_slide`; make it available to the module registry (§3 of this TODO) and the crash dump writer (→ XREF `TODO-16-crash-dump-generation.md §4`) so dumps carry the slide value for post-mortem analysis
- [ ] `KASLR_BASE = __kernel_text_start + g_kaslr_slide`; all linker-symbol references throughout the kernel must add `g_kaslr_slide` when used as runtime addresses (or be recalculated from runtime `RIP`)

### 11.4 Commit

- [ ] Commit: `"kernel/security: KASLR — bootloader RDRAND slide, ELF relocation, kaslr_slide in boot_info"`

---

## 12. Enclave and Code Signing Syscalls Wired to SSDT

Register VBS/SGX enclave management and code signing verification syscalls in the SSDT. (→ XREF: TODO-05-native-api-ssdt.md §4)

- [ ] `NtCreateEnclave(ProcessHandle, BaseAddress, ZeroBits, Size, InitialCommitment, EnclaveType, EnclaveInformation, InformationLength, EnclaveError)` → SSDT 0x01F0
- [ ] `NtLoadEnclaveData(ProcessHandle, BaseAddress, Buffer, BufferSize, Protect, PageInformation, InformationLength, NumberOfBytesWritten, EnclaveError)` → SSDT 0x01F1
- [ ] `NtInitializeEnclave(ProcessHandle, BaseAddress, EnclaveInformation, InformationLength, EnclaveError)` → SSDT 0x01F2
- [ ] `NtTerminateEnclave(BaseAddress, WaitForThread)` → SSDT 0x01F3
- [ ] `NtCallEnclave(EnclaveRoutine, WaitForThread, EnclaveRoutineReturn)` → SSDT 0x01F4
- [ ] `NtSetCachedSigningLevel(Flags, InputSigningLevel, SourceFiles, SourceFileCount, TargetFile)` → SSDT 0x02A2
- [ ] `NtGetCachedSigningLevel(File, Flags, SigningLevel, Thumbprint, ThumbprintSize, ThumbprintAlgorithm)` → SSDT 0x02A3
- [ ] `NtCompareSigningLevels(FirstSigningLevel, SecondSigningLevel)` → SSDT 0x02A4
- [ ] All functions return `NTSTATUS`
- [ ] Commit: `"kernel/security: wire Enclave and code signing syscalls to SSDT"`

**Test checkpoint:** `NtCreateEnclave` allocates enclave region. `NtSetCachedSigningLevel` stores signing level on file. `NtGetCachedSigningLevel` retrieves it. `NtCompareSigningLevels` returns correct ordering.

---

## OS Comparison


| ⭐ | Feature                                  | 🪟 Win11                            | 🐧 Linux                               | 🚀 Impossible OS                                   |
|----|------------------------------------------|----------------------------------|-------------------------------------|-------------------------------------------------|
| 💎 | NX / XD bit on data pages                | ✅ Since Windows XP SP2          | ✅ Since kernel 2.6.8               | ⬜ §1                                           |
| 💎 | SMEP                                     | ✅ Windows 8+                    | ✅ kernel 3.0+                      | ⬜ §2                                           |
| 💎 | SMAP                                     | ✅ Windows 10+                   | ✅ kernel 3.20+                     | ⬜ §2                                           |
| 💎 | KPTI                                     | ✅ Windows 10 Jan 2018           | ✅ kernel 4.15 (PTI)                | ⬜ §3                                           |
| 💎 | PCID TLB tagging for KPTI                | ✅ Full                          | ✅ Full                             | ⬜ §4                                           |
| 💎 | IBRS / IBPB                              | ✅ Full + Enhanced IBRS          | ✅ Full + spectre_v2 mitigations    | ⬜ §5                                           |
| 💎 | Retpoline                                | ✅ `/Qspectre` MSVC              | ✅ `-mindirect-branch=thunk-extern` | ⬜ §5                                           |
| 💎 | CET shadow stack                         | ✅ Windows 10 20H1+              | ✅ kernel 6.6+ (x86 CET-SS)         | ⬜ §6                                           |
| 💎 | CET IBT                                  | ✅ Windows 11 (HVCI)             | ✅ kernel 6.6+ (x86 CET-IBT)        | ⬜ §7                                           |
| 💎 | Heap corruption detection                | ✅ Pool header cookies           | ✅ SLUB `kmem_cache` redzone        | ⬜ §8                                           |
| 💎 | Stack canaries                           | ✅ `/GS` compiler switch         | ✅ `-fstack-protector-strong`       | ⬜ §9                                           |
| 💎 | Kernel stack guard pages                 | ✅ Full (kernel stack expansion) | ✅ `THREAD_SIZE` guard page         | ⬜ §10                                          |
| 💎 | KASLR                                    | ✅ KVA shadow + PatchGuard       | ✅ `CONFIG_RANDOMIZE_BASE`          | ⬜ §11                                          |
| ⭐ | Hardware RNG (RDRAND) for canary & KASLR | ✅ (internal, no disclosure)     | ✅ (entropy pool, no guarantee)     | ⬜ §9+§11 — (RDRAND-first) 🚀                   |
| ⭐ | Canary fallback visible in crash dump    | ❌ Opaque                        | ❌ Opaque                           | ⬜ §9 — +TODO-16-crash-dump-generation.md §4 🚀 |

After §1–10, Impossible OS reaches full Windows 11 / Linux security-hardening parity for
2026. Linux without CONFIG_RANDOMIZE_BASE, CET, or IBRS (common in embedded/legacy
configs) is weaker; Windows requires HVCI (Virtualization-Based Security) for CET IBT to
be mandatory. Impossible OS enforces all mitigations unconditionally at ring-0 without
requiring a hypervisor. The RDRAND-first canary initialization and the crash-dump-visible
KASLR slide (for post-mortem analysis) are minor implementation-quality exclusives.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_cpu_security()` (→ XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_cpu_security.c` with:
  - NX verification: `EFER.NXE` bit set (read MSR 0xC0000080)
  - SMEP verification: CR4.SMEP set OR hypervisor EPT enforces (platform-dependent)
  - SMAP verification: CR4.SMAP set OR hypervisor EPT enforces
  - KPTI: user-mode PML4 entries don't map kernel text (when per-process PT active)
  - Stack canary: `__stack_chk_guard` is non-zero and randomized
  - KASLR: kernel base != default 0x100000 (when KASLR enabled)
  - W^X kernel pages: text pages are R-X (not writable), data pages are RW- (not executable)
  - Spectre v2: IBRS or retpoline active (check MSR or compiler flag)
  - CET: if CPU supports CET, IBT/SHSTK status verified
  - UMIP: if CPU supports UMIP, CR4.UMIP set
- [ ] Register in `test_runner_init()`: `test_register_cpu_security()`
- [ ] Commit: `"test: add CPU security hardening test suite"`

---

## Verification

- [ ] **NX**: map a heap page and attempt to `jmp` to it; must raise `#PF` with error code bit 4 (Instruction Fetch) set.
- [ ] **SMEP**: write a ring-3 code page VA into a kernel function pointer and call it; must raise `#PF` with bit 4 set before executing user code.
- [ ] **SMAP**: dereference a user-space pointer from kernel context without `STAC`; must raise `#PF` with bit 5 (Protection Key Violation) set.
- [ ] **KPTI**: in user mode, attempt to read a known kernel VA (`0xFFFF800000000000`) via a side channel; must receive `#PF` with no data leak.
- [ ] **IBRS**: verify `rdmsr(MSR_IA32_SPEC_CTRL) & 1` is `1` during kernel execution, `0` after SYSRETQ.
- [ ] **Retpoline**: `objdump -d build/kernel.elf | grep -E 'jmp\s+\*%|call\s+\*%'` — must produce zero lines (all indirect branches replaced).
- [ ] **CET SS**: corrupt a return address on the kernel stack; return must cause `#CP (Control Protection)` exception, not jump to the corrupted address.
- [ ] **Heap cookie**: call `kfree(ptr)` after zeroing the cookie field; must trigger `BUGCHECK_HEAP_CORRUPTION` BSOD.
- [ ] **Stack canary**: overflow a local array past the canary slot and return; must trigger `__stack_chk_fail` → `BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE`.
- [ ] **Guard page**: write to `task->stack_guard_va`; must raise `BUGCHECK_KERNEL_STACK_INPAGE_ERROR`, not silent memory corruption.
- [ ] **KASLR**: two consecutive boots must load the kernel at different base addresses (verify via `dmpanalyze.exe` `ImpossibleOSInfoStream.KernelPath` showing different `kaslr_slide` values).
- [ ] **Remaining limits**: CET IBT requires every kernel assembly stub to carry `ENDBR64`; audit `src/kernel/smp/ap_trampoline.asm` and all IDT stubs before enabling `S_CET_ENDBR_EN`; missing ENDBR64 in a called indirect target causes an immediate `#CP` fault.
- [ ] Commit: `"kernel/security: CPU mitigations (NX/SMEP/SMAP/KPTI/Spectre/CET), heap/stack hardening, KASLR"`
