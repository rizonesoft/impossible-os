/* ============================================================================
 * uefi_runtime.c -- UEFI Runtime Services initialization and wrappers
 *
 * Calls SetVirtualAddressMap() to remap firmware runtime memory into the
 * kernel's virtual address space (identity-mapped: virt = phys), then
 * stores the runtime services function pointers for later use.
 *
 * All runtime service calls are serialized with a spinlock because
 * UEFI firmware runtime code is NOT reentrant.
 * ============================================================================ */

#include "kernel/uefi_runtime.h"
#include "kernel/boot_info.h"
#include "kernel/boot_init.h"
#include "kernel/uefi_config.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/drivers/lapic.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"
#include "kernel/system_state.h"
#include "kernel/fs/gpt.h"
#include "registry.h"

/* EFI_RUNTIME_SERVICES table signature: "RUNTSERV" (UEFI Spec §4.5) */
#define EFI_RUNTIME_SERVICES_SIGNATURE 0x56524553544E5552ULL

/* ---- Kernel-side EFI_RUNTIME_SERVICES struct ----
 * Re-declared with kernel types + UEFI_EFIAPI calling convention.
 * Layout must exactly match the UEFI spec table. */
typedef uint64_t efi_status_t;
typedef uint64_t efi_phys_addr_t;

/* EFI_TABLE_HEADER (24 bytes) */
struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

/* EFI_MEMORY_DESCRIPTOR for SetVirtualAddressMap */
struct efi_memory_descriptor {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
};

/* Runtime services function pointer types */
typedef efi_status_t (UEFI_EFIAPI *efi_get_time_t)(void *time, void *caps);
typedef efi_status_t (UEFI_EFIAPI *efi_set_time_t)(void *time);
typedef efi_status_t (UEFI_EFIAPI *efi_get_wakeup_time_t)(
    uint8_t *enabled, uint8_t *pending, void *time);
typedef efi_status_t (UEFI_EFIAPI *efi_set_wakeup_time_t)(
    uint8_t enable, void *time);
typedef efi_status_t (UEFI_EFIAPI *efi_set_virtual_address_map_t)(
    uint64_t map_size, uint64_t desc_size, uint32_t desc_version,
    struct efi_memory_descriptor *virt_map);
typedef efi_status_t (UEFI_EFIAPI *efi_convert_pointer_t)(
    uint64_t debug_disposition, void **address);
typedef efi_status_t (UEFI_EFIAPI *efi_get_variable_t)(
    uint16_t *name, void *guid, uint32_t *attrs, uint64_t *size, void *data);
typedef efi_status_t (UEFI_EFIAPI *efi_get_next_variable_name_t)(
    uint64_t *name_size, uint16_t *name, void *guid);
typedef efi_status_t (UEFI_EFIAPI *efi_set_variable_t)(
    uint16_t *name, void *guid, uint32_t attrs, uint64_t size, void *data);
typedef efi_status_t (UEFI_EFIAPI *efi_get_next_high_mono_count_t)(
    uint32_t *count);
typedef void (UEFI_EFIAPI *efi_reset_system_t)(
    uint32_t reset_type, efi_status_t status, uint64_t data_size, void *data);
typedef efi_status_t (UEFI_EFIAPI *efi_update_capsule_t)(
    void **hdr_array, uint64_t count, efi_phys_addr_t scatter);
typedef efi_status_t (UEFI_EFIAPI *efi_query_capsule_caps_t)(
    void **hdr_array, uint64_t count, uint64_t *max_size, uint32_t *reset_type);
typedef efi_status_t (UEFI_EFIAPI *efi_query_variable_info_t)(
    uint32_t attrs, uint64_t *max_storage, uint64_t *remaining, uint64_t *max_var);

/* Kernel-side EFI_RUNTIME_SERVICES -- must match UEFI spec layout exactly */
struct efi_runtime_services {
    struct efi_table_header         hdr;
    efi_get_time_t                  get_time;
    efi_set_time_t                  set_time;
    efi_get_wakeup_time_t           get_wakeup_time;
    efi_set_wakeup_time_t           set_wakeup_time;
    efi_set_virtual_address_map_t   set_virtual_address_map;
    efi_convert_pointer_t           convert_pointer;
    efi_get_variable_t              get_variable;
    efi_get_next_variable_name_t    get_next_variable_name;
    efi_set_variable_t              set_variable;
    efi_get_next_high_mono_count_t  get_next_high_mono_count;
    efi_reset_system_t              reset_system;
    efi_update_capsule_t            update_capsule;
    efi_query_capsule_caps_t        query_capsule_capabilities;
    efi_query_variable_info_t       query_variable_info;
};

/* ---- Module state ---- */
static struct efi_runtime_services *s_rt;    /* pointer to firmware RT table */
static spinlock_t s_rt_lock = SPINLOCK_INIT;       /* serialize all RT calls */
static int s_available;                      /* 1 = RT services usable */
static uint32_t s_supported;                 /* EFI_RT_SUPPORTED_* bitmask */

/* ---- Internal helpers ---- */

/* Build virtual address map with identity mapping (virt = phys) and call
 * SetVirtualAddressMap().  Can only be called ONCE -- irreversible.
 * Returns 0 on success, -1 on failure. */
static int call_set_virtual_address_map(void)
{
    uint32_t count = g_boot_info.rt_mmap_count;
    uint32_t desc_version = g_boot_info.uefi_mmap_desc_version;

    if (count == 0) {
        klog(LOG_WARN, "UEFI", "No runtime memory regions -- skipping SVAM");
        return 0;  /* not a failure -- firmware may have no RT regions */
    }

    /* We build the virtual map ourselves, so the descriptor stride matches
     * our struct size.  The firmware's desc_size may be larger (extended
     * fields), but we don't include those -- use our own stride. */
    uint32_t desc_size = (uint32_t)sizeof(struct efi_memory_descriptor);

    /* Build the virtual map in a stack-local array.
     * 64 entries x 40 bytes = 2560 bytes -- fits on the kernel stack. */
    struct efi_memory_descriptor vmap[BOOT_RT_MMAP_MAX];

    uint32_t i;
    for (i = 0; i < count; i++) {
        vmap[i].type            = g_boot_info.rt_mmap[i].type;
        vmap[i].pad             = 0;
        vmap[i].physical_start  = g_boot_info.rt_mmap[i].phys_addr;
        vmap[i].virtual_start   = g_boot_info.rt_mmap[i].phys_addr; /* identity */
        vmap[i].number_of_pages = g_boot_info.rt_mmap[i].num_pages;
        vmap[i].attribute       = g_boot_info.rt_mmap[i].attribute;
    }

    uint64_t map_size = (uint64_t)count * desc_size;

    klog(LOG_INFO, "UEFI", "SetVirtualAddressMap: %u runtime regions, "
         "desc_size=%u, desc_ver=%u", count, desc_size, desc_version);

    efi_status_t status = s_rt->set_virtual_address_map(
        map_size, (uint64_t)desc_size, desc_version, vmap);

    if (status != UEFI_SUCCESS) {
        klog(LOG_ERROR, "UEFI", "SetVirtualAddressMap FAILED: 0x%llx",
             (unsigned long long)status);
        return -1;
    }

    klog(LOG_INFO, "UEFI", "SetVirtualAddressMap OK");
    return 0;
}

