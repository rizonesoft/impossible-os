/*
 * Stand-in laptop devices for the reference disk under QEMU: an AC adapter,
 * a control-method battery and a lid, as a laptop's firmware describes them
 * (ACPI 6.5 sections 10.2 to 10.3 and 9.4), and backlight control on the
 * display. Windows binds its inbox drivers (cmbatt, the lid button), so
 * Settings shows the battery and lid pages.
 * run.py fills the @NAME@ values per variant before iasl compiles it.
 * Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 21.
 */
DefinitionBlock ("", "SSDT", 2, "IOS   ", "STANDIN ", 0x00000001)
{
    /* QEMU names the display at PCI slot 3 S18 (run.py pins it there). */
    External (\_SB.PCI0.S18, DeviceObj)

    /*
     * The display as a laptop's built-in panel with backlight control (ACPI 6.5
     * appendix B): _DOD lists one internal flat panel, LCD0 is that output, and
     * _BCL/_BCM/_BQC give its brightness levels, so Settings shows Brightness.
     */
    Scope (\_SB.PCI0.S18)
    {
        Method (_DOS, 1, NotSerialized) { }
        Method (_DOD, 0, NotSerialized) { Return (Package (0x01) { 0x80010400 }) }
        Device (LCD0)
        {
            Name (_ADR, 0x0400)
            Name (BRTL, 80)
            Method (_BCL, 0, NotSerialized)
            {
                Return (Package (0x0D) { 100, 50, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 })
            }
            Method (_BCM, 1, NotSerialized) { BRTL = Arg0 }
            Method (_BQC, 0, NotSerialized) { Return (BRTL) }
        }
    }

    Scope (\_SB)
    {
        Device (ADP1)
        {
            Name (_HID, "ACPI0003")
            Name (_PCL, Package (0x01) { \_SB })
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Method (_PSR, 0, NotSerialized) { Return (@AC_ONLINE@) }
        }

        Device (BAT0)
        {
            Name (_HID, EisaId ("PNP0C0A"))
            Name (_UID, One)
            Name (_PCL, Package (0x01) { \_SB })
            Method (_STA, 0, NotSerialized) { Return (0x1F) }
            /* mWh; design 50 Wh, full 48 Wh, warning 5 Wh, low 1 Wh, 11.1 V */
            Method (_BIF, 0, NotSerialized)
            {
                Return (Package (0x0D)
                {
                    Zero, 50000, 48000, One, 11100, 5000, 1000, 100, 100,
                    "Sample battery", "0001", "LION", "Sample"
                })
            }
            /* state (bit 0 discharging, bit 1 charging), rate mW, remaining mWh, mV */
            Method (_BST, 0, NotSerialized)
            {
                Return (Package (0x04) { @STATE@, @RATE@, @REMAINING@, 11100 })
            }
        }

        Device (LID0)
        {
            Name (_HID, EisaId ("PNP0C0D"))
            Method (_STA, 0, NotSerialized) { Return (0x0F) }
            Method (_LID, 0, NotSerialized) { Return (One) }
        }
    }
}
