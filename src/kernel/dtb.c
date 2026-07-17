/* ============================================================================
 * dtb.c -- Flattened Devicetree (FDT) header validation + minimal walker
 *
 * Validates the DTB handoff (UEFI config-table linaro,dtb GUID on EBBR
 * systems) and counts the top-level discovery nodes the firmware-
 * platform arbitration policy needs in order to call a DTB handoff
 * "real": /chosen, /memory or /memory@*, and /cpus/cpu@*. Per
 * Devicetree Specification v0.4 5.4 (FDT Block Layout) the structure
 * block is a stream of big-endian uint32 tokens (FDT_BEGIN_NODE,
 * FDT_PROP, FDT_END_NODE, FDT_NOP, FDT_END) whose payloads are 4-byte
 * aligned within the block.
 *
 * SMP: BSP-only init at Phase 1; module is read-only after init runs.
 * ============================================================================ */

#include "kernel/dtb.h"
#include "kernel/firmware_tables.h"
#include "kernel/klog.h"
#include "libc/string.h"

/* Devicetree Specification v0.4 5.4.1 structure-block tokens. */
#define FDT_BEGIN_NODE  0x00000001u
#define FDT_END_NODE    0x00000002u
#define FDT_PROP        0x00000003u
#define FDT_NOP         0x00000004u
#define FDT_END         0x00000009u

/* Devicetree Specification v0.4 5.2 FDT header (big-endian on wire). */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

_Static_assert(sizeof(struct fdt_header) == 40,
               "FDT header wire-format must be 40 bytes");

static uint8_t  s_dtb_valid;
static uint8_t  s_dtb_initialised;
static uint32_t s_dtb_total_size;
static uint32_t s_dtb_chosen;
static uint32_t s_dtb_memory;
static uint32_t s_dtb_cpu;

/* Read a 4-byte big-endian unsigned integer at an aligned source. */
static uint32_t fdt_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* Match leading literal `lit` against the NUL-terminated name string at
 * `p`. Returns 1 if the entire `lit` matches, otherwise 0. */
static int name_starts_with(const char *p, const char *lit)
{
    while (*lit) {
        if (*p++ != *lit++) return 0;
    }
    return 1;
}

/* Returns 1 iff `name` matches the unit-address-aware form of `lit`:
 * either `lit` exactly, or `lit@<unit-address>`. Used so "memory" and
 * "memory@40000000" both match without "memorytest" sneaking through
 * the prefix check. */
static int name_eq_or_at(const char *name, const char *lit)
{
    if (!name_starts_with(name, lit)) return 0;
    while (*lit) { lit++; name++; }
    return *name == '\0' || *name == '@';
}

/* Walker-local discovery counters. Accumulate during the walk and
 * commit to the file-scope statics only on a successful walk -- a
 * failed walk leaves the public getters at 0, honouring the header
 * contract that all counts are 0 when dtb_is_valid() is false. */
struct dtb_walk_counts {
    uint32_t chosen;
    uint32_t memory;
    uint32_t cpu;
};

/* Path-aware DTB node categorisation per the Devicetree Specification
 * v0.4 2.2.1 unit-address convention plus the canonical bindings:
 *   /chosen           -- direct child of root (depth 1)
 *   /memory[@unit]    -- direct child of root (depth 1)
 *   /cpus/cpu@*       -- grandchild of root (depth 2), parent name "cpus"
 *
 * `node_depth` is the depth of the node BEING ENTERED (root = 0,
 * direct child of root = 1, grandchild = 2). `parent_name` points at
 * the parent node's NUL-terminated name string, or NULL when entering
 * the root. Counting only inside these canonical paths prevents a
 * malformed blob with bogus wrappers (e.g. /bogus/memory@0 +
 * /bogus/cpu@0) from satisfying the HasDTB inventory gate. */
