/* ============================================================================
 * heap.c -- Kernel Heap Allocator
 *
 * First-fit free-list allocator with coalescing of adjacent free blocks.
 *
 * The heap uses contiguous physical frames from the PMM, accessed via
 * the boot-time identity mapping (phys == virt for the first 4 GiB).
 * This avoids conflicts with the 2 MiB huge pages in the page tables.
 *
 * Each block has a 48-byte combined header carrying the allocator fields
 * (size, next, is_free) plus hardening fields (cookie, req_size, pool tag,
 * front/back redzones). Free blocks are linked in memory order; on kfree()
 * adjacent free blocks are coalesced. kfree/krealloc validate the header
 * (O(1) block-start bitmap membership, cookie, redzones) and raise
 * BUGCHECK_IOS_HEAP_CORRUPTION on a wild free / smash / over-underflow.
 * ============================================================================ */

#include "kernel/mm/heap.h"
#include "kernel/mm/pmm.h"
#include "kernel/mm/vmm.h"
#include "kernel/klog.h"
#include "kernel/bugcheck.h"        /* KeBugCheckEx on heap-corruption detection */
#include "kernel/sched/spinlock.h"  /* irqsave heap lock for SMP-safe alloc/free */
#ifdef KERNEL_TESTS
#include "kernel/smp.h"                 /* smp_this_cpu() for per-CPU countdown */
#include "kernel/sched/irql.h"          /* KeGetCurrentIrql for thread-context gate */
#include "kernel/sched/task.h" /* task_current() for task-filter gate */
#endif
/* Heap size constants */
#define HEAP_INITIAL_PAGES  512      /* 512 pages = 2 MiB */
#define HEAP_PAGE_SIZE      4096

/* Block header -- sits before every allocation.
 *
 * One combined header carries BOTH the allocator control fields
 * (size/next/is_free, used by split/coalesce/kfree/krealloc) AND the
 * hardening fields (cookie/req_size/redzone_front). The hardening fields
 * are only meaningful while a block is allocated; free blocks need only
 * the allocator metadata, so split/coalesce do not touch them.
 *
 * The layout is exactly 48 bytes (a multiple of 16) so that the returned
 * user pointer (block + HEADER_SIZE) inherits the page-aligned heap base's
 * 16-byte alignment for every block (split preserves it: HEADER_SIZE and
 * all request sizes are 16-aligned). `redzone_front` is the LAST field so
 * it sits immediately before the user data and catches header underflow. */
struct block_header {
    uint64_t size;             /* data area size (NOT incl header); allocator-critical */
    struct block_header *next; /* next block in memory order; allocator-critical */
    uint64_t cookie;           /* HEAP_COOKIE_SECRET ^ (uintptr_t)header; 0 while free */
    uint64_t req_size;         /* caller-requested size; locates back redzone + wipe len */
    uint32_t tag;              /* pool tag (kmalloc_tagged); 0 = untagged */
    uint8_t  is_free;          /* 1 = free, 0 = allocated */
    uint8_t  _pad[3];          /* explicit pad to keep redzone_front 8-aligned */
    uint64_t redzone_front;    /* HEAP_REDZONE_FRONT; last field -> precedes user data */
};

#define HEADER_SIZE  sizeof(struct block_header)

_Static_assert(sizeof(struct block_header) == 48,
    "heap block_header must be 48 bytes (combined allocator + hardening header)");
_Static_assert(sizeof(struct block_header) % 16 == 0,
    "heap header size must be a multiple of 16 so kmalloc returns 16-aligned pointers");
_Static_assert(__builtin_offsetof(struct block_header, redzone_front) == 40,
    "redzone_front must be the trailing header field, immediately before user data");
_Static_assert(__builtin_offsetof(struct block_header, size) == 0,
    "allocator-critical: size must stay at offset 0 (block_split/coalesce assume it)");
_Static_assert(__builtin_offsetof(struct block_header, next) == 8,
    "allocator-critical: next must stay at offset 8 (free-list walk assumes it)");

/* Hardening sentinels. The cookie XORs a fixed secret with the block
 * address so a corrupted/forged/wild header is caught on kfree; the two
 * redzones bracket the user region to catch underflow (front) and overflow
 * (back). Fixed-magic secret is used because the heap is live in boot
 * Phase 0, before the CSPRNG (Phase 1) can supply per-boot entropy. */
#define HEAP_COOKIE_SECRET     0xDEADBEEFC0FFEE01ULL
#define HEAP_REDZONE_FRONT     0xFEFEFEFEFEFEFEFEULL  /* underflow sentinel */
#define HEAP_REDZONE_BACK      0xBDBDBDBDBDBDBDBDULL  /* overflow sentinel */
#define HEAP_REDZONE_BACK_SIZE 8u                     /* bytes after user data */
/* Sanity cap rejected before any size + overhead arithmetic, so the
 * (req + HEADER_SIZE + redzone) computation can never wrap a size_t. Far
 * above any real kernel allocation, far below SIZE_MAX. */
#define KMALLOC_MAX            (256u * 1024u * 1024u)

/* Zero every user region on allocation (Linux CONFIG_INIT_ON_ALLOC parity).
 * Compile-time knob: define to 0 to disable on perf-critical builds. */
#ifndef HEAP_INIT_ON_ALLOC
#define HEAP_INIT_ON_ALLOC 1
#endif

