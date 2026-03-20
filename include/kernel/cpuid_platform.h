/* ============================================================================
 * cpuid_platform.h — CPUID-based Platform Detection
 *
 * Detects whether the OS is running on bare metal or a hypervisor, and if so,
 * which hypervisor.  Used by the Unified Timer Subsystem (§6) to select the
 * right timer backend and calibration strategy.
 *
 * Detection method:
 *   1. Check CPUID.01H:ECX bit 31 (hypervisor present bit)
 *   2. If set: read CPUID leaf 0x40000000 for 12-byte vendor string
 *   3. Match against known hypervisor signatures
 *   4. If no hypervisor bit: PLATFORM_BARE_METAL
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Platform identification ---- */

typedef enum {
    PLATFORM_BARE_METAL,   /* no hypervisor detected */
    PLATFORM_HYPERV,       /* Microsoft Hv (Gen 1 or Gen 2) */
    PLATFORM_VMWARE,       /* VMwareVMware */
    PLATFORM_VIRTUALBOX,   /* VBoxVBoxVBox */
    PLATFORM_QEMU_KVM,     /* KVMKVMKVM — KVM with HW virt */
    PLATFORM_QEMU_TCG,     /* TCGTCGTCGTCG — software emulation */
    PLATFORM_UNKNOWN_HV,   /* hypervisor bit set, unknown vendor */
} platform_id_t;

/* ---- Public API ---- */

/* Detect the platform via CPUID.  Called once during early boot.
 * Result is cached — subsequent calls return the cached value. */
platform_id_t platform_detect(void);

/* Return the cached platform ID (must call platform_detect() first). */
platform_id_t platform_get(void);

/* Return a human-readable platform name (e.g., "Hyper-V", "QEMU/KVM"). */
const char *platform_name(void);

/* Return true if running under QEMU TCG (software emulation).
 * TCG has inaccurate LAPIC timing — PIT should be preferred. */
int platform_is_tcg(void);

/* Return true if the hypervisor provides an APIC frequency MSR or CPUID leaf.
 * True for: Hyper-V (MSR 0x40000023), VMware (CPUID 0x40000010),
 *           KVM (CPUID 0x40000010), bare metal with CPUID 0x15. */
int platform_has_apic_freq_msr(void);
