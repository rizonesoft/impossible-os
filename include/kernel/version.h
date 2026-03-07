/* ============================================================================
 * version.h — Kernel version information
 *
 * Version numbers are injected at build time by the Makefile from:
 *   - VERSION          (SemVer: MAJOR.MINOR.PATCH)
 *   - .build_number    (auto-incremented on each build)
 *   - Git HEAD hash    (short 8-char commit hash)
 *
 * The generated header build/generated/version_info.h provides the #defines.
 * This header declares the public API for accessing version info at runtime.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ---- Version components (injected by Makefile) ---- */

#ifndef VERSION_MAJOR
#define VERSION_MAJOR  0
#endif
#ifndef VERSION_MINOR
#define VERSION_MINOR  1
#endif
#ifndef VERSION_PATCH
#define VERSION_PATCH  0
#endif
#ifndef VERSION_BUILD
#define VERSION_BUILD  0
#endif
#ifndef VERSION_GIT_HASH
#define VERSION_GIT_HASH "unknown"
#endif

/* ---- Stringification helpers ---- */

#define _VER_STR(x) #x
#define VER_STR(x)  _VER_STR(x)

/* Full version string: "0.1.0.42" (major.minor.patch.build) */
#define KERNEL_VERSION_STRING \
    VER_STR(VERSION_MAJOR) "." \
    VER_STR(VERSION_MINOR) "." \
    VER_STR(VERSION_PATCH) "." \
    VER_STR(VERSION_BUILD)

/* Short version: "0.1.0" */
#define KERNEL_VERSION_SHORT \
    VER_STR(VERSION_MAJOR) "." \
    VER_STR(VERSION_MINOR) "." \
    VER_STR(VERSION_PATCH)

/* ---- API ---- */

/* Get the full version string (e.g., "0.1.0.42") */
const char *version_string(void);

/* Get the short version (e.g., "0.1.0") */
const char *version_short(void);

/* Get the Git commit hash (e.g., "a1b2c3d4") */
const char *version_git_hash(void);

/* Get the build number */
uint32_t version_build_number(void);

/* Print full version info to serial + console */
void version_print(void);