/* Scrub freed user data before returning a block to the pool (so freed
 * secrets do not linger). DEFAULT OFF: enabling it surfaces a pre-existing
 * use-after-free in the filesystem layer (a vnode/inode carrying i_size is
 * read after free; the scrub zeros it, so file size reads 0 and cmd.exe
 * load + mmap content break). The scrub is correct; the FS UAF must be
 * fixed first, then this flips on. */
#ifndef HEAP_ZERO_ON_FREE
#define HEAP_ZERO_ON_FREE 0
#endif

/* Minimum usable block size (avoid tiny fragments) */
#define MIN_BLOCK_SIZE  16

/* Head of the block list */
static struct block_header *heap_start_block;
static uint64_t total_heap_size;
static uint64_t used_bytes;

/* Block-start shadow bitmap: one bit per 16-byte-aligned offset in the
 * arena, set iff a real block header starts there. It gives O(1) exact-
 * membership for kfree/krealloc (an aligned pointer into the INTERIOR of a
 * live allocation maps to an offset with no bit set -> rejected as wild)
 * without the O(n) chain walk that would otherwise run with IRQs disabled.
 * Sized for the maximum heap (HEAP_INITIAL_PAGES); the fallback heap can
 * only be smaller. Touched only under s_heap_lock. */
#define HEAP_BITMAP_GRANULE  MIN_BLOCK_SIZE   /* block starts are 16-aligned */
#define HEAP_BITMAP_BITS     (HEAP_INITIAL_PAGES * HEAP_PAGE_SIZE / HEAP_BITMAP_GRANULE)
static uint64_t s_block_bitmap[HEAP_BITMAP_BITS / 64];

static inline uint64_t heap_bm_index(const struct block_header *b)
{
    return ((uintptr_t)b - (uintptr_t)heap_start_block) / HEAP_BITMAP_GRANULE;
}
static inline void heap_bm_set(const struct block_header *b)
{
    uint64_t i = heap_bm_index(b);
    if (i < HEAP_BITMAP_BITS)
        s_block_bitmap[i >> 6] |= (1ULL << (i & 63));
}
static inline void heap_bm_clear(const struct block_header *b)
{
    uint64_t i = heap_bm_index(b);
    if (i < HEAP_BITMAP_BITS)
        s_block_bitmap[i >> 6] &= ~(1ULL << (i & 63));
}
static inline int heap_bm_test(const struct block_header *b)
{
    uint64_t i = heap_bm_index(b);
    if (i >= HEAP_BITMAP_BITS)
        return 0;
    return (int)((s_block_bitmap[i >> 6] >> (i & 63)) & 1u);
}

/* SMP serialization: kmalloc/kfree/krealloc walk and mutate the shared
 * free list + used_bytes. The lock is irqsave because kmalloc is reachable
 * from interrupt context (e.g. the RTL8139 RX ISR). */
static spinlock_t s_heap_lock = SPINLOCK_INIT;

/* Set (under s_heap_lock) the instant a corrupt/forged/wild block is
 * detected, BEFORE the lock is released for the noreturn BugCheck. Once
 * set, kmalloc/kfree/krealloc refuse to walk or mutate the known-corrupt
 * list, so no other CPU (or a same-CPU interrupt after irqrestore) can
 * touch the heap in the window before the panic owner quiesces the system.
 * volatile + lock-published so the next lock acquirer observes it. */
static volatile int s_heap_poisoned;

/* Corruption classes, reported out of the lock so the BugCheck fires after
 * the lock is released (KeBugCheckEx is noreturn and must not deadlock on
 * the heap lock if the crashdump path ever allocates). */
enum heap_fault_class {
    HEAP_FAULT_NONE = 0,
    HEAP_FAULT_WILD,          /* ptr not owned by the heap */
    HEAP_FAULT_MISALIGNED,    /* ptr not 16-aligned (not a kmalloc result) */
    HEAP_FAULT_COOKIE,        /* header cookie mismatch (forged/smashed header) */
    HEAP_FAULT_REDZONE_FRONT, /* underflow into header */
    HEAP_FAULT_REDZONE_BACK,  /* overflow past user data */
    HEAP_FAULT_TAG,           /* kfree_tagged pool-tag mismatch */
};

struct heap_fault {
    enum heap_fault_class cls;
    uint64_t ptr;   /* offending user pointer */
    uint64_t got;   /* observed sentinel/cookie */
    uint64_t want;  /* expected sentinel/cookie */
};

/* Non-elidable zeroizer for the FREE-scrub path: the compiler must NOT
 * drop these stores even though the freed memory is never read again
 * (secret hygiene). Byte-at-a-time volatile is the portable barrier form;
 * the free-scrub is knob-gated (HEAP_ZERO_ON_FREE) and off by default, so
 * this is unused in the default build. */
static void __attribute__((unused)) heap_secure_zero(void *p, uint64_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    uint64_t i;
    for (i = 0; i < n; i++)
        b[i] = 0;
}

/* Word-wide scalar zeroizer for init-on-alloc / kmalloc_zeroed. The zero is
 * observable (the region is returned to the caller), so it need NOT be the
 * non-elidable form -- but it must stay SCALAR (no SIMD): kmalloc is
 * reachable from ISR context where the FPU/XMM state is not saved, so a
 * vector memset would corrupt user SIMD state. */
