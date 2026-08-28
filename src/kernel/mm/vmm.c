/* ============================================================================
 * vmm.c -- Virtual Memory Manager (x86-64 4-level paging)
 *
 * Takes over the boot-time page tables built by the UEFI bootloader
 * (setup_page_tables() in src/boot/uefi/bootx64.c) and provides fine-grained
 * 4 KiB page mapping via the PML4 -> PDPT -> PD -> PT hierarchy. There is no
 * assembly entry stub in the kernel; the bootloader loads CR3 and calls
 * kernel_main directly.
 *
 * The boot page tables already identity-map the first 4 GiB with 2 MiB pages.
 * This VMM adds the ability to map individual 4 KiB pages for dynamic use
 * (heap, userspace, device MMIO, etc).
 *
 * Page table structure (x86-64):
 *   Virtual address: [PML4 idx][PDPT idx][PD idx][PT idx][offset]
 *                     9 bits    9 bits    9 bits   9 bits  12 bits
 *
 * The canonical virtual layout this VMM will move to lives in
 * include/kernel/mm/memmap.h (docs/infrastructure/kernel-address-space.md).
 * Including it here keeps its _Static_assert layout gate live in every build.
 * ============================================================================ */

#include "kernel/mm/vmm.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/memmap.h"
#include "kernel/mm/user_range.h"
#include "kernel/cpuid.h"
#include "kernel/idt.h"
#include "kernel/klog.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/mm/swap.h"
#include "kernel/mm/mmap.h"
#include "kernel/panic.h"
#include "kernel/except.h"              /* #PF triage: EXCEPTION_RECORD/CONTEXT + dispatch ABI */
#include "kernel/wer.h"                  /* WerpReportFault -- serial WER hook on the user terminal */
#include "kernel/boot_halt.h"           /* boot_halt: fatal NX-policy enforcement failure */
#include "kernel/bugcheck.h"            /* KeBugCheckEx: pt_walk hard-error on a corrupt PTE */
#include "kernel/sched/spinlock.h"      /* s_mmio_lock: SMP-safe MMIO VA allocator (unconditional) */
#include "kernel/smp.h"                 /* smp_this_cpu() + MAX_CPUS: per-CPU #PF exception scratch */
/* UNCONDITIONAL, and it must stay outside the KERNEL_TESTS guard below:
 * page_fault_handler calls kread_u8_fixup_lookup on the production path, so
 * test-gating this declaration breaks the release flavor (-UKERNEL_TESTS
 * -Werror) while every test build stays green. */
#include "kernel/cpu_security.h"        /* kread_u8_fixup_lookup: guarded-read routing */
#ifdef KERNEL_TESTS
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif

/* pte_t and PTE_ADDR_MASK now live in vmm.h (shared with swap.c walkers). */

#ifdef KERNEL_TESTS
/* Test-only vmm_map fault injection (kernel-test-harness roadmap). Same shape as the
 * kmalloc/pmm hooks: IRQL gate, task filter, max-injections cap,
 * per-CPU countdown. On fire, vmm_map_page returns -1 without touching
 * page tables, so the caller's partial-map rollback path is exercised. */
static uint64_t s_vmm_map_fault_injections;

static int vmm_map_fault_should_fire(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return 0;
    struct per_cpu_data *pc = smp_this_cpu();
    if (!pc || !pc->vmm_map_fail_countdown)
        return 0;

    if (pc->vmm_map_fail_task_pid != 0) {
        struct task *t = task_current();
        if (!t || t->pid != pc->vmm_map_fail_task_pid)
            return 0;
    }
    if (pc->vmm_map_fail_max_injections != 0 &&
        pc->vmm_map_fail_fired_counter >= pc->vmm_map_fail_max_injections) {
        return 0;
    }
    if (--pc->vmm_map_fail_countdown != 0)
        return 0;

    __atomic_fetch_add(&s_vmm_map_fault_injections, 1ull, __ATOMIC_RELAXED);
    pc->vmm_map_fail_fired_counter++;
    /* auto-reload for multi-fire: see the heap-side comment. */
    if (pc->vmm_map_fail_max_injections != 0 &&
        pc->vmm_map_fail_fired_counter < pc->vmm_map_fail_max_injections) {
        pc->vmm_map_fail_countdown = 1;
    }
    return 1;
}

void vmm_map_fail_countdown_set(uint32_t n) { smp_this_cpu()->vmm_map_fail_countdown = n; }
void vmm_map_fail_countdown_clear(void)    { smp_this_cpu()->vmm_map_fail_countdown = 0; }
void vmm_map_fail_next(void)               { smp_this_cpu()->vmm_map_fail_countdown = 1; }
uint64_t vmm_map_fail_injections_triggered(void)
{
    return __atomic_load_n(&s_vmm_map_fault_injections, __ATOMIC_RELAXED);
}
void vmm_map_fail_task_filter_set(uint32_t task_pid)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->vmm_map_fail_task_pid     = task_pid;
    pc->vmm_map_fail_fired_counter = 0;
}
void vmm_map_fail_task_filter_clear(void)  { smp_this_cpu()->vmm_map_fail_task_pid = 0; }
void vmm_map_fail_max_injections_set(uint32_t max)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->vmm_map_fail_max_injections = max;
    pc->vmm_map_fail_fired_counter  = 0;
}
void vmm_map_fail_max_injections_clear(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->vmm_map_fail_max_injections = 0;
    pc->vmm_map_fail_fired_counter  = 0;
}
uint32_t vmm_map_fail_fired_counter(void)
{
    return smp_this_cpu()->vmm_map_fail_fired_counter;
}
#endif /* KERNEL_TESTS */

/* Number of entries per page table level */
#define PT_ENTRIES 512

/* PTE_ADDR_MASK moved to vmm.h (shared with swap.c). */

/* 1 GiB page alignment: lower 30 bits must be zero */
#define GIB_ALIGN_MASK  0x3FFFFFFFULL
#define GIB_SIZE        (1ULL << 30)

/* Kernel root page table as the HHDM WALK pointer (section 9 walker
 * conversion): the physical root translated through the direct map, so every
 * walker dereferences it safely once the identity map is gone. Written ONCE in
 * vmm_init on the BSP before any AP starts, read-only afterwards, so no lock is
 * needed. The PHYSICAL root (the value loaded raw into CR3) is NOT stored as a
 * second global -- it is derived on demand via mm_hhdm_to_phys(kernel_pml4) in
 * vmm_get_kernel_cr3(). This keeps the phys-vs-walk split (spec item 1) at zero
 * new BSS cost: a second static pointer trips the zero-headroom BSS ceiling
 * that section 7 has not yet retired, which would break section 9's
 * independent-bootability guarantee. */
static pte_t *kernel_pml4;

/* --- Address decomposition --- */

static inline uint64_t pml4_index(uintptr_t addr)
{
    return (addr >> 39) & 0x1FF;
}

static inline uint64_t pdpt_index(uintptr_t addr)
{
    return (addr >> 30) & 0x1FF;
}

static inline uint64_t pd_index(uintptr_t addr)
{
    return (addr >> 21) & 0x1FF;
}

static inline uint64_t pt_index(uintptr_t addr)
{
    return (addr >> 12) & 0x1FF;
}

/* --- CR3 helpers --- */

static inline uintptr_t read_cr3(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline uintptr_t read_cr2(void)
{
    uintptr_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uintptr_t val)
{
    __asm__ volatile ("mov %0, %%cr3" : : "r"(val) : "memory");
}

/* --- TLB management --- */

void vmm_flush_tlb(uintptr_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

void vmm_flush_tlb_all(void)
{
    write_cr3(read_cr3());
}

/* --- Zero a page-sized allocation --- */

static void zero_page(uintptr_t phys_addr)
{
    /* Write through the HHDM alias, not the raw physical address: a freshly
     * allocated page-table frame is zeroed here before it is installed, and
     * that write must land through a mapping that outlives the identity map. */
    uint64_t *page = (uint64_t *)pt_walk(phys_addr);
    uint32_t i;
    for (i = 0; i < VMM_PAGE_SIZE / 8; i++)
        page[i] = 0;
}

/* --- Page table walking / creation --- */

/* The single sanctioned page-table dereference primitive (see vmm.h). Kept
 * non-inline so its body exists once rather than at every walk site -- the
 * inlined form pushed the kernel image over the pre-section-7 user ceiling. */
pte_t *pt_walk(uintptr_t phys)
{
    void *v = mm_phys_to_hhdm(phys);
    if (!v)
        KeBugCheckEx(BUGCHECK_CRITICAL_STRUCTURE_CORRUPTION,
                     (uint64_t)phys, 0, 0, 0);
    return (pte_t *)v;
}

/*
 * Get or create a page table at the next level.
 * If 'create' is true and the entry doesn't exist, allocates a new frame.
 * Returns pointer to the next-level table, or NULL on failure.
 */
static pte_t *get_or_create_table(pte_t *table, uint64_t index, int create,
                                   uint64_t leaf_flags)
{
    pte_t entry = table[index];

    /* If already present, return the next-level table as an HHDM walk
     * pointer (pt_walk) -- and propagate the User bit upward if the caller
     * is installing a
     * user-mode leaf PTE. Intermediate levels (PML4/PDPT/PD) must have
     * the User bit set at EVERY level for ring-3 page walks to
     * succeed; without this, a leaf PTE with VMM_FLAG_USER still
     * faults on user-mode access because the CPU's page walk rejects
     * the first kernel-only intermediate entry. Bug surfaced by the
     * user-mode binary format loader coverage probe with PE (ERR=0x15:
     * present + user + instruction fetch at ImageBase). */
    if (entry & VMM_FLAG_PRESENT) {
        /* If it's a huge page (2 MiB), we can't drill down further */
        if (entry & VMM_FLAG_HUGE)
            return (pte_t *)0;

        if (leaf_flags & VMM_FLAG_USER)
            table[index] = entry | VMM_FLAG_USER;

        return pt_walk(entry & PTE_ADDR_MASK);
    }

    /* Not present -- create if requested */
    if (!create)
        return (pte_t *)0;

    /* Allocate a new page for the table */
    uintptr_t new_frame = pmm_alloc_frame();
    if (new_frame == 0)
        return (pte_t *)0;   /* out of memory */

    /* Zero the new table */
    zero_page(new_frame);

    /* Install the entry: present + writable, plus User if the leaf
     * is a user-mode mapping. x86-64 requires User at every level
     * for ring-3 walks; without this the CPU faults with ERR=0x15
     * (present + user + instr fetch) on the first user access that
     * traverses this freshly-created intermediate table. */
    uint64_t intermediate = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    if (leaf_flags & VMM_FLAG_USER)
        intermediate |= VMM_FLAG_USER;
    table[index] = new_frame | intermediate;

    return pt_walk(new_frame);
}

/* --- Public API --- */

int vmm_map_page(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

#ifdef KERNEL_TESTS
    /* fault-inject -- check BEFORE any page-table modification so a
     * forced failure leaves PTE state byte-identical to a real OOM. */
    if (vmm_map_fault_should_fire())
        return -1;
#endif

    /* Align addresses to page boundaries */
    virt &= ~((uintptr_t)0xFFF);
    phys &= ~((uintptr_t)0xFFF);

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk/create PML4 → PDPT. Pass `flags` so the User bit
     * propagates upward when the caller is installing a user-mode
     * leaf PTE (PE loader at ImageBase, MMIO maps, guard-page
     * splits, etc.). See get_or_create_table for why every level
     * must carry User when the leaf does. */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 1, flags);
    if (!pdpt) return -1;

    /* Walk/create PDPT → PD */
    pd = get_or_create_table(pdpt, pdpti, 1, flags);
    if (!pd) return -1;

    /* Walk/create PD → PT
     * Note: if PD[pdi] is a 2 MiB huge page, we can't create a PT here.
     * In that case we'd need to split the huge page -- for now, fail. */
    pt = get_or_create_table(pd, pdi, 1, flags);
    if (!pt) return -1;

    /* Clear NX flag if CPU doesn't support it (older hardware) */
    if (!cpu_has(CPU_FEATURE_NX))
        flags &= ~VMM_FLAG_NX;

    /* Set the PT entry */
    pt[pti] = phys | flags;

    /* Flush the TLB for this virtual address */
    vmm_flush_tlb(virt);

    return 0;
}

void vmm_unmap_page(uintptr_t virt, int free_frame)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uintptr_t phys;

    virt &= ~((uintptr_t)0xFFF);

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk PML4 → PDPT (don't create) */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return;

    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return;

    pt = get_or_create_table(pd, pdi, 0, 0);
    if (!pt) return;

    /* Get the physical address before clearing */
    phys = pt[pti] & PTE_ADDR_MASK;

    /* Clear the entry */
    pt[pti] = 0;

    /* Flush TLB */
    vmm_flush_tlb(virt);

    /* Optionally free the physical frame */
    if (free_frame && phys)
        pmm_free_frame(phys);
}

int vmm_protect(uintptr_t virt, uint64_t new_flags)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uintptr_t phys;

    virt &= ~((uintptr_t)0xFFF);

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk without creating */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return -1;
    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return -1;

    /* Cannot protect pages inside a 2 MiB huge page -- must split first */
    if (pd[pdi] & VMM_FLAG_HUGE)
        return -1;

    pt = get_or_create_table(pd, pdi, 0, 0);
    if (!pt) return -1;
    if (!(pt[pti] & VMM_FLAG_PRESENT))
        return -1;

    /* Preserve physical address, replace flags */
    phys = pt[pti] & PTE_ADDR_MASK;
    if (!cpu_has(CPU_FEATURE_NX))
        new_flags &= ~VMM_FLAG_NX;
    pt[pti] = phys | new_flags;
    vmm_flush_tlb(virt);

    return 0;
}