/* Read EFI_RT_PROPERTIES_TABLE from config table to determine which
 * runtime services are actually supported by this firmware. */
static void read_rt_properties(void)
{
    struct boot_uefi_guid rt_props_guid = UEFI_GUID_RT_PROPS;
    uintptr_t table_addr = uefi_find_config_table(&rt_props_guid);

    if (table_addr == 0) {
        /* Table absent -- assume all services supported (UEFI < 2.10) */
        s_supported = 0xFFFFFFFF;
        return;
    }

    const struct uefi_rt_properties_table *props =
        (const struct uefi_rt_properties_table *)table_addr;

    s_supported = props->runtime_services_supported;

    klog(LOG_INFO, "UEFI", "RT properties: supported=0x%04x", s_supported);
}

/* ---- Public API ---- */

boot_result_t uefi_runtime_init(void)
{
    (void)s_rt_lock;  /* initialized statically via SPINLOCK_INIT */
    s_available = 0;
    s_supported = 0;

    if (!g_boot_info.uefi_rt_available) {
        klog(LOG_WARN, "UEFI", "Runtime services: UNAVAILABLE (firmware limitation)");
        return BOOT_DEGRADED;
    }

    s_rt = (struct efi_runtime_services *)g_boot_info.uefi_runtime_services;
    if (!s_rt) {
        klog(LOG_ERROR, "UEFI", "Runtime services pointer is NULL");
        return BOOT_DEGRADED;
    }

    /* Validate RT table signature (UEFI Spec §4.5: "RUNTSERV") */
    if (s_rt->hdr.signature != EFI_RUNTIME_SERVICES_SIGNATURE) {
        klog(LOG_ERROR, "UEFI", "RT table signature mismatch: 0x%llx",
             (unsigned long long)s_rt->hdr.signature);
        s_rt = (struct efi_runtime_services *)0;
        return BOOT_DEGRADED;
    }

    /* Validate CRC32 (zero the field, compute, compare, restore) */
    {
        uint32_t saved_crc = s_rt->hdr.crc32;
        uint32_t hdr_size = s_rt->hdr.header_size;
        if (hdr_size == 0 || hdr_size > 4096)
            hdr_size = sizeof(struct efi_table_header);
        s_rt->hdr.crc32 = 0;
        uint32_t computed = gpt_crc32(&s_rt->hdr, hdr_size);
        s_rt->hdr.crc32 = saved_crc;
        if (computed != saved_crc) {
            klog(LOG_WARN, "UEFI", "RT table CRC32 mismatch: "
                 "expected=0x%08x computed=0x%08x (proceeding with caution)",
                 saved_crc, computed);
            /* Warn but don't fail -- some firmware has stale CRC after SVAM.
             * Windows hal.dll also warns-and-continues on CRC mismatch. */
        }
    }

    /* Read supported services mask before SVAM (config table is still valid) */
    read_rt_properties();

    /* Call SetVirtualAddressMap.  Per UEFI spec §7.4.2 this must happen after
     * ExitBootServices().  The bootloader preserves pointers only; SVAM is
     * always the kernel's responsibility.  svam_called is kept as a safety
     * guard in case a future loader calls SVAM before handoff. */
    if (g_boot_info.uefi_runtime.svam_called) {
        klog(LOG_INFO, "UEFI", "SetVirtualAddressMap already called by loader");
    } else {
        if (call_set_virtual_address_map() != 0) {
            klog(LOG_ERROR, "UEFI", "Runtime services: UNAVAILABLE (SVAM failed)");
            return BOOT_DEGRADED;
        }
    }

    /* Validate critical function pointers */
    int missing = 0;

    if (!s_rt->get_time)     { klog(LOG_WARN, "UEFI", "RT GetTime: NULL");     missing++; }
    if (!s_rt->set_time)     { klog(LOG_WARN, "UEFI", "RT SetTime: NULL");     missing++; }
    if (!s_rt->get_variable) { klog(LOG_WARN, "UEFI", "RT GetVariable: NULL"); missing++; }
    if (!s_rt->set_variable) { klog(LOG_WARN, "UEFI", "RT SetVariable: NULL"); missing++; }
    if (!s_rt->reset_system) { klog(LOG_WARN, "UEFI", "RT ResetSystem: NULL"); missing++; }

    if (missing > 0) {
        klog(LOG_WARN, "UEFI", "Runtime services: UNAVAILABLE "
             "(firmware limitation, %d/%d pointers NULL)", missing, 5);
        s_available = 0;
        return BOOT_DEGRADED;
    }

    /* Mark available -- all critical pointers validated */
    s_available = 1;

    /* Build human-readable service list for log */
    char services[128];
    uint32_t pos = 0;

    struct {
        uint32_t flag;
        const char *name;
    } svc_names[] = {
        { EFI_RT_SUPPORTED_GET_TIME,      "GetTime" },
        { EFI_RT_SUPPORTED_SET_TIME,      "SetTime" },
        { EFI_RT_SUPPORTED_GET_VARIABLE,  "GetVariable" },
        { EFI_RT_SUPPORTED_SET_VARIABLE,  "SetVariable" },
        { EFI_RT_SUPPORTED_RESET_SYSTEM,  "ResetSystem" },
        { EFI_RT_SUPPORTED_UPDATE_CAPSULE, "UpdateCapsule" },
    };

    uint32_t n = sizeof(svc_names) / sizeof(svc_names[0]);
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (s_supported & svc_names[i].flag) {
            if (pos > 0 && pos < sizeof(services) - 1)
                services[pos++] = ' ';
            const char *nm = svc_names[i].name;
            uint32_t j;
            for (j = 0; nm[j] && pos < sizeof(services) - 1; j++)
                services[pos++] = nm[j];
        }
    }
    services[pos] = '\0';

    klog(LOG_INFO, "UEFI", "Runtime services: OK");
    klog(LOG_INFO, "UEFI", "Runtime services active: %s", services);

    return BOOT_OK;
}

