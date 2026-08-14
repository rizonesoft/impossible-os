/* ============================================================================
 * cache.c -- Cache writeback primitives (x86-64)
 *
 * ARCH: x86-64 -- will move to arch/ when the ARM64 port lands. The API in
 * kernel/cache.h is deliberately architecture-neutral so arch-neutral callers
 * (klog.c) can persist a record without naming an instruction; only this file
 * knows about CLFLUSH, WBINVD and MFENCE.
 *
 * WHY EVERY CALL RE-PROBES CPUID. cpuid.h's cpu_has() reads a table built once
 * on the BSP by cpuid_init(), and cpu_configure_pat() is likewise BSP-ordered
 * from boot_hw.c. Both are the wrong shape for this module: its callers are
 * panic-path code that can run on an AP, before that table exists, or after
 * memory holding it has been corrupted by the very fault being reported. A
 * cached feature bit taken from another CPU can also mean executing an
 * instruction this CPU does not implement, and a #UD raised inside the panic
 * path destroys the evidence the call was made to save. CPUID is cheap
 * relative to a cache flush, is legal on every CPU at every point in boot, and
 * always describes the processor actually executing it -- so it is re-run per
 * call and no state is kept anywhere.
 *
 * WHY CLFLUSH AND NOT CLFLUSHOPT. CLFLUSHOPT is faster but is not ordered
 * against other CLFLUSHOPTs, so a batch needs its own fencing discipline, and
 * the fencing that discipline requires differs between vendors. CLFLUSH is
 * ordered against prior writes on Intel; AMD documents CLFLUSH as ordered by
 * MFENCE and explicitly NOT by SFENCE. Bracketing every batch with MFENCE is
 * correct on both, and the throughput CLFLUSHOPT would buy is worth nothing on
 * the only paths that call this -- a machine that is about to reset.
 * ============================================================================ */

#include "kernel/cache.h"
#include "kernel/cpuid.h"

/* CPUID leaves and fields this module reads. Leaf 1 is present on every CPU
 * that reports a basic-leaf maximum of at least 1, which is checked rather
 * than assumed: a leaf read past the maximum returns whatever the highest
 * implemented leaf returns, which would manufacture a plausible-looking
 * feature bit out of unrelated data. */
#define CPUID_LEAF_MAX_BASIC              0x00000000u
#define CPUID_LEAF_FEATURES               0x00000001u
/* CPUID.01H:EDX[19] -- CLFLUSH instruction supported. */
#define CPUID_01_EDX_CLFSH                (1u << 19)
/* CPUID.01H:EBX[15:8] -- CLFLUSH line size, in 8-byte units. */
#define CPUID_01_EBX_CLFLUSH_SIZE_SHIFT   8u
#define CPUID_01_EBX_CLFLUSH_SIZE_MASK    0xFFu
#define CPUID_CLFLUSH_SIZE_UNIT           8u

/* PURE decoder, split out from the CPUID read so all three rejections can be
 * driven from a unit test with synthetic register values. None of them is
 * reachable on this host -- the machine either enumerates CLFLUSH with a sane
 * line size or it does not -- and they guard the panic path against executing
 * an unsupported instruction or flushing on a stride that skips dirty lines,
 * which is exactly the code that must not be covered by assertion only.
 * Takes the raw leaf-0 EAX and leaf-1 EBX/EDX; returns the line size or 0. */
uint32_t cache_decode_line_size(uint32_t max_basic_leaf, uint32_t leaf1_ebx,
                                uint32_t leaf1_edx)
{
    uint32_t line;

    if (max_basic_leaf < CPUID_LEAF_FEATURES)
        return 0u;
    if ((leaf1_edx & CPUID_01_EDX_CLFSH) == 0u)
        return 0u;

    line = ((leaf1_ebx >> CPUID_01_EBX_CLFLUSH_SIZE_SHIFT) &
            CPUID_01_EBX_CLFLUSH_SIZE_MASK) * CPUID_CLFLUSH_SIZE_UNIT;

    /* Believe the reported size only if it is a power of two inside the
     * plausible range. A stride that is too large skips lines and silently
     * leaves part of a record dirty; one that is not a power of two breaks the
     * align-down mask in the planner. Either way the honest answer is "no line
     * granularity", which routes the caller to a full writeback. */
    if (line < CACHE_LINE_MIN || line > CACHE_LINE_MAX)
        return 0u;
    if ((line & (line - 1u)) != 0u)
        return 0u;

    return line;
}