int vmm_protect_range(uintptr_t addr, uint64_t size, uint64_t new_flags)
{
    uintptr_t end;
    addr &= ~((uintptr_t)0xFFF);
    /* Overflow-safe end computation */
    if (size > 0x7FFFFFFFFFFF || addr + size < addr)
        return -1;  /* reject overflow */
    end = (addr + size + 0xFFF) & ~((uintptr_t)0xFFF);
    if (end < addr)
        return -1;  /* wrapped */
    /* NOTE: No SMP TLB shootdown or page-table lock. Currently safe because
     * vmm_protect_range is only called during single-threaded ELF load.
     * When called from multi-CPU context, add IPI-based TLB shootdown
     * and per-address-space locking. */
    while (addr < end) {
        if (vmm_protect(addr, new_flags) != 0)
            return -1;
        addr += VMM_PAGE_SIZE;
    }
    return 0;
}

/* Read-modify-write the PTE flag bits for one 4 KiB page, splitting a covering
 * huge page first and PRESERVING all other flags (unlike vmm_protect, which
 * replaces them wholesale). or_bits are set; and_clear bits are cleared. Local
 * TLB flush only -- the caller must guarantee single-CPU context. Returns 0, or
 * -1 if the page is absent or a split fails. Used by the UEFI runtime W^X pass
 * (TODO-27 sec3). */
static int vmm_pte_rmw(uintptr_t virt, uint64_t or_bits, uint64_t and_clear)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    virt &= ~((uintptr_t)0xFFF);
    if (vmm_split_huge_page(virt) != 0)         /* ensure a 4 KiB leaf PTE */
        return -1;
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return -1;
    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return -1;
    if (pd[pdi] & VMM_FLAG_HUGE)                 /* split should have cleared this */
        return -1;
    pt = get_or_create_table(pd, pdi, 0, 0);
    if (!pt) return -1;
    if (!(pt[pti] & VMM_FLAG_PRESENT))
        return -1;
    if (!cpu_has(CPU_FEATURE_NX))               /* never set NX without CPU support */
        or_bits &= ~VMM_FLAG_NX;
    pt[pti] = (pt[pti] & ~and_clear) | or_bits;
    vmm_flush_tlb(virt);
    return 0;
}

/* Range helper for vmm_set_nx / vmm_set_ro: walk every 4 KiB page in
 * [virt, virt+size), applying the RMW. Overflow-checked. */
static int vmm_rmw_range(uintptr_t virt, uint64_t size,
                         uint64_t or_bits, uint64_t and_clear)
{
    uintptr_t addr, end;
    if (size == 0) return 0;
    /* Overflow-check + derive [addr, end) from the ORIGINAL virt (not the
     * floored addr) so an unaligned start still covers its tail page -- e.g.
     * virt=0x1001,size=0x1000 must protect pages 0x1000 AND 0x2000. */
    if (size > 0x7FFFFFFFFFFFu || virt + size < virt)
        return -1;
    addr = virt & ~((uintptr_t)0xFFF);
    end = (virt + size + 0xFFFu) & ~((uintptr_t)0xFFF);
    if (end < addr) return -1;
    while (addr < end) {
        if (vmm_pte_rmw(addr, or_bits, and_clear) != 0)
            return -1;
        addr += VMM_PAGE_SIZE;
    }
    return 0;
}

/* Mark a virtual range No-eXecute (sets PTE.NX), preserving R/W. */
int vmm_set_nx(uintptr_t virt, uint64_t size)
{
    return vmm_rmw_range(virt, size, VMM_FLAG_NX, 0);
}

/* Mark a virtual range read-only (clears PTE.WRITABLE), preserving NX/exec. */
int vmm_set_ro(uintptr_t virt, uint64_t size)
{
    return vmm_rmw_range(virt, size, 0, VMM_FLAG_WRITABLE);
}

/* Return the PTE flag bits for the 4 KiB page containing virt (0 if not present
 * or inside an unsplit huge page). Read-only; used by W^X tests (TODO-27 sec3)
 * to confirm vmm_set_nx/vmm_set_ro flipped the right bits. */
uint64_t vmm_query_flags(uintptr_t virt)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    virt &= ~((uintptr_t)0xFFF);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return 0;
    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return 0;
    if (pd[pdi] & VMM_FLAG_HUGE) return 0;
    pt = get_or_create_table(pd, pdi, 0, 0);
    if (!pt) return 0;
    if (!(pt[pti] & VMM_FLAG_PRESENT)) return 0;
    return pt[pti] & ~PTE_ADDR_MASK;        /* flag bits only */
}

/* Walk the kernel tables for `virt`. Returns 1 and stores the translation in
 * *phys_out when a mapping exists; returns 0 when the VA is not present.
 *
 * Presence and address are reported SEPARATELY because one return value cannot
 * carry both: vmm_get_physical() answers 0 for "not mapped" AND for "mapped to
 * physical frame 0". The guard-page teardown gate turns that answer into a
 * decision to hand a frame back to the PMM, so it must not have to guess -- a
 * present alias to frame 0 would otherwise read as absent and get silently
 * overwritten with an identity mapping. */
static int vmm_translate(uintptr_t virt, uintptr_t *phys_out, uint64_t *flags_out)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uint64_t offset;

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);
    offset = virt & 0xFFF;

    /* Walk PML4 -> PDPT */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return 0;

    /* Check for 1 GiB huge page at PDPT level */
    if ((pdpt[pdpti] & VMM_FLAG_PRESENT) && (pdpt[pdpti] & VMM_FLAG_HUGE)) {
        uintptr_t gib_base = pdpt[pdpti] & ~GIB_ALIGN_MASK & PTE_ADDR_MASK;
        *phys_out = gib_base + (virt & GIB_ALIGN_MASK);
        if (flags_out) *flags_out = pdpt[pdpti] & ~PTE_ADDR_MASK;
        return 1;
    }

    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return 0;

    /* Check for 2 MiB huge page at PD level */
    if (pd[pdi] & VMM_FLAG_HUGE) {
        uintptr_t huge_base;
        if (!(pd[pdi] & VMM_FLAG_PRESENT))
            return 0;
        huge_base = pd[pdi] & PTE_ADDR_MASK;
        *phys_out = huge_base + (virt & 0x1FFFFF);  /* offset within 2 MiB page */
        if (flags_out) *flags_out = pd[pdi] & ~PTE_ADDR_MASK;
        return 1;
    }

    pt = get_or_create_table(pd, pdi, 0, 0);
    if (!pt) return 0;

    if (!(pt[pti] & VMM_FLAG_PRESENT))
        return 0;

    *phys_out = (pt[pti] & PTE_ADDR_MASK) + offset;
    if (flags_out) *flags_out = pt[pti] & ~PTE_ADDR_MASK;
    return 1;
}

uintptr_t vmm_get_physical(uintptr_t virt)
{
    uintptr_t phys = 0;
    return vmm_translate(virt, &phys, (uint64_t *)0) ? phys : 0;
}

/* Does this translation snapshot describe `page` mapped to ITSELF and
 * kernel-writable? That is the exact condition a guarded frame must satisfy
 * before it can go back to the PMM: identity mapping restored, writable so the
 * next allocator's zero-fill does not fault, and User clear so a freed kernel
 * frame is not left reachable from ring 3.
 *
 * It takes a SNAPSHOT rather than walking itself so a decision and the value it
 * was made from cannot drift apart -- guard_page_lock does not serialize
 * unrelated mappers, so every extra walk is another chance to decide on
 * different evidence than it reports. */
static int guard_map_is_kernel_identity(uintptr_t page, int present,
                                        uintptr_t phys, uint64_t flags)
{
    if (!present || phys != page)
        return 0;
    if (!(flags & VMM_FLAG_PRESENT) || !(flags & VMM_FLAG_WRITABLE))
        return 0;
    return (flags & VMM_FLAG_USER) ? 0 : 1;
}

/* Would this frame be safe to hand back to the PMM as it stands?
 *
 * Weaker than guard_map_is_kernel_identity by exactly one bit: it does NOT
 * require User to be clear. That is not laxity, it is this kernel's boot
 * identity map -- SMEP is disabled globally and the boot PML4 sets User on
 * every 2 MiB page (CLAUDE.md "Bare Metal Gotchas"), so EVERY frame the PMM
 * hands out lives under a User-flagged mapping until something splits it.
 * Requiring User clear here would refuse every legitimate guard install and
 * halt the boot in ist_alloc(); that is measured, not theoretical (2026-07-28).
 * Writable IS required: the next owner's zero-fill writes through the identity
 * address. The stricter predicate applies where it can be honoured -- after
 * uninstall rewrites the leaf itself as a kernel-only 4 KiB PTE. */
static int guard_map_is_reusable_frame(uintptr_t page, int present,
                                       uintptr_t phys, uint64_t flags)
{
    if (!present || phys != page)
        return 0;
    return (flags & VMM_FLAG_PRESENT) && (flags & VMM_FLAG_WRITABLE);
}

/* Walk-and-evaluate wrapper, for confirming the state left behind by a rewrite. */
static int guard_va_is_kernel_identity(uintptr_t page)
{
    uintptr_t phys = 0;
    uint64_t flags = 0;
    int present = vmm_translate(page, &phys, &flags);

    return guard_map_is_kernel_identity(page, present, phys, flags);
}

/* --- Guard page tracking -------------------------------------------------- */

