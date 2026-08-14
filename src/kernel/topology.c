/* ============================================================================
 * topology.c -- CPU topology detection (Zen chiplets + Intel hybrid)
 *
 * Parses CPUID topology leaves to build a per-CPU topology map.
 * Called once from boot Phase 2 after SMP bringup.
 *
 * AMD Zen path: CPUID leaf 0x8000001E (requires CPU_FEATURE_TOPO_EXT)
 * Intel hybrid path: CPUID leaf 0x1A (core type) + 0x1F (extended topology)
 * Fallback: all cores treated as CORE_TYPE_GENERIC
 *
 * XREF: 02-kernel-core/TODO-09-x86-64-architecture.md S9
 * ============================================================================ */

#include "kernel/topology.h"
#include "kernel/cpuid.h"
#include "kernel/smp.h"
#include "kernel/klog.h"

/* Global topology state */
cpu_topo_t g_cpu_topo[MAX_CPUS];
uint32_t   g_topo_cpu_count;
uint64_t   p_core_mask;
uint64_t   e_core_mask;
uint32_t   g_numa_nodes;

/* ---- AMD Zen topology via CPUID 0x8000001E ---- */

static void topology_parse_zen(void)
{
    extern struct cpu_features g_cpu;
    uint32_t max_node = 0;
    uint32_t i;

    /* BSP Zen topology from cpuid_init: compute_unit_id is the physical
     * core within the CCD (not the CCD itself). The CCD is encoded in
     * the extended APIC ID bits, which requires per-AP CPUID queries
     * (-> XREF: TODO-04 S4). Until then, only BSP node_id is reliable;
     * APs get node_id=0 and ccd_id=0 (unknown). */
    for (i = 0; i < g_topo_cpu_count; i++) {
        struct per_cpu_data *cpu = smp_get_cpu(i);
        /* Membership through the mask accessor, not a plain read of is_online:
         * the field is asynchronously mutable (a CPU parks on an async fault)
         * and this walked it unsynchronised (TODO-10 S21). */
        if (cpu && !smp_cpu_is_online(i) && i > 0)
            continue;  /* skip offline AP slots */
        if (i == 0) {
            /* BSP: compute_unit_id = core within CCD */
            g_cpu_topo[i].core_id = g_cpu.compute_unit_id;
            g_cpu_topo[i].node_id = g_cpu.node_id;
        } else if (cpu) {
            g_cpu_topo[i].core_id = cpu->lapic_id;
            g_cpu_topo[i].node_id = 0;  /* unknown without per-AP CPUID */
        }
        /* CCD is not available from BSP-only data; leave 0 (unknown) */
        g_cpu_topo[i].ccd_id       = 0;
        g_cpu_topo[i].core_type    = CORE_TYPE_GENERIC;
        g_cpu_topo[i].smt_siblings = g_cpu.threads_per_core ? g_cpu.threads_per_core : 1;

        if (g_cpu_topo[i].node_id > max_node)
            max_node = g_cpu_topo[i].node_id;
    }

    g_numa_nodes = max_node + 1;

    klog(LOG_INFO, "topo",
         "Zen: %u NUMA node(s), %u CPU(s) (CCD detection deferred to per-AP CPUID)",
         (uint64_t)g_numa_nodes, (uint64_t)g_topo_cpu_count);
}

/* ---- Intel hybrid topology via CPUID 0x1A + 0x1F ---- */