static void categorise_name(const char *name, uint32_t node_depth,
                            const char *parent_name,
                            struct dtb_walk_counts *counts)
{
    if (node_depth == 1) {
        /* Direct children of root: chosen + memory[@unit]. */
        if (name_eq_or_at(name, "chosen")) {
            counts->chosen++;
            return;
        }
        if (name_eq_or_at(name, "memory")) {
            counts->memory++;
            return;
        }
        return;
    }
    if (node_depth == 2 && parent_name && name_eq_or_at(parent_name, "cpus")) {
        /* Grandchildren of root under /cpus: count cpu@*. */
        if (name_starts_with(name, "cpu@"))
            counts->cpu++;
        return;
    }
}

/* Walk the DTB structure block, counting categorised nodes. Returns 0
 * on malformed token stream (caller leaves s_dtb_valid = 0). The walk
 * is bounded by the declared totalsize; every advance is verified
 * against the structure-block end pointer before dereferencing.
 *
 * Tracks BEGIN_NODE / END_NODE depth so unbalanced streams are
 * rejected: a hostile blob with BEGIN BEGIN BEGIN FDT_END would
 * otherwise satisfy categorise_name on bogus children before falling
 * through to FDT_END as success. Devicetree Specification v0.4 5.4.1
 * requires every BEGIN_NODE to be matched by an END_NODE before the
 * FDT_END token, and FDT_END is only valid when depth has returned
 * to zero (i.e. the root node has been closed). */
/* Maximum nesting depth tracked for parent-name attribution. Real DTBs
 * are well below this; deeper trees still walk correctly but parent
 * tracking saturates and child categorisation falls back to "no parent
 * known" (counters stay zero for the affected subtree). */
#define DTB_PATH_MAX 16

static int walk_structure_block(const uint8_t *block, const uint8_t *end,
                                struct dtb_walk_counts *counts)
{
    const uint8_t *p = block;
    int depth = 0;
    int saw_root = 0;
    int root_closed = 0;
    /* path_names[i] points at the NUL-terminated name of the node at
     * depth i+1 (root sits at index 0). Stored as bytes from the
     * firmware blob, which is read-only for this walk. */
    const char *path_names[DTB_PATH_MAX] = { (const char *)0 };

    /* Devicetree Specification v0.4 5.1: tokens and their inline
     * payloads are 4-byte aligned within the structure block. The walk
     * advances by aligned 4-byte steps, so a misaligned `block` start
     * is rejected up front. */
    if (((uintptr_t)block & 0x3u) != 0) return 0;

    while (p + 4 <= end) {
        uint32_t tok = fdt_be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            /* Followed by NUL-terminated name; padded to 4-byte align. */
            const uint8_t *name_start = p;
            while (p < end && *p != 0) p++;
            if (p >= end) return 0;
            /* Devicetree Specification v0.4 5.4.1: exactly one root
             * node in a well-formed structure block. Reject any
             * second BEGIN_NODE that lands at depth 0 after the root
             * has already been opened, AND require the root name to
             * be empty (canonical "/"). Multi-root or named-root
             * streams are not legal DTB and must not feed the
             * inventory counters. */
            if (depth == 0) {
                if (root_closed || saw_root) return 0;
                if (*name_start != 0) return 0;  /* root must be "" */
            }
            /* Categorise BEFORE depth++, so node_depth here equals the
             * depth of the node being entered (root = 0, root's child
             * = 1, ...).  parent_name is the name at depth (node_depth
             * - 1) -- i.e., path_names[node_depth - 1]; NULL when
             * entering the root. */
            uint32_t node_depth = (uint32_t)depth;
            const char *parent_name = (const char *)0;
            if (node_depth >= 1 && node_depth <= DTB_PATH_MAX)
                parent_name = path_names[node_depth - 1];
            categorise_name((const char *)name_start, node_depth,
                            parent_name, counts);
            /* Record this node's name at its own depth slot so its
             * children can use it as parent_name. */
            if (node_depth < DTB_PATH_MAX)
                path_names[node_depth] = (const char *)name_start;
            p++;  /* skip NUL */
            uintptr_t off = (uintptr_t)(p - block);
            uintptr_t pad = (4u - (off & 0x3u)) & 0x3u;
            if (p + pad > end) return 0;
            p += pad;
            depth++;
            saw_root = 1;
            break;
        }
        case FDT_END_NODE:
            /* Cannot end a node that hasn't begun. */
            if (depth == 0) return 0;
            depth--;
            /* When depth returns to 0 we have just closed the root.
             * Latch root_closed so any subsequent BEGIN_NODE at depth
             * 0 is rejected as a malformed second root. */
            if (depth == 0 && saw_root) root_closed = 1;
            break;
        case FDT_NOP:
            break;
        case FDT_PROP: {
            /* Devicetree Specification 5.4.1: len(u32 BE) + nameoff(u32
             * BE) + len bytes, padded to 4-byte align. Properties only
             * appear inside an open node. */
            if (depth == 0) return 0;
            if (p + 8 > end) return 0;
            uint32_t plen = fdt_be32(p);
            p += 8;  /* skip len + nameoff */
            if (plen > FDT_TOTALSIZE_MAX) return 0;
            if (p + plen > end) return 0;
            p += plen;
            uintptr_t off = (uintptr_t)(p - block);
            uintptr_t pad = (4u - (off & 0x3u)) & 0x3u;
            if (p + pad > end) return 0;
            p += pad;
            break;
        }
        case FDT_END:
            /* Spec: FDT_END terminates the structure block. The root
             * node and all its descendants must already be closed. */
            if (depth != 0) return 0;
            if (!saw_root) return 0;
            return 1;
        default:
            /* Unknown token -- malformed stream. */
            return 0;
        }
    }
    /* Walked off the end without seeing FDT_END. */
    return 0;
}

