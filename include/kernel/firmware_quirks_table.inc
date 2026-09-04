/* ============================================================================
 * firmware_quirks_table.inc -- Shared X-macro for firmware quirk identity
 *
 * Single source of truth for FW_QUIRK_<id> bit value and canonical name.
 * Consumed by:
 *   - src/kernel/firmware_quirks.c       (kernel descriptor table s_quirks[])
 *   - src/boot/uefi/bootx64.c            (bootloader override parser qmap[])
 *
 * Caller defines FW_QUIRK_DEF(id, bit_value, name_str) before #include and
 * undefs it after. Bit values fixed; names are the canonical lowercase
 * tokens consumed by `boot.conf firmware_quirk_disable=...`.
 * ============================================================================ */

FW_QUIRK_DEF(BROKEN_FPDT,           0x01u, "broken_fpdt")
FW_QUIRK_DEF(BAD_MADT_CHECKSUM,     0x02u, "bad_madt_checksum")
FW_QUIRK_DEF(GOP_PITCH_LIES,        0x04u, "gop_pitch_lies")
FW_QUIRK_DEF(BOGUS_MAT,             0x08u, "bogus_mat")
FW_QUIRK_DEF(USB_HANDOFF_BLACKLIST, 0x10u, "usb_handoff_blacklist")
FW_QUIRK_DEF(EC_ECDT_PORTS_SWAPPED, 0x20u, "ec_ecdt_ports_swapped")