static void heap_fast_zero(void *p, uint64_t n)
{
    uint8_t *b = (uint8_t *)p;
    while (n >= 8) { *(uint64_t *)b = 0; b += 8; n -= 8; }
    while (n--) *b++ = 0;
}

/* Cookie for a given header address. */
static inline uint64_t heap_cookie_for(const struct block_header *h)
{
    return HEAP_COOKIE_SECRET ^ (uint64_t)(uintptr_t)h;
}

/* Address of the 8-byte back redzone for an allocated block: immediately
 * after the caller-requested region. */
static inline uint64_t *heap_redzone_back(struct block_header *block)
{
    uint8_t *user = (uint8_t *)block + HEADER_SIZE;
    return (uint64_t *)(user + block->req_size);
}

/* Classify a user pointer handed back to kfree/krealloc (lock held).
 * Returns 0 = clean (allocated, all sentinels intact; *out is the header),
 *         1 = block is already free (double-free / use-after-free),
 *        -1 = corruption (f filled, *out unset).
 * Ordered so that range + alignment + header-bounds are checked with NO
 * dereference of the candidate header, so a wild/non-heap pointer never
 * faults before classification. */
static int heap_classify(void *ptr, struct block_header **out,
                         struct heap_fault *f)
{
    struct block_header *block;

    /* Not part of the heap arena (e.g. a PMM pointer or a wild value). */
    if (!heap_owns(ptr)) {
        f->cls = HEAP_FAULT_WILD;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        return -1;
    }
    /* Every kmalloc result is 16-aligned; anything else is not ours. */
    if (((uintptr_t)ptr & 15u) != 0) {
        f->cls = HEAP_FAULT_MISALIGNED;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        return -1;
    }
    /* The header sits HEADER_SIZE below the user pointer; reject pointers
     * so close to the arena base that the header would precede it. */
    if ((uint8_t *)ptr < (uint8_t *)heap_start_block + HEADER_SIZE) {
        f->cls = HEAP_FAULT_WILD;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        return -1;
    }

    block = (struct block_header *)((uint8_t *)ptr - HEADER_SIZE);

    /* Exact-membership proof in O(1): the block-start bitmap has a bit set
     * iff a real header starts at this offset. An aligned pointer into the
     * INTERIOR of a live allocation derives a fake header whose offset has
     * no bit set, so a crafted payload cannot pose as a block start and get
     * the middle of a live block freed. (Replaces the prior O(n) chain walk
     * that ran with IRQs disabled under s_heap_lock.) */
    if (!heap_bm_test(block)) {
        f->cls = HEAP_FAULT_WILD;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        return -1;
    }

    /* is_free is self-validated against the cookie. Every genuinely free
     * block carries cookie == 0 (cleared on free + on split); an allocated
     * block carries a nonzero cookie. So a single-byte header smash that
     * flips is_free on a LIVE block (is_free sits below the front redzone)
     * is caught here -- it would have is_free == 1 but a nonzero cookie. */
    if (block->is_free) {
        if (block->cookie == 0)
            return 1;   /* genuine double-free / use-after-free (silent) */
        f->cls = HEAP_FAULT_COOKIE;   /* is_free set on an allocated block */
        f->ptr = (uint64_t)(uintptr_t)ptr;
        f->got = block->cookie;
        f->want = 0;
        return -1;
    }

    if (block->cookie != heap_cookie_for(block)) {
        f->cls = HEAP_FAULT_COOKIE;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        f->got = block->cookie;
        f->want = heap_cookie_for(block);
        return -1;
    }
    /* Bound the back-redzone offset to the block's data area before
     * dereferencing it. req_size is itself a (possibly corrupt) header
     * field, so the check must NOT use overflow-prone addition: a forged
     * req_size near UINT64_MAX would wrap req_size + 8 to a small value,
     * pass, and let heap_redzone_back() read out of bounds. Use a
     * subtraction that cannot wrap (guarded by the req_size > size case). */
    if (block->req_size > block->size ||
        block->size - block->req_size < HEAP_REDZONE_BACK_SIZE) {
        f->cls = HEAP_FAULT_REDZONE_BACK;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        f->got = block->req_size;
        f->want = block->size;
        return -1;
    }
    if (block->redzone_front != HEAP_REDZONE_FRONT) {
        f->cls = HEAP_FAULT_REDZONE_FRONT;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        f->got = block->redzone_front;
        f->want = HEAP_REDZONE_FRONT;
        return -1;
    }
    if (*heap_redzone_back(block) != HEAP_REDZONE_BACK) {
        f->cls = HEAP_FAULT_REDZONE_BACK;
        f->ptr = (uint64_t)(uintptr_t)ptr;
        f->got = *heap_redzone_back(block);
        f->want = HEAP_REDZONE_BACK;
        return -1;
    }

    *out = block;
    return 0;
}

/* --- Internal: split a block if it's large enough --- */
static void block_split(struct block_header *block, size_t size)
{
    if (block->size >= size + HEADER_SIZE + MIN_BLOCK_SIZE) {
        struct block_header *new_block;
        new_block = (struct block_header *)((uint8_t *)block + HEADER_SIZE + size);
        new_block->size = block->size - size - HEADER_SIZE;
        new_block->is_free = 1;
        new_block->cookie = 0;   /* free blocks carry cookie == 0 (free marker) */
        new_block->next = block->next;
        heap_bm_set(new_block);   /* a new block header starts here */

        block->size = size;
        block->next = new_block;
    }
}