/* Track guard page ranges so the page fault handler can identify them, and so
 * vmm_uninstall_guard_page() knows which VAs it must restore before their
 * frames go back to the PMM. Guard pages are NOT boot-only: every task_create
 * and every task_exec installs one, so the table is live-mutated at runtime.
 * Capacity (VMM_MAX_GUARD_PAGES) and its TASK_MAX relation live in vmm.h.
 *
 * Registration is FAILABLE and transactional: the slot is reserved BEFORE the
 * PTE is cleared. The reverse order is the defect this replaces -- a saturated
 * table silently dropped the entry AFTER vmm_install_guard_page() had already
 * unmapped the page and returned success, uninstall then found no entry and
 * skipped the remap, and the caller handed a still-unmapped frame back to the
 * PMM; the next allocator writing through its identity address faulted in
 * kernel mode. heap.c's heap-end guard already documents the fail-before-
 * clearing contract this restores.
 *
 * SMP: guard_page_lock spans the WHOLE install/uninstall transaction -- the
 * registry slot AND the page-table work -- not merely the table. Two CPUs
 * installing guards on different frames inside the same unsplit 2 MiB region
 * would otherwise each observe the huge PDE, build their own page table, and
 * the later PDE publication would drop the earlier caller's cleared PTE: both
 * calls return success, both entries stay registered, and one stack is silently
 * unguarded with a page-table frame leaked. Doing the split and the clear under
 * one lock removes that lost update BETWEEN GUARD INSTALLS. It does not make
 * guard installs atomic against unrelated mappers -- the VMM has no global
 * page-table lock (see the note in vmm_protect_range) -- which is tracked with
 * the TLB-shootdown gap in 03-memory-concurrency/TODO-07-smp-phase2.
 *
 * vmm_guard_page_label() runs inside the #PF
 * handler -- a panic-context path that must never BLOCK on a lock some faulting
 * context might already hold (kernel-code-quality Gate 4) -- so it acquires the
 * same lock with spin_trylock and simply reports no label if the acquire fails.
 * Non-blocking gives both properties at once: no deadlock, and no torn read.
 * An unlocked scan would have neither -- removal overwrites a slot's addr and
 * label as two independent stores, so an unlocked reader can pair one entry's
 * address with another's label and mislabel the crash it is there to explain.
 * Exception vectors are 0x8E interrupt gates (idt.c), so IRQs are already off
 * in the handler and the scan cannot be preempted into a same-CPU mutator. */
struct guard_page_entry {
    uintptr_t addr;
    const char *label;
};

static struct guard_page_entry guard_pages[VMM_MAX_GUARD_PAGES];
static uint32_t guard_page_count;
static spinlock_t guard_page_lock = SPINLOCK_INIT;

/* Reserve a slot for `page`. Returns 0 on success, -1 when the table is full.
 * Caller holds guard_page_lock. */
static int guard_page_register_locked(uintptr_t page, const char *label)
{
    if (guard_page_count >= VMM_MAX_GUARD_PAGES)
        return -1;
    guard_pages[guard_page_count].addr = page;
    guard_pages[guard_page_count].label = label;
    guard_page_count++;
    return 0;
}

/* Is `page` a registered guard? Peek only -- uninstall must know this BEFORE it
 * decides anything, but must not surrender the registration until the identity
 * mapping is actually restored. Caller holds guard_page_lock. */
static int guard_page_is_registered_locked(uintptr_t page)
{
    uint32_t i;

    for (i = 0; i < guard_page_count; i++) {
        if (guard_pages[i].addr == page)
            return 1;
    }
    return 0;
}

/* Release the slot for `page`. Returns 1 if an entry was removed, 0 if `page`
 * was not registered. Caller holds guard_page_lock. */
static int guard_page_unregister_locked(uintptr_t page)
{
    uint32_t i;

    for (i = 0; i < guard_page_count; i++) {
        if (guard_pages[i].addr != page)
            continue;
        /* Swap-remove: move the tail into the hole, then retire the tail. */
        if (i != guard_page_count - 1)
            guard_pages[i] = guard_pages[guard_page_count - 1];
        guard_page_count--;
        return 1;
    }
    return 0;
}

uint32_t vmm_guard_pages_free(void)
{
    uint64_t irq_flags;
    uint32_t n;

    spin_lock_irqsave(&guard_page_lock, &irq_flags);
    n = guard_page_count;
    spin_unlock_irqrestore(&guard_page_lock, irq_flags);
    return (n < VMM_MAX_GUARD_PAGES) ? (VMM_MAX_GUARD_PAGES - n) : 0;
}

/* Public form (vmm.h): name the guard a faulting address landed in, or NULL.
 * Non-blocking, so it is callable from any fault context -- the #PF handler
 * below, and the #DF abort path in idt.c, which is the ONLY report a downward
 * kernel-stack overflow ever produces (crossing the guard faults while RSP can
 * no longer take an exception frame, so #PF delivery escalates to #DF). */
const char *vmm_guard_page_label(uintptr_t fault_addr)
{
    uintptr_t page = fault_addr & ~((uintptr_t)0xFFF);
    const char *label = (const char *)0;
    uint32_t i, n;

    /* Non-blocking by construction -- see the block comment above (Gate 4).
     * A failed acquire means a mutator holds the table right now; the fault is
     * then reported with the generic PAGE_FAULT text rather than a label that
     * might belong to a different guard. */
    if (!spin_trylock(&guard_page_lock))
        return (const char *)0;

    n = guard_page_count;
    for (i = 0; i < n; i++) {
        if (guard_pages[i].addr == page) {
            label = guard_pages[i].label;
            break;
        }
    }
    spin_tryunlock(&guard_page_lock);
    return label;
}

/* --- Page Fault Handler (ISR 14) ---
 *
 * ARCH: x86-64 -- CR2 and the #PF error code are AMD64-specific. Will move to
 * arch/ with the HAL split.
 *
 * #PF error-code bits (Intel SDM Vol 3, "Page-Fault Error Code"). Named so the
 * triage never tests a magic bit. */
#define PF_EC_PRESENT   0x01u   /* P:  0 = not-present page, 1 = protection violation */
#define PF_EC_WRITE     0x02u   /* W/R: 1 = write access, 0 = read */
#define PF_EC_USER      0x04u   /* U/S: 1 = fault taken in ring 3 */
#define PF_EC_RESERVED  0x08u   /* RSVD: reserved bit set in a paging structure */
#define PF_EC_FETCH     0x10u   /* I/D: 1 = instruction fetch (requires NXE) */
#define PF_EC_PROTKEY   0x20u   /* PK:  protection-key violation */
#define PF_EC_SHADOW    0x40u   /* SS:  shadow-stack access (CET) */

/* Per-CPU exception scratch. EXCEPTION_RECORD (152 B) + CONTEXT (1232 B) are
 * far too large to build as locals on the arbitrary-depth kernel stack a ring-0
 * #PF runs on: reserving them in the prologue could touch the guard page or
 * corrupt an already-deep stack before panic_screen even runs, destroying the
 * diagnostic path precisely during stack exhaustion. Each CPU owns one slot
 * (only the faulting CPU touches it -- no lock), 16-byte aligned to satisfy the
 * CONTEXT alignment ABI. `in_use` is a one-shot recursion guard: a nested #PF
 * that arrives while a slot is being populated cannot reuse it, so it escalates
 * straight to a terminal panic instead of scribbling over a live record.
 *
 * Aligned to a 64-byte cache line (not just the 16-byte CONTEXT ABI minimum) and
 * padded so the per-element stride is a cache-line multiple: otherwise one CPU's
 * trailing bytes and the next CPU's leading bytes would share a line and bounce
 * between cores on simultaneous faults, defeating the per-CPU ownership. */
#define PF_CACHELINE 64u
struct pf_exc_scratch {
    EXCEPTION_RECORD rec;
    CONTEXT          ctx;
    volatile uint32_t in_use;
} __attribute__((aligned(PF_CACHELINE)));

_Static_assert(sizeof(struct pf_exc_scratch) % PF_CACHELINE == 0,
               "pf_exc_scratch stride must be a cache-line multiple (no false sharing)");

static struct pf_exc_scratch pf_exc_scratch[MAX_CPUS] __attribute__((aligned(PF_CACHELINE)));

/* Build a STATUS_ACCESS_VIOLATION EXCEPTION_RECORD + a CONTROL/INTEGER CONTEXT
 * describing this fault. Pure (no logging, no locks, no allocation) so it is
 * safe in fault context and directly unit-testable. context_from_frame does the
 * full CONTEXT scrub itself (zeroes the struct, parks FltSave at architectural
 * init state) after snapshotting the requested mask -- so we only seed
 * ContextFlags with the groups a frame can satisfy and let it clear the rest;
 * no separate pre-zero of the per-CPU scratch is needed. */
void pf_build_access_violation(EXCEPTION_RECORD *rec, CONTEXT *ctx,
                               const struct interrupt_frame *frame,
                               uintptr_t fault_addr, uint64_t err_code)
{
    ctx->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context_from_frame(frame, ctx);

    rec->ExceptionCode     = STATUS_ACCESS_VIOLATION;
    rec->ExceptionFlags    = EXCEPTION_CONTINUABLE;   /* first-chance may resolve */
    rec->ExceptionRecord   = (EXCEPTION_RECORD *)0;
    rec->ExceptionAddress  = (void *)(uintptr_t)frame->rip;
    rec->NumberParameters  = 2;
    /* winnt.h contract: [0] = access type, [1] = faulting linear address. */
    rec->ExceptionInformation[EXCEPTION_INFO_ACCESS_TYPE] =
        (err_code & PF_EC_FETCH) ? EXCEPTION_ACCESS_EXECUTE :
        (err_code & PF_EC_WRITE) ? EXCEPTION_ACCESS_WRITE   :
                                   EXCEPTION_ACCESS_READ;
    rec->ExceptionInformation[EXCEPTION_INFO_FAULT_ADDR] = (uint64_t)fault_addr;
}

