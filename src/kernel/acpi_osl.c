/* ============================================================================
 * acpi_osl.c -- ACPICA OS Services Layer for Impossible OS
 *
 * The AcpiOs* interface ACPICA calls out through (acpiosxf.h). This is the
 * ONLY file that knows both ACPICA's types and the kernel's; the platform
 * header src/kernel/acpica/include/platform/acimpossible.h keeps ACPICA's
 * 164 translation units free of kernel headers by declaring its handle types
 * as void pointers, and every cast back happens here.
 *
 * Threading: ACPI_SINGLE_THREADED is deliberately NOT defined, so ACPICA
 * serialises itself through the locks and semaphores below. Mutexes are left
 * at ACPICA's default ACPI_BINARY_SEMAPHORE, which builds them on top of the
 * semaphore calls rather than requiring an AcpiOsCreateMutex family here.
 * ============================================================================ */

#include "kernel/types.h"
#include "kernel/klog.h"
#include "kernel/printk.h"
#include "kernel/boot_info.h"
#include "kernel/irq.h"
#include "kernel/timer.h"
#include "kernel/mm/heap.h"
#include "kernel/mm/vmm.h"
#include "kernel/mm/memmap.h"
#include "kernel/sched/spinlock.h"
#include "kernel/sched/semaphore.h"
#include "kernel/sched/task.h"
#include "kernel/drivers/pci.h"
#include "kernel/drivers/ioapic.h"
#include "kernel/acpi_global_lock.h"

#include "acpi.h"
#include "accommon.h"

/* Note: libc/string.h is deliberately NOT included. ACPICA's bundled acclib.h
 * declares strlen/strncat/vsnprintf with its own prototypes, and pulling the
 * kernel's string.h into this translation unit makes the two collide. */

#define _COMPONENT          ACPI_OS_SERVICES
ACPI_MODULE_NAME            ("osimpossible")

/* ARCH: x86-64 -- port I/O has no arch-neutral wrapper in this tree yet, and
 * every other driver defines these locally for the same reason. */
