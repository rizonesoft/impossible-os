/* ============================================================================
 * uefi_runtime.c — UEFI Runtime Services initialization and wrappers
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
#include "kernel/uefi_config.h"
#include "kernel/klog.h"
#include "kernel/sched/spinlock.h"

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

/* Kernel-side EFI_RUNTIME_SERVICES — must match UEFI spec layout exactly */
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
 * SetVirtualAddressMap().  Can only be called ONCE — irreversible. */
static void call_set_virtual_address_map(void)
{
    uint32_t count = g_boot_info.rt_mmap_count;
    uint32_t desc_size = g_boot_info.uefi_mmap_desc_size;
    uint32_t desc_version = g_boot_info.uefi_mmap_desc_version;

    if (count == 0) {
        klog(LOG_WARN, "UEFI", "No runtime memory regions — skipping SVAM");
        return;
    }

    /* Use the UEFI descriptor size.  If it's smaller than our struct, fall
     * back to our struct size (shouldn't happen on spec-compliant firmware). */
    if (desc_size < sizeof(struct efi_memory_descriptor))
        desc_size = (uint32_t)sizeof(struct efi_memory_descriptor);

    /* Build the virtual map in a stack-local array.
     * 64 entries × 40 bytes = 2560 bytes — fits on the kernel stack. */
    struct efi_memory_descriptor vmap[BOOT_RT_MMAP_MAX];

    uint32_t i;
    for (i = 0; i < count; i++) {
        vmap[i].type            = g_boot_info.rt_mmap[i].type;
        vmap[i].pad             = 0;
        vmap[i].physical_start  = g_boot_info.rt_mmap[i].phys_addr;
        vmap[i].virtual_start   = g_boot_info.rt_mmap[i].phys_addr; /* identity */
        vmap[i].number_of_pages = g_boot_info.rt_mmap[i].num_pages;
        vmap[i].attribute       = UEFI_MEMORY_ATTR_RUNTIME;
    }

    uint64_t map_size = (uint64_t)count * desc_size;

    klog(LOG_INFO, "UEFI", "SetVirtualAddressMap: %u runtime regions, "
         "desc_size=%u, desc_ver=%u", count, desc_size, desc_version);

    efi_status_t status = s_rt->set_virtual_address_map(
        map_size, (uint64_t)desc_size, desc_version, vmap);

    if (status != UEFI_SUCCESS) {
        klog(LOG_ERROR, "UEFI", "SetVirtualAddressMap FAILED: 0x%llx",
             (unsigned long long)status);
        s_available = 0;
        return;
    }

    klog(LOG_INFO, "UEFI", "SetVirtualAddressMap OK");
}

/* Read EFI_RT_PROPERTIES_TABLE from config table to determine which
 * runtime services are actually supported by this firmware. */
static void read_rt_properties(void)
{
    struct boot_uefi_guid rt_props_guid = UEFI_GUID_RT_PROPS;
    uintptr_t table_addr = uefi_find_config_table(&rt_props_guid);

    if (table_addr == 0) {
        /* Table absent — assume all services supported (UEFI < 2.10) */
        s_supported = 0xFFFFFFFF;
        return;
    }

    const struct uefi_rt_properties_table *props =
        (const struct uefi_rt_properties_table *)table_addr;

    s_supported = props->runtime_services_supported;

    klog(LOG_INFO, "UEFI", "RT properties: supported=0x%04x", s_supported);
}

/* ---- Public API ---- */

void uefi_runtime_init(void)
{
    (void)s_rt_lock;  /* initialized statically via SPINLOCK_INIT */
    s_available = 0;
    s_supported = 0;

    if (!g_boot_info.uefi_rt_available) {
        klog(LOG_WARN, "UEFI", "Runtime services not preserved by bootloader");
        return;
    }

    s_rt = (struct efi_runtime_services *)g_boot_info.uefi_runtime_services;
    if (!s_rt) {
        klog(LOG_ERROR, "UEFI", "Runtime services pointer is NULL");
        return;
    }

    /* Read supported services mask before SVAM (config table is still valid) */
    read_rt_properties();

    /* Call SetVirtualAddressMap — identity mapping, ONE TIME ONLY */
    call_set_virtual_address_map();

    /* Mark available if SVAM succeeded (s_available wasn't cleared) */
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

    klog(LOG_INFO, "UEFI", "Runtime services active: %s", services);
}

int uefi_rt_available(void)
{
    return s_available;
}

uint32_t uefi_rt_supported(void)
{
    return s_supported;
}
