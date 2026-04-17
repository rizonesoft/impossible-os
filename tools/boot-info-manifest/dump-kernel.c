/* ============================================================================
 * dump-kernel.c -- emit build/boot-info-abi.kernel.json
 *
 * Host binary. Includes include/kernel/boot_info.h (the kernel's view of
 * struct boot_info) and dumps every field's offset + size as JSON.
 * Compiled with clang-19 targeting x86_64-elf so the host layout matches
 * the target kernel layout exactly.
 *
 * tools/boot-info-manifest/dump-mirror.c is the bootloader counterpart;
 * tools/boot-info-manifest/compare.sh diffs the two JSONs and names the
 * first mismatching field. Canonical per-field ownership lives in
 * docs/boot/boot-info-fields.md.
 * ============================================================================ */

/* The kernel header pulls in kernel/types.h which declares uint8_t etc.
 * Provide a minimal <stdint.h> include first so this translation unit has
 * matching fixed-width types before boot_info.h is evaluated. */
#include <stdint.h>
#include <stddef.h>

/* Redirect kernel/types.h expectations. kernel/types.h is a thin typedef
 * layer on top of stdint.h in freestanding builds; we can include it
 * directly because the host's libc already provides the same types. */
#include "kernel/types.h"

/* Tell boot_init.h that boot_result_t is an enum -- defined in that header
 * and referenced by boot_info.h's validator prototypes. */
#include "kernel/boot_init.h"
#include "kernel/boot_info.h"

#include "dump-common.h"

int main(void) {
    manifest_begin("kernel", BOOT_INFO_VERSION, sizeof(struct boot_info));
#include "dump-fields.inc"
    manifest_end();
    return 0;
}
