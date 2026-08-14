/* ============================================================================
 * gdt.c -- Global Descriptor Table (x86-64 Long Mode)
 *
 * Sets up the GDT with:
 *   [0] Null descriptor
 *   [1] Kernel code  (Ring 0, 64-bit)
 *   [2] Kernel data  (Ring 0)
 *   [3] User code    (Ring 3, 64-bit)
 *   [4] User data    (Ring 3)
 *   [5-6] TSS        (16-byte descriptor in Long Mode)
 *
 * Then loads the GDT and TSS via assembly helpers.
 * ============================================================================ */

#include "kernel/gdt.h"
#include "kernel/klog.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/boot_init.h"
#include "kernel/boot_halt.h"
/* A single GDT entry (8 bytes) */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   /* flags (4 bits) + limit_high (4 bits) */
    uint8_t  base_high;
} __attribute__((packed));

/* GDT pointer (loaded by lgdt) */
struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The GDT (7 entries: null + 4 segments + 2 for TSS) */
static struct gdt_entry gdt[GDT_NUM_ENTRIES];

/* The GDTR value */
static struct gdt_pointer gdtr;

/* The TSS */
static struct tss kernel_tss;

/* External assembly: reload segment registers after loading new GDT */
extern void gdt_flush(uint64_t gdtr_addr);

/* External assembly: load the TSS */
extern void tss_flush(uint16_t selector);

/* --- Helper: set a standard GDT entry --- */
static void gdt_set_entry(uint32_t index, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t flags)
{
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access      = access;
    gdt[index].granularity  = (uint8_t)(((flags & 0x0F) << 4) | ((limit >> 16) & 0x0F));
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);
}

/* --- Helper: set the 64-bit TSS descriptor (spans 2 GDT slots) --- */
static void gdt_set_tss(uint32_t index, uint64_t base, uint32_t limit)
{
    /* First 8 bytes -- standard descriptor format */
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access      = 0x89;   /* Present=1, DPL=0, Type=9 (available 64-bit TSS) */
    gdt[index].granularity  = (uint8_t)(((limit >> 16) & 0x0F));
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    /* Second 8 bytes -- upper 32 bits of base address + reserved */
    /* We treat gdt[index+1] as raw bytes for the upper base */
    uint32_t *upper = (uint32_t *)&gdt[index + 1];
    upper[0] = (uint32_t)(base >> 32);  /* base[63:32] */
    upper[1] = 0;                        /* reserved */
}

/* IST stacks for critical exceptions. Each IST = 1 guard page (bottom) +
 * IST_STACK_PAGES usable pages. 4 KiB was too tight: the #DF/NMI/MCE path
 * falls through to panic_screen() -> write_crash_dump(), which puts a 2 KiB
 * buffer on the stack before the VFS/NVRAM/framebuffer render chain.
 *
 * The panic path's stack consumers, kept current because overflowing IST1 while
 * already handling #DF hits the guard page and triple-faults -- the exact
 * failure IST exists to prevent. panic_screen_impl holds the caller-string
 * snapshot desc_snap[256] + file_snap[64] and the shared frame capture
 * frames[PANIC_MAX_STACK_DEPTH] (128 B) live across its whole body; below it
 * write_crash_dump adds char buf[2048]. klog_emit -- reachable from these paths
 * and treated as panic-context by its own clock choice -- adds a klog_entry_t
 * snapshot (288 B) plus the line[512] serial buffer, ~800 B whenever anything
 * on the path logs. That is ~3.3 KiB of named locals inside 8 KiB, before the
 * VFS chain. Anything that adds a large panic-path local belongs in this
 * list. */
#define IST_STACK_PAGES 2   /* usable pages above the guard (8 KiB) */

/* Allocate one IST stack with a guard page at the bottom; return the stack
 * top. IST is critical boot infrastructure: without a valid, guarded stack a
 * #DF/NMI/MCE triple-faults instead of showing a BSOD, so any allocation or
 * guard failure is fatal, not degraded. */
static uint64_t ist_alloc(const char *guard_label)
{
    uintptr_t base = pmm_alloc_contiguous(IST_STACK_PAGES + 1);
    if (!base)
        boot_halt("IST stack allocation failed (out of physical memory)");
    if (vmm_install_guard_page(base, guard_label) != 0)
        boot_halt("IST guard page install failed");
    return (uint64_t)(base + (uint64_t)(IST_STACK_PAGES + 1) * VMM_PAGE_SIZE);
}

