/* ============================================================================
 * gdt.c — Global Descriptor Table (x86-64 Long Mode)
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

#include "gdt.h"
#include "printk.h"

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
    /* First 8 bytes — standard descriptor format */
    gdt[index].limit_low   = (uint16_t)(limit & 0xFFFF);
    gdt[index].base_low    = (uint16_t)(base & 0xFFFF);
    gdt[index].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[index].access      = 0x89;   /* Present=1, DPL=0, Type=9 (available 64-bit TSS) */
    gdt[index].granularity  = (uint8_t)(((limit >> 16) & 0x0F));
    gdt[index].base_high   = (uint8_t)((base >> 24) & 0xFF);

    /* Second 8 bytes — upper 32 bits of base address + reserved */
    /* We treat gdt[index+1] as raw bytes for the upper base */
    uint32_t *upper = (uint32_t *)&gdt[index + 1];
    upper[0] = (uint32_t)(base >> 32);  /* base[63:32] */
    upper[1] = 0;                        /* reserved */
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

    /* [3] User code segment (Ring 3, 64-bit)
     * Access: Present=1, DPL=11, S=1, Type=Execute/Read = 0xFA
     * Flags:  L=1 (Long Mode), D=0, G=0 = 0x02 */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0x0A);

    /* [4] User data segment (Ring 3)
     * Access: Present=1, DPL=11, S=1, Type=Read/Write = 0xF2
     * Flags:  L=0, D=1, G=1 = 0x0C */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0x0C);

    /* [5-6] TSS descriptor (16 bytes in Long Mode) */
    gdt_set_tss(5, tss_base, tss_limit);

    /* --- Load the GDT --- */
    gdtr.limit = (uint16_t)(sizeof(gdt) - 1);
    gdtr.base  = (uint64_t)(uintptr_t)&gdt;

    gdt_flush((uint64_t)(uintptr_t)&gdtr);
    tss_flush(GDT_TSS_SEG);

    printk("[OK] GDT loaded (%u entries, TSS at %p)\n",
           (uint64_t)GDT_NUM_ENTRIES, tss_base);
}

void tss_set_kernel_stack(uint64_t stack_top)
{
    kernel_tss.rsp0 = stack_top;
}