static inline void osl_outb (uint16_t port, uint8_t v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint8_t osl_inb (uint16_t port)
{ uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void osl_outw (uint16_t port, uint16_t v)
{ __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint16_t osl_inw (uint16_t port)
{ uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void osl_outl (uint16_t port, uint32_t v)
{ __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline uint32_t osl_inl (uint16_t port)
{ uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ---- Environment ---------------------------------------------------------- */

ACPI_STATUS AcpiOsInitialize (void)
{
    return (AE_OK);
}

ACPI_STATUS AcpiOsTerminate (void)
{
    return (AE_OK);
}

/*
 * The bootloader already located the RSDP through the UEFI configuration
 * table and published it in boot_info, which is strictly better than
 * ACPICA's legacy scan: on a UEFI system the low-memory/EBDA scan that
 * AcpiFindRootPointer() performs is not guaranteed to find anything.
 */
ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer (void)
{
    if (g_boot_info.acpi_available && g_boot_info.acpi_rsdp_addr) {
        return ((ACPI_PHYSICAL_ADDRESS)g_boot_info.acpi_rsdp_addr);
    }

    /* No UEFI ACPI table: fall back to the legacy scan rather than failing
     * outright, so a BIOS-style environment still initialises. */
    {
        ACPI_PHYSICAL_ADDRESS phys = 0;
        if (ACPI_SUCCESS (AcpiFindRootPointer (&phys))) {
            return (phys);
        }
    }

    klog (LOG_ERROR, "acpica", "no RSDP: boot_info has none and the legacy scan failed");
    return (0);
}

/* No table overrides: firmware tables are used as found. */
ACPI_STATUS AcpiOsPredefinedOverride (const ACPI_PREDEFINED_NAMES *InitVal,
                                      ACPI_STRING *NewVal)
{
    if (!InitVal || !NewVal) {
        return (AE_BAD_PARAMETER);
    }
    *NewVal = NULL;
    return (AE_OK);
}

ACPI_STATUS AcpiOsTableOverride (ACPI_TABLE_HEADER *ExistingTable,
                                 ACPI_TABLE_HEADER **NewTable)
{
    if (!ExistingTable || !NewTable) {
        return (AE_BAD_PARAMETER);
    }
    *NewTable = NULL;
    return (AE_OK);
}

ACPI_STATUS AcpiOsPhysicalTableOverride (ACPI_TABLE_HEADER *ExistingTable,
                                         ACPI_PHYSICAL_ADDRESS *NewAddress,
                                         UINT32 *NewTableLength)
{
    if (!ExistingTable || !NewAddress || !NewTableLength) {
        return (AE_BAD_PARAMETER);
    }
    *NewAddress = 0;
    *NewTableLength = 0;
    return (AE_OK);
}

/* ---- Memory --------------------------------------------------------------- */

/*
 * Mapping policy.
 *
 * Everything goes through the MMIO mapper. Returning mm_phys_to_hhdm()
 * whenever mm_phys_extent_in_hhdm() is true looks like a free fast path and
 * is wrong twice over:
 *
 *  1. mm_phys_in_hhdm() is a pure BOUNDS test (`phys != 0 && phys <
 *     MM_HHDM_SIZE`). It proves the address is inside the 64 TiB window, not
 *     that a leaf PTE exists for it. ACPI tables are EfiACPIReclaimMemory and
 *     device space is MMIO; neither is guaranteed present in the direct map,
 *     so the "fast path" would have handed ACPICA a pointer into an absent
 *     mapping and faulted during subsystem init.
 *  2. The fallback passed the raw address to vmm_map_mmio_uc(), which rejects
 *     any base with low bits set (`phys_base & 0xFFF`). ACPI addresses are
 *     routinely unaligned, so that path returned NULL for most real inputs.
 *
 * So: everything goes through the MMIO mapper, page-aligned, with the byte
 * offset re-applied to the returned pointer. Correctness over speed -- UC on
 * table RAM is slower than a cached direct-map read, but a fault is not a
 * performance problem.
 *
 * The mapper's VA allocator is a bump pointer that vmm_unmap_mmio() does not
 * reclaim, so a naive map/unmap per AML operation-region access would exhaust
 * the 1 GiB MMIO window. The table below therefore CACHES mappings: a repeat
 * request for a covered range reuses the existing VA and bumps a refcount,
 * and unmap only drops the refcount. Entries are never torn down, which is
 * the correct trade for a fixed, small set of firmware regions.
 */
#define ACPI_OSL_MAX_MAPPINGS   64

struct acpi_osl_mapping {
    uint64_t phys_base;     /* page-aligned */
    uint64_t pages;
    void    *virt;          /* page-aligned VA from the MMIO mapper */
    uint32_t refs;
};

static struct acpi_osl_mapping s_maps[ACPI_OSL_MAX_MAPPINGS];
static uint32_t                s_map_count;
static spinlock_t              s_map_lock = SPINLOCK_INIT;

void *AcpiOsMapMemory (ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    uint64_t base, offset, span, pages, flags;
    uint32_t i;
    void *virt = NULL;

    if (Length == 0) {
        return (NULL);
    }

    base   = (uint64_t)Where & ~0xFFFull;
    offset = (uint64_t)Where & 0xFFFull;
    span   = offset + (uint64_t)Length;          /* bytes from the aligned base */
    pages  = (span + 0xFFFull) >> 12;

    if (span < offset || pages == 0 || span > 0xFFFFFFFFull) {
        klog (LOG_ERROR, "acpica", "bad map request at 0x%llx len %llu",
              (uint64_t)Where, (uint64_t)Length);
        return (NULL);
    }

    spin_lock_irqsave (&s_map_lock, &flags);

    /* Reuse any existing mapping that already covers the whole span. */
    for (i = 0; i < s_map_count; i++) {
        if (s_maps[i].phys_base <= base &&
            base + pages <= s_maps[i].phys_base + s_maps[i].pages) {
            s_maps[i].refs++;
            virt = (void *)((uint8_t *)s_maps[i].virt +
                            (base - s_maps[i].phys_base) * 0x1000ull + offset);
            spin_unlock_irqrestore (&s_map_lock, flags);
            return (virt);
        }
    }

    if (s_map_count >= ACPI_OSL_MAX_MAPPINGS) {
        spin_unlock_irqrestore (&s_map_lock, flags);
        klog (LOG_ERROR, "acpica", "mapping table full (%u entries)",
              (uint64_t)ACPI_OSL_MAX_MAPPINGS);
        return (NULL);
    }
    spin_unlock_irqrestore (&s_map_lock, flags);

    /* Map outside the lock: the mapper allocates page-table frames and does
     * its own locking, and holding ours across it would nest two locks for
     * no benefit. */
    virt = vmm_map_mmio_uc (base, (uint32_t)(pages * 0x1000ull));
    if (!virt) {
        klog (LOG_ERROR, "acpica", "map failed: phys 0x%llx, %llu page(s)",
              base, pages);
        return (NULL);
    }

    spin_lock_irqsave (&s_map_lock, &flags);
    /* Re-check: another CPU may have installed a covering entry while we
     * were mapping. Keeping both is harmless (ours simply goes unused for
     * lookups) and is cheaper than unwinding a completed mapping. */
    if (s_map_count < ACPI_OSL_MAX_MAPPINGS) {
        s_maps[s_map_count].phys_base = base;
        s_maps[s_map_count].pages     = pages;
        s_maps[s_map_count].virt      = virt;
        s_maps[s_map_count].refs      = 1;
        s_map_count++;
    }
    spin_unlock_irqrestore (&s_map_lock, flags);

    return ((void *)((uint8_t *)virt + offset));
}

void AcpiOsUnmapMemory (void *LogicalAddress, ACPI_SIZE Length)
{
    uint64_t flags;
    uint32_t i;

    if (!LogicalAddress || Length == 0) {
        return;
    }

    spin_lock_irqsave (&s_map_lock, &flags);
    for (i = 0; i < s_map_count; i++) {
        uint8_t *start = (uint8_t *)s_maps[i].virt;
        uint8_t *end   = start + s_maps[i].pages * 0x1000ull;
        if ((uint8_t *)LogicalAddress >= start &&
            (uint8_t *)LogicalAddress <  end) {
            if (s_maps[i].refs > 0) {
                s_maps[i].refs--;
            }
            /* Deliberately NOT torn down at refs == 0: vmm_unmap_mmio() does
             * not return the VA to the bump allocator, so releasing here
             * would leak address space on every remap. The entry stays live
             * and is reused by the next request for the same region. */
            break;
        }
    }
    spin_unlock_irqrestore (&s_map_lock, flags);
}

ACPI_STATUS AcpiOsGetPhysicalAddress (void *LogicalAddress,
                                      ACPI_PHYSICAL_ADDRESS *PhysicalAddress)
{
    if (!LogicalAddress || !PhysicalAddress) {
        return (AE_BAD_PARAMETER);
    }

    /* Resolve through the mapping table. The HHDM relation is NOT usable
     * here: AcpiOsMapMemory hands out MMIO-window pointers, and
     * mm_hhdm_to_phys() rejects anything outside the direct map. */
    {
        uint64_t flags;
        uint32_t i;
        ACPI_STATUS st = AE_ERROR;

        spin_lock_irqsave (&s_map_lock, &flags);
        for (i = 0; i < s_map_count; i++) {
            uint8_t *start = (uint8_t *)s_maps[i].virt;
            uint8_t *end   = start + s_maps[i].pages * 0x1000ull;
            if ((uint8_t *)LogicalAddress >= start &&
                (uint8_t *)LogicalAddress <  end) {
                *PhysicalAddress = (ACPI_PHYSICAL_ADDRESS)
                    (s_maps[i].phys_base +
                     (uint64_t)((uint8_t *)LogicalAddress - start));
                st = AE_OK;
                break;
            }
        }
        spin_unlock_irqrestore (&s_map_lock, flags);
        return (st);
    }
}

/* Sanity ceiling on a single ACPICA allocation. ACPICA's largest objects are
 * namespace and parse-op arrays sized from firmware tables, so anything past
 * this is a corrupt length field rather than a real request; the kernel heap
 * itself would accept far more and quietly hand out the garbage size. */
#define ACPI_OSL_ALLOC_MAX      (4u * 1024u * 1024u)

void *AcpiOsAllocate (ACPI_SIZE Size)
{
    if (Size == 0 || Size > ACPI_OSL_ALLOC_MAX) {
        return (NULL);
    }
    return (kmalloc ((size_t)Size));
}

void AcpiOsFree (void *Memory)
{
    kfree (Memory);
}

/*
 * ACPICA uses these only as defensive sanity checks before dereferencing.
 * The kernel has no per-range probe, so the honest answer is limited to what
 * is cheaply provable: a non-NULL pointer with a non-zero length that does
 * not wrap. An HHDM bounds test was used here initially and was wrong for the
 * same reason it was wrong in AcpiOsMapMemory -- being inside the direct-map
 * window says nothing about a mapping existing, and OSL pointers live in the
 * MMIO window anyway, which the test would have rejected outright.
 */
BOOLEAN AcpiOsReadable (void *Pointer, ACPI_SIZE Length)
{
    uintptr_t p = (uintptr_t)Pointer;

    if (!Pointer || Length == 0) {
        return (FALSE);
    }
    return ((p + (uintptr_t)Length >= p) ? TRUE : FALSE);
}

BOOLEAN AcpiOsWritable (void *Pointer, ACPI_SIZE Length)
{
    return (AcpiOsReadable (Pointer, Length));
}

/* ---- Spin locks ----------------------------------------------------------- */

ACPI_STATUS AcpiOsCreateLock (ACPI_SPINLOCK *OutHandle)
{
    spinlock_t *lock;

    if (!OutHandle) {
        return (AE_BAD_PARAMETER);
    }

    lock = (spinlock_t *)kmalloc (sizeof (spinlock_t));
    if (!lock) {
        return (AE_NO_MEMORY);
    }

    *lock = (spinlock_t)SPINLOCK_INIT;
    *OutHandle = (ACPI_SPINLOCK)lock;
    return (AE_OK);
}

void AcpiOsDeleteLock (ACPI_SPINLOCK Handle)
{
    kfree ((void *)Handle);
}

/*
 * IRQ-saving variants are mandatory, not a preference: the SCI handler runs
 * ACPICA code, so a plain spin_lock here would deadlock the moment an SCI
 * arrived on a CPU already holding the lock at task level.
 */
ACPI_CPU_FLAGS AcpiOsAcquireLock (ACPI_SPINLOCK Handle)
{
    uint64_t flags = 0;

    if (!Handle) {
        return (0);
    }

    spin_lock_irqsave ((spinlock_t *)Handle, &flags);
    return ((ACPI_CPU_FLAGS)flags);
}

void AcpiOsReleaseLock (ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    if (!Handle) {
        return;
    }
    spin_unlock_irqrestore ((spinlock_t *)Handle, (uint64_t)Flags);
}

/* ---- Semaphores ----------------------------------------------------------- */

ACPI_STATUS AcpiOsCreateSemaphore (UINT32 MaxUnits, UINT32 InitialUnits,
                                   ACPI_SEMAPHORE *OutHandle)
{
    semaphore_t *sem;

    (void)(MaxUnits);

    if (!OutHandle) {
        return (AE_BAD_PARAMETER);
    }

    sem = (semaphore_t *)kmalloc (sizeof (semaphore_t));
    if (!sem) {
        return (AE_NO_MEMORY);
    }

    sem_init (sem, "acpica", (int32_t)InitialUnits);
    *OutHandle = (ACPI_SEMAPHORE)sem;
    return (AE_OK);
}

ACPI_STATUS AcpiOsDeleteSemaphore (ACPI_SEMAPHORE Handle)
{
    if (!Handle) {
        return (AE_BAD_PARAMETER);
    }
    kfree ((void *)Handle);
    return (AE_OK);
}

/*
 * Timeout handling. The kernel semaphore exposes a blocking wait and a
 * non-blocking trywait, with no timed variant, so the three cases ACPICA
 * actually uses are served exactly and anything else polls at millisecond
 * granularity rather than silently blocking forever.
 */
ACPI_STATUS AcpiOsWaitSemaphore (ACPI_SEMAPHORE Handle, UINT32 Units,
                                 UINT16 Timeout)
{
    semaphore_t *sem = (semaphore_t *)Handle;
    UINT32 i;

    if (!sem || Units == 0) {
        return (AE_BAD_PARAMETER);
    }

    for (i = 0; i < Units; i++) {
        if (Timeout == ACPI_DO_NOT_WAIT) {
            if (!sem_trywait (sem)) {
                /* Undo the partial acquisition so the count is unchanged. */
                while (i-- > 0) {
                    sem_signal (sem);
                }
                return (AE_TIME);
            }
        } else if (Timeout == ACPI_WAIT_FOREVER) {
            sem_wait (sem);
        } else {
            UINT32 waited_ms = 0;
            int got = 0;

            while (waited_ms <= (UINT32)Timeout) {
                if (sem_trywait (sem)) {
                    got = 1;
                    break;
                }
                AcpiOsStall (1000);
                waited_ms++;
            }

            if (!got) {
                while (i-- > 0) {
                    sem_signal (sem);
                }
                return (AE_TIME);
            }
        }
    }

    return (AE_OK);
}

ACPI_STATUS AcpiOsSignalSemaphore (ACPI_SEMAPHORE Handle, UINT32 Units)
{
    semaphore_t *sem = (semaphore_t *)Handle;

    if (!sem || Units == 0) {
        return (AE_BAD_PARAMETER);
    }

    sem_signal_n (sem, (int32_t)Units);
    return (AE_OK);
}

/* ---- Threads and scheduling ----------------------------------------------- */

/*
 * ACPICA requires a non-zero thread id and uses it for mutex ownership, so
 * a caller running before the scheduler exists must still get a distinct
 * stable value. Thread ids are unique within a task and never 0 there, so
 * the +1 bias only matters for the pre-scheduler case.
 */
ACPI_THREAD_ID AcpiOsGetThreadId (void)
{
    struct thread *t = thread_current ();

    if (!t) {
        return ((ACPI_THREAD_ID)1);
    }
    return ((ACPI_THREAD_ID)t->id + 1);
}

/*
 * ACPICA defers GPE and Notify work through this. Running the callback
 * inline is correct but not free: it lengthens the SCI handler. The
 * deferred-work path belongs with the ACPI power-button SCI work in
 * 04-drivers-hardware (power management), which is where the workqueue
 * hand-off lands; until then, inline execution is the behaviour that
 * cannot drop an event.
 */
ACPI_STATUS AcpiOsExecute (ACPI_EXECUTE_TYPE Type,
                           ACPI_OSD_EXEC_CALLBACK Function, void *Context)
{
    (void)(Type);

    if (!Function) {
        return (AE_BAD_PARAMETER);
    }

    Function (Context);
    return (AE_OK);
}

void AcpiOsWaitEventsComplete (void)
{
    /* AcpiOsExecute runs callbacks synchronously, so nothing is outstanding. */
}

void AcpiOsSleep (UINT64 Milliseconds)
{
    UINT64 i;

    for (i = 0; i < Milliseconds; i++) {
        AcpiOsStall (1000);
    }
}

void AcpiOsStall (UINT32 Microseconds)
{
    uint64_t deadline = uptime_ns () + ((uint64_t)Microseconds * 1000ull);

    while (uptime_ns () < deadline) {
        __asm__ volatile ("pause" ::: "memory");
    }
}

/* ---- Interrupts ----------------------------------------------------------- */

/*
 * ACPICA installs exactly one handler, for the SCI. The GSI translation
 * mirrors acpi_register_sci(): FADT SCI_INT below 16 is an ISA IRQ that must
 * go through the MADT override table, at or above 16 it is already a GSI
 * (ACPI 6.5 FADT SCI_INT). The SCI is level-triggered active-low (0x0F) and
 * shared, since firmware may route other sources onto the same line.
 */
static ACPI_OSD_HANDLER  s_sci_handler;
static void             *s_sci_context;
static uint32_t          s_sci_gsi;
static uint8_t           s_sci_installed;

static int osl_sci_trampoline (uint8_t vector, void *ctx)
{
    (void)(vector);
    (void)(ctx);

    if (!s_sci_handler) {
        return (IRQ_NONE);
    }

    return (s_sci_handler (s_sci_context) == ACPI_INTERRUPT_HANDLED
                ? IRQ_HANDLED : IRQ_NONE);
}

ACPI_STATUS AcpiOsInstallInterruptHandler (UINT32 InterruptNumber,
                                           ACPI_OSD_HANDLER ServiceRoutine,
                                           void *Context)
{
    uint32_t gsi;
    uint8_t  vector;

    if (!ServiceRoutine) {
        return (AE_BAD_PARAMETER);
    }
    if (s_sci_installed) {
        return (AE_ALREADY_EXISTS);
    }

    gsi = (InterruptNumber < 16)
              ? ioapic_isa_to_gsi ((uint8_t)InterruptNumber)
              : InterruptNumber;

    s_sci_handler = ServiceRoutine;
    s_sci_context = Context;

    vector = irq_request_gsi_ex (gsi, osl_sci_trampoline, NULL,
                                 "acpica-sci", 0x0F, 1);
    if (!vector) {
        s_sci_handler = NULL;
        s_sci_context = NULL;
        klog (LOG_ERROR, "acpica", "SCI GSI %u not routable", (uint64_t)gsi);
        return (AE_NOT_ACQUIRED);
    }

    s_sci_gsi = gsi;
    s_sci_installed = 1;
    klog (LOG_INFO, "acpica", "SCI installed (int %u, GSI %u, vec 0x%x)",
          (uint64_t)InterruptNumber, (uint64_t)gsi, (uint64_t)vector);
    return (AE_OK);
}

ACPI_STATUS AcpiOsRemoveInterruptHandler (UINT32 InterruptNumber,
                                          ACPI_OSD_HANDLER ServiceRoutine)
{
    (void)(InterruptNumber);

    if (!ServiceRoutine || !s_sci_installed) {
        return (AE_NOT_EXIST);
    }
    if (ServiceRoutine != s_sci_handler) {
        return (AE_BAD_PARAMETER);
    }

    if (irq_release_gsi_shared (s_sci_gsi, osl_sci_trampoline, NULL) != 0) {
        return (AE_ERROR);
    }

    s_sci_installed = 0;
    s_sci_handler = NULL;
    s_sci_context = NULL;
    return (AE_OK);
}

/* ---- Port I/O ------------------------------------------------------------- */

ACPI_STATUS AcpiOsReadPort (ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    if (!Value) {
        return (AE_BAD_PARAMETER);
    }

    switch (Width) {
    case 8:  *Value = osl_inb ((uint16_t)Address); return (AE_OK);
    case 16: *Value = osl_inw ((uint16_t)Address); return (AE_OK);
    case 32: *Value = osl_inl ((uint16_t)Address); return (AE_OK);
    default: break;
    }

    klog (LOG_ERROR, "acpica", "bad port read width %u at 0x%llx",
          (uint64_t)Width, (uint64_t)Address);
    return (AE_BAD_PARAMETER);
}

ACPI_STATUS AcpiOsWritePort (ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    switch (Width) {
    case 8:  osl_outb ((uint16_t)Address, (uint8_t)Value);  return (AE_OK);
    case 16: osl_outw ((uint16_t)Address, (uint16_t)Value); return (AE_OK);
    case 32: osl_outl ((uint16_t)Address, Value);           return (AE_OK);
    default: break;
    }

    klog (LOG_ERROR, "acpica", "bad port write width %u at 0x%llx",
          (uint64_t)Width, (uint64_t)Address);
    return (AE_BAD_PARAMETER);
}

/* ---- Memory-space I/O ----------------------------------------------------- */

ACPI_STATUS AcpiOsReadMemory (ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value,
                              UINT32 Width)
{
    void *virt;

    if (!Value) {
        return (AE_BAD_PARAMETER);
    }

    virt = AcpiOsMapMemory (Address, Width / 8);
    if (!virt) {
        return (AE_ERROR);
    }

    switch (Width) {
    case 8:  *Value = *(volatile uint8_t  *)virt; break;
    case 16: *Value = *(volatile uint16_t *)virt; break;
    case 32: *Value = *(volatile uint32_t *)virt; break;
    case 64: *Value = *(volatile uint64_t *)virt; break;
    default:
        AcpiOsUnmapMemory (virt, Width / 8);
        return (AE_BAD_PARAMETER);
    }

    AcpiOsUnmapMemory (virt, Width / 8);
    return (AE_OK);
}

ACPI_STATUS AcpiOsWriteMemory (ACPI_PHYSICAL_ADDRESS Address, UINT64 Value,
                               UINT32 Width)
{
    void *virt = AcpiOsMapMemory (Address, Width / 8);

    if (!virt) {
        return (AE_ERROR);
    }

    switch (Width) {
    case 8:  *(volatile uint8_t  *)virt = (uint8_t)Value;  break;
    case 16: *(volatile uint16_t *)virt = (uint16_t)Value; break;
    case 32: *(volatile uint32_t *)virt = (uint32_t)Value; break;
    case 64: *(volatile uint64_t *)virt = Value;           break;
    default:
        AcpiOsUnmapMemory (virt, Width / 8);
        return (AE_BAD_PARAMETER);
    }

    AcpiOsUnmapMemory (virt, Width / 8);
    return (AE_OK);
}

/* ---- PCI configuration space ---------------------------------------------- */

ACPI_STATUS AcpiOsReadPciConfiguration (ACPI_PCI_ID *PciId, UINT32 Reg,
                                        UINT64 *Value, UINT32 Width)
{
    uint32_t raw;

    if (!PciId || !Value) {
        return (AE_BAD_PARAMETER);
    }
    if (Reg > 0xFF) {
        /* Extended (PCIe ECAM) config space is not reachable through the
         * 0xCF8/0xCFC window this kernel's pci_read32 uses. */
        return (AE_SUPPORT);
    }

    raw = pci_read32 ((uint8_t)PciId->Bus, (uint8_t)PciId->Device,
                      (uint8_t)PciId->Function, (uint8_t)(Reg & 0xFC));

    switch (Width) {
    case 8:  *Value = (raw >> ((Reg & 3) * 8)) & 0xFFu;    return (AE_OK);
    case 16: *Value = (raw >> ((Reg & 2) * 8)) & 0xFFFFu;  return (AE_OK);
    case 32: *Value = raw;                                 return (AE_OK);
    default: break;
    }

    return (AE_BAD_PARAMETER);
}

ACPI_STATUS AcpiOsWritePciConfiguration (ACPI_PCI_ID *PciId, UINT32 Reg,
                                         UINT64 Value, UINT32 Width)
{
    uint8_t bus, dev, func;

    if (!PciId) {
        return (AE_BAD_PARAMETER);
    }
    if (Reg > 0xFF) {
        return (AE_SUPPORT);
    }

    bus  = (uint8_t)PciId->Bus;
    dev  = (uint8_t)PciId->Device;
    func = (uint8_t)PciId->Function;

    /*
     * Native-width writes only. Composing 8- and 16-bit writes from a dword
     * read-modify-write is unsafe here: PCI Status at offset 0x06 holds
     * write-1-to-clear bits, so a 16-bit write to Command at 0x04 would read
     * Status, write the observed bits straight back, and silently acknowledge
     * error conditions the driver never saw. Any narrow write whose dword
     * also covers a W1C register carries that hazard.
     */
    switch (Width) {
    case 32:
        pci_write32 (bus, dev, func, (uint8_t)(Reg & 0xFC), (uint32_t)Value);
        return (AE_OK);
    case 16:
        if (Reg & 1u) {
            return (AE_BAD_PARAMETER);   /* unaligned 16-bit config access */
        }
        pci_write16 (bus, dev, func, (uint8_t)Reg, (uint16_t)Value);
        return (AE_OK);
    case 8:
        /* No pci_write8() exists in the PCI driver, and synthesising one
         * here would reintroduce exactly the read-modify-write hazard
         * above. Refused rather than silently corrupting a neighbour;
         * filed against the PCI driver to add a native byte write. */
        klog (LOG_WARN, "acpica",
              "8-bit PCI config write to %02x:%02x.%u reg 0x%x refused "
              "(no native byte write; RMW would clobber W1C neighbours)",
              (uint64_t)bus, (uint64_t)dev, (uint64_t)func, (uint64_t)Reg);
        return (AE_SUPPORT);
    default:
        break;
    }

    return (AE_BAD_PARAMETER);
}

/* ---- Output --------------------------------------------------------------- */

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf (const char *Format, ...)
{
    va_list args;

    va_start (args, Format);
    AcpiOsVprintf (Format, args);
    va_end (args);
}

void AcpiOsVprintf (const char *Format, va_list Args)
{
    char buf[512];

    if (!Format) {
        return;
    }

    vsnprintf (buf, sizeof (buf), Format, Args);
    printk ("%s", buf);
}

/* ---- Miscellaneous -------------------------------------------------------- */

/* ACPICA's timer contract is 100 ns units. */
UINT64 AcpiOsGetTimer (void)
{
    return (uptime_ns () / 100ull);
}

ACPI_STATUS AcpiOsSignal (UINT32 Function, void *Info)
{
    switch (Function) {
    case ACPI_SIGNAL_FATAL: {
        ACPI_SIGNAL_FATAL_INFO *fatal = (ACPI_SIGNAL_FATAL_INFO *)Info;
        if (fatal) {
            klog (LOG_ERROR, "acpica", "AML fatal: type %u code %u arg %u",
                  (uint64_t)fatal->Type, (uint64_t)fatal->Code,
                  (uint64_t)fatal->Argument);
        } else {
            klog (LOG_ERROR, "acpica", "AML fatal signal with no info block");
        }
        return (AE_OK);
    }
    case ACPI_SIGNAL_BREAKPOINT:
        /* AML Breakpoint op outside the debugger: note and continue. */
        klog (LOG_DEBUG, "acpica", "AML breakpoint: %s",
              Info ? (const char *)Info : "(no message)");
        return (AE_OK);
    default:
        return (AE_BAD_PARAMETER);
    }
}

ACPI_STATUS AcpiOsEnterSleep (UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue)
{
    (void)(RegaValue);
    (void)(RegbValue);

    klog (LOG_INFO, "acpica", "entering S%u", (uint64_t)SleepState);
    return (AE_OK);
}

/* ---- ACPI 6.5 section 5.2.10.1 global lock -------------------------------- */

/*
 * Thin adapters over the always-compiled implementation in
 * acpi_global_lock.c. The algorithm lives there, not here, so the unit suite
 * can cover it in the default build where ACPICA is not linked at all.
 */
int AcpiOsImpossibleAcquireGlobalLock (volatile unsigned int *Lock)
{
    return (acpi_global_lock_acquire ((volatile uint32_t *)Lock));
}

int AcpiOsImpossibleReleaseGlobalLock (volatile unsigned int *Lock)
{
    return (acpi_global_lock_release ((volatile uint32_t *)Lock));
}