/* --- Internal: coalesce adjacent free blocks (lock held) ---
 * Enforces the free-cookie invariant: a block is treated as free only when
 * is_free is set AND cookie == 0. A live neighbor whose is_free byte was
 * smashed to 1 (cookie still nonzero) is NOT absorbed -- it is reported as
 * corruption (fault filled, heap poisoned) so the caller BugChecks rather
 * than swallowing a live allocation into an overlapping free block. */
static void coalesce_free_blocks(struct heap_fault *f)
{
    struct block_header *curr = heap_start_block;

    while (curr && curr->next) {
        struct block_header *nxt = curr->next;

        if (nxt->is_free && nxt->cookie != 0) {
            f->cls  = HEAP_FAULT_COOKIE;
            f->ptr  = (uint64_t)(uintptr_t)((uint8_t *)nxt + HEADER_SIZE);
            f->got  = nxt->cookie;
            f->want = 0;
            s_heap_poisoned = 1;
            return;   /* stop walking a known-corrupt list */
        }

        if (curr->is_free && curr->cookie == 0 &&
            nxt->is_free && nxt->cookie == 0) {
            heap_bm_clear(nxt);   /* absorbed block header disappears */
            curr->size += HEADER_SIZE + nxt->size;
            curr->next = nxt->next;
        } else {
            curr = curr->next;
        }
    }
}

boot_result_t heap_init(void)
{
    uintptr_t heap_base;
    size_t i;
    int primary_ok;   /* the +1 guard frame is owned ONLY on this path */

    total_heap_size = 0;
    used_bytes = 0;

    /* Allocate the full heap (plus its guard frame) as ONE contiguous run.
     * The PMM bitmap search skips reserved windows (kernel image, user ELF
     * range at 0x800000), so this lands wherever 2 MiB + 4 KiB actually
     * fits. The historical frame-by-frame loop silently truncated the heap
     * at the first reserved frame: a kernel image growing toward 0x800000
     * shrank the heap to a fraction of HEAP_INITIAL_PAGES and boot died of
     * heap exhaustion much later (fork's user-stack kmalloc was the first
     * visible casualty). Since we're identity-mapped, phys == virt. */
    heap_base = pmm_alloc_contiguous(HEAP_INITIAL_PAGES + 1);
    if (heap_base != 0) {
        i = HEAP_INITIAL_PAGES;
        primary_ok = 1;   /* HEAP_INITIAL_PAGES+1 frames are all owned */
    } else {
        primary_ok = 0;
        /* Degraded fallback: grow frame-by-frame from the lowest free
         * frame and keep whatever contiguous run exists. */
        klog(LOG_WARN, "mm",
             "Heap: no contiguous %u KiB run -- falling back to partial heap",
             (uint64_t)((HEAP_INITIAL_PAGES + 1) * HEAP_PAGE_SIZE / 1024));
        heap_base = pmm_alloc_frame();
        if (heap_base == 0) {
            klog(LOG_ERROR, "mm", "Heap: out of physical memory");
            return BOOT_FATAL;
        }
        for (i = 1; i < HEAP_INITIAL_PAGES; i++) {
            uintptr_t frame = pmm_alloc_frame();
            if (frame == 0) {
                klog(LOG_ERROR, "mm",
                     "Heap: out of physical memory at page %u", (uint64_t)i);
                break;
            }
            /* Verify contiguity */
            if (frame != heap_base + i * HEAP_PAGE_SIZE) {
                /* Non-contiguous -- still usable but we stop here */
                pmm_free_frame(frame);
                break;
            }
        }
    }

    total_heap_size = i * HEAP_PAGE_SIZE;

    /* Zero the heap region */
    {
        uint8_t *p = (uint8_t *)heap_base;
        uint64_t j;
        for (j = 0; j < total_heap_size; j++)
            p[j] = 0;
    }

    /* Create the initial free block spanning the entire heap. The heap was
     * just zeroed, so cookie == 0 (the free-state marker) already holds. The
     * block-start bitmap is BSS-zeroed; set the bit for this first header. */
    heap_start_block = (struct block_header *)heap_base;
    heap_start_block->size = total_heap_size - HEADER_SIZE;
    heap_start_block->is_free = 1;
    heap_start_block->next = (struct block_header *)0;
    heap_bm_set(heap_start_block);

    /* Install guard page immediately after heap end. Ownership of the
     * guard frame is tracked by primary_ok, NOT by `i`: the fallback loop
     * can also reach i == HEAP_INITIAL_PAGES (if 512 frames happened to be
     * contiguous) WITHOUT having allocated the +1 frame, so keying off `i`
     * would clear a PTE for a frame PMM still considers free -- a later
     * pmm_alloc_frame() could then hand that not-present frame to another
     * subsystem (false guard-page panic / ownership corruption). On the
     * fallback path we explicitly claim the adjacent frame and only guard
     * it if PMM actually returned heap_end. */
    {
        uintptr_t heap_end = heap_base + total_heap_size;
        uintptr_t guard;

        if (primary_ok) {
            guard = heap_end;  /* +1 frame of the contiguous run, owned */
        } else {
            guard = pmm_alloc_frame();
            if (guard != heap_end) {
                if (guard) pmm_free_frame(guard);
                guard = 0;
            }
        }
        if (guard == heap_end) {
            if (vmm_install_guard_page(guard, "GUARD: kernel heap overflow") != 0) {
                /* Guard install failed (vmm_install_guard_page fails BEFORE
                 * clearing the PTE, so the frame is still present and still
                 * adjacent to the heap). Do NOT free it: returning it to PMM
                 * would let a later allocation reuse the heap-adjacent frame,
                 * so a one-page heap overrun would silently corrupt that new
                 * owner instead of landing in owned padding. Keep it as a
                 * silent sentinel and log the degraded guard state. */
                klog(LOG_WARN, "mm", "Kernel heap: %u KiB at %p (guard install failed -- frame retained as padding)",
                       (uint64_t)(total_heap_size / 1024), heap_base);
            } else {
                klog(LOG_INFO, "mm", "Kernel heap: %u KiB at %p (guard at %p)",
                       (uint64_t)(total_heap_size / 1024), heap_base, guard);
            }
        } else {
            klog(LOG_INFO, "mm", "Kernel heap: %u KiB at %p (no guard -- non-contiguous)",
                   (uint64_t)(total_heap_size / 1024), heap_base);
        }
    }

    return (heap_start_block && total_heap_size > 0) ? BOOT_OK : BOOT_FATAL;
}

