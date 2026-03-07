# 55 — Dynamic Linking (.so / .dll)

## Overview

Load shared libraries at runtime. Required for: PE DLL stubs (advapi32, ws2_32), `.spl` Settings Panel applets, and plugin systems.

---

## How it works

```
1. App is loaded (ELF or PE)
2. Loader reads its dependency list (DT_NEEDED / Import Table)
3. For each dependency:
   a. Find library file (search path: C:\Impossible\System\, app directory)
   b. Load library into memory
   c. Relocate (fix up addresses)
   d. Resolve symbols (function pointers)
4. App can now call library functions
```

## ELF dynamic linking
```c
/* dynlink.h */
void *dlopen(const char *path);           /* load .so */
void *dlsym(void *handle, const char *symbol); /* find function */
int   dlclose(void *handle);               /* unload */
```

## PE import resolution (for Windows compatibility)
```c
/* PE loader resolves imports from DLL name table */
/* e.g., app imports "RegOpenKeyExA" from "advapi32.dll" */
/* → find our stub advapi32.dll → resolve symbol → patch IAT */
```

## Implementation phases
1. **Symbol table lookup** — scan ELF `.symtab`/`.dynsym` for exports (~200 lines)
2. **Relocation** — apply ELF `R_X86_64_*` relocations (~300 lines)
3. **Library loading** — `dlopen`/`dlsym` API (~200 lines)
4. **PE IAT patching** — for Windows DLL compatibility (~200 lines)

## Search paths (in order):
1. App's directory
2. `C:\Impossible\System\`
3. `C:\Impossible\System\Drivers\`

## Files: `src/kernel/dynlink.c` (~700 lines), `include/dynlink.h`
## Implementation: 2-3 weeks