int uefi_rt_available(void)
{
    return s_available;
}

uint32_t uefi_rt_supported(void)
{
    return s_supported;
}

/* ============================================================================
 * UEFI Variable Services (§1.2)
 *
 * GetVariable, SetVariable, GetNextVariableName wrappers.
 * All calls are serialized via s_rt_lock (firmware is not reentrant).
 * LAPIC timer masked during all calls -- firmware SMI may enable interrupts.
 * ============================================================================ */

/* ---- Centralized RT call entry/exit ----
 * Mask LAPIC timer during ALL UEFI runtime calls -- firmware may enable
 * interrupts internally via SMI, causing timer to fire into wrong context.
 * Acquire the RT spinlock to serialize (firmware is not reentrant). */

struct rt_call_state {
    uint32_t saved_lvt;
};

static void rt_call_enter(struct rt_call_state *state)
{
    state->saved_lvt = 0;
    if (lapic_available() && kernel_subsystem_ready(SUBSYS_TIMER)) {
        uint32_t lvt = lapic_read(LAPIC_REG_LVT_TIMER);
        lapic_write(LAPIC_REG_LVT_TIMER, lvt | LVT_MASKED);
        state->saved_lvt = lvt;
    }
    spin_lock(&s_rt_lock);
}

static void rt_call_exit(struct rt_call_state *state)
{
    spin_unlock(&s_rt_lock);
    if (state->saved_lvt)
        lapic_write(LAPIC_REG_LVT_TIMER, state->saved_lvt);
}

uint64_t uefi_get_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data)
{
    struct rt_call_state rcs;
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_VARIABLE)) return UEFI_UNSUPPORTED;

    rt_call_enter(&rcs);
    efi_status_t status = s_rt->get_variable(
        (uint16_t *)name, (void *)guid, attributes, data_size, data);
    rt_call_exit(&rcs);

    return status;
}

uint64_t uefi_set_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t attributes,
                           uint64_t data_size,
                           const void *data)
{
    struct rt_call_state rcs;
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_SET_VARIABLE)) return UEFI_UNSUPPORTED;

    rt_call_enter(&rcs);
    efi_status_t status = s_rt->set_variable(
        (uint16_t *)name, (void *)guid, attributes, data_size, (void *)data);
    rt_call_exit(&rcs);

    return status;
}

uint32_t uefi_enumerate_variables(void)
{
    if (!s_available || !s_rt) return 0;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME))
        return 0;

    /* Buffer for variable name -- UEFI spec says max 1024 bytes */
    uint16_t name_buf[512];  /* 512 × 2 = 1024 bytes */
    struct boot_uefi_guid guid;
    uint64_t name_size;
    uint32_t count = 0;

    /* Start enumeration: empty name + zero GUID */
    name_buf[0] = 0;
    uint32_t i;
    uint8_t *gp = (uint8_t *)&guid;
    for (i = 0; i < 16; i++) gp[i] = 0;

    for (;;) {
        struct rt_call_state rcs;
        name_size = sizeof(name_buf);

        rt_call_enter(&rcs);
        efi_status_t status = s_rt->get_next_variable_name(
            &name_size, name_buf, &guid);
        rt_call_exit(&rcs);

        if (status != UEFI_SUCCESS)
            break;

        count++;
    }

    return count;
}

uint64_t uefi_get_next_variable_name(uint64_t *name_size, uint16_t *name,
                                     struct boot_uefi_guid *guid)
{
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME))
        return UEFI_UNSUPPORTED;

    struct rt_call_state rcs;
    rt_call_enter(&rcs);
    efi_status_t status = s_rt->get_next_variable_name(name_size, name, guid);
    rt_call_exit(&rcs);

    return status;
}

boot_result_t uefi_vars_init(void)
{
    if (!s_available) {
        klog(LOG_INFO, "UEFI", "Variable services: unavailable");
        return BOOT_DEGRADED;
    }

    /* Enumerate all variables */
    uint32_t total = uefi_enumerate_variables();

    /* Try to read BootOrder */
    struct boot_uefi_guid global_guid = EFI_GLOBAL_VARIABLE_GUID;
    static const uint16_t boot_order_name[] = {
        'B','o','o','t','O','r','d','e','r', 0
    };

    uint8_t order_buf[32];  /* max 16 boot entries */
    uint64_t order_size = sizeof(order_buf);
    uint32_t attrs = 0;

    efi_status_t status = uefi_get_variable(
        &global_guid, boot_order_name, &attrs, &order_size, order_buf);

    if (status == UEFI_SUCCESS && order_size >= 2) {
        uint16_t *order = (uint16_t *)order_buf;
        uint32_t num_entries = (uint32_t)(order_size / 2);

        /* Build BootOrder string for logging */
        char order_str[64];
        uint32_t pos = 0;
        uint32_t e;
        for (e = 0; e < num_entries && pos < sizeof(order_str) - 6; e++) {
            if (e > 0 && pos < sizeof(order_str) - 1)
                order_str[pos++] = ',';
            /* Format as 4-digit hex */
            uint16_t val = order[e];
            order_str[pos++] = "0123456789ABCDEF"[(val >> 12) & 0xF];
            order_str[pos++] = "0123456789ABCDEF"[(val >> 8) & 0xF];
            order_str[pos++] = "0123456789ABCDEF"[(val >> 4) & 0xF];
            order_str[pos++] = "0123456789ABCDEF"[val & 0xF];
        }
        order_str[pos] = '\0';

        klog(LOG_INFO, "UEFI", "NVRAM: %u variables, BootOrder=[%s]",
             total, order_str);
    } else {
        klog(LOG_INFO, "UEFI", "NVRAM: %u variables (BootOrder not available)",
             total);
    }

    /* Try to read BootCurrent */
    static const uint16_t boot_current_name[] = {
        'B','o','o','t','C','u','r','r','e','n','t', 0
    };
    uint16_t boot_current = 0;
    uint64_t bc_size = sizeof(boot_current);
    attrs = 0;

    status = uefi_get_variable(
        &global_guid, boot_current_name, &attrs, &bc_size, &boot_current);

    if (status == UEFI_SUCCESS) {
        char bc_str[5];
        bc_str[0] = "0123456789ABCDEF"[(boot_current >> 12) & 0xF];
        bc_str[1] = "0123456789ABCDEF"[(boot_current >> 8) & 0xF];
        bc_str[2] = "0123456789ABCDEF"[(boot_current >> 4) & 0xF];
        bc_str[3] = "0123456789ABCDEF"[boot_current & 0xF];
        bc_str[4] = '\0';
        klog(LOG_INFO, "UEFI", "BootCurrent: Boot%s", bc_str);
    }

    return BOOT_OK;
}

