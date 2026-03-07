/* ============================================================================
 * elf.h — ELF64 binary format definitions
 *
 * Structures and constants for loading 64-bit ELF executables.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* ELF magic bytes */
#define ELF_MAGIC       0x464C457F  /* "\x7FELF" as little-endian uint32 */

/* ELF class */
#define ELFCLASS64      2

/* ELF data encoding */
#define ELFDATA2LSB     1           /* little-endian */

/* ELF type */
#define ET_EXEC         2           /* executable */

/* ELF machine */
#define EM_X86_64       62

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1           /* loadable segment */

/* Program header flags */
#define PF_X            0x1         /* execute */
#define PF_W            0x2         /* write */
#define PF_R            0x4         /* read */

/* ELF64 file header */
struct elf64_header {
    uint8_t  e_ident[16];           /* magic + class + encoding + ... */
    uint16_t e_type;                /* ET_EXEC, ET_DYN, etc. */
    uint16_t e_machine;             /* EM_X86_64 */
    uint32_t e_version;
    uint64_t e_entry;               /* entry point virtual address */
    uint64_t e_phoff;               /* program header table offset */
    uint64_t e_shoff;               /* section header table offset */
    uint32_t e_flags;
    uint16_t e_ehsize;              /* ELF header size */
    uint16_t e_phentsize;           /* program header entry size */
    uint16_t e_phnum;               /* number of program headers */
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

/* ELF64 program header */
struct elf64_phdr {
    uint32_t p_type;                /* PT_LOAD, etc. */
    uint32_t p_flags;               /* PF_R | PF_W | PF_X */
    uint64_t p_offset;              /* offset in file */
    uint64_t p_vaddr;               /* virtual address */
    uint64_t p_paddr;               /* physical address (unused) */
    uint64_t p_filesz;              /* size in file */
    uint64_t p_memsz;               /* size in memory (>= filesz, diff is BSS) */
    uint64_t p_align;               /* alignment */
} __attribute__((packed));

/* Result of loading an ELF */
struct elf_load_result {
    uint64_t entry;                 /* entry point address */
    uint64_t load_base;             /* lowest loaded address */
    uint64_t load_end;              /* highest loaded address + 1 */
    int      success;               /* 1 on success, 0 on failure */
};

/* Validate and load an ELF64 executable from memory.
 * 'data' points to the raw ELF file, 'size' is the file size.
 * Returns the load result with entry point. */
struct elf_load_result elf_load(const uint8_t *data, uint64_t size);