#ifdef KERNEL_TESTS
/* Aggregate count across all CPUs of kmalloc calls that were forced to
 * return NULL by the fault-injection hook. Updated with __atomic ops so
 * tests on either CPU read a coherent value. */
static uint64_t s_kmalloc_fault_injections;

void kmalloc_fail_countdown_set(uint32_t n)
{
    /* smp_this_cpu() returns this CPU's per-CPU block; the counter is
     * per-CPU by construction so concurrent tests on different CPUs
     * never step on each other. */
    smp_this_cpu()->kmalloc_fail_countdown = n;
}

void kmalloc_fail_countdown_clear(void)
{
    smp_this_cpu()->kmalloc_fail_countdown = 0;
}

void kmalloc_fail_next(void)
{
    smp_this_cpu()->kmalloc_fail_countdown = 1;
}

uint64_t kmalloc_fail_injections_triggered(void)
{
    return __atomic_load_n(&s_kmalloc_fault_injections, __ATOMIC_RELAXED);
}

/* task-filter: when non-zero, countdown only decrements for the
 * task whose pid matches. Resets fired_counter so a new filter starts
 * with a clean cap budget. */
void kmalloc_fail_task_filter_set(uint32_t task_pid)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->kmalloc_fail_task_pid    = task_pid;
    pc->kmalloc_fail_fired_counter = 0;
}

void kmalloc_fail_task_filter_clear(void)
{
    smp_this_cpu()->kmalloc_fail_task_pid = 0;
}

/* max-injections cap: once fired_counter reaches this value, the
 * hook stops firing even when the countdown is armed. 0 disables the
 * cap (classic single-shot countdown). _set also zeros the fired
 * counter so the cap is relative to the arm-point. */
void kmalloc_fail_max_injections_set(uint32_t max)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->kmalloc_fail_max_injections = max;
    pc->kmalloc_fail_fired_counter  = 0;
}

void kmalloc_fail_max_injections_clear(void)
{
    struct per_cpu_data *pc = smp_this_cpu();
    pc->kmalloc_fail_max_injections = 0;
    pc->kmalloc_fail_fired_counter  = 0;
}

uint32_t kmalloc_fail_fired_counter(void)
{
    return smp_this_cpu()->kmalloc_fail_fired_counter;
}
#endif /* KERNEL_TESTS */

/* Core allocator -- assumes s_heap_lock is held. `req` is the caller's
 * exact requested size (already validated <= KMALLOC_MAX); `tag` is the
 * pool tag (0 = untagged). Reserves req user bytes + an 8-byte back
 * redzone, stamps cookie + both redzones, and (when init-on-alloc is on)
 * zeroes the user region. Returns the 16-aligned user pointer, or NULL on
 * OOM / poisoned heap. */
static void *kmalloc_locked(size_t req, uint32_t tag)
{
    struct block_header *curr;
    size_t need;

    if (s_heap_poisoned)
        return (void *)0;

    /* Data area must hold req user bytes + the back redzone. Rounded to 16
     * so the next split block stays 16-aligned. req <= KMALLOC_MAX so this
     * cannot wrap. */
    need = (req + HEAP_REDZONE_BACK_SIZE + 15) & ~((size_t)15);

    curr = heap_start_block;
    while (curr) {
        /* Only reuse a GENUINELY free block: a free block carries cookie==0.
         * A block whose is_free byte was smashed to 1 on a live allocation
         * still holds its allocated (nonzero) cookie, so it is skipped here
         * rather than reused into an overlapping allocation. */
        if (curr->is_free && curr->cookie == 0 && curr->size >= need) {
            uint8_t *user;

            block_split(curr, need);
            curr->is_free       = 0;
            curr->req_size      = req;
            curr->tag           = tag;
            curr->cookie        = heap_cookie_for(curr);
            curr->redzone_front = HEAP_REDZONE_FRONT;
            user = (uint8_t *)curr + HEADER_SIZE;
            *heap_redzone_back(curr) = HEAP_REDZONE_BACK;
            used_bytes += curr->size;
            /* Zeroing the user region is done by the caller AFTER releasing
             * s_heap_lock (the block is now privately owned) so the
             * IRQ-disabled lock is not held across an O(req) loop. */
            return (void *)user;
        }
        curr = curr->next;
    }

    /* Out of heap memory */
    return (void *)0;
}