/* ============================================================================
 * System Reset (§1.3)
 *
 * ResetSystem() is a UEFI runtime service that handles all platform-specific
 * details for shutdown, reboot, and power cycling.  Preferred over direct
 * ACPI register writes or keyboard controller reset.
 * ============================================================================ */

/* x86 port I/O for keyboard controller fallback */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void uefi_reset(uint32_t reset_type)
{
    const char *type_names[] = { "Cold", "Warm", "Shutdown", "PlatformSpecific" };
    const char *name = (reset_type < 4) ? type_names[reset_type] : "Unknown";

    klog(LOG_INFO, "UEFI", "ResetSystem(%s)...", name);

    if (s_available && s_rt &&
        (s_supported & EFI_RT_SUPPORTED_RESET_SYSTEM)) {
        struct rt_call_state rcs;
        rt_call_enter(&rcs);
        /* ResetSystem() does NOT return on success */
        s_rt->reset_system(reset_type, UEFI_SUCCESS, 0, (void *)0);
        rt_call_exit(&rcs);
        /* If we get here, it failed */
        klog(LOG_ERROR, "UEFI", "ResetSystem() returned -- falling back");
    }

    /* Fallback: keyboard controller reset (0x64/0xFE) */
    klog(LOG_WARN, "UEFI", "Fallback: keyboard controller reset");
    outb(0x64, 0xFE);

    /* If that failed too, halt */
    for (;;)
        __asm__ volatile("hlt");
}

/* ============================================================================
 * RTC Time Services (§2.1)
 *
 * GetTime/SetTime/GetWakeupTime wrappers.
 * ============================================================================ */

uint64_t uefi_get_time(struct efi_time *time,
                       struct efi_time_capabilities *caps)
{
    struct rt_call_state rcs;
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_TIME)) return UEFI_UNSUPPORTED;

    rt_call_enter(&rcs);
    efi_status_t status = s_rt->get_time(time, caps);
    rt_call_exit(&rcs);

    return status;
}

uint64_t uefi_set_time(const struct efi_time *time)
{
    struct rt_call_state rcs;
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_SET_TIME)) return UEFI_UNSUPPORTED;

    rt_call_enter(&rcs);
    efi_status_t status = s_rt->set_time((void *)time);
    rt_call_exit(&rcs);

    return status;
}

uint64_t uefi_get_wakeup_time(uint8_t *enabled, uint8_t *pending,
                              struct efi_time *time)
{
    struct rt_call_state rcs;
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_WAKEUP_TIME))
        return UEFI_UNSUPPORTED;

    rt_call_enter(&rcs);
    efi_status_t status = s_rt->get_wakeup_time(enabled, pending, time);
    rt_call_exit(&rcs);

    return status;
}

boot_result_t uefi_time_init(void)
{
    struct efi_time t;
    struct efi_time_capabilities caps;

    efi_status_t status = uefi_get_time(&t, &caps);
    if (status != UEFI_SUCCESS) {
        klog(LOG_WARN, "UEFI", "GetTime failed -- wall clock not seeded");
        return BOOT_DEGRADED;
    }

    /* Format: "YYYY-MM-DD HH:MM:SS" -- manual zero-padding (no %02u in klog) */
    char ts[20];
    uint16_t y = t.year;
    ts[0]  = '0' + (char)(y / 1000);
    ts[1]  = '0' + (char)((y / 100) % 10);
    ts[2]  = '0' + (char)((y / 10) % 10);
    ts[3]  = '0' + (char)(y % 10);
    ts[4]  = '-';
    ts[5]  = '0' + (char)(t.month / 10);
    ts[6]  = '0' + (char)(t.month % 10);
    ts[7]  = '-';
    ts[8]  = '0' + (char)(t.day / 10);
    ts[9]  = '0' + (char)(t.day % 10);
    ts[10] = ' ';
    ts[11] = '0' + (char)(t.hour / 10);
    ts[12] = '0' + (char)(t.hour % 10);
    ts[13] = ':';
    ts[14] = '0' + (char)(t.minute / 10);
    ts[15] = '0' + (char)(t.minute % 10);
    ts[16] = ':';
    ts[17] = '0' + (char)(t.second / 10);
    ts[18] = '0' + (char)(t.second % 10);
    ts[19] = '\0';

    klog(LOG_INFO, "UEFI", "RTC: %s", ts);

    /* Timezone info */
    if (t.timezone == (int16_t)EFI_UNSPECIFIED_TIMEZONE) {
        klog(LOG_INFO, "UEFI", "RTC: timezone unspecified (local time)");
    } else {
        int16_t tz = t.timezone;
        uint32_t abs_tz = (uint32_t)((tz >= 0) ? tz : -tz);
        klog(LOG_INFO, "UEFI", "RTC: UTC%s%u:%s%u",
             (tz >= 0) ? "+" : "-",
             abs_tz / 60,
             (abs_tz % 60 < 10) ? "0" : "",
             abs_tz % 60);
    }

    klog(LOG_INFO, "UEFI", "RTC: resolution=%u Hz, accuracy=%u ppm",
         caps.resolution, caps.accuracy);

    return BOOT_OK;
}

/* ============================================================================
 * Secure Boot State Detection (§5.1)
 *
 * Reads UEFI NVRAM variables to determine Secure Boot state.
 * ============================================================================ */

static int s_sb_enabled;    /* SecureBoot variable = 1 */
static int s_sb_setup_mode; /* SetupMode variable = 1 */
static int s_sb_pk_present; /* PK variable exists with data */
static int s_sb_kek_present;/* KEK variable exists with data */

