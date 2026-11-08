/* uki_payload_limits.h -- bounds on the UKI signed payload copies.
 *
 * Shared by the UEFI bootloader (bootx64.c, which copies the .initrd /
 * .recovery / .modules PE sections into EfiLoaderData before
 * ExitBootServices and publishes the copies in boot_info v14) and the kernel
 * (boot_reserved.c, which pins those copies so the PMM never hands their
 * frames out). One definition, so the producer's refusal and the consumer's
 * admission cannot drift apart.
 *
 * Preprocessor constants only: this file compiles under the loader's UEFI
 * types and the kernel's freestanding types alike.
 */
#ifndef UKI_PAYLOAD_LIMITS_H
#define UKI_PAYLOAD_LIMITS_H

/* Per-payload sanity ceiling. A legitimate initrd is a few MiB; a
 * multi-hundred-MiB section is an attack or a build mistake, and the boot
 * aborts visibly rather than trying to allocate it. */
#define UKI_PAYLOAD_MAX_BYTES (256ULL * 1024ULL * 1024ULL)

/* Highest physical address a copy may END at. The kernel reads the copies
 * through the boot identity map, which covers the first 4 GiB only
 * (BOOT_INFO_EARLY_MAP_END in include/kernel/boot_info.h), so the loader
 * allocates them with AllocateMaxAddress at this ceiling and the kernel
 * refuses anything beyond it. */
#define UKI_PAYLOAD_MAX_ADDRESS 0xFFFFFFFFULL

/* The three payload slots, in UKI section order (load-bearing for measured
 * boot, docs/guides/secure-boot-keys.md). The kernel records the slot as the
 * reservation's source index. */
#define UKI_PAYLOAD_SLOT_INITRD   0u
#define UKI_PAYLOAD_SLOT_RECOVERY 1u
#define UKI_PAYLOAD_SLOT_MODULES  2u
#define UKI_PAYLOAD_SLOT_COUNT    3u

#endif /* UKI_PAYLOAD_LIMITS_H */