static uint64_t page_fault_handler(struct interrupt_frame *frame)
{
    uintptr_t fault_addr = read_cr2();
    uint64_t  err_code   = frame->err_code;
    struct pf_exc_scratch *s;
    uint32_t  cpu;

    /* Fault-recoverable KERNEL read: __kstack_read_u64 (cpu_security.c) is the
     * fault-safe primitive for kernel stack walking (RtlCaptureStackBackTrace /
     * crash frame-chain walks). A #PF at its guarded load means the walker chased
     * a corrupt/off-stack/guard-page RBP into an unmapped or read-protected KERNEL
     * VA; redirect to the fixup so the walk stops gracefully instead of
     * bugchecking. Matched by the EXACT faulting RIP (static exception table, no
     * per-CPU state -> SMP/preempt-safe) and read direction only. Runs BEFORE the
     * swap/mmap pager: legitimate kernel stack pages are always resident, so a
     * kstack-read fault is never a demand-page/COW event -- letting it reach
     * swap_handle_fault (which can allocate, log, and vfs_read) would enter
     * blocking I/O from a possibly-DISPATCH_LEVEL/crash context. A real user
     * demand/COW fault never executes at this RIP, so the pager still handles
     * those below. */
    {
        extern char __kstack_read_fault[], __kstack_read_fixup[];
        if (!(err_code & PF_EC_WRITE) &&
            frame->rip == (uint64_t)(uintptr_t)__kstack_read_fault) {
            frame->rip = (uint64_t)(uintptr_t)__kstack_read_fixup;
            return (uint64_t)frame;  /* guarded kernel read reports fault: RAX path */
        }
    }

    /* Fault-recoverable KERNEL byte read: __kread_u8 (cpu_security.c) is the
     * panic path's guarded load for walking a caller-supplied C string whose
     * pointer may itself be the corruption being reported -- serial.c's
     * emergency writer and panic.c's evidence collector. Same reasoning as the
     * kstack read above: matched by EXACT faulting RIP plus read direction, no
     * per-CPU state, and redirected BEFORE the pager so a panic-context fault
     * can never reach swap_handle_fault and its blocking I/O. Kept as a separate
     * block rather than folded into the one above because each guarded load
     * needs its own label pair. */
    {
        uint64_t kread_fixup;
        if (kread_u8_fixup_lookup(frame->rip, (err_code & PF_EC_WRITE) != 0,
                                  &kread_fixup)) {
            frame->rip = kread_fixup;
            return (uint64_t)frame;  /* guarded kernel byte read reports fault */
        }
    }

    /* Fault-recoverable KERNEL string walk: __kstr_read_guarded (cpu_security.c)
     * is the bounded form of the load above -- one protected loop instead of a
     * call per byte, for the ~2,900 bytes the panic collector copies before its
     * record is durable. Identical routing rules (exact faulting RIP, READ
     * direction only, ahead of the pager) and its own label pair, because the
     * handler matches one instruction. The loop's STORE is not covered: `dst` is
     * kernel-owned by contract, so a write fault there is a real kernel bug and
     * must stay terminal. */
    {
        uint64_t kstr_fixup;
        if (kstr_read_fixup_lookup(frame->rip, (err_code & PF_EC_WRITE) != 0,
                                   &kstr_fixup)) {
            frame->rip = kstr_fixup;
            return (uint64_t)frame;  /* guarded kernel string walk reports fault */
        }
    }

    /* Pager chain runs FIRST: swap-in for evicted pages, then mmap demand-load
     * and MAP_PRIVATE copy-on-write (a present-bit write fault -- must NOT be
     * gated out by a not-present check). A guarded user copy to a swapped /
     * demand / COW page is paged in here and the copy retried, not failed. Only
     * faults the pager declines fall through. (Guard pages have no swap/mmap
     * backing, so a genuine guard hit declines both and reaches the terminal
     * guard check below unchanged.) */
    if (swap_handle_fault(fault_addr, err_code)) {
        return (uint64_t)frame;  /* page swapped in, retry instruction */
    }
    if (mmap_handle_fault(fault_addr, err_code)) {
        return (uint64_t)frame;  /* page loaded / COW-resolved, retry instruction */
    }

    /* Fault-recoverable user access: a guarded copy/write-touch primitive
     * (__uaccess_* in cpu_security.c) faulting on its USER operand is turned
     * into a graceful failure instead of a kernel bugcheck. Recognized by the
     * EXACT faulting instruction RIP (a static exception table -- no per-CPU
     * state, so inherently SMP/preempt-safe) AND the fault DIRECTION: the kernel
     * heap lives at low identity-mapped VAs, so an operand address alone cannot
     * say which operand faulted. A copy_from recovers only a READ fault (its
     * user SOURCE); a copy_to / write-touch recovers only a WRITE fault (its
     * user DEST). A wrong-direction fault is the KERNEL operand and falls
     * through to the guard/terminal path, so a kernel-buffer overflow into a
     * guard page still bugchecks. Runs BEFORE vmm_guard_page_label so a user
     * pointer that happens to hit a registered (low, identity-mapped) guard page
     * fails the copy gracefully instead of panicking the kernel. */
    if (fault_addr < MM_USER_END) {
        int is_write = (err_code & PF_EC_WRITE) != 0;
        uint64_t rip = frame->rip;
        extern char __uaccess_copy_from_fault[], __uaccess_copy_from_fixup[];
        extern char __uaccess_copy_to_fault[], __uaccess_copy_to_fixup[];
        extern char __uaccess_touch_fault[], __uaccess_touch_fixup[];
        if (!is_write && rip == (uint64_t)(uintptr_t)__uaccess_copy_from_fault) {
            frame->rip = (uint64_t)(uintptr_t)__uaccess_copy_from_fixup;
            return (uint64_t)frame;  /* user source unreadable: RCX = bytes left */
        }
        if (is_write && rip == (uint64_t)(uintptr_t)__uaccess_copy_to_fault) {
            frame->rip = (uint64_t)(uintptr_t)__uaccess_copy_to_fixup;
            return (uint64_t)frame;  /* user dest unwritable: RCX = bytes left */
        }
        if (is_write && rip == (uint64_t)(uintptr_t)__uaccess_touch_fault) {
            frame->rip = (uint64_t)(uintptr_t)__uaccess_touch_fixup;
            return (uint64_t)frame;  /* write-probe reports not-writable */
        }
    }

    /* Guard pages -- intentional not-present pages (stack/heap/buffer overrun).
     * Terminal with a specific label. Runs after the pager (guards have no
     * swap/mmap backing) and after the uaccess fixup (a user-operand fault at a
     * guard address is recovered above; a wrong-direction or kernel-operand
     * guard fault reaches here and stays terminal). User-stack auto-grow hooks
     * in ahead of this in a later section; today every remaining hit is fatal. */
    {
        const char *label = vmm_guard_page_label(fault_addr);
        if (label) {
            panic_screen(frame, err_code, label, "vmm.c", 0);
            return (uint64_t)frame;  /* unreachable */
        }
    }

    /* Triage the unresolved fault. Build the exception record in per-CPU scratch
     * (never on this fault's kernel stack) under a one-shot recursion guard. */
    cpu = smp_this_cpu()->cpu_id;
    if (cpu >= MAX_CPUS || pf_exc_scratch[cpu].in_use) {
        /* Bad CPU index (cannot happen post-boot) or a nested #PF while a record
         * is mid-build: do not reuse the slot -- escalate straight to terminal. */
        panic_screen(frame, err_code, "PAGE_FAULT (nested)", "vmm.c", 0);
        return (uint64_t)frame;  /* unreachable */
    }
    s = &pf_exc_scratch[cpu];
    s->in_use = 1;
    pf_build_access_violation(&s->rec, &s->ctx, frame, fault_addr, err_code);

    {
        KPROCESSOR_MODE mode = (err_code & PF_EC_USER) ? UserMode : KernelMode;
        KI_EXCEPTION_DISPOSITION disp;

        /* Both modes go through the master dispatcher; the mode arg selects the
         * user (ring-3 delivery) vs kernel (KD notify -> kernel SEH -> bugcheck)
         * flow. The dispatcher internally calls ki_raise_kernel_exception for a
         * kernel fault, so the #PF handler never routes around it. The stub
         * returns UNHANDLED until later sections land. */
        disp = ki_dispatch_exception(&s->rec, &s->ctx, frame, mode, 1 /*first_chance*/);
        if (disp == KI_EXCEPTION_HANDLED) {
            s->in_use = 0;           /* resolved -- release the slot */
            return (uint64_t)frame;  /* frame carries the resume state, IRET */
        }

        /* Unhandled: terminal. Leave in_use SET so a fault DURING panic hits the
         * nested-fault guard instead of rebuilding a record in this live slot.
         * Log ONLY here (never before dispatch, so a future HANDLED path stays
         * log-free) and ONLY for user faults: a user fault entered from ring 3,
         * so no kernel spinlock (klog's s_klog_lock included) is held. A kernel
         * #PF does NOT reach the else branch anymore: ki_dispatch_exception owns
         * the kernel terminal (KeBugCheckExFrame, noreturn) and never returns
         * UNHANDLED for KernelMode -- the else is defense-in-depth (fault-safe
         * panic_screen) if that contract ever changes. Ring-3 delivery +
         * per-process termination land with the ring-3-delivery stage. */
        if (mode == UserMode) {
            /* WER hook: serial-only (no VFS on the #PF path -- adding the JSON
             * report writer here would extend its filed reentrancy risk to the
             * most-common fault; TODO-24 s.BlackBox). Terminal action stays
             * panic_screen until the TODO-23 s5 per-process terminate primitive
             * lands -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md. */
            WerpReportFault((uint32_t)s->rec.ExceptionCode, fault_addr);
            klog(LOG_ERROR, "mm",
                 "pf: user fault at %p code=0x%x rip=%p (access violation)",
                 (void *)fault_addr, (unsigned int)err_code, (void *)(uintptr_t)frame->rip);
            panic_screen(frame, err_code, "USER_ACCESS_VIOLATION", "vmm.c", 0);
        } else {
            panic_screen(frame, err_code, "PAGE_FAULT", "vmm.c", 0);
        }
        return (uint64_t)frame;  /* unreachable */
    }
}

void vmm_register_page_fault_handler(void)
{
    idt_register_handler(14, page_fault_handler);
}

/* --- Per-process page tables -------------------------------------------- */

/* USER_PD_INDEX provided by kernel/mm/user_range.h (single source of truth) */

uintptr_t vmm_create_user_pml4(void)
{
    uintptr_t pml4_phys, pdpt_phys, pd_phys, pt_phys;
    pte_t *pml4, *pdpt, *pd, *pt;
    pte_t *kern_pdpt, *kern_pd;
    uint32_t i;

    /* Allocate 4 pages: PML4, PDPT, PD, PT for the user 2 MiB region */
    pml4_phys = pmm_alloc_frame();
    pdpt_phys = pmm_alloc_frame();
    pd_phys   = pmm_alloc_frame();
    pt_phys   = pmm_alloc_frame();
    if (!pml4_phys || !pdpt_phys || !pd_phys || !pt_phys) {
        /* Free any frames that DID allocate. The all-or-nothing return
         * below would otherwise strand 1-3 page-table frames in the PMM
         * bitmap with no owner on every failed process creation / fork
         * under memory pressure (or fault injection). */
        if (pml4_phys) pmm_free_frame(pml4_phys);
        if (pdpt_phys) pmm_free_frame(pdpt_phys);
        if (pd_phys)   pmm_free_frame(pd_phys);
        if (pt_phys)   pmm_free_frame(pt_phys);
        return 0;
    }

    pml4 = pt_walk(pml4_phys);
    pdpt = pt_walk(pdpt_phys);
    pd   = pt_walk(pd_phys);
    pt   = pt_walk(pt_phys);

    /* Zero all new tables */
    zero_page(pml4_phys);
    zero_page(pdpt_phys);
    zero_page(pd_phys);
    zero_page(pt_phys);

    /* Get kernel's PDPT and PD through the HHDM. No VMM_FLAG_PRESENT guard
     * before pt_walk here (unlike the user-facing lookup paths): kernel PML4[0]
     * and its PDPT[0] are unconditionally present in the boot map, so the
     * masked frame is never zero and pt_walk cannot bugcheck. If that ever
     * changes the whole kernel is already unbootable, not just this clone. */
    kern_pdpt = pt_walk(kernel_pml4[0] & PTE_ADDR_MASK);
    kern_pd   = pt_walk(kern_pdpt[0] & PTE_ADDR_MASK);

    /* Clone kernel PD entries into the new PD (all 512 entries).
     * These are 2 MiB huge pages -- kernel-only, no User bit. */
    for (i = 0; i < PT_ENTRIES; i++)
        pd[i] = kern_pd[i] & ~((pte_t)VMM_FLAG_USER);

    /* Split PD[USER_PD_INDEX] (0x800000-0x9FFFFF) from a 2 MiB huge page
     * into 512 x 4 KiB pages. This lets us set User bit on individual pages. */
    {
        uintptr_t base_phys = (uintptr_t)USER_PD_INDEX << 21;
        uint32_t guard_idx = (USER_ELF_END - base_phys) / VMM_PAGE_SIZE;
        for (i = 0; i < PT_ENTRIES; i++) {
            uintptr_t page_phys = base_phys + (uintptr_t)i * VMM_PAGE_SIZE;
            /* Default: kernel-only (Present + Writable, no User) */
            pt[i] = page_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
        }
        /* Guard page at USER_ELF_END (0x900000): clear Present bit entirely.
         * This catches both user-mode AND kernel-mode overflow past the user
         * range -- any access triggers #PF regardless of CPL. */
        pt[guard_idx] = 0;
    }

    /* Replace the huge page PD entry with the fine-grained PT.
     * Tag PT_OWNED so vmm_destroy_user_pml4 knows this PT frame was
     * allocated on behalf of this user pml4 and is safe to free. */
    pd[USER_PD_INDEX] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                                | VMM_FLAG_USER | VMM_FLAG_PT_OWNED;

    /* Copy remaining kernel PDPT entries (PDPT[1..3] for 1-4 GiB) */
    for (i = 0; i < PT_ENTRIES; i++)
        pdpt[i] = kern_pdpt[i];
    /* Override PDPT[0] to point to our cloned PD.
     * User bit required at EVERY level for ring 3 access. */
    pdpt[0] = pd_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    /* PML4[0] points to our cloned PDPT */
    pml4[0] = pdpt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    /* Copy any other kernel PML4 entries (currently only [0] is used) */
    for (i = 1; i < PT_ENTRIES; i++)
        pml4[i] = kernel_pml4[i];

    return pml4_phys;
}