/* Helper: read a single-byte UEFI global variable. Returns the byte, or -1. */
static int read_global_byte(const uint16_t *name)
{
    struct boot_uefi_guid global = EFI_GLOBAL_VARIABLE_GUID;
    uint8_t val = 0;
    uint64_t sz = sizeof(val);
    uint32_t attrs = 0;

    efi_status_t status = uefi_get_variable(&global, name, &attrs, &sz, &val);
    if (status != UEFI_SUCCESS)
        return -1;
    return (int)val;
}

/* Helper: check if a UEFI global variable exists with non-zero size. */
static int global_var_exists(const uint16_t *name)
{
    struct boot_uefi_guid global = EFI_GLOBAL_VARIABLE_GUID;
    uint64_t sz = 0;
    uint32_t attrs = 0;

    /* Call with size=0 to get the actual size via BUFFER_TOO_SMALL */
    efi_status_t status = uefi_get_variable(
        &global, name, &attrs, &sz, (void *)0);

    /* BUFFER_TOO_SMALL means it exists; sz > 0 means it has data */
    if (status == UEFI_BUFFER_TOO_SMALL && sz > 0)
        return 1;
    /* SUCCESS with sz=0 also means it exists (empty variable) */
    if (status == UEFI_SUCCESS)
        return 1;
    return 0;
}

boot_result_t uefi_secureboot_init(void)
{
    s_sb_enabled = 0;
    s_sb_setup_mode = 0;
    s_sb_pk_present = 0;
    s_sb_kek_present = 0;

    if (!s_available) {
        klog(LOG_INFO, "UEFI", "Secure Boot: unknown (runtime unavailable)");
        return BOOT_DEGRADED;
    }

    /* Read SecureBoot variable (uint8_t: 0=off, 1=on) */
    static const uint16_t sb_name[] = {
        'S','e','c','u','r','e','B','o','o','t', 0
    };
    int sb_val = read_global_byte(sb_name);
    if (sb_val >= 0)
        s_sb_enabled = (sb_val == 1) ? 1 : 0;

    /* Read SetupMode variable (uint8_t: 0=User Mode, 1=Setup Mode) */
    static const uint16_t sm_name[] = {
        'S','e','t','u','p','M','o','d','e', 0
    };
    int sm_val = read_global_byte(sm_name);
    if (sm_val >= 0)
        s_sb_setup_mode = (sm_val == 1) ? 1 : 0;

    /* Check PK existence */
    static const uint16_t pk_name[] = { 'P','K', 0 };
    s_sb_pk_present = global_var_exists(pk_name);

    /* Check KEK existence */
    static const uint16_t kek_name[] = { 'K','E','K', 0 };
    s_sb_kek_present = global_var_exists(kek_name);

    /* Log summary */
    if (s_sb_enabled) {
        klog(LOG_INFO, "UEFI", "Secure Boot: ENABLED (%s)",
             s_sb_setup_mode ? "Setup Mode" : "User Mode");
    } else {
        klog(LOG_INFO, "UEFI", "Secure Boot: DISABLED (%s)",
             s_sb_setup_mode ? "Setup Mode" : "User Mode");
    }

    klog(LOG_INFO, "UEFI", "Secure Boot keys: PK=%s, KEK=%s",
         s_sb_pk_present ? "enrolled" : "absent",
         s_sb_kek_present ? "enrolled" : "absent");

    /* Publish state to boot_info and global system state for desktop/tray */
    g_boot_info.secure_boot_enabled = (uint8_t)s_sb_enabled;
    g_system_state.secure_boot       = (uint8_t)s_sb_enabled;

    /* Canonical one-line summary consumed by monitoring tools */
    if (s_sb_enabled) {
        klog(LOG_INFO, "SecureBoot", "state=ENABLED");
    } else {
        klog(LOG_INFO, "SecureBoot", "state=DISABLED (firmware or user override)");
    }

    return BOOT_OK;
}

/* Called from registry_populate_defaults() after registry_init() -- writes
 * HKLM\SYSTEM\SecureBoot\State once the registry tree is ready. */
void uefi_secureboot_populate_registry(void)
{
    HKEY hKey;
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, "SYSTEM\\SecureBoot", 0,
                       NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        const struct secureboot_db_info *dbi = secureboot_get_db_info();
        RegSetDword(hKey, "State", (uint32_t)s_sb_enabled);
        /* Mirror db/dbx inventory from secureboot_keys_init() (S9) */
        if (dbi) {
            RegSetDword(hKey, "DbEntries",  dbi->db_entries);
            RegSetDword(hKey, "DbxEntries", dbi->dbx_entries);
            RegSetDword(hKey, "DbSha256",   dbi->db_sha256_count);
            RegSetDword(hKey, "DbxSha256",  dbi->dbx_sha256_count);
            RegSetDword(hKey, "DbX509",     dbi->db_x509_count);
        }
        RegCloseKey(hKey);
        klog(LOG_DEBUG, "SecureBoot",
             "HKLM\\SYSTEM\\SecureBoot: State=%u Db=%u Dbx=%u",
             (uint64_t)s_sb_enabled,
             dbi ? (uint64_t)dbi->db_entries : 0,
             dbi ? (uint64_t)dbi->dbx_entries : 0);
    }
}

int uefi_secureboot_enabled(void)  { return s_sb_enabled; }
int uefi_secureboot_setup_mode(void) { return s_sb_setup_mode; }
int uefi_secureboot_pk_present(void) { return s_sb_pk_present; }
int uefi_secureboot_kek_present(void) { return s_sb_kek_present; }

/* ============================================================================
 * Secure Boot Key Management (§5.2)
 *
 * Reads and parses db/dbx/dbt signature databases.
 * Walking EFI_SIGNATURE_LIST chains to enumerate trust entries.
 * Authenticated writes (dbx update) deferred until crypto stack available.
 * ============================================================================ */

static struct secureboot_db_info s_db_info;

/* GUID comparison helper (reuse from uefi_config.c pattern) */
static int sb_guid_equal(const struct boot_uefi_guid *a,
                         const struct boot_uefi_guid *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint32_t i;
    for (i = 0; i < 16; i++)
        if (pa[i] != pb[i]) return 0;
    return 1;
}

