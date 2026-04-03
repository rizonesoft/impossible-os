/* ============================================================================
 * version.h -- Kernel version information
 *
 * Version numbers come from the auto-generated include/build_info.h header,
 * which is created by the Makefile's build-info target before compilation.
 * This header declares the public API for accessing version info at runtime.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "build_info.h"

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

/* Get the Git branch name (e.g., "main") */
const char *version_branch(void);

/* Get the build timestamp (e.g., "2026-03-17T12:00:00Z") */
const char *version_timestamp(void);

/* Get the build number */
uint32_t version_build_number(void);

/* Print full version info to serial + console */
void version_print(void);
