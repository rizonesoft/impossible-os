/* ============================================================================
 * codex.c — Codex Registry System (in-memory tree)
 *
 * Hierarchical key-value store for OS configuration.
 * Keys are organized in a tree with backslash-separated paths.
 * Values are typed (STRING, INT32, INT64, BINARY, BOOL).
 *
 * Memory: all nodes are statically allocated from fixed pools to avoid
 * heap pressure.  This limits the total number of keys and values but
 * makes the Codex usable very early in boot.
 * ============================================================================ */

#include "codex.h"
#include "kernel/printk.h"
#include "kernel/drivers/framebuffer.h"
#include "kernel/mm/pmm.h"
#include "kernel/fs/vfs.h"
#include "kernel/drivers/pit.h"

/* ---- Static node pools ---- */

#define KEY_POOL_SIZE   256
#define VALUE_POOL_SIZE 512

static codex_key_t   key_pool[KEY_POOL_SIZE];
static uint32_t      key_pool_next = 0;

static codex_value_t value_pool[VALUE_POOL_SIZE];
static uint32_t      value_pool_next = 0;

/* Root of the entire Codex tree */
static codex_key_t  *codex_root = (void *)0;
static uint8_t       codex_ready = 0;

/* Dirty flags for the four root trees */
static uint8_t dirty_system   = 0;
static uint8_t dirty_hardware = 0;
static uint8_t dirty_user     = 0;
static uint8_t dirty_apps     = 0;
static uint64_t last_flush_sec = 0;
#define CODEX_FLUSH_INTERVAL  2   /* seconds between auto-flushes */
#define CODEX_CONFIG_DIR  "C:\\Impossible\\System\\Config\\Codex\\"

/* ---- String helpers ---- */

static uint32_t cx_strlen(const char *s)
{
    uint32_t len = 0;
    while (s[len]) len++;
    return len;
}

static void cx_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static int cx_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/* ---- Pool allocators ---- */

static codex_key_t *alloc_key(const char *name)
{
    codex_key_t *k;
    if (key_pool_next >= KEY_POOL_SIZE)
        return (void *)0;

    k = &key_pool[key_pool_next++];
    cx_strcpy(k->name, name, CODEX_MAX_NAME);
    k->parent   = (void *)0;
    k->children = (void *)0;
    k->sibling  = (void *)0;
    k->values   = (void *)0;
    return k;
}

static codex_value_t *alloc_value(const char *name, codex_type_t type)
{
    codex_value_t *v;
    if (value_pool_next >= VALUE_POOL_SIZE)
        return (void *)0;

    v = &value_pool[value_pool_next++];
    cx_strcpy(v->name, name, CODEX_MAX_NAME);
    v->type      = type;
    v->data_size = 0;
    v->next      = (void *)0;

    /* Zero the data union */
    {
        uint32_t i;
        for (i = 0; i < sizeof(v->data); i++)
            ((uint8_t *)&v->data)[i] = 0;
    }
    return v;
}

/* ---- Child key management ---- */

/* Find a child key by name under 'parent'. Returns NULL if not found. */
static codex_key_t *find_child(codex_key_t *parent, const char *name)
{
    codex_key_t *child;
    if (!parent) return (void *)0;

    child = parent->children;
    while (child) {
        if (cx_strcmp(child->name, name) == 0)
            return child;
        child = child->sibling;
    }
    return (void *)0;
}

/* Add a child key to 'parent'. */
static void add_child(codex_key_t *parent, codex_key_t *child)
{
    child->parent  = parent;
    child->sibling = parent->children;
    parent->children = child;
}

/* Remove a child key from its parent's child list. */
static void remove_child(codex_key_t *parent, codex_key_t *child)
{
    codex_key_t **pp;
    if (!parent) return;

    pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->sibling;
            child->sibling = (void *)0;
            child->parent = (void *)0;
            return;
        }
        pp = &(*pp)->sibling;
    }
}

/* ---- Value management ---- */

/* Find a value by name in a key. */
static codex_value_t *find_value(codex_key_t *key, const char *name)
{
    codex_value_t *v;
    if (!key) return (void *)0;

    v = key->values;
    while (v) {
        if (cx_strcmp(v->name, name) == 0)
            return v;
        v = v->next;
    }
    return (void *)0;
}

