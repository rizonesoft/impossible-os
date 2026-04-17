/* ============================================================================
 * dump-mirror.c -- emit build/boot-info-abi.mirror.json
 *
 * Host binary. Includes src/boot/uefi/boot_info_mirror.h (the bootloader's
 * view of struct boot_info, written in UEFI UINT* types) after typedef'ing
 * UINT8/16/32/64 from stdint, then dumps every field via the same
 * dump-fields.inc macro list the kernel dumper uses.
 *
 * Paired with tools/boot-info-manifest/dump-kernel.c. The comparison script
 * tools/boot-info-manifest/compare.sh fails the build on the first row that
 * disagrees by name / offset / size.
 * ============================================================================ */

#include <stdint.h>
#include <stddef.h>

/* Provide the UEFI type names the mirror header uses, without pulling in
 * efi.h (which drags in UEFI-specific macros this host tool does not
 * need). The fixed-width typedefs are bit-for-bit identical to what
 * efi.h defines. */
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;

/* Same CHAR8 typedef as efi.h so `char cmdline[...]` matches layout. */
typedef char CHAR8;

#include "boot_info_mirror.h"

#include "dump-common.h"

int main(void) {
    manifest_begin("mirror", BOOT_INFO_VERSION, sizeof(struct boot_info));
#include "dump-fields.inc"
    manifest_end();
    return 0;
}