uint32_t cache_line_size(void)
{
    uint32_t eax = 0u, ebx = 0u, ecx = 0u, edx = 0u;
    uint32_t max_basic;

    cpuid_raw(CPUID_LEAF_MAX_BASIC, 0u, &eax, &ebx, &ecx, &edx);
    max_basic = eax;

    /* Leaf 1 is read unconditionally and its registers handed to the pure
     * decoder, which is what decides whether they mean anything. A leaf read
     * past the maximum returns the highest implemented leaf's data, so the
     * max-leaf check has to gate the INTERPRETATION rather than the read --
     * otherwise unrelated register contents could pass for a feature bit. */
    cpuid_raw(CPUID_LEAF_FEATURES, 0u, &eax, &ebx, &ecx, &edx);

    return cache_decode_line_size(max_basic, ebx, edx);
}

/* PURE range planner. Returns the mode and, for CACHE_PLAN_RANGE, the aligned
 * first line address and the COUNT of lines -- a count rather than an end
 * address, because an end address forces the caller into a `p < end` loop
 * whose `p += line` can wrap past the top of the address space and then never
 * terminate. The kernel image window ends at UINT64_MAX, so that wrap is
 * reachable here rather than theoretical. */
int cache_plan_range(uintptr_t addr, uint64_t len, uint32_t line,
                     uintptr_t *out_first, uint64_t *out_lines)
{
    uintptr_t first, last;
    uint64_t  last_byte;

    *out_first = 0u;
    *out_lines = 0u;

    if (len == 0u)
        return CACHE_PLAN_NONE;

    /* No usable line granularity: a full writeback is a strict superset of the
     * requested range, so the durability guarantee still holds. Degrading to a
     * no-op instead would silently drop it on exactly the processors that
     * cannot make it any other way. */
    if (line == 0u)
        return CACHE_PLAN_ALL;

    /* Wrap is decided on the LAST BYTE, never on the exclusive end. A range
     * that stops exactly at the top of the address space has an exclusive end
     * of 0, so an end-based test reads a perfectly ordinary range as a wrap
     * and silently escalates it to a full writeback -- and the kernel image
     * window ends at UINT64_MAX, so that is a range this kernel can really
     * hand us. The last byte is unambiguous: it is below the start only when
     * the caller genuinely described memory that does not exist. */
    last_byte = (uint64_t)addr + (len - 1u);
    if (last_byte < (uint64_t)addr)
        return CACHE_PLAN_ALL;

    first = addr & ~(uintptr_t)(line - 1u);
    /* The LAST line's address. Derived from the last byte, so nothing here can
     * overflow: it is a real address inside the range. */
    last  = (uintptr_t)last_byte & ~(uintptr_t)(line - 1u);

    *out_first = first;
    *out_lines = ((uint64_t)(last - first) / line) + 1u;
    return CACHE_PLAN_RANGE;
}

void cache_writeback_all(void)
{
    /* Writes back and invalidates the whole hierarchy. Privileged; every
     * caller of this module runs in ring 0. */
    __asm__ volatile ("wbinvd" ::: "memory");
}

void cache_writeback_range(const void *addr, uint64_t len)
{
    /* Probed HERE, every call, on the CPU actually executing it. See the note
     * in kernel/cache.h on why there is no pre-probed variant. */
    uint32_t  line = cache_line_size();
    uintptr_t p;
    uint64_t  lines;

    switch (cache_plan_range((uintptr_t)addr, len, line, &p, &lines)) {
    case CACHE_PLAN_NONE:
        return;
    case CACHE_PLAN_ALL:
        cache_writeback_all();
        return;
    default:
        break;
    }

    /* MFENCE before, so every store the caller made to this range has retired
     * and the flush cannot write back a line that is about to be dirtied
     * again; MFENCE after, so later work cannot be reordered ahead of the
     * flush. Intel documents CLFLUSH as ordered against prior writes and AMD
     * documents it as ordered by MFENCE and explicitly not by SFENCE, so the
     * fences are what make this correct on both vendors rather than an
     * Intel-only accident.
     *
     * Counted rather than bounded by an end address: on the last iteration
     * `p += line` may wrap to 0 at the top of the address space, and a
     * `p < end` loop would then restart from 0 and never terminate. */
    __asm__ volatile ("mfence" ::: "memory");
    for (uint64_t i = 0u; i < lines; i++, p += line)
        __asm__ volatile ("clflush (%0)" :: "r"(p) : "memory");
    __asm__ volatile ("mfence" ::: "memory");
}
