---
description: Complete FAT32 filesystem test workflow using test disk images
---

# FAT32 Test Workflow

// turbo-all

> Fully automated: builds, generates test disks, launches QEMU with the FAT32
> test disk on AHCI port 1, captures serial output to serial.log, verifies
> expected boot log patterns, and leaves QEMU running for manual shell testing.
>
> Drive letter mapping:
>   C:\ = IXFS (system partition)
>   D:\ = first non-EFI FAT32 partition (e.g., test disk)
>   EFI partition = hidden (no drive letter, like Windows)

## Steps

1. Clean and build:
```bash
make clean && make all
```

2. Generate test disks (cached if already exist):
```bash
make test-disks
```

3. Remove stale serial log:
```bash
rm -f serial.log
```

4. Copy OVMF vars for QEMU:
```bash
cp /usr/share/OVMF/OVMF_VARS_4M.fd build/OVMF_VARS_4M.fd
```

5. Launch QEMU in background with FAT32 test disk on AHCI port 1 and serial to file:
```bash
qemu-system-x86_64 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=build/OVMF_VARS_4M.fd \
  -drive id=disk0,file=build/system-disk.img,format=raw,if=none \
  -drive id=testdisk,file=build/test-disks/fat32.img,format=raw,if=none \
  -device ich9-ahci,id=ahci0 \
  -device ide-hd,drive=disk0,bus=ahci0.0 \
  -device ide-hd,drive=testdisk,bus=ahci0.1 \
  -m 2G \
  -serial file:serial.log \
  -vga none \
  -device VGA,xres=1280,yres=720 \
  -device rtl8139,netdev=net0 \
  -netdev user,id=net0 \
  -device virtio-tablet-pci \
  -rtc base=localtime \
  -no-reboot &
```

6. Wait for the kernel to finish booting (8 seconds):
```bash
sleep 8
```

7. Verify boot log — check that AHCI detected 2 disks and FAT32 partitions found:
```bash
echo "=== FAT32 Test Disk Verification ===" && \
grep -c "AHCI" serial.log | xargs -I{} echo "[CHECK] AHCI references: {}" && \
grep "Disk 0" serial.log | head -2 && \
grep "Disk 1" serial.log | head -2 && \
echo "---" && \
grep "FAT32" serial.log | head -5 && \
echo "---" && \
grep "VFS.*mounted" serial.log && \
echo "---" && \
if grep -q "Disk 1" serial.log; then echo "[PASS] Disk 1 (FAT32 test disk) detected on AHCI port 1"; else echo "[FAIL] Disk 1 NOT detected — check AHCI port 1"; fi && \
echo "=== QEMU is still running — switch to the QEMU window for manual shell testing ==="
```

## What to Test Manually in the QEMU Shell

Once verified, switch to the QEMU window and use the shell:

```
dir D:\                    # List FAT32 test disk root
type D:\test.txt           # Should print: "Hello from FAT32 test disk!"
dir D:\subdir              # List subdirectory
type D:\subdir\nested.txt  # Should print: "Nested file in subdirectory"
```

## Troubleshooting

- **Disk 1 not detected:** Check that `build/test-disks/fat32.img` exists and is valid
- **FAT32 not mounted:** Check `partition.c` FAT32 probe — BPB magic `0xAA55`
- **serial.log empty:** QEMU may not have started — check for port conflicts
- **Regenerate test disk:** `rm -f build/test-disks/fat32.img && make test-disks`