/* SMP note: per-process PML4s are task-local -- only the owning task's
 * create/exec path touches them, and those run on a single CPU with
 * interrupts disabled or the scheduler in a known state.  No lock needed
 * currently.  If future multi-threaded process creation calls this from
 * multiple CPUs, add a per-process PML4 spinlock. */
void vmm_set_user_page(uintptr_t pml4_phys, uintptr_t virt)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    pml4 = pt_walk(pml4_phys);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Absent upper-level entry: nothing to set, return without touching it.
     * Check PRESENT *before* pt_walk -- for an absent entry the masked frame is
     * zero and pt_walk(0) would bugcheck (the pre-conversion cast produced NULL
     * and the old `if (!pdpt) return` handled it; that idiom is now dead). */
    if (!(pml4[pml4i] & VMM_FLAG_PRESENT)) return;
    pdpt = pt_walk(pml4[pml4i] & PTE_ADDR_MASK);
    if (!(pdpt[pdpti] & VMM_FLAG_PRESENT)) return;
    pd = pt_walk(pdpt[pdpti] & PTE_ADDR_MASK);

    /* Ensure User bit is set at all upper levels (PML4, PDPT, PD).
     * x86-64 requires User bit at EVERY level for ring-3 access. */
    pml4[pml4i] |= VMM_FLAG_USER;
    pdpt[pdpti] |= VMM_FLAG_USER;

    /* If PD entry is a 2 MiB huge page, split it into 4 KiB PTEs so we
     * can set the User bit on individual pages.  This makes vmm_set_user_page
     * work for ANY address in the per-process PML4, not just the pre-split
     * user ELF range.  Required for future Win32 PE loading, VirtualAlloc,
     * and per-process heap at arbitrary addresses. */
    if (pd[pdi] & VMM_FLAG_HUGE) {
        uintptr_t huge_phys = pd[pdi] & PTE_ADDR_MASK;
        uint64_t  old_flags = pd[pdi] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);
        uintptr_t pt_frame  = pmm_alloc_frame();
        uint32_t  j;
        if (!pt_frame) {
            klog(LOG_ERROR, "mm", "vmm_set_user_page: cannot split PD[%u] (OOM)",
                 (uint64_t)pdi);
            return;
        }
        pt = pt_walk(pt_frame);
        for (j = 0; j < PT_ENTRIES; j++)
            pt[j] = (huge_phys + (uintptr_t)j * VMM_PAGE_SIZE) | old_flags;
        /* Tag PT_OWNED so vmm_destroy_user_pml4 can safely free this
         * PT frame on process exit. */
        pd[pdi] = pt_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                          | VMM_FLAG_USER | VMM_FLAG_PT_OWNED;
    }

    if (!(pd[pdi] & VMM_FLAG_PRESENT)) return;
    pt = pt_walk(pd[pdi] & PTE_ADDR_MASK);

    /* Set User bit on the specific 4 KiB page */
    pt[pti] |= VMM_FLAG_USER;
}

/* --- Per-process user page mapping ----------------------------------------
 *
 * vmm_map_user_page:  map a PMM-allocated physical frame at an arbitrary
 *                     user VA in a per-process PML4.
 * vmm_unmap_user_page: reverse -- clear PTE, free frame.
 *
 * Unlike vmm_set_user_page (which only ORs the User bit on an existing
 * identity-mapped page), these functions install a DIFFERENT physical frame
 * at a chosen VA.  Required by uthread_create (per-thread user stacks)
 * and future VirtualAlloc(MEM_COMMIT).
 *
 * XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md
 * XREF: 02-kernel-core/TODO-11-peb-teb-user-abi.md (consumer)
 */

/* Core map-at-user-VA routine shared by vmm_map_user_page (zero-fill)
 * and vmm_share_user_page (preserve existing frame content). Handles
 * PML4/PDPT/PD/PT walk + split + User-bit propagation + PT_OWNED
 * tagging. Caller chooses whether the backing frame is zeroed.
 *
 * `zero_frame`: 1 = pre-zero the backing frame before mapping (prevents
 *              kernel data leak to user mode -- the default for fresh
 *              PMM allocations). 0 = preserve frame content (shared
 *              memory sections, existing named mappings). */
static int map_user_page_impl(uintptr_t cr3, uintptr_t virt, uintptr_t phys,
                              int zero_frame)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uint64_t user_rw = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    virt &= ~((uintptr_t)0xFFF);
    phys &= ~((uintptr_t)0xFFF);

    pml4  = pt_walk(cr3);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Zero-fill the frame BEFORE mapping -- prevents kernel data
     * leaking to user mode.  zero_page() writes through the HHDM alias
     * (pt_walk), so the zero-fill survives the section-5 identity teardown.
     * Shared-memory callers skip this (zero_frame=0) so pre-written data
     * survives. */
    if (zero_frame)
        zero_page(phys);

    /* --- Walk / create PML4 -> PDPT --- */
    if (pml4[pml4i] & VMM_FLAG_PRESENT) {
        pdpt = pt_walk(pml4[pml4i] & PTE_ADDR_MASK);
        pml4[pml4i] |= VMM_FLAG_USER;  /* ensure User at PML4 level */
    } else {
        uintptr_t f = pmm_alloc_frame();
        if (!f) return -1;
        zero_page(f);
        pml4[pml4i] = f | user_rw;
        pdpt = pt_walk(f);
    }

    /* --- Walk / create PDPT -> PD --- */
    if (pdpt[pdpti] & VMM_FLAG_PRESENT) {
        pd = pt_walk(pdpt[pdpti] & PTE_ADDR_MASK);
        pdpt[pdpti] |= VMM_FLAG_USER;
    } else {
        uintptr_t f = pmm_alloc_frame();
        if (!f) return -1;
        zero_page(f);
        pdpt[pdpti] = f | user_rw;
        pd = pt_walk(f);
    }

    /* --- Walk / create PD -> PT (handle huge page split) ---
     *
     * Any PT frame we allocate here (either via huge-page split or via
     * a fresh frame when PD entry is empty) is tagged PT_OWNED so
     * vmm_destroy_user_pml4 will reclaim it. Entries cloned from
     * kernel_pml4 (non-huge, PT_OWNED=0) are preserved. */
    if (pd[pdi] & VMM_FLAG_PRESENT) {
        if (pd[pdi] & VMM_FLAG_HUGE) {
            /* Split 2 MiB huge page into 512 x 4 KiB PTEs */
            uintptr_t huge_phys = pd[pdi] & PTE_ADDR_MASK;
            uint64_t  old_flags = pd[pdi] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);
            uintptr_t pt_frame  = pmm_alloc_frame();
            uint32_t  j;
            if (!pt_frame) return -1;
            pt = pt_walk(pt_frame);
            for (j = 0; j < PT_ENTRIES; j++)
                pt[j] = (huge_phys + (uintptr_t)j * VMM_PAGE_SIZE) | old_flags;
            pd[pdi] = pt_frame | user_rw | VMM_FLAG_PT_OWNED;
        } else {
            pt = pt_walk(pd[pdi] & PTE_ADDR_MASK);
            pd[pdi] |= VMM_FLAG_USER;
        }
    } else {
        uintptr_t f = pmm_alloc_frame();
        if (!f) return -1;
        zero_page(f);
        pd[pdi] = f | user_rw | VMM_FLAG_PT_OWNED;
        pt = pt_walk(f);
    }

    /* --- Install final PTE ---
     *
     * Reject ONLY if a user-visible mapping already exists at this VA --
     * overlapping user mappings would hide each other and mask caller
     * bugs, so the failure must surface. A present PTE WITHOUT the User
     * bit (typical: a kernel identity-map slice left behind by a fresh
     * huge-page split above) is not a real caller-observable mapping;
     * overwriting it with the new user PTE is correct and expected.
     * The previous check rejected EVERY present PTE, which made
     * map_user_page_impl fail on the first call for any VA in a
     * freshly-split 2 MiB region because the split initialised all
     * 512 PTEs with the old huge-page identity mapping (Present +
     * Writable, User=0). */
    if ((pt[pti] & VMM_FLAG_PRESENT) && (pt[pti] & VMM_FLAG_USER)) {
        klog(LOG_ERROR, "mm",
             "vmm_map_user_page: VA 0x%lx already mapped to user (PTE=0x%lx)",
             (uint64_t)virt, (uint64_t)pt[pti]);
        return -1;  /* caller must unmap first */
    }
    {
        uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;
        if (cpu_has(CPU_FEATURE_NX))
            flags |= VMM_FLAG_NX;
        pt[pti] = phys | flags;
    }

    vmm_flush_tlb(virt);
    return 0;
}

int vmm_map_user_page(uintptr_t cr3, uintptr_t virt, uintptr_t phys)
{
    return map_user_page_impl(cr3, virt, phys, /*zero_frame=*/1);
}

int vmm_share_user_page(uintptr_t cr3, uintptr_t virt, uintptr_t phys)
{
    return map_user_page_impl(cr3, virt, phys, /*zero_frame=*/0);
}

void vmm_remap_user_page(uintptr_t pml4_phys, uintptr_t virt,
                         uintptr_t new_phys)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    virt     &= ~((uintptr_t)0xFFF);
    new_phys &= ~((uintptr_t)0xFFF);

    pml4  = pt_walk(pml4_phys);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    if (!(pml4[pml4i] & VMM_FLAG_PRESENT)) return;
    pdpt = pt_walk(pml4[pml4i] & PTE_ADDR_MASK);

    if (!(pdpt[pdpti] & VMM_FLAG_PRESENT)) return;
    pd = pt_walk(pdpt[pdpti] & PTE_ADDR_MASK);

    /* Absent PD entry: nothing mapped here, bail (check PRESENT before
     * pt_walk -- a zero frame would bugcheck). A huge page would mean the PT
     * hasn't been split yet; the caller (task_exec) is expected to have
     * triggered a split (vmm_create_user_pml4 does this for USER_PD_INDEX),
     * so silently bail rather than split on the fly. */
    if (!(pd[pdi] & VMM_FLAG_PRESENT)) return;
    if (pd[pdi] & VMM_FLAG_HUGE) return;

    pt = pt_walk(pd[pdi] & PTE_ADDR_MASK);

    /* Tag PAGE_OWNED so vmm_destroy_user_pml4 will free the new frame on
     * process exit. Do NOT free the previous PTE's frame here: the caller
     * knows whether the old frame was identity-mapped (kernel-owned, must
     * not free) or private (its own concern). */
    pt[pti] = new_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                       | VMM_FLAG_USER | VMM_FLAG_PAGE_OWNED;
}