#ifdef KERNEL_TESTS
/* Shared fault-injection decision for every allocator entry point. Returns
 * non-zero when this call should be forced to fail. Kept in ONE place so
 * kmalloc() and kmalloc_zeroed() cannot drift apart -- a caller of the
 * zeroing allocator must be just as testable on its failure path as a caller
 * of the plain one.
 *
 * Thread-context gate: the hook only consumes the countdown when we are at
 * PASSIVE_LEVEL. IRQ / DPC / spinlock-holding callers on the same CPU (e.g.
 * the RTL8139 RX ISR calling kmalloc for a work packet) would otherwise steal
 * the pending injection from the thread-context test that armed it -- turning
 * the test nondeterministic and giving false confidence. Tests that want to
 * inject into IRQ-context allocations need a different harness scoped to that
 * context; this hook is thread-context only.
 *
 * Three gates, applied in order (each skips the fire without touching the
 * countdown so subsequent qualifying calls can still fire):
 *   1. Task filter: kmalloc_fail_task_pid != 0 requires
 *      task_current()->pid match.
 *   2. Max-injections cap: if fired_counter has reached the cap,
 *      don't fire (even with countdown armed). Cap of 0 = no cap.
 *   3. Countdown check: --countdown == 0 triggers the fire. */
static int heap_fault_injection_fires(void)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return 0;

    /* Site mode first, and it returns WITHOUT touching the per-CPU
     * countdown: a named-site arm is a complete program, not a modifier on
     * the ordinal one. Arming a site clears the same allocator's ordinal
     * state (see sys_fault_inject_dispatch), so a fired site can never fall
     * through to a stale countdown and fail an unrelated branch. */
    if (fault_site_claim(FI_ALLOC_KMALLOC)) {
        __atomic_fetch_add(&s_kmalloc_fault_injections, 1ull,
                           __ATOMIC_RELAXED);
        return 1;
    }

    struct per_cpu_data *pc = smp_this_cpu();
    if (!pc || !pc->kmalloc_fail_countdown)
        return 0;

    if (pc->kmalloc_fail_task_pid != 0) {
        struct task *t = task_current();
        if (!t || t->pid != pc->kmalloc_fail_task_pid)
            return 0;
    }

    if (pc->kmalloc_fail_max_injections != 0 &&
        pc->kmalloc_fail_fired_counter >= pc->kmalloc_fail_max_injections) {
        return 0;
    }

    if (--pc->kmalloc_fail_countdown != 0)
        return 0;

    __atomic_fetch_add(&s_kmalloc_fault_injections, 1ull, __ATOMIC_RELAXED);
    pc->kmalloc_fail_fired_counter++;
    /* auto-reload: with a max-injections cap set and not yet reached, re-arm
     * countdown=1 so the next qualifying call fires too. Delivers the 'fail N
     * of the next M calls' multi-fire semantic from one arm. */
    if (pc->kmalloc_fail_max_injections != 0 &&
        pc->kmalloc_fail_fired_counter < pc->kmalloc_fail_max_injections) {
        pc->kmalloc_fail_countdown = 1;
    }
    return 1;
}
#endif

void *kmalloc(size_t size)
{
    void *result;
    uint64_t irq_flags;

    if (size == 0)
        return (void *)0;

#ifdef KERNEL_TESTS
    if (heap_fault_injection_fires())
        return (void *)0;
#endif

    /* Overflow guard: reject absurd sizes before any size + overhead
     * arithmetic so the kmalloc_locked rounding cannot wrap a size_t. */
    if (size > KMALLOC_MAX)
        return (void *)0;

    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    result = kmalloc_locked(size, 0);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);
#if HEAP_INIT_ON_ALLOC
    /* Zero the now-privately-owned region OUTSIDE the lock so the
     * IRQ-disabled critical section is not held across an O(size) loop. */
    if (result)
        heap_fast_zero(result, size);
#endif
    return result;
}

/* Like kmalloc but always zeroes the user region, independent of the
 * HEAP_INIT_ON_ALLOC build knob (for security-sensitive allocations such
 * as token / security-descriptor structs). */
void *kmalloc_zeroed(size_t size)
{
    void *result;
    uint64_t irq_flags;

#ifdef KERNEL_TESTS
    /* Same injection gate as kmalloc(), and in the SAME position relative to
     * the size checks: consuming the countdown here but not there (or vice
     * versa) would make an armed "fail the next allocation" behave differently
     * across the two entry points, which is exactly the drift extracting the
     * gate was meant to remove. Checked before the heap is touched, so a
     * forced failure leaves the free list exactly as a real OOM would. */
    if (heap_fault_injection_fires())
        return (void *)0;
#endif

    if (size == 0 || size > KMALLOC_MAX)
        return (void *)0;

    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    result = kmalloc_locked(size, 0);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);
    if (result)   /* zero outside the lock (block is privately owned) */
        heap_fast_zero(result, size);
    return result;
}

