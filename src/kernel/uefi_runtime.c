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

/* ============================================================================
 * UEFI Variable Services (§1.2)
 *
 * GetVariable, SetVariable, GetNextVariableName wrappers.
 * All calls are serialized via s_rt_lock (firmware is not reentrant).
 * ============================================================================ */

uint64_t uefi_get_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t *attributes,
                           uint64_t *data_size,
                           void *data)
{
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_VARIABLE)) return UEFI_UNSUPPORTED;

    spin_lock(&s_rt_lock);
    efi_status_t status = s_rt->get_variable(
        (uint16_t *)name, (void *)guid, attributes, data_size, data);
    spin_unlock(&s_rt_lock);

    return status;
}

uint64_t uefi_set_variable(const struct boot_uefi_guid *guid,
                           const uint16_t *name,
                           uint32_t attributes,
                           uint64_t data_size,
                           const void *data)
{
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_SET_VARIABLE)) return UEFI_UNSUPPORTED;

    spin_lock(&s_rt_lock);
    efi_status_t status = s_rt->set_variable(
        (uint16_t *)name, (void *)guid, attributes, data_size, (void *)data);
    spin_unlock(&s_rt_lock);

    return status;
}

uint32_t uefi_enumerate_variables(void)
{
    if (!s_available || !s_rt) return 0;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_NEXT_VARIABLE_NAME))
        return 0;

    /* Buffer for variable name — UEFI spec says max 1024 bytes */
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
        name_size = sizeof(name_buf);

        spin_lock(&s_rt_lock);
        efi_status_t status = s_rt->get_next_variable_name(
            &name_size, name_buf, &guid);
        spin_unlock(&s_rt_lock);

        if (status != UEFI_SUCCESS)
            break;

        count++;
    }

    return count;
}

void uefi_vars_init(void)
{
    if (!s_available) {
        klog(LOG_INFO, "UEFI", "Variable services: unavailable");
        return;
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
        spin_lock(&s_rt_lock);
        /* ResetSystem() does NOT return on success */
        s_rt->reset_system(reset_type, UEFI_SUCCESS, 0, (void *)0);
        spin_unlock(&s_rt_lock);
        /* If we get here, it failed */
        klog(LOG_ERROR, "UEFI", "ResetSystem() returned — falling back");
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
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_TIME)) return UEFI_UNSUPPORTED;

    spin_lock(&s_rt_lock);
    efi_status_t status = s_rt->get_time(time, caps);
    spin_unlock(&s_rt_lock);

    return status;
}

uint64_t uefi_set_time(const struct efi_time *time)
{
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_SET_TIME)) return UEFI_UNSUPPORTED;

    spin_lock(&s_rt_lock);
    efi_status_t status = s_rt->set_time((void *)time);
    spin_unlock(&s_rt_lock);

    return status;
}

uint64_t uefi_get_wakeup_time(uint8_t *enabled, uint8_t *pending,
                              struct efi_time *time)
{
    if (!s_available || !s_rt) return UEFI_UNSUPPORTED;
    if (!(s_supported & EFI_RT_SUPPORTED_GET_WAKEUP_TIME))
        return UEFI_UNSUPPORTED;

    spin_lock(&s_rt_lock);
    efi_status_t status = s_rt->get_wakeup_time(enabled, pending, time);
    spin_unlock(&s_rt_lock);

    return status;
}

void uefi_time_init(void)
{
    struct efi_time t;
    struct efi_time_capabilities caps;

    efi_status_t status = uefi_get_time(&t, &caps);
    if (status != UEFI_SUCCESS) {
        klog(LOG_WARN, "UEFI", "GetTime failed — wall clock not seeded");
        return;
    }

    /* Format: "YYYY-MM-DD HH:MM:SS" — manual zero-padding (no %02u in klog) */
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

void uefi_secureboot_init(void)
{
    s_sb_enabled = 0;
    s_sb_setup_mode = 0;
    s_sb_pk_present = 0;
    s_sb_kek_present = 0;

    if (!s_available) {
        klog(LOG_INFO, "UEFI", "Secure Boot: unknown (runtime unavailable)");
        return;
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

    /* Use a static buffer — security DBs are typically 1-4 KB in OVMF,
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
