/* ============================================================================
 * elf_types.h -- minimal ELF64 types used by the bootloader.
 *
 * Extracted from bootx64.c so multiple translation units
 * (elf_bootproto.c, bootx64.c, future ELF consumers) can share one
 * definition. Field names + widths follow the ELF64 specification
 * (System V ABI, AMD64 Supplement).
 * ============================================================================ */

#ifndef SRC_BOOT_UEFI_ELF_TYPES_H
#define SRC_BOOT_UEFI_ELF_TYPES_H

#include "efi.h"

#define ELF_MAGIC 0x464C457Fu   /* "\x7FELF" little-endian */

/* ELF64 file header. Fields appear in the order required by the ELF64
 * specification; pad bytes are inlined so the struct matches the on-
 * disk layout exactly. */
typedef struct {
    UINT32 e_magic;        /* "\x7FELF" */
    UINT8  e_class;        /* 2 = 64-bit */
    UINT8  e_data;         /* 1 = little-endian */
    UINT8  e_version;
    UINT8  e_osabi;
    UINT8  e_pad[8];
    UINT16 e_type;
    UINT16 e_machine;      /* 0x3E = x86-64 */
    UINT32 e_version2;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

#define PT_LOAD 1

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

#define SHT_NULL    0
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_PROGBITS 1

typedef struct {
    UINT32 sh_name;        /* offset into shstrtab */
    UINT32 sh_type;
    UINT64 sh_flags;
    UINT64 sh_addr;
    UINT64 sh_offset;
    UINT64 sh_size;
    UINT32 sh_link;
    UINT32 sh_info;
    UINT64 sh_addralign;
    UINT64 sh_entsize;
} Elf64_Shdr;

typedef struct {
    UINT32 st_name;
    UINT8  st_info;
    UINT8  st_other;
    UINT16 st_shndx;
    UINT64 st_value;
    UINT64 st_size;
} Elf64_Sym;

#endif /* SRC_BOOT_UEFI_ELF_TYPES_H */
