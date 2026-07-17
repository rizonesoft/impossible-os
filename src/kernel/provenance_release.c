/* ============================================================================
 * provenance_release.c -- release-flavor provenance marker
 *
 * This translation unit is linked into kernel.exe ONLY when the build is cut
 * at KERNEL_TESTS=off (the release/pruned flavor). The Makefile excludes it
 * from the auto-glob and re-adds it exclusively under `ifeq ($(KERNEL_TESTS),
 * off)`, so a test-flavor (KERNEL_TESTS=on) kernel never carries the marker.
 *
 * It emits a single NON-ALLOC ELF section `.ipos.provenance` holding a magic
 * string. Non-alloc means the section carries no SHF_ALLOC flag and is not
 * covered by any PT_LOAD program header, so the UEFI bootloader
 * (src/boot/uefi/bootx64.c) -- which loads PT_LOAD segments only -- never sees
 * it and the boot path is unaffected. Precedent: clang already emits a
 * non-alloc `.comment` orphan section (Address 0, flags MS, no A) into every
 * kernel.exe today and it boots on bare metal; this marker behaves identically.
 *
 * Because the marker is present at LINK TIME, kernel.exe is born with it and
 * every derived artifact (kernel.map, kernel.sym, the disk image) is naturally
 * consistent -- there is no post-hoc objcopy mutation that would stale a
 * signature or symbol map.
 *
 * The marker attests FLAVOR (this image was pruned of the test surface); the
 * no-leak PROOF is scripts/check-release-symbols.sh, which packaging paths run
 * in addition to `--verify-provenance`. The magic string below is the shared
 * format contract with that script (it greps the section for `flavor=off`);
 * keep the two in sync.
 * ==========================================================================*/

/*
 * The empty flag string ("") makes the section non-alloc (no SHF_ALLOC); a
 * default C `static const` would otherwise land in .rodata with SHF_ALLOC.
 * @progbits keeps real bytes on disk. .pushsection/.popsection and @progbits
 * are generic ELF assembler directives (no CPU instructions), so this file
 * stays architecture-neutral. The string is versioned so a future format
 * change is detectable, and carries `flavor=off` for the gate to match.
 */
__asm__(
    ".pushsection .ipos.provenance,\"\",@progbits\n"
    ".ascii \"IPOS-RELEASE-PROVENANCE-V1 flavor=off\\0\"\n"
    ".popsection\n");