/* Add a value to a key's value list. */
static void add_value(codex_key_t *key, codex_value_t *val)
{
    val->next   = key->values;
    key->values = val;
}

/* ---- Path walking ---- */

/* Walk a backslash-separated path from 'root'.
 * If 'create' is non-zero, create missing keys along the way.
 * Returns the final key, or NULL if not found / allocation failed. */
static codex_key_t *walk_path(codex_key_t *root, const char *path, int create)
{
    codex_key_t *cur = root;
    char component[CODEX_MAX_NAME];
    uint32_t ci = 0;
    const char *p = path;

    /* Skip leading backslash */
    if (*p == '\\') p++;

    while (cur) {
        /* Extract next path component */
        ci = 0;
        while (*p && *p != '\\' && ci < CODEX_MAX_NAME - 1) {
            component[ci++] = *p++;
        }
        component[ci] = '\0';

        if (ci == 0)
            break;  /* trailing backslash or empty component */

        /* Look for existing child */
        {
            codex_key_t *child = find_child(cur, component);
            if (child) {
                cur = child;
            } else if (create) {
                child = alloc_key(component);
                if (!child)
                    return (void *)0;  /* pool exhausted */
                add_child(cur, child);
                cur = child;
            } else {
                return (void *)0;  /* not found */
            }
        }

        /* Skip backslash separator */
        if (*p == '\\') p++;
    }

    return cur;
}

/* ---- Public API ---- */

void codex_init(void)
{
    /* Allocate the invisible root node */
    codex_root = alloc_key("Codex");
    if (!codex_root) {
        printk("[FAIL] Codex: cannot allocate root\n");
        return;
    }

    /* Create the four standard root keys */
    {
        codex_key_t *system_key   = alloc_key("System");
        codex_key_t *hardware_key = alloc_key("Hardware");
        codex_key_t *user_key     = alloc_key("User");
        codex_key_t *apps_key     = alloc_key("Apps");

        if (!system_key || !hardware_key || !user_key || !apps_key) {
            printk("[FAIL] Codex: cannot allocate root keys\n");
            return;
        }

        add_child(codex_root, system_key);
        add_child(codex_root, hardware_key);
        add_child(codex_root, user_key);
        add_child(codex_root, apps_key);
    }

    codex_ready = 1;

    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Codex registry initialized (pool: %u keys, %u values)\n",
           (uint64_t)KEY_POOL_SIZE, (uint64_t)VALUE_POOL_SIZE);
}

codex_key_t *codex_open(const char *path)
{
    if (!codex_ready || !path)
        return (void *)0;
    return walk_path(codex_root, path, 0);
}

codex_key_t *codex_create(const char *path)
{
    if (!codex_ready || !path)
        return (void *)0;
    return walk_path(codex_root, path, 1);
}

/* Recursively mark a key and its children as freed.
 * (Pool memory is not actually reclaimed — keys are simply unlinked.) */
static void free_key_tree(codex_key_t *key)
{
    codex_key_t *child;
    if (!key) return;

    /* Recurse into children */
    child = key->children;
    while (child) {
        codex_key_t *next = child->sibling;
        free_key_tree(child);
        child = next;
    }

    /* Clear the key (mark as unused for debugging) */
    key->children = (void *)0;
    key->values   = (void *)0;
    key->sibling  = (void *)0;
    key->parent   = (void *)0;
    key->name[0]  = '\0';
}

int codex_delete_key(const char *path)
{
    codex_key_t *key;

    if (!codex_ready || !path)
        return -1;

    key = walk_path(codex_root, path, 0);
    if (!key || key == codex_root)
        return -1;

    /* Unlink from parent */
    remove_child(key->parent, key);

    /* Free the subtree */
    free_key_tree(key);

    return 0;
}

/* Forward declaration — defined in persistence section below */
static void auto_dirty(codex_key_t *key);

/* ---- Value accessors ---- */

int codex_get_string(codex_key_t *key, const char *name, char *buf, uint32_t buf_size)
{
    codex_value_t *v = find_value(key, name);
    if (!v || v->type != CODEX_STRING)
        return -1;
    cx_strcpy(buf, v->data.str, buf_size);
    return 0;
}

int codex_get_int32(codex_key_t *key, const char *name, int32_t *out)
{
    codex_value_t *v = find_value(key, name);
    if (!v || v->type != CODEX_INT32)
        return -1;
    *out = v->data.i32;
    return 0;
}