void dtb_init(uintptr_t phys_addr)
{
    if (s_dtb_initialised) return;
    s_dtb_initialised = 1;
    s_dtb_valid = 0;
    s_dtb_total_size = 0;
    s_dtb_chosen = 0;
    s_dtb_memory = 0;
    s_dtb_cpu = 0;

    if (phys_addr == 0) return;

    /* Header-span gate: read 40 bytes safely. The unified firmware-table
     * validator only base-byte-checks DTB cfg-table entries (full FDT
     * shape validation is owned here). Refuse to dereference the
     * remaining 39 header bytes unless the entire 40-byte header lives
     * in a firmware-bearing memory-map descriptor; otherwise a
     * malformed cfg-table pointer that just happens to land on a mapped
     * page boundary could fault inside this function. The KERNEL_TESTS
     * test fixtures live in kernel BSS, which the firmware mmap does
     * not contain; production callers go through firmware_table_mmap_contains.
     * The test path uses dtb_reset_for_test + skips this gate via the
     * firmware-tables KERNEL_TESTS bypass that mmap_contains already
     * honours. */
    if (!firmware_table_mmap_contains(phys_addr, sizeof(struct fdt_header))) {
        klog(LOG_WARN, "DTB",
             "header span [0x%lx, +%u) outside firmware mmap -- skipping",
             (uint64_t)phys_addr, (uint64_t)sizeof(struct fdt_header));
        return;
    }

    const struct fdt_header *raw = (const struct fdt_header *)phys_addr;
    uint32_t magic = fdt_be32((const uint8_t *)&raw->magic);
    if (magic != FDT_MAGIC) {
        klog(LOG_WARN, "DTB", "magic 0x%x != 0x%x at 0x%lx -- skipping",
             (uint64_t)magic, (uint64_t)FDT_MAGIC, (uint64_t)phys_addr);
        return;
    }

    /* Pull the BE header fields once, into a local in CPU byte order. */
    struct fdt_header h;
    h.magic             = magic;
    h.totalsize         = fdt_be32((const uint8_t *)&raw->totalsize);
    h.off_dt_struct     = fdt_be32((const uint8_t *)&raw->off_dt_struct);
    h.off_dt_strings    = fdt_be32((const uint8_t *)&raw->off_dt_strings);
    h.off_mem_rsvmap    = fdt_be32((const uint8_t *)&raw->off_mem_rsvmap);
    h.version           = fdt_be32((const uint8_t *)&raw->version);
    h.last_comp_version = fdt_be32((const uint8_t *)&raw->last_comp_version);
    h.size_dt_strings   = fdt_be32((const uint8_t *)&raw->size_dt_strings);
    h.size_dt_struct    = fdt_be32((const uint8_t *)&raw->size_dt_struct);

    /* Structural sanity. We require a v17-compatible blob so the
     * size_dt_struct field is present and the structure-block can be
     * bounded without trusting the FDT_END token alone. */
    if (h.totalsize < sizeof(struct fdt_header) ||
        h.totalsize > FDT_TOTALSIZE_MAX) {
        klog(LOG_WARN, "DTB", "totalsize %u out of range -- skipping",
             (uint64_t)h.totalsize);
        return;
    }
    if (h.last_comp_version > FDT_SUPPORTED_VERSION ||
        h.version           < FDT_FIRST_SUPPORTED_VERSION) {
        klog(LOG_WARN, "DTB", "version %u/last_comp %u unsupported -- skipping",
             (uint64_t)h.version, (uint64_t)h.last_comp_version);
        return;
    }
    if (h.off_dt_struct  >= h.totalsize ||
        h.size_dt_struct == 0 ||
        h.size_dt_struct >  h.totalsize ||
        h.off_dt_struct + h.size_dt_struct > h.totalsize) {
        klog(LOG_WARN, "DTB", "structure block out of bounds -- skipping");
        return;
    }

    /* Body-span gate: the structure-block walker only checks token-by-
     * token bounds against `end`; it cannot tell us whether the
     * underlying physical pages are firmware-bearing. Validate the
     * entire declared totalsize lives in a firmware-region descriptor
     * before walking. */
    if (!firmware_table_mmap_contains(phys_addr, h.totalsize)) {
        klog(LOG_WARN, "DTB",
             "totalsize span [0x%lx, +%u) outside firmware mmap -- skipping",
             (uint64_t)phys_addr, (uint64_t)h.totalsize);
        return;
    }

    const uint8_t *base  = (const uint8_t *)phys_addr;
    const uint8_t *block = base + h.off_dt_struct;
    const uint8_t *end   = block + h.size_dt_struct;

    /* Accumulate counts locally so a failed walk leaves the public
     * getters at 0 (header contract: counts are 0 when invalid). */
    struct dtb_walk_counts counts = { 0, 0, 0 };
    if (!walk_structure_block(block, end, &counts)) {
        klog(LOG_WARN, "DTB", "structure block walk failed -- skipping");
        return;
    }

    s_dtb_valid = 1;
    s_dtb_total_size = h.totalsize;
    s_dtb_chosen = counts.chosen;
    s_dtb_memory = counts.memory;
    s_dtb_cpu    = counts.cpu;
    klog(LOG_INFO, "DTB",
         "valid v%u (%u bytes): chosen=%u memory=%u cpu=%u",
         (uint64_t)h.version, (uint64_t)h.totalsize,
         (uint64_t)s_dtb_chosen, (uint64_t)s_dtb_memory,
         (uint64_t)s_dtb_cpu);
}

int      dtb_is_valid(void)     { return (int)s_dtb_valid; }
uint32_t dtb_total_size(void)   { return s_dtb_total_size; }
uint32_t dtb_chosen_count(void) { return s_dtb_chosen; }
uint32_t dtb_memory_count(void) { return s_dtb_memory; }
uint32_t dtb_cpu_count(void)    { return s_dtb_cpu; }

#ifdef KERNEL_TESTS
/* Test-only reset hook so unit tests can re-run dtb_init against
 * fixture buffers. Prototype lives in kernel/dtb.h under the same guard. */
void dtb_reset_for_test(void)
{
    s_dtb_valid = 0;
    s_dtb_initialised = 0;
    s_dtb_total_size = 0;
    s_dtb_chosen = 0;
    s_dtb_memory = 0;
    s_dtb_cpu = 0;
}
#endif