void vmm_unshare_user_page(uintptr_t cr3, uintptr_t virt)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;

    virt &= ~((uintptr_t)0xFFF);

    pml4  = pt_walk(cr3);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    if (!(pml4[pml4i] & VMM_FLAG_PRESENT)) return;
    pdpt = pt_walk(pml4[pml4i] & PTE_ADDR_MASK);
    if (!(pdpt[pdpti] & VMM_FLAG_PRESENT)) return;
    pd = pt_walk(pdpt[pdpti] & PTE_ADDR_MASK);
    if (!(pd[pdi] & VMM_FLAG_PRESENT)) return;
    if (pd[pdi] & VMM_FLAG_HUGE) return;
    pt = pt_walk(pd[pdi] & PTE_ADDR_MASK);

    pt[pti] = 0;
    vmm_flush_tlb(virt);
    /* Deliberately do NOT pmm_free_frame -- the frame belongs to a
     * SECTION_OBJECT and must outlive this task's view. */
}

void vmm_unmap_user_page(uintptr_t cr3, uintptr_t virt)
{
    pte_t *pml4, *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi, pti;
    uintptr_t frame;

    virt &= ~((uintptr_t)0xFFF);

    pml4  = pt_walk(cr3);
    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);
    pti   = pt_index(virt);

    /* Walk without creating -- if any level is absent, nothing to unmap */
    if (!(pml4[pml4i] & VMM_FLAG_PRESENT)) return;
    pdpt = pt_walk(pml4[pml4i] & PTE_ADDR_MASK);

    if (!(pdpt[pdpti] & VMM_FLAG_PRESENT)) return;
    pd = pt_walk(pdpt[pdpti] & PTE_ADDR_MASK);

    if (!(pd[pdi] & VMM_FLAG_PRESENT)) return;
    if (pd[pdi] & VMM_FLAG_HUGE) return;  /* don't unmap inside a huge page */
    pt = pt_walk(pd[pdi] & PTE_ADDR_MASK);

    if (!(pt[pti] & VMM_FLAG_PRESENT)) return;

    /* Extract frame, clear PTE, free frame */
    frame = pt[pti] & PTE_ADDR_MASK;
    pt[pti] = 0;
    vmm_flush_tlb(virt);

    if (frame)
        pmm_free_frame(frame);
}

void vmm_destroy_user_pml4(uintptr_t pml4_phys)
{
    pte_t *pml4, *pdpt, *pd;
    uint32_t i;

    if (!pml4_phys) return;

    /* Walk pointers come from pt_walk(); the *_phys values are kept separately
     * because pmm_free_frame() must be handed a PHYSICAL frame, never an HHDM
     * walk pointer (section 9). Guard pt_walk() against a zero frame -- an
     * absent PDPT/PD entry is a legal "nothing to free", not a corrupt PTE. */
    pml4 = pt_walk(pml4_phys);
    uintptr_t pdpt_phys = pml4[0] & PTE_ADDR_MASK;
    if (pdpt_phys) {
        pdpt = pt_walk(pdpt_phys);
        uintptr_t pd_phys = pdpt[0] & PTE_ADDR_MASK;
        if (pd_phys) {
            pd = pt_walk(pd_phys);
            /* Free ONLY PT frames tagged PT_OWNED.
             *
             * vmm_create_user_pml4 clones kernel_pml4's 0-1 GiB PD
             * entries verbatim (line ~511). The kernel may have split
             * some of its own PD entries into 4 KiB PTs for guard
             * pages (kernel/AP/IST stack guards, heap-end guard). Those
             * cloned PD entries still point at KERNEL-allocated PTs
             * that must NEVER be freed from a per-process destroy path
             * or kernel memory accesses through those VAs start
             * walking into freed frames. The old "free everything
             * non-huge" loop corrupted the kernel heap on any
             * fork->exec path because the child's cloned PD inherited
             * kernel guard PTs and destroy freed them.
             *
             * vmm_create_user_pml4, vmm_set_user_page, and
             * vmm_map_user_page all set PT_OWNED on any PT frame they
             * allocate, so this check exactly catches user-owned PTs
             * without disturbing shared kernel mappings. */
            for (i = 0; i < PT_ENTRIES; i++) {
                if ((pd[i] & VMM_FLAG_PRESENT) && !(pd[i] & VMM_FLAG_HUGE) &&
                    (pd[i] & VMM_FLAG_PT_OWNED)) {
                    uintptr_t pt_phys = pd[i] & PTE_ADDR_MASK;
                    /* Before freeing the PT itself, walk its 512 PTEs and
                     * reclaim any private physical frame tagged PAGE_OWNED.
                     * Set by vmm_remap_user_page for fork+exec isolation.
                     * Identity-mapped PTEs (PAGE_OWNED=0) are kernel-owned
                     * and must NOT be freed. */
                    if (pt_phys) {
                        pte_t *pt = pt_walk(pt_phys);
                        uint32_t j;
                        for (j = 0; j < PT_ENTRIES; j++) {
                            if ((pt[j] & VMM_FLAG_PRESENT) &&
                                (pt[j] & VMM_FLAG_PAGE_OWNED)) {
                                uintptr_t pg_phys = pt[j] & PTE_ADDR_MASK;
                                if (pg_phys)
                                    pmm_free_frame(pg_phys);
                            }
                        }
                        pmm_free_frame(pt_phys);
                    }
                }
            }
            pmm_free_frame(pd_phys);
        }
        pmm_free_frame(pdpt_phys);
    }
    pmm_free_frame(pml4_phys);
}

uintptr_t vmm_get_kernel_cr3(void)
{
    /* The PHYSICAL root, loaded raw into CR3 (task.c/smp.c). Derived from the
     * HHDM walk pointer via the sanctioned inverse (section 9) rather than held
     * as a second global: mm_hhdm_to_phys() undoes the direct-map offset that
     * mm_phys_to_hhdm() applied in vmm_init, so this round-trips exactly. */
    return mm_hhdm_to_phys(kernel_pml4);
}

/* --- MMIO mapping (UC -- Uncacheable) ------------------------------------ */

/* Bump allocator for MMIO virtual addresses.
 * The kernel VA windows are picked disjoint by hand (no central allocator yet):
 *   0-4 GiB   identity map (entry.asm)
 *   4-5 GiB   mmap.c        (MMAP_BASE..MMAP_END)
 *   5 GiB+    PE image bases (pe.c, DEFAULT 0x140000000 + SizeOfImage; note the
 *             ImageBase is file-controlled, so a PE could in principle request
 *             this window -- the PE loader rejecting reserved ranges + a central
 *             VA allocator is the real guard, not this hand-picked base)
 *   8 GiB     test "unmapped" probe sentinel (test_vmm.c)
 *   9-10 GiB  MMIO          (this window)
 * Do not overlap these: the allocators are independent and vmm_map_page()
 * overwrites PTEs unconditionally, so an overlap silently corrupts mappings.
 * A central kernel VA allocator that reserves non-overlapping ranges is the
 * real fix (tracked in the VMM memory-protection TODO). */
#define MMIO_VA_BASE  0x240000000ULL  /* 9 GiB (clear of default PE base + test sentinel) */
#define MMIO_VA_LIMIT 0x280000000ULL  /* 10 GiB -- 1 GiB for MMIO */

static uintptr_t s_mmio_next_va = MMIO_VA_BASE;
/* Serializes the bump allocator + PTE installation. MMIO maps happen from
 * the BSP at boot AND from driver init that may run post-SMP / on APs
 * (nvme.c, xhci.c, hot-plug), so concurrent callers must not race on
 * s_mmio_next_va or hand out the same VA range. */
static spinlock_t s_mmio_lock = SPINLOCK_INIT;

/* UC flags: PCD=1 (bit 4) + PWT=1 (bit 3) = Strong Uncacheable */
#define VMM_MMIO_UC  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                      VMM_FLAG_NOCACHE | VMM_FLAG_WRITETHROUGH | VMM_FLAG_NX)

/* WC flags: PWT=1 (bit 3), PCD=0 -> PAT index 1 = Write-Combining.
 * Requires PAT MSR entry 1 to be programmed as WC (boot_phase0). */
#define VMM_MMIO_WC  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | \
                      VMM_FLAG_WRITETHROUGH | VMM_FLAG_NX)

/* Reserve a VA range from the MMIO bump allocator and install PTEs with the
 * given cache flags. Overflow-safe page math (size is uint32, widened before
 * the round-up so it cannot wrap), SMP-safe reservation, and full rollback if
 * any page fails to map. Returns the VA base or NULL on failure. */
static void *mmio_map_range(uint64_t phys_base, uint32_t size, uint64_t flags)
{
    uintptr_t va_start;
    uint64_t pages, i, j, irq_flags;

    if (size == 0 || (phys_base & 0xFFF) != 0)
        return (void *)0;

    pages = ((uint64_t)size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    if (pages == 0)
        return (void *)0;

    /* Reject a physical range that wraps or runs past the 52-bit physical
     * address space a PTE can encode. pages * VMM_PAGE_SIZE is at most ~4 GiB
     * (size is uint32), so it cannot overflow; phys_base near 2^64 still can.
     * The VA side is bounds-checked under the lock below; this guards phys. */
    {
        uint64_t phys_end = phys_base + pages * VMM_PAGE_SIZE;
        if (phys_end <= phys_base || phys_end > (PTE_ADDR_MASK + VMM_PAGE_SIZE))
            return (void *)0;
    }

    /* Reserve a unique VA range under the lock, then release BEFORE installing
     * PTEs. The reserved range is exclusively ours, so the (potentially long,
     * PT-frame-allocating) map loop and any rollback need no lock -- and we
     * never hold IRQs off across O(pages) page-table work (framebuffer maps
     * thousands of pages). vmm_map_page() page-table SMP-safety across adjacent
     * ranges is a separate VMM-wide concern, not introduced here. */
    spin_lock_irqsave(&s_mmio_lock, &irq_flags);
    va_start = s_mmio_next_va;
    if (va_start + pages * VMM_PAGE_SIZE > MMIO_VA_LIMIT) {
        spin_unlock_irqrestore(&s_mmio_lock, irq_flags);
        return (void *)0;  /* out of MMIO VA space */
    }
    s_mmio_next_va = va_start + pages * VMM_PAGE_SIZE;
    spin_unlock_irqrestore(&s_mmio_lock, irq_flags);

    for (i = 0; i < pages; i++) {
        if (vmm_map_page(va_start + i * VMM_PAGE_SIZE,
                          phys_base + i * VMM_PAGE_SIZE, flags) != 0) {
            /* Roll back the pages already mapped this call. The reserved VA
             * range itself is not returned to the allocator (VA-reclaim is the
             * documented minimal-scope limitation). */
            for (j = 0; j < i; j++)
                vmm_unmap_page(va_start + j * VMM_PAGE_SIZE, 0);
            return (void *)0;
        }
    }

    return (void *)va_start;
}

void *vmm_map_mmio_uc(uint64_t phys_base, uint32_t size)
{
    return mmio_map_range(phys_base, size, VMM_MMIO_UC);
}

void *vmm_map_mmio_wc(uint64_t phys_base, uint32_t size)
{
    return mmio_map_range(phys_base, size, VMM_MMIO_WC);
}

void vmm_unmap_mmio(void *virt, uint32_t size)
{
    uintptr_t va = (uintptr_t)virt;
    uint64_t pages, i;

    if (!virt || size == 0)
        return;

    pages = ((uint64_t)size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;
    for (i = 0; i < pages; i++)
        vmm_unmap_page(va + i * VMM_PAGE_SIZE, 0);
}

/* --- Split 2 MiB huge page into 4 KiB pages ----------------------------- */

int vmm_split_huge_page(uintptr_t virt)
{
    pte_t *pdpt, *pd, *pt;
    uint64_t pml4i, pdpti, pdi;
    uintptr_t huge_phys;
    uint64_t old_flags;
    uintptr_t pt_frame;
    uint32_t i;

    virt &= ~((uintptr_t)0x1FFFFF);  /* align to 2 MiB boundary */

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);
    pdi   = pd_index(virt);

    pdpt = get_or_create_table(kernel_pml4, pml4i, 0, 0);
    if (!pdpt) return -1;

    /* If the PDPT entry is a 1 GiB huge page, split it into 512 x 2 MiB
     * PD entries first, then fall through to the 2 MiB -> 4 KiB split. */
    if ((pdpt[pdpti] & VMM_FLAG_PRESENT) && (pdpt[pdpti] & VMM_FLAG_HUGE)) {
        uintptr_t gib_phys = pdpt[pdpti] & ~GIB_ALIGN_MASK & PTE_ADDR_MASK;
        uint64_t gib_flags = pdpt[pdpti] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);
        uintptr_t pd_frame = pmm_alloc_frame();
        if (!pd_frame) return -1;

        pte_t *new_pd = pt_walk(pd_frame);
        uint32_t j;
        for (j = 0; j < PT_ENTRIES; j++)
            new_pd[j] = (gib_phys + ((uintptr_t)j << 21)) | gib_flags | VMM_FLAG_HUGE;

        /* Replace the 1 GiB PDPTE with a PD pointer */
        pdpt[pdpti] = pd_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE
                     | (gib_flags & VMM_FLAG_USER);
        vmm_flush_tlb_all();

        klog(LOG_INFO, "mm", "Split 1 GiB page at 0x%x into 512 x 2 MiB entries",
             (uint64_t)(gib_phys));
    }

    pd = get_or_create_table(pdpt, pdpti, 0, 0);
    if (!pd) return -1;

    /* Not a huge page -- already split or not present */
    if (!(pd[pdi] & VMM_FLAG_HUGE))
        return 0;  /* idempotent success */

    huge_phys = pd[pdi] & PTE_ADDR_MASK;
    old_flags = pd[pdi] & ~(PTE_ADDR_MASK | VMM_FLAG_HUGE);

    /* Allocate a page table to replace the huge page */
    pt_frame = pmm_alloc_frame();
    if (!pt_frame) return -1;

    pt = pt_walk(pt_frame);

    /* Fill 512 x 4 KiB PTEs preserving the same mapping + flags */
    for (i = 0; i < PT_ENTRIES; i++)
        pt[i] = (huge_phys + (uintptr_t)i * VMM_PAGE_SIZE) | old_flags;

    /* Replace the huge page PDE with the new PT */
    pd[pdi] = pt_frame | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;

    /* Flush TLB for the entire 2 MiB range */
    vmm_flush_tlb_all();

    return 0;
}