/* Walk an EFI_SIGNATURE_LIST chain in a buffer, counting entries by type. */
static void count_sig_entries(const uint8_t *buf, uint64_t buf_sz,
                              uint32_t *total,
                              uint32_t *sha256_count,
                              uint32_t *x509_count)
{
    struct boot_uefi_guid sha256_type = EFI_CERT_SHA256_GUID;
    struct boot_uefi_guid x509_type   = EFI_CERT_X509_GUID;

    *total = 0;
    *sha256_count = 0;
    *x509_count = 0;

    uint64_t offset = 0;
    while (offset + sizeof(struct efi_signature_list) <= buf_sz) {
        const struct efi_signature_list *sl =
            (const struct efi_signature_list *)(buf + offset);

        if (sl->signature_list_size == 0) break;
        if (offset + sl->signature_list_size > buf_sz) break;

        /* Count entries in this list */
        uint32_t data_size = sl->signature_list_size -
            (uint32_t)sizeof(struct efi_signature_list) -
            sl->signature_header_size;
        uint32_t entries = 0;
        if (sl->signature_size > 0)
            entries = data_size / sl->signature_size;

        *total += entries;

        if (sb_guid_equal(&sl->signature_type, &sha256_type))
            *sha256_count += entries;
        else if (sb_guid_equal(&sl->signature_type, &x509_type))
            *x509_count += entries;

        offset += sl->signature_list_size;
    }
}

/* Read a security database variable and count its entries. */
static void read_security_db(const uint16_t *name,
                             uint32_t *total,
                             uint32_t *sha256_count,
                             uint32_t *x509_count)
{
    struct boot_uefi_guid db_guid = EFI_IMAGE_SECURITY_DATABASE_GUID;
    uint32_t attrs = 0;

    *total = 0;
    *sha256_count = 0;
    *x509_count = 0;

    /* First call with size=0 to get actual size */
    uint64_t sz = 0;
    efi_status_t status = uefi_get_variable(
        &db_guid, name, &attrs, &sz, (void *)0);

    if (status != UEFI_BUFFER_TOO_SMALL || sz == 0)
        return;  /* Variable doesn't exist or is empty */

    /* Use a static buffer -- security DBs are typically 1-4 KB in OVMF,
     * up to ~32 KB on real hardware with many entries.
     * We cap at 8 KB to avoid stack overflow. */
    if (sz > 8192) sz = 8192;

    uint8_t dbuf[8192];
    status = uefi_get_variable(&db_guid, name, &attrs, &sz, dbuf);
    if (status != UEFI_SUCCESS)
        return;

    count_sig_entries(dbuf, sz, total, sha256_count, x509_count);
}

void secureboot_keys_init(void)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)&s_db_info;
    for (i = 0; i < (uint32_t)sizeof(s_db_info); i++)
        p[i] = 0;

    if (!s_available) return;

    /* Read db (Authorized Signature Database) */
    static const uint16_t db_name[] = { 'd','b', 0 };
    uint32_t db_sha = 0, db_x509 = 0;
    read_security_db(db_name, &s_db_info.db_entries, &db_sha, &db_x509);
    s_db_info.db_sha256_count = db_sha;
    s_db_info.db_x509_count = db_x509;

    /* Read dbx (Forbidden Signature Database) */
    static const uint16_t dbx_name[] = { 'd','b','x', 0 };
    uint32_t dbx_sha = 0, dbx_x509 = 0;
    read_security_db(dbx_name, &s_db_info.dbx_entries, &dbx_sha, &dbx_x509);
    s_db_info.dbx_sha256_count = dbx_sha;

    /* Read dbt (Timestamp Database) */
    static const uint16_t dbt_name[] = { 'd','b','t', 0 };
    uint32_t dbt_sha = 0, dbt_x509 = 0;
    read_security_db(dbt_name, &s_db_info.dbt_entries, &dbt_sha, &dbt_x509);

    /* Log summary */
    klog(LOG_INFO, "UEFI", "Secure Boot db: %u entries "
         "(%u X.509, %u SHA-256)",
         s_db_info.db_entries,
         s_db_info.db_x509_count,
         s_db_info.db_sha256_count);

    klog(LOG_INFO, "UEFI", "Secure Boot dbx: %u revocations "
         "(%u SHA-256)",
         s_db_info.dbx_entries,
         s_db_info.dbx_sha256_count);

    if (s_db_info.dbt_entries > 0) {
        klog(LOG_INFO, "UEFI", "Secure Boot dbt: %u timestamp entries",
             s_db_info.dbt_entries);
    }
}

const struct secureboot_db_info *secureboot_get_db_info(void)
{
    return &s_db_info;
}

/* ============================================================================
 * Crypto Agility (§5.3 -- UEFI 2.10)
 *
 * Reads the CryptoIndication variables to determine firmware algorithm
 * capabilities and active negotiated set.  These variables enable OS-driven
 * algorithm upgrades (e.g., SHA-256 → SHA-384) without firmware reflash.
 *
 * Variable names (all under EFI_GLOBAL_VARIABLE GUID):
 *   "CryptoIndicationsSupported" -- firmware lists everything it can do
 *   "CryptoIndications"          -- OS writes what it wants for next boot
 *   "CryptoIndicationsActivated" -- firmware confirms what's active now
 *
 * OVMF/EDK2 does not currently implement these (UEFI 2.10 is new), so
 * this will gracefully report "not available" on most test platforms.
 * ============================================================================ */

static struct crypto_agility_info s_crypto_info;

/* Read a uint32_t UEFI variable by UCS-2 name (global GUID). */
static uint32_t read_crypto_var(const uint16_t *name)
{
    struct boot_uefi_guid global = EFI_GLOBAL_VARIABLE_GUID;
    uint32_t attrs = 0;
    uint32_t val = 0;
    uint64_t sz = sizeof(val);
    efi_status_t status = uefi_get_variable(
        &global, name, &attrs, &sz, &val);
    if (status != UEFI_SUCCESS) return 0;
    return val;
}

/* Append algorithm name to buffer if bitmask bit is set. */
static int append_algo(char *buf, int pos, int max,
                       uint32_t mask, uint32_t bit, const char *name)
{
    if (!(mask & bit)) return pos;
    if (pos > 0 && pos < max - 1)
        buf[pos++] = '+';
    int i = 0;
    while (name[i] && pos < max - 1)
        buf[pos++] = name[i++];
    buf[pos] = '\0';
    return pos;
}

