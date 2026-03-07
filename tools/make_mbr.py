import struct

# Create a 1 MiB disk image filled with zeros
disk = bytearray(1024 * 1024)

# MBR Signature
disk[510] = 0x55
disk[511] = 0xAA

def write_part(idx, status, ptype, start_lba, num_sectors):
    offset = 446 + (idx * 16)
    disk[offset] = status          # Status (0x80 = bootable)
    # CHS start (3 bytes, ignored)
    disk[offset+1] = 0; disk[offset+2] = 0; disk[offset+3] = 0
    disk[offset+4] = ptype         # Partition Type
    # CHS end (3 bytes, ignored)
    disk[offset+5] = 0; disk[offset+6] = 0; disk[offset+7] = 0
    # Start LBA (4 bytes, little-endian)
    struct.pack_into('<I', disk, offset+8, start_lba)
    # Sector count (4 bytes, little-endian)
    struct.pack_into('<I', disk, offset+12, num_sectors)

# Partition 1: FAT32 LBA (0x0C), Bootable, 16 MiB size
write_part(0, 0x80, 0x0C, 2048, 32768)

# Partition 2: Linux (0x83), 16 MiB size
write_part(1, 0x00, 0x83, 34816, 32768)

# Partition 3: IXFS (0xDA), 32 MiB size
write_part(2, 0x00, 0xDA, 67584, 65536)

# Partition 4: Empty
write_part(3, 0x00, 0x00, 0, 0)

import struct
import sys

if len(sys.argv) != 2:
    print("Usage: make_mbr.py <output_image_path>")
    sys.exit(1)

out_path = sys.argv[1]

with open(out_path, 'wb') as f:
    f.write(disk)

print(f"Created {out_path} (1MB disk with MBR)")
