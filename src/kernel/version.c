/* ============================================================================
 * version.c — Kernel version information
 *
 * Provides runtime access to version strings injected at compile time.
 * The Makefile passes -DVERSION_MAJOR=x -DVERSION_MINOR=y etc. via CFLAGS.
 * ============================================================================ */

#include "kernel/version.h"
#include "kernel/printk.h"

/* Static version strings — computed at compile time from macros */
static const char ver_full[]  = KERNEL_VERSION_STRING;
static const char ver_short[] = KERNEL_VERSION_SHORT;
static const char ver_git[]   = VERSION_GIT_HASH;

const char *version_string(void)
{
    return ver_full;
}

const char *version_short(void)
{
    return ver_short;
}

const char *version_git_hash(void)
{
    return ver_git;
}

uint32_t version_build_number(void)
{
    return VERSION_BUILD;
}

void version_print(void)
{
    printk("  Impossible OS v%s (build %u, git %s)\n",
           ver_full, (uint64_t)VERSION_BUILD, ver_git);
}
