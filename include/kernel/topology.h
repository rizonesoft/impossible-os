/* ============================================================================
 * topology.h -- CPU topology detection (Zen chiplets + Intel hybrid)
 *
 * Parses per-CPU topology: physical core, chiplet/cluster, NUMA node,
 * core type (Intel P-core vs E-core), and SMT sibling count.
 *
 * AMD Zen: CPUID leaf 0x8000001E (compute unit, node ID).
 * Intel hybrid: CPUID leaf 0x1A (native model ID, core type).
 * Intel extended topology: CPUID leaf 0x1F (SMT/core/module/die).
 *
 * XREF: 02-kernel-core/TODO-19-x86-64-architecture.md S9
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/acpi.h"    /* MAX_CPUS */

/* Core type constants (Intel SDM Vol. 2A, CPUID leaf 0x1A) */
#define CORE_TYPE_GENERIC  0x00
#define CORE_TYPE_P        0x40   /* Intel Performance core (Atom: 0x20) */
#define CORE_TYPE_E        0x20   /* Intel Efficiency core */

/* Per-CPU topology record */
typedef struct {
    uint32_t logical_id;    /* OS CPU number (index into cpu_data[]) */
    uint32_t core_id;       /* physical core within CCD/package */
    uint32_t ccd_id;        /* AMD: CCD (Compute Complex Die); Intel: module/cluster */
    uint32_t node_id;       /* NUMA domain */
    uint8_t  core_type;     /* CORE_TYPE_P, CORE_TYPE_E, or CORE_TYPE_GENERIC */
    uint8_t  smt_siblings;  /* threads per core (1 = no SMT) */
    uint8_t  _pad[2];
} cpu_topo_t;

/* Global topology data (populated by topology_init) */
extern cpu_topo_t g_cpu_topo[MAX_CPUS];
extern uint32_t   g_topo_cpu_count;
extern uint64_t   p_core_mask;    /* bitmask of P-core logical CPUs */
extern uint64_t   e_core_mask;    /* bitmask of E-core logical CPUs */
extern uint32_t   g_numa_nodes;   /* number of NUMA nodes (1 = UMA) */

/* Initialize topology. Call after cpuid_init() and smp_init(). */
void topology_init(void);

/* Query helpers */
static inline int topology_is_hybrid(void)
{
    return (p_core_mask != 0) && (e_core_mask != 0);
}