int codex_get_int64(codex_key_t *key, const char *name, int64_t *out)
{
    codex_value_t *v = find_value(key, name);
    if (!v || v->type != CODEX_INT64)
        return -1;
    *out = v->data.i64;
    return 0;
}

int codex_get_bool(codex_key_t *key, const char *name, uint8_t *out)
{
    codex_value_t *v = find_value(key, name);
    if (!v || v->type != CODEX_BOOL)
        return -1;
    *out = v->data.boolean;
    return 0;
}

int codex_set_string(codex_key_t *key, const char *name, const char *value)
{
    codex_value_t *v;
    if (!key || !name || !value)
        return -1;

    v = find_value(key, name);
    if (v) {
        /* Update existing value */
        v->type = CODEX_STRING;
        cx_strcpy(v->data.str, value, CODEX_MAX_STRING);
        v->data_size = cx_strlen(value) + 1;
        auto_dirty(key);
        return 0;
    }

    /* Create new value */
    v = alloc_value(name, CODEX_STRING);
    if (!v) return -1;
    cx_strcpy(v->data.str, value, CODEX_MAX_STRING);
    v->data_size = cx_strlen(value) + 1;
    add_value(key, v);
    auto_dirty(key);
    return 0;
}

int codex_set_int32(codex_key_t *key, const char *name, int32_t value)
{
    codex_value_t *v;
    if (!key || !name)
        return -1;

    v = find_value(key, name);
    if (v) {
        v->type = CODEX_INT32;
        v->data.i32 = value;
        v->data_size = 4;
        auto_dirty(key);
        return 0;
    }

    v = alloc_value(name, CODEX_INT32);
    if (!v) return -1;
    v->data.i32 = value;
    v->data_size = 4;
    add_value(key, v);
    auto_dirty(key);
    return 0;
}

int codex_set_int64(codex_key_t *key, const char *name, int64_t value)
{
    codex_value_t *v;
    if (!key || !name)
        return -1;

    v = find_value(key, name);
    if (v) {
        v->type = CODEX_INT64;
        v->data.i64 = value;
        v->data_size = 8;
        auto_dirty(key);
        return 0;
    }

    v = alloc_value(name, CODEX_INT64);
    if (!v) return -1;
    v->data.i64 = value;
    v->data_size = 8;
    add_value(key, v);
    auto_dirty(key);
    return 0;
}

int codex_set_bool(codex_key_t *key, const char *name, uint8_t value)
{
    codex_value_t *v;
    if (!key || !name)
        return -1;

    v = find_value(key, name);
    if (v) {
        v->type = CODEX_BOOL;
        v->data.boolean = value ? 1 : 0;
        v->data_size = 1;
        auto_dirty(key);
        return 0;
    }

    v = alloc_value(name, CODEX_BOOL);
    if (!v) return -1;
    v->data.boolean = value ? 1 : 0;
    v->data_size = 1;
    add_value(key, v);
    auto_dirty(key);
    return 0;
}