void gdt_init(void)
{
    uint64_t tss_base = (uint64_t)(uintptr_t)&kernel_tss;
    uint32_t tss_limit = sizeof(struct tss) - 1;
    uint32_t i;

    /* Zero the TSS */
    uint8_t *tss_ptr = (uint8_t *)&kernel_tss;
    for (i = 0; i < sizeof(struct tss); i++)
        tss_ptr[i] = 0;

    /* Set RSP0 to the boot stack so ring 3→0 transitions have a valid
     * kernel stack.  Also required on some bare-metal Intel CPUs that
     * check RSP0 validity on interrupt delivery. */
    {
        extern char stack_top[];  /* defined in entry.asm */
        kernel_tss.rsp0 = (uint64_t)(uintptr_t)stack_top;
    }

    /* IST stacks for critical exceptions: #DF, NMI, MCE. Allocated from PMM
     * (identity-mapped, phys = virt). PMM is up in Phase 0, GDT in Phase 1.
     * Failure is fatal (see ist_alloc). NOTE: this configures the BSP TSS
     * only; per-CPU TSS/IST for APs (AP bringup hardening) is not set up here,
     * so AP critical-exception IST delivery is not yet SMP-covered. */
    POST16(0xD200);
    kernel_tss.ist1 = ist_alloc("GUARD: IST #DF stack overflow");
    kernel_tss.ist2 = ist_alloc("GUARD: IST NMI stack overflow");
    kernel_tss.ist3 = ist_alloc("GUARD: IST MCE stack overflow");
    klog(LOG_INFO, "cpu",
         "IST stacks: DF=%p NMI=%p MCE=%p (%u KiB each + guard page)",
         kernel_tss.ist1, kernel_tss.ist2, kernel_tss.ist3,
         (uint64_t)(IST_STACK_PAGES * 4));
    POST16(0xD201);

    /* Set the I/O Permission Bitmap offset to beyond the TSS (no IOPB) */
    kernel_tss.iopb_offset = sizeof(struct tss);

    /* --- Populate GDT entries --- */

    /* [0] Null descriptor */
    gdt_set_entry(0, 0, 0, 0, 0);

    /* [1] Kernel code segment (Ring 0, 64-bit)
     * Access: Present=1, DPL=00, S=1, Type=Execute/Read = 0x9A
     * Flags:  L=1 (Long Mode), D=0, G=0 = 0x02 */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0x0A);

    /* [2] Kernel data segment (Ring 0)
     * Access: Present=1, DPL=00, S=1, Type=Read/Write = 0x92
     * Flags:  L=0, D=1, G=1 = 0x0C */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0x0C);

    /* [3] User data segment (Ring 3) -- MUST be before user code for SYSRET
     * Access: Present=1, DPL=11, S=1, Type=Read/Write = 0xF2
     * Flags:  L=0, D=1, G=1 = 0x0C */
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0x0C);

    /* [4] User code segment (Ring 3, 64-bit)
     * Access: Present=1, DPL=11, S=1, Type=Execute/Read = 0xFA
     * Flags:  L=1 (Long Mode), D=0, G=0 = 0x02 */
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0x0A);

    /* [5-6] TSS descriptor (16 bytes in Long Mode) */
    gdt_set_tss(5, tss_base, tss_limit);

    /* --- Load the GDT --- */
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    tss_flush(GDT_TSS_SEG);

    /* --- Runtime verification of SYSRET-critical GDT ordering ---
     * Read back the access bytes from the populated entries and verify
     * user data is at index 3 (0x18) and user code is at index 4 (0x20).
     * A mismatch here means gdt_set_entry() calls are in the wrong order. */
    {
        uint8_t udata_access = gdt[GDT_USER_DATA / 8].access;
        uint8_t ucode_access = gdt[GDT_USER_CODE / 8].access;

        /* User data: Present=1, DPL=11, S=1, Type=Read/Write = 0xF2 */
        if (udata_access != 0xF2) {
            klog(LOG_FATAL, "GDT",
                 "FATAL: GDT[0x%x] access=0x%x, expected 0xF2 (user data). "
                 "SYSRET will triple-fault!",
                 (uint32_t)GDT_USER_DATA, (uint32_t)udata_access);
            for (;;) __asm__ volatile("cli; hlt");
        }

        /* User code: Present=1, DPL=11, S=1, Type=Execute/Read = 0xFA */
        if (ucode_access != 0xFA) {
            klog(LOG_FATAL, "GDT",
                 "FATAL: GDT[0x%x] access=0x%x, expected 0xFA (user code). "
                 "SYSRET will triple-fault!",
                 (uint32_t)GDT_USER_CODE, (uint32_t)ucode_access);
            for (;;) __asm__ volatile("cli; hlt");
        }
    }

    klog(LOG_INFO, "cpu", "GDT loaded (%u entries, TSS at %p)",
           (uint64_t)GDT_NUM_ENTRIES, tss_base);
}

void tss_set_kernel_stack(uint64_t stack_top)
{
    kernel_tss.rsp0 = stack_top;
}

void gdt_get_gdtr(void *out_gdtr)
{
    uint8_t *dst = (uint8_t *)out_gdtr;
    const uint8_t *src = (const uint8_t *)&gdtr;
    uint32_t i;
    for (i = 0; i < 10; i++)
        dst[i] = src[i];
}