/* Tagged allocation: stamps a 4-byte ASCII pool tag into the header so
 * kfree_tagged can verify matched alloc/free pools (catches mixed-pool
 * use-after-free). Plain kfree ignores the tag. */
void *kmalloc_tagged(size_t size, uint32_t tag)
{
    void *result;
    uint64_t irq_flags;

    if (size == 0 || size > KMALLOC_MAX)
        return (void *)0;

    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    result = kmalloc_locked(size, tag);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);
#if HEAP_INIT_ON_ALLOC
    if (result)   /* zero outside the lock (block is privately owned) */
        heap_fast_zero(result, size);
#endif
    return result;
}

/* Validate + free a block (lock held). Sets fault->cls on corruption (the
 * caller raises the BugCheck after releasing the lock); a double-free is a
 * silent no-op (unchanged behavior). `check_tag` verifies the pool tag. */
static void kfree_locked(void *ptr, uint32_t want_tag, int check_tag,
                         struct heap_fault *fault)
{
    struct block_header *block = (void *)0;
    int r;

    if (s_heap_poisoned)
        return;   /* heap already known-corrupt; refuse to mutate */

    r = heap_classify(ptr, &block, fault);
    if (r == 1)
        return;   /* double-free protection (silent, as before) */
    if (r < 0) {
        s_heap_poisoned = 1;   /* published under the lock */
        return;
    }

    if (check_tag && block->tag != want_tag) {
        fault->cls = HEAP_FAULT_TAG;
        fault->ptr = (uint64_t)(uintptr_t)ptr;
        fault->got = block->tag;
        fault->want = want_tag;
        s_heap_poisoned = 1;
        return;
    }

#if HEAP_ZERO_ON_FREE
    /* Scrub the user payload so freed secrets do not linger in the pool.
     * Gated OFF by default pending the FS use-after-free fix (see the
     * HEAP_ZERO_ON_FREE knob comment). */
    heap_secure_zero((uint8_t *)block + HEADER_SIZE, block->req_size);
#endif
    block->cookie  = 0;
    block->is_free = 1;
    used_bytes -= block->size;
    /* Coalescing may detect a corrupt (is_free-smashed) neighbor and fill
     * `fault`; the caller raises the BugCheck after releasing the lock. */
    coalesce_free_blocks(fault);
}

/* Raise the heap-corruption BugCheck for a detected fault. Called only
 * AFTER s_heap_lock is released (KeBugCheckEx is noreturn; s_heap_poisoned
 * already blocks any concurrent mutation of the corrupt heap). */
static void heap_corruption_panic(const struct heap_fault *f)
{
    KeBugCheckEx(BUGCHECK_IOS_HEAP_CORRUPTION,
                 f->ptr, f->got, f->want, (uint64_t)f->cls);
}

void kfree(void *ptr)
{
    struct heap_fault fault = { HEAP_FAULT_NONE, 0, 0, 0 };
    uint64_t irq_flags;

    if (!ptr)
        return;

    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    kfree_locked(ptr, 0, 0, &fault);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);

    if (fault.cls != HEAP_FAULT_NONE)
        heap_corruption_panic(&fault);
}

/* Tag-checked free: verifies the block was allocated with the matching
 * pool tag (catches mixed-pool / wrong-type free). */
void kfree_tagged(void *ptr, uint32_t tag)
{
    struct heap_fault fault = { HEAP_FAULT_NONE, 0, 0, 0 };
    uint64_t irq_flags;

    if (!ptr)
        return;

    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    kfree_locked(ptr, tag, 1, &fault);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);

    if (fault.cls != HEAP_FAULT_NONE)
        heap_corruption_panic(&fault);
}