int vmm_install_guard_page(uintptr_t virt, const char *label)
{
    uintptr_t page = virt & ~((uintptr_t)0xFFF);
    uintptr_t phys = 0;
    uint64_t flags = 0;
    uint64_t irq_flags;
    int present;
    int not_reusable = 0;
    int full = 0;
    int split_failed = 0;

    /* The split MUST happen under the lock, not before it. vmm_split_huge_page
     * fills its new page table from the ORIGINAL huge mapping and then
     * publishes the PDE unconditionally, without re-checking whether another
     * CPU published one meanwhile. So a split that starts before a competing
     * install and publishes after it would restore the identity PTE that
     * install had already cleared: the earlier caller returns success, stays
     * registered, and runs on a silently unguarded stack. Serializing the split
     * and the clear together is what makes an install atomic against another
     * install.
     *
     * The cost is that this critical section can allocate a PMM frame, reload
     * CR3, and -- only when demoting a 1 GiB page, which happens at most once
     * per region -- klog while IRQs are off. That is the accepted trade: a rare
     * bounded latency spike against a silently missing guard page. Shortening
     * it needs a split that preallocates outside the lock and publishes under
     * one, which changes vmm_split_huge_page for every caller and belongs with
     * the VMM-wide page-table lock (03-memory-concurrency/TODO-07-smp-phase2). */
    spin_lock_irqsave(&guard_page_lock, &irq_flags);
    present = vmm_translate(page, &phys, &flags);
    if (!guard_map_is_reusable_frame(page, present, phys, flags)) {
        /* Guarding means "clear the identity mapping of this frame", so refuse
         * a VA that is not currently its own frame -- clearing it would destroy
         * an unrelated live mapping, and the teardown gate would later stamp an
         * identity mapping over whatever it was. This is not hypothetical in an
         * identity-mapped kernel: a fixed VA like KUSER_SHARED_DATA is just a
         * number, and nothing stops the PMM from handing out the frame whose
         * physical address equals it (pmm.c reserves the user PT window for
         * exactly this collision class, after it cost a boot hang once).
         *
         * The same predicate decides the failure CODE for the branches below,
         * not just this one: a run whose identity mapping is absent, aliased or
         * read-only is not PMM-safe no matter WHY the install failed, so it is
         * never reported as merely unavailable. The refusal touches no PTE. */
        not_reusable = 1;
    } else if (guard_page_register_locked(page, label) != 0) {
        full = 1;
    } else if (vmm_split_huge_page(page) != 0) {
        guard_page_unregister_locked(page);
        split_failed = 1;
    } else {
        /* Clear the PTE to make the page not-present */
        vmm_unmap_page(page, 0);
    }
    spin_unlock_irqrestore(&guard_page_lock, irq_flags);

    /* Serial output never happens under the lock (Gate 2). */
    if (not_reusable) {
        klog(LOG_ERROR, "mm",
             "guard install: %p is not a kernel-writable identity mapping -- refusing, frame is NOT reusable (%s)",
             (void *)page, label);
        return VMM_GUARD_VA_UNSAFE;
    }
    if (full) {
        klog(LOG_WARN, "mm",
             "guard table full (%u entries) -- refusing guard at %p (%s)",
             (uint64_t)VMM_MAX_GUARD_PAGES, (void *)page, label);
        return VMM_GUARD_UNAVAILABLE;
    }
    if (split_failed) {
        klog(LOG_WARN, "mm", "guard install: huge-page split failed at %p (%s)",
             (void *)page, label);
        return VMM_GUARD_UNAVAILABLE;
    }
    return VMM_GUARD_OK;
}

int vmm_uninstall_guard_page(uintptr_t virt)
{
    uintptr_t page = virt & ~((uintptr_t)0xFFF);
    uintptr_t phys = 0;
    uint64_t flags = 0;
    uint64_t irq_flags;
    int was_registered;
    int present;
    int aliased = 0;
    int unowned = 0;
    int rc;

    /* Virtual page 0 is never guarded, and accepting it would make the identity
     * test below (phys == page) trivially true for a zeroed answer. */
    if (!page)
        return -1;

    spin_lock_irqsave(&guard_page_lock, &irq_flags);
    /* PEEK, do not remove. The registration is the only record that this VA is
     * ours to restore, so it is surrendered as the COMMIT step below, after the
     * identity mapping is verified back. Dropping it first meant a failed
     * restore forgot the guard forever: the retry would land on the
     * unregistered path, which refuses by design, and the caller's run would be
     * stranded for the life of the boot. */
    was_registered = guard_page_is_registered_locked(page);

    /* Success MUST mean "the IDENTITY mapping is back" -- vmm_get_physical
     * returning exactly `page` -- not merely "the table holds no entry" and not
     * merely "something is present here". This function is the last gate before
     * the caller hands `page` to the PMM, and it answers two distinct failures:
     *
     *   - Not present: the original code returned 0 for any unregistered VA
     *     without ever reading the PTE, so a dropped registration read as a
     *     clean uninstall and the caller freed a frame whose identity mapping
     *     was still cleared. Failable registration closes that at the source;
     *     restoring here is the second layer.
     *   - Present but ALIASED to a different frame: the VA now belongs to
     *     someone else, so freeing `page` would hand out a frame whose identity
     *     address writes through another owner's memory. Never report success.
     *
     * Presence comes from vmm_translate rather than vmm_get_physical: the
     * latter's 0 cannot distinguish an absent PTE from a present alias to
     * physical frame 0, and mistaking that alias for "absent" would overwrite
     * someone else's mapping and then authorize the free.
     *
     * A registered guard always has its leaf REWRITTEN, even when the address
     * already reads back right: the mapping must also be writable and
     * kernel-only before the frame is reusable, and a same-frame mapping left
     * read-only or user-accessible by some other path would otherwise be waved
     * through. An unregistered VA is only INSPECTED -- normalizing there would
     * rewrite a mapping this function does not own, and the boot identity map
     * legitimately carries User on its 2 MiB pages. */
    present = vmm_translate(page, &phys, &flags);

    if (present && phys != page) {
        aliased = 1;
        rc = -1;
    } else if (was_registered) {
        /* Restore the identity mapping at `page`. The frame physically backing
         * the VA in the boot identity map is `page` itself: Present + Writable,
         * kernel-only (no User bit). Rewritten even when the address already
         * reads back right, because the permissions may not. */
        rc = (vmm_map_page(page, page,
                           VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE) == 0 &&
              guard_va_is_kernel_identity(page)) ? 0 : -1;
        if (rc == 0)
            guard_page_unregister_locked(page);   /* commit */
    } else {
        /* Never guarded: INSPECT, never rewrite. This mapping is not ours --
         * the boot identity map carries User on its 2 MiB pages, and an ABSENT
         * unregistered VA is the very state vmm_install_guard_page() refuses as
         * VMM_GUARD_VA_UNSAFE, so repairing it here would launder a refused
         * install into an authorized free. Judge the snapshot and fail CLOSED;
         * the caller quarantines. */
        unowned = !present;
        rc = guard_map_is_kernel_identity(page, present, phys, flags) ? 0 : -1;
    }
    spin_unlock_irqrestore(&guard_page_lock, irq_flags);

    if (aliased)
        klog(LOG_ERROR, "mm",
             "guard uninstall: %p maps a DIFFERENT frame -- refusing to report it freeable",
             (void *)page);
    else if (unowned)
        klog(LOG_ERROR, "mm",
             "guard uninstall: %p is unmapped with no table entry -- refusing, not ours to repair",
             (void *)page);
    else if (rc != 0)
        klog(LOG_ERROR, "mm",
             "guard uninstall: %p could not be restored to its identity mapping",
             (void *)page);
    return rc;
}

/* --- 1 GiB huge page support --- */

int vmm_map_huge_1g(uintptr_t virt, uintptr_t phys, uint64_t flags)
{
    pte_t *pdpt;
    uint64_t pml4i, pdpti;

    if (!cpu_has(CPU_FEATURE_PAGE1GB))
        return -1;

    /* Both addresses must be 1 GiB aligned */
    if ((virt & GIB_ALIGN_MASK) || (phys & GIB_ALIGN_MASK))
        return -1;

    if (!kernel_pml4)
        return -1;

    pml4i = pml4_index(virt);
    pdpti = pdpt_index(virt);

    /* Walk PML4 to PDPT (create if needed) */
    pdpt = get_or_create_table(kernel_pml4, pml4i, 1, 0);
    if (!pdpt)
        return -1;

    /* Clear NX if CPU lacks it */
    if (!cpu_has(CPU_FEATURE_NX))
        flags &= ~VMM_FLAG_NX;

    /* Refuse to overwrite an existing non-huge PDPT entry (subtree with
     * dynamic mappings). Only allow overwriting if the slot is empty or
     * already a 1 GiB page. */
    if ((pdpt[pdpti] & VMM_FLAG_PRESENT) && !(pdpt[pdpti] & VMM_FLAG_HUGE))
        return -1;

    /* Set PDPT entry with PS=1 for 1 GiB page.
     * Physical address occupies bits 51:30 (30-bit aligned).
     * Intel SDM Vol. 3A Table 4-15: PDPTE format for 1 GiB pages. */
    pdpt[pdpti] = (phys & ~GIB_ALIGN_MASK) | flags | VMM_FLAG_HUGE;

    /* Full TLB flush: a single invlpg only covers one page size; replacing
     * an entire GiB of translations requires a global invalidation. */
    vmm_flush_tlb_all();
    return 0;
}

