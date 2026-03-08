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
        return 0;
    }

    /* Create new value */
    v = alloc_value(name, CODEX_STRING);
    if (!v) return -1;
    cx_strcpy(v->data.str, value, CODEX_MAX_STRING);
    v->data_size = cx_strlen(value) + 1;
    add_value(key, v);
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
        return 0;
    }

    v = alloc_value(name, CODEX_INT32);
    if (!v) return -1;
    v->data.i32 = value;
    v->data_size = 4;
    add_value(key, v);
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
        return 0;
    }

    v = alloc_value(name, CODEX_INT64);
    if (!v) return -1;
    v->data.i64 = value;
    v->data_size = 8;
    add_value(key, v);
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
        return 0;
    }

    v = alloc_value(name, CODEX_BOOL);
    if (!v) return -1;
    v->data.boolean = value ? 1 : 0;
    v->data_size = 1;
    add_value(key, v);
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