void uefi_crypto_agility_init(void)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)&s_crypto_info;
    for (i = 0; i < (uint32_t)sizeof(s_crypto_info); i++)
        p[i] = 0;

    if (!s_available) return;

    /* Read CryptoIndicationsSupported (firmware-owned) */
    static const uint16_t name_sup[] = {
        'C','r','y','p','t','o','I','n','d','i','c','a','t','i','o','n','s',
        'S','u','p','p','o','r','t','e','d', 0 };
    s_crypto_info.supported = read_crypto_var(name_sup);

    /* If firmware doesn't support CryptoIndications, skip the rest */
    if (s_crypto_info.supported == 0) {
        klog(LOG_INFO, "UEFI",
             "Crypto agility: not available "
             "(firmware lacks UEFI 2.10 CryptoIndications)");
        return;
    }

    s_crypto_info.available = 1;

    /* Read CryptoIndications (OS-owned -- what we previously requested) */
    static const uint16_t name_req[] = {
        'C','r','y','p','t','o','I','n','d','i','c','a','t','i','o','n','s',
        0 };
    s_crypto_info.requested = read_crypto_var(name_req);

    /* Read CryptoIndicationsActivated (firmware-owned -- what's active now) */
    static const uint16_t name_act[] = {
        'C','r','y','p','t','o','I','n','d','i','c','a','t','i','o','n','s',
        'A','c','t','i','v','a','t','e','d', 0 };
    s_crypto_info.activated = read_crypto_var(name_act);

    /* Log what's active */
    char active_str[64];
    int pos = 0;
    active_str[0] = '\0';
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_RSA_4096_SHA512, "RSA-4096");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_RSA_3072_SHA384, "RSA-3072");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_ECDSA_P384, "ECDSA-P384");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_ECDSA_P256, "ECDSA-P256");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_RSA_2048_SHA256, "RSA-2048");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_SHA384, "SHA-384");
    pos = append_algo(active_str, pos, 64,
                      s_crypto_info.activated,
                      CRYPTO_IND_SHA256, "SHA-256");
    (void)pos;

    if (active_str[0] != '\0') {
        klog(LOG_INFO, "UEFI",
             "Crypto agility: active [%s], %u supported, %u requested",
             active_str,
             s_crypto_info.supported,
             s_crypto_info.requested);
    } else {
        klog(LOG_INFO, "UEFI",
             "Crypto agility: supported=0x%x, none activated yet",
             s_crypto_info.supported);
    }

    /* TODO: When crypto policy engine is built, implement:
     *   1. Analyze supported bitmask to find strongest common set
     *   2. Write CryptoIndications to request upgrade (e.g., SHA-384 + RSA-3072)
     *   3. On next boot, firmware reads our request and activates it
     *   4. Check CryptoIndicationsActivated to confirm upgrade took effect */
}

const struct crypto_agility_info *uefi_crypto_agility_info(void)
{
    return &s_crypto_info;
}

/* ============================================================================
 * Capsule Firmware Update (§6.1)
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  ⚠️  DANGER -- READ EVERY WORD BEFORE MODIFYING THIS SECTION  ⚠️     ║
 * ╠══════════════════════════════════════════════════════════════════════╣
 * ║                                                                    ║
 * ║  UpdateCapsule() writes to SPI flash.  A bad capsule BRICKS the   ║
 * ║  motherboard.  There is NO software recovery -- you need a $5      ║
 * ║  CH341A SPI programmer and steady hands to solder an SOIC clip.   ║
 * ║                                                                    ║
 * ║  Real-world bricking scenarios:                                    ║
 * ║    • Wrong CapsuleGuid → firmware interprets data as wrong type   ║
 * ║    • Truncated capsule → partial flash write, CMOS gone           ║
 * ║    • Power loss mid-flash → half-written firmware image           ║
 * ║    • Unsigned capsule → attacker reflashes with rootkit firmware  ║
 * ║    • Version < LowestSupported → firmware rejects but may corrupt ║
 * ║                                                                    ║
 * ║  PREREQUISITES for safe UpdateCapsule() implementation:            ║
 * ║    1. OEM-signed capsule (PKCS#7/CMS with RSA-2048+ signature)   ║
 * ║    2. Kernel crypto stack for signature verification               ║
 * ║    3. ESRT FwClass GUID matching to verify capsule target         ║
 * ║    4. Version check against ESRT LowestSupportedFwVersion         ║
 * ║    5. AC power detection (ACPI battery/power supply status)       ║
 * ║    6. Explicit user confirmation with countdown timer             ║
 * ║    7. Pre-update NVRAM backup of current firmware settings        ║
 * ║                                                                    ║
 * ║  This implementation provides QUERY ONLY.                          ║
 * ║  UpdateCapsule() is NOT called, NOT exposed, NOT stubbed.          ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 * ============================================================================ */

static struct capsule_capability_info s_capsule_info;