int codex_delete_value(codex_key_t *key, const char *name)
{
    codex_value_t **pp;
    if (!key || !name)
        return -1;

    pp = &key->values;
    while (*pp) {
        if (cx_strcmp((*pp)->name, name) == 0) {
            codex_value_t *doomed = *pp;
            *pp = doomed->next;
            doomed->name[0] = '\0';
            doomed->next = (void *)0;
            auto_dirty(key);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

/* ---- Enumeration ---- */

int codex_enum_keys(codex_key_t *key, uint32_t index, char *name, uint32_t size)
{
    codex_key_t *child;
    uint32_t i = 0;

    if (!key) return -1;

    child = key->children;
    while (child) {
        if (i == index) {
            cx_strcpy(name, child->name, size);
            return 0;
        }
        i++;
        child = child->sibling;
    }
    return -1;
}

codex_value_t *codex_enum_values(codex_key_t *key, uint32_t index)
{
    codex_value_t *v;
    uint32_t i = 0;

    if (!key) return (void *)0;

    v = key->values;
    while (v) {
        if (i == index)
            return v;
        i++;
        v = v->next;
    }
    return (void *)0;
}

/* ---- CPUID helper ---- */

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

/* ---- Pre-populated defaults ---- */

void codex_populate_defaults(void)
{
    codex_key_t *key;
    uint32_t count = 0;

    if (!codex_ready) return;

    /* --- System\Display\ --- */
    key = codex_create("System\\Display");
    if (key) {
        codex_set_int32(key, "Width",  (int32_t)fb_get_width());
        codex_set_int32(key, "Height", (int32_t)fb_get_height());
        codex_set_int32(key, "DPI",    96);   /* default DPI */
        codex_set_int32(key, "Scale",  100);  /* 100% scaling */
        count += 4;
    }

    /* --- System\Theme\ --- */
    key = codex_create("System\\Theme");
    if (key) {
        codex_set_string(key, "AccentColor",  "#0078D4");
        codex_set_bool(key, "DarkMode",       1);
        codex_set_string(key, "Font",         "Selawik");
        codex_set_int32(key, "FontSize",      12);
        codex_set_int32(key, "CornerRadius",  8);
        codex_set_string(key, "Wallpaper",    "C:\\Impossible\\Wallpapers\\default.raw");
        codex_set_string(key, "WallpaperMode", "stretch");
        codex_set_bool(key, "EnableAnimations", 1);
        count += 8;
    }

    /* --- System\Shell\ --- */
    key = codex_create("System\\Shell");
    if (key) {
        codex_set_int32(key, "TaskbarHeight",    48);
        codex_set_string(key, "TaskbarPosition", "bottom");
        codex_set_bool(key, "ShowClock",         1);
        codex_set_bool(key, "ShowStartButton",   1);
        count += 4;
    }

    /* --- System\Network\ --- */
    key = codex_create("System\\Network");
    if (key) {
        codex_set_string(key, "Hostname", "IMPOSSIBLE-PC");
        codex_set_bool(key, "DHCP",      1);
        codex_set_string(key, "DNS",      "8.8.8.8");
        count += 3;
    }

    /* --- System\DateTime\ --- */
    key = codex_create("System\\DateTime");
    if (key) {
        codex_set_bool(key, "Use24Hour",       0);
        codex_set_string(key, "DateFormat",    "MM/DD/YYYY");
        codex_set_int32(key, "TimezoneOffset", 0);  /* UTC */
        codex_set_string(key, "TimezoneName",  "UTC");
        codex_set_bool(key, "NTPEnabled",      1);
        count += 5;
    }

    /* --- Hardware\CPU\ --- */
    key = codex_create("Hardware\\CPU");
    if (key) {
        char vendor[13];
        uint32_t eax, ebx, ecx, edx;

        /* CPUID leaf 0: vendor string */
        cpuid(0, &eax, &ebx, &ecx, &edx);
        /* Vendor string is in EBX:EDX:ECX (12 chars) */
        {
            uint32_t *v32 = (uint32_t *)vendor;
            v32[0] = ebx;
            v32[1] = edx;
            v32[2] = ecx;
        }
        vendor[12] = '\0';
        codex_set_string(key, "Vendor", vendor);

        /* CPUID leaf 0x80000002-4: brand string (48 chars) */
        {
            char brand[49];
            uint32_t leaf;
            uint32_t idx = 0;
            for (leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
                cpuid(leaf, &eax, &ebx, &ecx, &edx);
                {
                    uint32_t *b32 = (uint32_t *)&brand[idx];
                    b32[0] = eax;
                    b32[1] = ebx;
                    b32[2] = ecx;
                    b32[3] = edx;
                }
                idx += 16;
            }
            brand[48] = '\0';
            /* Trim leading spaces */
            {
                const char *p = brand;
                while (*p == ' ') p++;
                codex_set_string(key, "Model", p);
            }
        }
        count += 2;
    }

    /* --- Hardware\Memory\ --- */
    key = codex_create("Hardware\\Memory");
    if (key) {
        uint64_t total_mb = (pmm_get_total_frames() * 4096) / (1024 * 1024);
        uint64_t free_mb  = (pmm_get_free_frames()  * 4096) / (1024 * 1024);
        codex_set_int64(key, "TotalMB", (int64_t)total_mb);
        codex_set_int64(key, "FreeMB",  (int64_t)free_mb);
        count += 2;
    }

    /* --- User\Default\ --- */
    key = codex_create("User\\Default");
    if (key) {
        codex_set_string(key, "HomeDir", "C:\\Users\\Default");
        codex_set_string(key, "Shell",   "C:\\shell.exe");
        count += 2;
    }

    key = codex_create("User\\Default\\Shell");
    if (key) {
        codex_set_string(key, "Prompt", "C:\\>");
        count += 1;
    }

    key = codex_create("User\\Default\\Desktop");
    if (key) {
        codex_set_string(key, "Wallpaper", "C:\\Impossible\\Wallpapers\\default.raw");
        count += 1;
    }

    fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
    printk("[OK] ");
    fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
    printk("Codex defaults populated (%u values, %u/%u pool used)\n",
           (uint64_t)count,
           (uint64_t)value_pool_next, (uint64_t)VALUE_POOL_SIZE);
}

/* ============================================================================
 * Disk Persistence — INI-style .codex files
 *
 * File format:
 *   [KeyPath]
 *   Name:STRING=value
 *   Name:INT32=12345
 *   Name:INT64=9876543210
 *   Name:BOOL=1
 *
 * One file per root key: system.codex, hardware.codex, user.codex, apps.codex
 * ============================================================================ */

/* ---- Dirty flag helpers ---- */

static uint8_t *dirty_flag_for(const char *root_name)
{
    if (cx_strcmp(root_name, "System") == 0)   return &dirty_system;
    if (cx_strcmp(root_name, "Hardware") == 0) return &dirty_hardware;
    if (cx_strcmp(root_name, "User") == 0)     return &dirty_user;
    if (cx_strcmp(root_name, "Apps") == 0)     return &dirty_apps;
    return (void *)0;
}

/* Walk up to find the root key name for a given key */
static const char *root_name_for_key(codex_key_t *key)
{
    codex_key_t *cur = key;
    while (cur && cur->parent && cur->parent != codex_root)
        cur = cur->parent;
    if (cur && cur->parent == codex_root)
        return cur->name;
    return (void *)0;
}

void codex_mark_dirty(const char *root_name)
{
    uint8_t *flag = dirty_flag_for(root_name);
    if (flag) *flag = 1;
}

/* Auto-mark dirty from a key pointer */
static void auto_dirty(codex_key_t *key)
{
    const char *rn = root_name_for_key(key);
    if (rn) codex_mark_dirty(rn);
}

/* ---- INI Serializer ---- */

/* Scratch buffer for building file content.
 * 16 KiB should be plenty for even a large tree. */
#define SERIALIZE_BUF_SIZE  16384
static char serialize_buf[SERIALIZE_BUF_SIZE];
static uint32_t ser_pos;

static void ser_reset(void) { ser_pos = 0; }

static void ser_putc(char c)
{
    if (ser_pos < SERIALIZE_BUF_SIZE - 1)
        serialize_buf[ser_pos++] = c;
}

static void ser_puts(const char *s)
{
    while (*s) ser_putc(*s++);
}

/* Integer to decimal string */
static void ser_puti64(int64_t val)
{
    char tmp[24];
    int i = 0;
    uint64_t uval;
    int neg = 0;

    if (val < 0) { neg = 1; uval = (uint64_t)(-val); }
    else          { uval = (uint64_t)val; }

    if (uval == 0) { tmp[i++] = '0'; }
    else {
        while (uval > 0) {
            tmp[i++] = '0' + (char)(uval % 10);
            uval /= 10;
        }
    }
    if (neg) ser_putc('-');
    while (i > 0) ser_putc(tmp[--i]);
}

static void ser_puti32(int32_t val) { ser_puti64((int64_t)val); }

/* Type name strings */
static const char *type_str(codex_type_t t)
{
    switch (t) {
        case CODEX_STRING: return "STRING";
        case CODEX_INT32:  return "INT32";
        case CODEX_INT64:  return "INT64";
        case CODEX_BOOL:   return "BOOL";
        case CODEX_BINARY: return "BINARY";
    }
    return "STRING";
}

/* Serialize all values in a key, with a [Section] header.
 * 'path' is the full path relative to the root key (e.g., "Display" or "Theme"). */
static void serialize_key(codex_key_t *key, const char *path)
{
    codex_value_t *v;
    codex_key_t *child;

    /* Write values if any */
    v = key->values;
    if (v) {
        ser_putc('[');
        ser_puts(path[0] ? path : ".");
        ser_puts("]\n");

        while (v) {
            ser_puts(v->name);
            ser_putc(':');
            ser_puts(type_str(v->type));
            ser_putc('=');
            switch (v->type) {
                case CODEX_STRING: ser_puts(v->data.str); break;
                case CODEX_INT32:  ser_puti32(v->data.i32); break;
                case CODEX_INT64:  ser_puti64(v->data.i64); break;
                case CODEX_BOOL:   ser_putc(v->data.boolean ? '1' : '0'); break;
                case CODEX_BINARY: ser_puts("(binary)"); break;
            }
            ser_putc('\n');
            v = v->next;
        }
        ser_putc('\n');
    }

    /* Recurse into children */
    child = key->children;
    while (child) {
        char subpath[CODEX_MAX_PATH];
        uint32_t pi = 0;
        const char *p;

        /* Build subpath */
        p = path;
        while (*p && pi < CODEX_MAX_PATH - 2) subpath[pi++] = *p++;
        if (pi > 0) subpath[pi++] = '\\';
        p = child->name;
        while (*p && pi < CODEX_MAX_PATH - 1) subpath[pi++] = *p++;
        subpath[pi] = '\0';

        serialize_key(child, subpath);
        child = child->sibling;
    }
}

/* Save a single root tree to disk */
static int save_tree(const char *root_name, const char *filename)
{
    codex_key_t *root_key;
    struct vfs_node *file;
    char path[CODEX_MAX_PATH];
    uint32_t pi = 0;
    const char *p;
    int rc;

    root_key = codex_open(root_name);
    if (!root_key) return -1;

    /* Build full file path */
    p = CODEX_CONFIG_DIR;
    while (*p && pi < CODEX_MAX_PATH - 1) path[pi++] = *p++;
    p = filename;
    while (*p && pi < CODEX_MAX_PATH - 1) path[pi++] = *p++;
    path[pi] = '\0';

    /* Serialize the tree */
    ser_reset();
    ser_puts("; Codex Registry — ");
    ser_puts(root_name);
    ser_puts("\n; Auto-generated, do not edit manually\n\n");
    serialize_key(root_key, "");

    /* Create and write file */
    vfs_create(path, VFS_FILE);
    file = vfs_open(path, VFS_O_WRITE | VFS_O_TRUNC);
    if (!file) return -1;

    rc = vfs_write(file, 0, ser_pos, (const uint8_t *)serialize_buf);
    vfs_close(file);

    return rc >= 0 ? 0 : -1;
}

int codex_save(void)
{
    int saved = 0;

    if (!codex_ready) return 0;

    if (dirty_system)   { if (save_tree("System",   "system.codex")   == 0) { dirty_system = 0;   saved++; } }
    if (dirty_hardware) { if (save_tree("Hardware", "hardware.codex") == 0) { dirty_hardware = 0; saved++; } }
    if (dirty_user)     { if (save_tree("User",     "user.codex")     == 0) { dirty_user = 0;     saved++; } }
    if (dirty_apps)     { if (save_tree("Apps",     "apps.codex")     == 0) { dirty_apps = 0;     saved++; } }

    return saved;
}

/* ---- INI Parser ---- */

/* Parse a type string back to codex_type_t */
static codex_type_t parse_type(const char *s)
{
    if (cx_strcmp(s, "INT32") == 0)  return CODEX_INT32;
    if (cx_strcmp(s, "INT64") == 0)  return CODEX_INT64;
    if (cx_strcmp(s, "BOOL") == 0)   return CODEX_BOOL;
    if (cx_strcmp(s, "BINARY") == 0) return CODEX_BINARY;
    return CODEX_STRING;
}

/* Parse a decimal integer from a string */
static int64_t parse_int(const char *s)
{
    int64_t result = 0;
    int neg = 0;

    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return neg ? -result : result;
}

/* Load a single .codex file and populate the tree */
static int load_file(const char *root_name, const char *filename)
{
    char path[CODEX_MAX_PATH];
    uint32_t pi = 0;
    const char *p;
    struct vfs_node *file;
    int n;
    char section[CODEX_MAX_PATH];
    uint32_t count = 0;

    /* Build full file path */
    p = CODEX_CONFIG_DIR;
    while (*p && pi < CODEX_MAX_PATH - 1) path[pi++] = *p++;
    p = filename;
    while (*p && pi < CODEX_MAX_PATH - 1) path[pi++] = *p++;
    path[pi] = '\0';

    file = vfs_open(path, VFS_O_READ);
    if (!file) return 0;

    /* Read entire file into serialize_buf (reuse it as read buffer) */
    n = vfs_read(file, 0, SERIALIZE_BUF_SIZE - 1, (uint8_t *)serialize_buf);
    vfs_close(file);

    if (n <= 0) return 0;
    serialize_buf[n] = '\0';

    /* Parse line by line */
    section[0] = '\0';
    {
        char *line = serialize_buf;
        while (*line) {
            /* Find end of line */
            char *eol = line;
            while (*eol && *eol != '\n') eol++;

            /* Null-terminate this line */
            if (*eol == '\n') { *eol = '\0'; eol++; }

            /* Skip empty lines and comments */
            if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
                line = eol;
                continue;
            }

            /* [Section] header */
            if (line[0] == '[') {
                char *end = line + 1;
                uint32_t si = 0;
                while (*end && *end != ']' && si < CODEX_MAX_PATH - 1)
                    section[si++] = *end++;
                section[si] = '\0';
                line = eol;
                continue;
            }

            /* Name:TYPE=Value line */
            {
                char name[CODEX_MAX_NAME];
                char type_s[16];
                char *val_start;
                uint32_t ni = 0, ti = 0;
                codex_key_t *key;
                char key_path[CODEX_MAX_PATH];
                uint32_t kpi = 0;

                /* Parse name */
                p = line;
                while (*p && *p != ':' && ni < CODEX_MAX_NAME - 1)
                    name[ni++] = *p++;
                name[ni] = '\0';
                if (*p == ':') p++;

                /* Parse type */
                while (*p && *p != '=' && ti < 15)
                    type_s[ti++] = *p++;
                type_s[ti] = '\0';
                if (*p == '=') p++;
                val_start = (char *)p;

                /* Build full key path: root_name\section (or just root_name if section is ".") */
                kpi = 0;
                p = root_name;
                while (*p && kpi < CODEX_MAX_PATH - 1) key_path[kpi++] = *p++;
                if (section[0] && cx_strcmp(section, ".") != 0) {
                    key_path[kpi++] = '\\';
                    p = section;
                    while (*p && kpi < CODEX_MAX_PATH - 1) key_path[kpi++] = *p++;
                }
                key_path[kpi] = '\0';

                key = codex_create(key_path);
                if (key && name[0]) {
                    codex_type_t type = parse_type(type_s);
                    switch (type) {
                        case CODEX_STRING:
                            codex_set_string(key, name, val_start);
                            break;
                        case CODEX_INT32:
                            codex_set_int32(key, name, (int32_t)parse_int(val_start));
                            break;
                        case CODEX_INT64:
                            codex_set_int64(key, name, parse_int(val_start));
                            break;
                        case CODEX_BOOL:
                            codex_set_bool(key, name, (val_start[0] == '1') ? 1 : 0);
                            break;
                        case CODEX_BINARY:
                            break;  /* skip binary for now */
                    }
                    count++;
                }
            }

            line = eol;
        }
    }

    return (int)count;
}

int codex_load(void)
{
    int total = 0;

    if (!codex_ready) return 0;
    if (!vfs_is_mounted('C')) return 0;

    total += load_file("System",   "system.codex");
    total += load_file("Hardware", "hardware.codex");
    total += load_file("User",     "user.codex");
    total += load_file("Apps",     "apps.codex");

    /* Clear dirty flags — loading shouldn't count as a change */
    dirty_system = dirty_hardware = dirty_user = dirty_apps = 0;

    if (total > 0) {
        fb_set_color(FB_COLOR_GREEN, FB_COLOR_BG_DEFAULT);
        printk("[OK] ");
        fb_set_color(FB_COLOR_FG_DEFAULT, FB_COLOR_BG_DEFAULT);
        printk("Codex loaded %u values from disk\n", (uint64_t)total);
    }

    return total;
}

void codex_flush(void)
{
    uint64_t now;

    if (!codex_ready) return;
    if (!vfs_is_mounted('C')) return;

    now = uptime();
    if (now - last_flush_sec < CODEX_FLUSH_INTERVAL)
        return;  /* too soon */

    last_flush_sec = now;

    if (dirty_system || dirty_hardware || dirty_user || dirty_apps) {
        codex_save();
    }
}
