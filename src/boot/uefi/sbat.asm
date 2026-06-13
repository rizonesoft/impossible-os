; ============================================================================
; SBAT revocation metadata section
;
; Embeds src/boot/uefi/sbat.csv as a .sbat ELF section so the ELF->PE
; objcopy step (-j .sbat in the Makefile) carries it into BOOTX64.EFI with
; valid content AND a loadable PE layout. Post-hoc objcopy --add-section is
; not viable: a GNU ELF->PE --add-section drops the section content, GNU
; PE->PE --add-section corrupts the PE optional header (firmware rejects the
; image as "Unsupported"), and llvm-objcopy --add-section produces a 0-byte
; PE section. Link-time embedding via incbin is the only reliable path.
;
; Single source of truth: sbat.csv. The build-time gate in scripts/build.sh
; byte-compares the embedded .sbat of both BOOTX64.EFI and BOOTX64.UKI.efi
; (the UKI inherits the section from the stub) against sbat.csv.
; ============================================================================

section .sbat progbits alloc noexec nowrite align=1
incbin "sbat.csv"