void uefi_capsule_init(void)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)&s_capsule_info;
    for (i = 0; i < (uint32_t)sizeof(s_capsule_info); i++)
        p[i] = 0;

    if (!s_available || !s_rt) {
        klog(LOG_INFO, "UEFI",
             "Capsule updates: not available (no runtime services)");
        return;
    }

    /* Check if firmware advertises capsule support via the
     * EFI_RT_PROPERTIES_TABLE supported bitmask.
     *
     * This is the SAFE way to probe -- it only reads a previously-cached
     * bitmask (s_supported), no firmware calls are made.
     *
     * We deliberately do NOT call QueryCapsuleCapabilities() here because:
     *   • It requires a valid EFI_CAPSULE_HEADER array with a real CapsuleGuid
     *   • Using a dummy/zero GUID may confuse or crash some firmware
     *   • The RT_SUPPORTED flag tells us everything we need for capability
     *     reporting without any risk */
    if (s_supported & EFI_RT_SUPPORTED_UPDATE_CAPSULE) {
        s_capsule_info.supported = 1;

        /* Log with appropriate warnings */
        klog(LOG_INFO, "UEFI",
             "Capsule updates: firmware supports UpdateCapsule()");
        klog(LOG_DEBUG, "UEFI",
             "Capsule updates: write path not yet implemented "
             "(planned -- requires signed capsules + crypto verification)");

        /* TODO: When we have ESRT parsing + crypto stack, implement:
         *
         * Phase 1 -- Query (SAFE, read-only):
         *   For each ESRT entry, build a minimal EFI_CAPSULE_HEADER with
         *   the FwClass GUID and call QueryCapsuleCapabilities() to get
         *   max_capsule_size and reset_type for that firmware component.
         *
         * Phase 2 -- Validate (SAFE, no flash writes):
         *   Given a capsule file:
         *   a. Parse the capsule header (CapsuleGuid, size, flags)
         *   b. Verify CapsuleGuid matches an ESRT FwClass entry
         *   c. Extract embedded PKCS#7 signature
         *   d. Verify signature against db (Authorized Signature Database)
         *   e. Check capsule version >= ESRT LowestSupportedFwVersion
         *   f. Check AC power via ACPI
         *
         * Phase 3 -- Apply (DANGEROUS, writes firmware flash):
         *   a. Display user confirmation with 10-second countdown
         *   b. Allocate capsule in EfiRuntimeServicesData memory
         *   c. Set CAPSULE_FLAGS_PERSIST_ACROSS_RESET
         *   d. Call UpdateCapsule() -- firmware stages for next reboot
         *   e. Call ResetSystem(EfiResetCold) to trigger firmware update
         *   f. On next boot, read CapsuleResultVariable for status
         *
         * NEVER implement Phase 3 without Phase 2.  An unsignated capsule
         * write is a firmware rootkit delivery mechanism. */
    } else {
        klog(LOG_INFO, "UEFI",
             "Capsule updates: not supported by this firmware");
    }
}

int uefi_capsule_supported(void)
{
    return s_capsule_info.supported;
}

const struct capsule_capability_info *uefi_capsule_info(void)
{
    return &s_capsule_info;
}

/* ---- SSDT handlers: NtQuerySystemEnvironmentValueEx / NtSetSystemEnvironmentValueEx ---- */

/* Map EFI status to NTSTATUS */
static NTSTATUS efi_to_ntstatus(uint64_t efi_status)
{
    if (efi_status == UEFI_SUCCESS)   return STATUS_SUCCESS;
    if (efi_status == UEFI_NOT_FOUND) return STATUS_NOT_FOUND;
    /* Buffer too small: high bit set, code 5 */
    if (efi_status == (5ULL | (1ULL << 63))) return STATUS_BUFFER_TOO_SMALL;
    return STATUS_UNSUCCESSFUL;
}

/* NtQuerySystemEnvironmentValueEx(Name, VendorGuid, Value, ValueLength, Attributes)
 * SSDT 0x00D4 -- Win32 GetFirmwareEnvironmentVariableExW maps here */
static NTSTATUS nt_query_env_value_ex(uint64_t name_ptr, uint64_t guid_ptr,
                                       uint64_t value_ptr, uint64_t length_ptr,
                                       uint64_t attrs_ptr, uint64_t a6)
{
    const uint16_t *name;
    const struct boot_uefi_guid *guid;
    uint64_t data_size;
    uint32_t attrs = 0;
    uint64_t efi_status;

    (void)a6;
    if (!name_ptr || !guid_ptr || !value_ptr || !length_ptr)
        return STATUS_INVALID_PARAMETER;

    name = (const uint16_t *)name_ptr;
    guid = (const struct boot_uefi_guid *)guid_ptr;
    data_size = *(uint32_t *)length_ptr;

    efi_status = uefi_get_variable(guid, name, &attrs, &data_size,
                                    (void *)value_ptr);

    *(uint32_t *)length_ptr = (uint32_t)data_size;
    if (attrs_ptr)
        *(uint32_t *)attrs_ptr = attrs;

    return efi_to_ntstatus(efi_status);
}

/* NtSetSystemEnvironmentValueEx(Name, VendorGuid, Value, ValueLength, Attributes)
 * SSDT 0x00D5 -- Win32 SetFirmwareEnvironmentVariableExW maps here */
static NTSTATUS nt_set_env_value_ex(uint64_t name_ptr, uint64_t guid_ptr,
                                     uint64_t value_ptr, uint64_t length,
                                     uint64_t attrs, uint64_t a6)
{
    const uint16_t *name;
    const struct boot_uefi_guid *guid;
    uint64_t efi_status;

    (void)a6;
    if (!name_ptr || !guid_ptr)
        return STATUS_INVALID_PARAMETER;

    name = (const uint16_t *)name_ptr;
    guid = (const struct boot_uefi_guid *)guid_ptr;

    efi_status = uefi_set_variable(guid, name, (uint32_t)attrs,
                                    length, (const void *)value_ptr);

    return efi_to_ntstatus(efi_status);
}

/* Legacy non-Ex variants: same but no attributes parameter */
static NTSTATUS nt_query_env_value(uint64_t name_ptr, uint64_t guid_ptr,
                                    uint64_t value_ptr, uint64_t length_ptr,
                                    uint64_t a5, uint64_t a6)
{
    return nt_query_env_value_ex(name_ptr, guid_ptr, value_ptr, length_ptr,
                                 0, a6);
    (void)a5;
}

static NTSTATUS nt_set_env_value(uint64_t name_ptr, uint64_t guid_ptr,
                                  uint64_t value_ptr, uint64_t length,
                                  uint64_t a5, uint64_t a6)
{
    /* Legacy: use NV+BS+RT attributes */
    return nt_set_env_value_ex(name_ptr, guid_ptr, value_ptr, length,
                                7, a6);  /* 7 = NV|BS|RT */
    (void)a5;
}

void uefi_register_ssdt(void)
{
    ssdt_register(SSDT_NtQuerySystemEnvironmentValue,
                  (SSDT_HANDLER)nt_query_env_value);
    ssdt_register(SSDT_NtSetSystemEnvironmentValue,
                  (SSDT_HANDLER)nt_set_env_value);
    ssdt_register(SSDT_NtQuerySystemEnvironmentValueEx,
                  (SSDT_HANDLER)nt_query_env_value_ex);
    ssdt_register(SSDT_NtSetSystemEnvironmentValueEx,
                  (SSDT_HANDLER)nt_set_env_value_ex);

    klog(LOG_INFO, "uefi",
         "Firmware variable syscalls registered (SSDT 0x%03X-0x%03X)",
         (uint64_t)SSDT_NtQuerySystemEnvironmentValue,
         (uint64_t)SSDT_NtSetSystemEnvironmentValueEx);
}