void vmm_promote_to_1g(void)
{
    pte_t *pdpt;
    uint64_t pdpti;
    uint32_t promoted = 0;

    if (!cpu_has(CPU_FEATURE_PAGE1GB)) {
        klog(LOG_INFO, "mm", "1 GiB pages: not supported, keeping 2 MiB identity map");
        return;
    }

    if (!kernel_pml4 || !(kernel_pml4[0] & VMM_FLAG_PRESENT))
        return;

    pdpt = pt_walk(kernel_pml4[0] & PTE_ADDR_MASK);

    /* Promote PDPT entries 1-3 (GiB 1-3) from PD trees to 1 GiB pages.
     * Skip PDPT[0] (first GiB) because it contains kernel text and needs
     * fine-grained NX at 2 MiB or 4 KiB level (vmm_apply_nx_policy). */
    for (pdpti = 1; pdpti < 4; pdpti++) {
        pte_t pdpte = pdpt[pdpti];
        uintptr_t phys_base;

        /* Only promote entries that point to a PD (present, not already 1 GiB) */
        if (!(pdpte & VMM_FLAG_PRESENT))
            continue;
        if (pdpte & VMM_FLAG_HUGE)
            continue;  /* already a 1 GiB page */

        /* The PD maps pdpti * 1 GiB as an identity map (phys == virt) */
        phys_base = pdpti * GIB_SIZE;

        /* Validate: all 512 PDEs must be present 2 MiB huge pages with
         * contiguous identity-mapped physical addresses AND identical
         * flags. Skip promotion if any PDE has been split, has a guard
         * page, or has different flags (NX, RO, WC, etc.). */
        {
            pte_t *pd = pt_walk(pdpte & PTE_ADDR_MASK);
            uint32_t pdi;
            int safe = 1;
            /* Flag mask: all flag bits except address, accessed, dirty */
            uint64_t flag_mask = ~(PTE_ADDR_MASK | VMM_FLAG_ACCESSED | VMM_FLAG_DIRTY);
            uint64_t expected_flags = pd[0] & flag_mask;
            for (pdi = 0; pdi < 512; pdi++) {
                pte_t pde = pd[pdi];
                uintptr_t expected_phys = phys_base + ((uintptr_t)pdi << 21);
                if (!(pde & VMM_FLAG_PRESENT) || !(pde & VMM_FLAG_HUGE)) {
                    safe = 0;
                    break;
                }
                if ((pde & PTE_ADDR_MASK) != expected_phys) {
                    safe = 0;
                    break;
                }
                if ((pde & flag_mask) != expected_flags) {
                    safe = 0;
                    break;
                }
            }
            if (!safe) {
                klog(LOG_WARN, "mm",
                     "1 GiB: PDPT[%u] PD not uniform at PDE %u; skipping promotion",
                     (uint64_t)pdpti, (uint64_t)pdi);
                continue;
            }
            /* Build PDPTE from validated PDE flags (preserve P, W, U, etc.)
             * plus PS=1 for the 1 GiB page. Strip the 2 MiB PS bit from
             * the flags first (it's the same bit, but conceptually it now
             * means 1 GiB at PDPT level). */
            pdpt[pdpti] = phys_base | expected_flags;
        }
        promoted++;
    }

    if (promoted > 0) {
        vmm_flush_tlb_all();
        klog(LOG_INFO, "mm", "1 GiB pages: promoted %u PDPT entries (GiB 1-%u)",
             (uint64_t)promoted, (uint64_t)(promoted));
    }
}

/* --- Initialization --- */

boot_result_t vmm_init(void)
{
    /* Take over the PML4 the UEFI bootloader built and loaded into CR3
     * (setup_page_tables() in src/boot/uefi/bootx64.c). It is adopted, never
     * rebuilt, so that table is the kernel's page table for the life of the
     * system.
     *
     * The root splits into its two roles here (section 9 walker conversion):
     * the PHYSICAL value is what stays in CR3; the WALK pointer is that value
     * translated through the section-2 HHDM direct map, which the bootloader
     * installed before jumping to the kernel, so it is live at this point. Every
     * page-table deref past this line goes through the walk pointer, so the
     * walkers keep working once section 5 retires the bootloader identity map.
     * This is the cutover point -- a bad HHDM root faults on the first deref
     * (apply_nx_policy / first map) with POST16_HHDM_WALK as the last code. */
    POST16(POST16_HHDM_WALK);
    uintptr_t root_phys = read_cr3() & PTE_ADDR_MASK;
    /* Do NOT use pt_walk() here: this is the earliest walk-pointer install and
     * a NULL translation must degrade to BOOT_FATAL, not a bugcheck. */
    kernel_pml4 = (pte_t *)mm_phys_to_hhdm(root_phys);
    if (!kernel_pml4) {
        klog(LOG_FATAL, "mm", "VMM init: kernel root phys %p has no HHDM alias",
             (uint64_t)root_phys);
        return BOOT_FATAL;
    }
    POST16(POST16_HHDM_WALK_OK);

    /* Register the page fault handler (ISR 14). NOTE: idt_init() in boot
     * phase 1 zeroes handlers[] AFTER this phase-0 call, so this registration
     * is erased; boot_phase1 calls vmm_register_page_fault_handler() again to
     * make it effective. The phase-0 call is kept so the handler is present if
     * a #PF somehow fires between VMM bring-up and idt_init(). */
    vmm_register_page_fault_handler();

    klog(LOG_INFO, "mm", "VMM initialized (PML4 phys %p, walk %p, page fault handler registered)",
           (uint64_t)root_phys, (uint64_t)(uintptr_t)kernel_pml4);

    return BOOT_OK;
}

/* ---- NX policy: mark all non-text pages as non-executable --------------- */

/* Linker symbols for kernel text boundaries */
extern char __text_start[];
extern char __text_end[];

void vmm_apply_nx_policy(void)
{
    uint64_t pml4i, pdpti, pdi;
    uintptr_t text_start, text_end;
    uint32_t nx_count = 0;

    if (!cpu_has(CPU_FEATURE_NX))
        return;

    text_start = (uintptr_t)__text_start;
    text_end   = (uintptr_t)__text_end;

    /* Only apply NX to the kernel's own address range (1 MiB .. __kernel_end).
     * Skip firmware regions, MMIO, UEFI runtime, and high memory to avoid
     * breaking WHPX synthetic pages and UEFI callbacks. */
    uintptr_t kernel_base = 0x100000;  /* 1 MiB -- kernel load address */
    extern char __kernel_end[];
    uintptr_t kernel_top = (uintptr_t)__kernel_end;

    /* Walk all PML4 entries (only first few are populated by boot) */
    for (pml4i = 0; pml4i < 512; pml4i++) {
        pte_t pml4e = kernel_pml4[pml4i];
        pte_t *pdpt;

        if (!(pml4e & VMM_FLAG_PRESENT))
            continue;

        pdpt = pt_walk(pml4e & PTE_ADDR_MASK);

        for (pdpti = 0; pdpti < 512; pdpti++) {
            pte_t pdpte = pdpt[pdpti];
            pte_t *pd;

            if (!(pdpte & VMM_FLAG_PRESENT))
                continue;

            /* 1 GiB huge page -- skip (too coarse for NX policy) */
            if (pdpte & VMM_FLAG_HUGE)
                continue;

            pd = pt_walk(pdpte & PTE_ADDR_MASK);

            for (pdi = 0; pdi < 512; pdi++) {
                pte_t pde = pd[pdi];
                uintptr_t page_base, page_end_addr;

                if (!(pde & VMM_FLAG_PRESENT))
                    continue;

                /* 2 MiB huge page -- the common case for boot mappings */
                if (pde & VMM_FLAG_HUGE) {
                    page_base     = mm_canonical_from_indices(pml4i, pdpti, pdi, 0);
                    page_end_addr = page_base + (1UL << 21);

                    /* Only NX pages within the kernel's own range */
                    if (page_base < kernel_base || page_end_addr > kernel_top)
                        continue;

                    /* If this 2 MiB page overlaps the text section, split it
                     * into 4 KiB pages so we can apply NX per-page -- only the
                     * exact pages covering .text stay executable. Without this,
                     * adjacent rodata/data/BSS in the same huge page would be
                     * left executable (Gate 11/13: spec compliance). */
                    if (page_base < text_end && page_end_addr > text_start) {
                        if (vmm_split_huge_page(page_base) == 0) {
                            /* Re-read PDE -- it's now a PT pointer, not huge.
                             * Fall through to the 4 KiB walk below. */
                            pde = pd[pdi];
                        } else {
                            /* Split failed: this 2 MiB page overlaps .text and
                             * could not be broken into 4 KiB pages, so the
                             * non-text kernel data sharing it would stay
                             * executable. NX is a REQUIRED guarantee
                             * (cpu_enable_nx bug-checks when the CPU lacks NX),
                             * and cpu_verify_hardening only reads EFER.NXE -- it
                             * would log "NX enabled" while data stayed
                             * executable. This runs in boot_phase0 with nearly
                             * all RAM free, so a single-frame split failure means
                             * the PMM is broken: halt rather than ship a false
                             * NX-pass with W+X kernel data. */
                            /* LOG_ERROR (not LOG_FATAL): klog(LOG_FATAL)
                             * halts in its own bare for(;;) loop and never
                             * returns, which would make boot_halt below
                             * unreachable and skip the styled halt screen +
                             * POST16_BOOT_FAILED + subsystem dump. Log the
                             * detail, then let boot_halt own the halt. */
                            klog(LOG_ERROR, "mm",
                                 "NX policy: huge-page split failed at 0x%x "
                                 "(.text overlap) -- kernel data would stay "
                                 "executable", (uint64_t)page_base);
                            boot_halt("NX policy: cannot enforce NX on kernel data");
                        }
                    } else {
                        /* Entire huge page is non-text kernel data -- NX it */
                        pd[pdi] |= VMM_FLAG_NX;
                        nx_count++;
                        continue;
                    }
                }

                /* 4 KiB page table -- walk PT entries */
                {
                    pte_t *pt = pt_walk(pde & PTE_ADDR_MASK);
                    uint32_t pti;
                    for (pti = 0; pti < 512; pti++) {
                        pte_t pte = pt[pti];
                        uintptr_t va;
                        if (!(pte & VMM_FLAG_PRESENT))
                            continue;
                        va = mm_canonical_from_indices(pml4i, pdpti, pdi, pti);
                        if (va < kernel_base || va >= kernel_top)
                            continue;  /* outside kernel range */
                        if (va >= text_start && va < text_end)
                            continue;  /* text -- must execute */
                        pt[pti] |= VMM_FLAG_NX;
                        nx_count++;
                    }
                }
            }
        }
    }

    /* NOTE: U/S bit clearing deferred until per-process page tables exist.
     * The shared identity-mapped address space uses 2 MiB pages; user stacks
     * are kmalloc'd from the kernel heap, so user and kernel data share the
     * same 2 MiB pages.  Clearing U/S on kernel pages breaks user-mode stack
     * access.  SMEP/SMAP are skipped on bare metal for the same reason.
     * See 03-memory-concurrency/TODO-01-vmm-memory-protection.md and
     * TODO-05-advanced-virtual-memory.md for the per-process page-table
     * work that unlocks SMEP/SMAP on bare metal. */

    vmm_flush_tlb_all();
    klog(LOG_INFO, "mm", "NX policy applied: %u pages marked non-executable (text: 0x%x-0x%x)",
         (uint64_t)nx_count, (uint64_t)text_start, (uint64_t)text_end);
}
