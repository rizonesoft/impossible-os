# 81 — Kernel Module / Driver Loading

## Overview
Load drivers and kernel extensions at runtime without recompiling. Like Windows `.sys` files or Linux kernel modules.

## Format: `.kmod` files (ELF relocatable objects)
```c
/* kmod.h */
struct kmod {
    char name[32];
    int (*init)(void);     /* called on load */
    void (*cleanup)(void); /* called on unload */
};

int kmod_load(const char *path);    /* load and call init() */
int kmod_unload(const char *name);  /* call cleanup() and free */
int kmod_list(struct kmod *out, int max);
```

## Use cases: load GPU driver, NIC driver, filesystem driver at boot based on detected hardware
## Files: `src/kernel/kmod.c` (~300 lines) | 1-2 weeks