static void topology_parse_intel_hybrid(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint32_t i;

    /* CPUID 0x1A: Native Model ID (core type detection).
     * EAX[31:24] = core type: 0x40 = Intel Core (P), 0x20 = Intel Atom (E).
     * This is a per-CPU leaf: each core returns its own type. Without
     * per-AP CPUID (-> XREF: TODO-04 S4), we can only read BSP type.
     * Do NOT assign BSP type to all CPUs; that publishes false P/E masks
     * on real hybrid hardware. Leave all cores GENERIC and masks zero
     * until per-AP CPUID is available. Log the BSP type for diagnostics. */
    cpuid_raw(0x1A, 0, &eax, &ebx, &ecx, &edx);
    {
        uint8_t bsp_core_type = (uint8_t)((eax >> 24) & 0xFF);
        const char *type_name = "unknown";
        if (bsp_core_type == CORE_TYPE_P) type_name = "Performance";
        else if (bsp_core_type == CORE_TYPE_E) type_name = "Efficiency";
        else type_name = "Core";

        /* All CPUs stay GENERIC until per-AP CPUID classifies each one */
        for (i = 0; i < g_topo_cpu_count; i++)
            g_cpu_topo[i].core_type = CORE_TYPE_GENERIC;

        klog(LOG_INFO, "topo",
             "Intel hybrid detected: BSP is %s (0x%02x); "
             "P/E mask deferred to per-AP CPUID",
             type_name, (uint64_t)bsp_core_type);
    }

    /* CPUID 0x1F: V2 Extended Topology Enumeration.
     * Walk subleaves to find SMT and Core levels.
     * ECX[15:8] = level type: 1=SMT, 2=Core, 3=Module, 4=Tile, 5=Die.
     * EAX[4:0] = shift count for this level.
     * EBX[15:0] = number of logical processors at this level.
     *
     * To extract core_id: bits between SMT shift and Core shift.
     * core_id = (apic_id >> smt_shift) & ((1 << (core_shift - smt_shift)) - 1)
     */
    if (cpuid_get()->max_leaf >= 0x1F) {
        uint32_t subleaf;
        uint32_t smt_shift = 0, core_shift = 0;

        for (subleaf = 0; subleaf < 8; subleaf++) {
            cpuid_raw(0x1F, subleaf, &eax, &ebx, &ecx, &edx);
            uint32_t level_type = (ecx >> 8) & 0xFF;
            uint32_t shift      = eax & 0x1F;
            uint32_t lp_count   = ebx & 0xFFFF;

            if (level_type == 0)
                break;

            if (level_type == 1) {
                /* SMT level */
                smt_shift = shift;
                if (lp_count > 0 && lp_count <= 255) {
                    for (i = 0; i < g_topo_cpu_count; i++)
                        g_cpu_topo[i].smt_siblings = (uint8_t)lp_count;
                }
            }
            if (level_type == 2) {
                /* Core level */
                core_shift = shift;
            }
        }

        /* Extract core_id from bits between SMT and Core shifts */
        if (core_shift > smt_shift) {
            uint32_t core_mask = (1u << (core_shift - smt_shift)) - 1;
            for (i = 0; i < g_topo_cpu_count; i++) {
                struct per_cpu_data *cpu = smp_get_cpu(i);
                if (cpu)
                    g_cpu_topo[i].core_id = (cpu->lapic_id >> smt_shift) & core_mask;
            }
        }
    }

    g_numa_nodes = 1;  /* Intel non-NUMA; SRAT parsing deferred to D03 */

    klog(LOG_INFO, "topo",
         "Intel: %u CPU(s), hybrid P/E masks deferred",
         (uint64_t)g_topo_cpu_count);
}

/* ---- Fallback: all cores generic ---- */

static void topology_parse_generic(void)
{
    uint32_t i;
    for (i = 0; i < g_topo_cpu_count; i++) {
        struct per_cpu_data *cpu = smp_get_cpu(i);
        /* Same mask-accessor rule as topology_parse_zen (TODO-10 S21). */
        if (cpu && !smp_cpu_is_online(i) && i > 0)
            continue;
        if (cpu) {
            g_cpu_topo[i].core_id = cpu->lapic_id;
            g_cpu_topo[i].ccd_id  = 0;
            g_cpu_topo[i].node_id = 0;
        }
        g_cpu_topo[i].core_type    = CORE_TYPE_GENERIC;
        g_cpu_topo[i].smt_siblings = 1;
    }

    g_numa_nodes = 1;

    klog(LOG_INFO, "topo",
         "Generic: %u CPU(s), no topology extensions detected",
         (uint64_t)g_topo_cpu_count);
}

/* ---- Public API ---- */

void topology_init(void)
{
    /* SLOT BOUND, so the DISCOVERED count, not the live one (TODO-10 S21).
     * Logical CPU slots are sparse after a partial bringup, so bounding the
     * walk by the number of ONLINE CPUs skipped a live high slot entirely; the
     * per-slot online filter below is what excludes the CPUs that are down. */
    uint32_t cpu_count = smp_cpu_present_count();
    uint32_t i;

    if (cpu_count == 0)
        cpu_count = 1;
    if (cpu_count > MAX_CPUS)
        cpu_count = MAX_CPUS;

    g_topo_cpu_count = cpu_count;
    p_core_mask = 0;
    e_core_mask = 0;

    /* Initialize logical IDs */
    for (i = 0; i < cpu_count; i++) {
        g_cpu_topo[i].logical_id   = i;
        g_cpu_topo[i].core_id      = 0;
        g_cpu_topo[i].ccd_id       = 0;
        g_cpu_topo[i].node_id      = 0;
        g_cpu_topo[i].core_type    = CORE_TYPE_GENERIC;
        g_cpu_topo[i].smt_siblings = 1;
    }

    /* Detect topology based on CPU vendor and feature flags */
    if (cpu_has(CPU_FEATURE_TOPO_EXT)) {
        topology_parse_zen();
    } else {
        /* Check for Intel hybrid (CPUID.07H:EDX[15]) */
        uint32_t eax, ebx, ecx, edx;
        cpuid_raw(0x07, 0, &eax, &ebx, &ecx, &edx);
        if (edx & (1u << 15)) {
            topology_parse_intel_hybrid();
        } else {
            topology_parse_generic();
        }
    }

    klog(LOG_INFO, "topo",
         "CPU 0: logical=%u core=%u ccd=%u node=%u type=0x%02x smt=%u",
         (uint64_t)g_cpu_topo[0].logical_id,
         (uint64_t)g_cpu_topo[0].core_id,
         (uint64_t)g_cpu_topo[0].ccd_id,
         (uint64_t)g_cpu_topo[0].node_id,
         (uint64_t)g_cpu_topo[0].core_type,
         (uint64_t)g_cpu_topo[0].smt_siblings);
}
