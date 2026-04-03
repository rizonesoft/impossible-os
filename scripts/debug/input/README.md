# Input Device Test Runners

Test runners for PS/2 and USB HID input devices.

## PS/2 (working now)

| Script | Accel | What it tests |
|--------|-------|---------------|
| `run-ps2-keyboard-test.bat` | WHPX | PS/2 keyboard with forced i8042 in ACPI |
| `run-ps2-keyboard-test-tcg.bat` | TCG | Same on software emulation |

QEMU's default ACPI tables report `IAPC_BOOT_ARCH.8042=0` on WHPX, which causes the kernel to skip PS/2 keyboard init. These runners add `-machine pc,i8042=on` to force the i8042 controller, enabling keyboard IRQ delivery.

## USB HID (placeholder -- not yet implemented)

| Script | Accel | What it tests |
|--------|-------|---------------|
| `run-usb-hid-keyboard-test.bat` | WHPX | USB keyboard via xHCI |
| `run-usb-hid-mouse-test.bat` | WHPX | USB mouse via xHCI |

These add QEMU USB devices but require the xHCI driver + USB HID class driver to be implemented in the kernel. Currently the devices will be detected by PCI scan but not functional.