void *krealloc(void *ptr, size_t new_size)
{
    struct block_header *block = (void *)0;
    struct heap_fault fault = { HEAP_FAULT_NONE, 0, 0, 0 };
    void *new_ptr;
    void *ret;
    size_t copy_size;
    size_t need;
    uint64_t irq_flags;
    int r;

    if (!ptr)
        return kmalloc(new_size);

    if (new_size == 0) {
        kfree(ptr);
        return (void *)0;
    }

    if (new_size > KMALLOC_MAX)
        return (void *)0;

    /* Data-area requirement for the new size, including the back redzone. */
    need = (new_size + HEAP_REDZONE_BACK_SIZE + 15) & ~((size_t)15);

    spin_lock_irqsave(&s_heap_lock, &irq_flags);

    if (s_heap_poisoned) {
        spin_unlock_irqrestore(&s_heap_lock, irq_flags);
        return (void *)0;
    }

    /* A realloc of a corrupt or already-freed block is a use-after-free
     * bug -- classify and BugCheck rather than trust block->size/next. */
    r = heap_classify(ptr, &block, &fault);
    if (r != 0) {
        if (r == 1) {                       /* freed block -> UAF */
            fault.cls = HEAP_FAULT_COOKIE;
            fault.ptr = (uint64_t)(uintptr_t)ptr;
        }
        s_heap_poisoned = 1;
        spin_unlock_irqrestore(&s_heap_lock, irq_flags);
        heap_corruption_panic(&fault);
        return (void *)0;                   /* unreachable */
    }

    /* Already large enough: keep the block, move the back redzone to the
     * new user-data end and record the new request size. */
    if (block->size >= need) {
        size_t old_req = block->req_size;
        block->req_size = new_size;
        *heap_redzone_back(block) = HEAP_REDZONE_BACK;
        spin_unlock_irqrestore(&s_heap_lock, irq_flags);
#if HEAP_INIT_ON_ALLOC
        /* Zero the newly-exposed slack (init-on-alloc only covered the old
         * request) so a grow does not leak stale block contents. */
        if (new_size > old_req)
            heap_fast_zero((uint8_t *)ptr + old_req, new_size - old_req);
#endif
        return ptr;
    }

    /* Try to grow in place by merging with the next free block. */
    if (block->next && block->next->is_free) {
        /* Enforce the free-cookie invariant before absorbing the neighbor:
         * a live block whose is_free byte was smashed (cookie still nonzero)
         * must NOT be merged into this allocation. */
        if (block->next->cookie != 0) {
            fault.cls  = HEAP_FAULT_COOKIE;
            fault.ptr  = (uint64_t)(uintptr_t)((uint8_t *)block->next + HEADER_SIZE);
            fault.got  = block->next->cookie;
            fault.want = 0;
            s_heap_poisoned = 1;
            spin_unlock_irqrestore(&s_heap_lock, irq_flags);
            heap_corruption_panic(&fault);
            return (void *)0;   /* unreachable */
        }
        uint64_t combined = block->size + HEADER_SIZE + block->next->size;
        if (combined >= need) {
            size_t old_req = block->req_size;
            heap_bm_clear(block->next);   /* absorbed block header disappears */
            used_bytes -= block->size;
            block->size = combined;
            block->next = block->next->next;
            block_split(block, need);
            used_bytes += block->size;
            block->req_size = new_size;
            *heap_redzone_back(block) = HEAP_REDZONE_BACK;
            spin_unlock_irqrestore(&s_heap_lock, irq_flags);
#if HEAP_INIT_ON_ALLOC
            /* Zero from the old request end through the new size: the merged
             * region held freed-block bytes that must not leak to the caller. */
            if (new_size > old_req)
                heap_fast_zero((uint8_t *)ptr + old_req, new_size - old_req);
#endif
            return ptr;
        }
    }

    /* Allocate the new block (preserving the pool tag) under the lock, but
     * do the O(copy_size) copy + the old-block free OUTSIDE the lock so the
     * IRQ-disabled critical section never spans a large memcpy. The old
     * block stays allocated (caller-owned, single-owner) during the copy,
     * so it is stable; the new block is privately owned. */
    new_ptr = kmalloc_locked(new_size, block->tag);
    if (!new_ptr) {
        spin_unlock_irqrestore(&s_heap_lock, irq_flags);
        return (void *)0;   /* old block left intact */
    }
    copy_size = block->req_size < new_size ? block->req_size : new_size;
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);

#if HEAP_INIT_ON_ALLOC
    heap_fast_zero(new_ptr, new_size);   /* zero, then copy over the head */
#endif
    {
        uint8_t *src = (uint8_t *)ptr;
        uint8_t *dst = (uint8_t *)new_ptr;
        size_t i;
        for (i = 0; i < copy_size; i++)
            dst[i] = src[i];
    }

    /* Re-acquire the lock to free the old (still-valid, caller-owned)
     * block. If another CPU poisoned the heap meanwhile, kfree_locked
     * no-ops and the old block leaks -- harmless, the system is panicking. */
    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    kfree_locked(ptr, 0, 0, &fault);
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);
    if (fault.cls != HEAP_FAULT_NONE)
        heap_corruption_panic(&fault);

    ret = new_ptr;
    return ret;
}

uint64_t heap_get_total(void)
{
    return total_heap_size;
}

/* Returns 1 if `ptr` falls inside the kmalloc heap range, 0 otherwise.
 * Used by callers that may be holding a pointer that could be either
 * kmalloc-allocated or PMM-allocated (e.g., task_cleanup's stack_base
 * which is kmalloc'd by task_create_user but pmm_alloc_contiguous'd by
 * task_create + task_exec). Avoids passing PMM pointers to kfree, which
 * would dereference garbage as a block_header and corrupt the free list.
 *
 * Discovered 2026-04-20: the user-mode test launcher's task_cleanup hung
 * indefinitely on `kfree(stack_base)` because stack_base was a PMM
 * pointer from task_exec, not a kmalloc pointer. */
int heap_owns(const void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)heap_start_block;
    if (addr < base) return 0;
    if (addr >= base + total_heap_size) return 0;
    return 1;
}

uint64_t heap_get_used(void)
{
    return used_bytes;
}

uint64_t heap_get_free(void)
{
    struct block_header *curr;
    uint64_t free_bytes = 0;
    uint64_t irq_flags;

    /* Walks the free list, so it must hold the lock against a concurrent
     * split/coalesce mutating the `next` chain. */
    spin_lock_irqsave(&s_heap_lock, &irq_flags);
    /* Refuse to walk a known-corrupt free list (a `next` pointer may be
     * garbage in the unlock->BugCheck window); report 0 conservatively. */
    if (s_heap_poisoned) {
        spin_unlock_irqrestore(&s_heap_lock, irq_flags);
        return 0;
    }
    curr = heap_start_block;
    while (curr) {
        if (curr->is_free)
            free_bytes += curr->size;
        curr = curr->next;
    }
    spin_unlock_irqrestore(&s_heap_lock, irq_flags);

    return free_bytes;
}
