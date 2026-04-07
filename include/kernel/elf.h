/* ============================================================================
 * elf.h -- ELF64 binary format definitions
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
#define ET_EXEC         2           /* executable (fixed address) */
#define ET_DYN          3           /* shared / PIE (position-independent) */

/* ELF machine */
#define EM_X86_64       62

/* Program header types */
#define PT_NULL         0
#define PT_LOAD         1           /* loadable segment */
#define PT_INTERP       3           /* path to dynamic linker */
#define PT_PHDR         6           /* program header table itself */
#define PT_GNU_STACK    0x6474E551  /* stack executability control */
#define PT_GNU_RELRO    0x6474E552  /* read-only after relocation */
#define PT_GNU_PROPERTY 0x6474E553  /* GNU property notes (CET flags) */

/* Program header flags */
#define PF_X            0x1         /* execute */
#define PF_W            0x2         /* write */
#define PF_R            0x4         /* read */

/* GNU property note types (inside PT_GNU_PROPERTY) */
#define GNU_PROPERTY_X86_FEATURE_1_AND  0xC0000002
#define GNU_PROPERTY_X86_FEATURE_1_IBT  (1u << 0)
#define GNU_PROPERTY_X86_FEATURE_1_SHSTK (1u << 1)

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

/* ELF auxiliary vector type constants (Linux ABI -- see elf(5) and Linux
 * include/uapi/linux/auxvec.h). Used by glibc/musl crt and dynamic linker
 * to read process startup metadata from the initial user stack. The values
 * are a hard ABI contract: they must match Linux exactly or user-mode C
 * libraries will misinterpret the auxv block. -> XREF: TODO-04 §13 */
#define AT_NULL    0    /* end of vector (terminator) */
#define AT_IGNORE  1    /* entry should be ignored */
#define AT_EXECFD  2    /* file descriptor of program */
#define AT_PHDR    3    /* program headers for program */
#define AT_PHENT   4    /* size of program header entry */
#define AT_PHNUM   5    /* number of program headers */
#define AT_PAGESZ  6    /* system page size */
#define AT_BASE    7    /* base address of dynamic linker (0 if static) */
#define AT_FLAGS   8    /* flags */
#define AT_ENTRY   9    /* entry point of program */
#define AT_NOTELF 10    /* program is not ELF */
#define AT_UID    11    /* real uid */
#define AT_EUID   12    /* effective uid */
#define AT_GID    13    /* real gid */
#define AT_EGID   14    /* effective gid */
#define AT_PLATFORM 15  /* string identifying CPU for optimizations */
#define AT_HWCAP  16    /* arch-dependent hints at CPU capabilities */
#define AT_CLKTCK 17    /* frequency at which times() increments */
#define AT_SECURE 23    /* secure mode boolean (1 if setuid/setgid) */
#define AT_BASE_PLATFORM 24  /* string identifying base platform */
#define AT_RANDOM 25    /* address of 16 random bytes (stack canary seed) */
#define AT_HWCAP2 26    /* extension of AT_HWCAP */
#define AT_EXECFN 31    /* filename of program */

/* Result of loading an ELF */
struct elf_load_result {
    uint64_t entry;                 /* entry point address */
    uint64_t load_base;             /* lowest loaded address */
    uint64_t load_end;              /* highest loaded address + 1 */
    int      success;               /* 1 on success, 0 on failure */
    /* --- Security metadata (§3) --- */
    uint8_t  nx_stack;              /* 1 = stack should be non-executable */
    uint8_t  has_relro;             /* 1 = PT_GNU_RELRO present */
    uint8_t  cet_ibt;              /* 1 = GNU_PROPERTY requests IBT */
    uint8_t  cet_shstk;            /* 1 = GNU_PROPERTY requests SHSTK */
    uint64_t relro_start;           /* RELRO range start (0 if no RELRO) */
    uint64_t relro_size;            /* RELRO range size */
    /* --- ELF auxv metadata (§13) ---
     * Populated by elf_load() in the same parse pass that loads PT_LOAD
     * segments. Used by task_exec() to push AT_PHDR/AT_PHENT/AT_PHNUM into
     * the initial user stack. The values are derived from PT_PHDR if
     * present, otherwise from the PT_LOAD that contains e_phoff. */
    uint64_t phdr_vaddr;            /* virtual address of program headers (0 if unavailable) */
    uint16_t phnum;                 /* number of program headers (0 if unavailable) */
    uint16_t phent;                 /* size of one program header entry (0 if unavailable) */
    uint16_t _pad_phdr[2];          /* alignment */
};

/* Validate and load an ELF64 executable from memory.
 * 'data' points to the raw ELF file, 'size' is the file size.
 * Returns the load result with entry point. */
struct elf_load_result elf_load(const uint8_t *data, uint64_t size);

/* Extract program-header metadata from an ELF64 image without copying any
 * segments. Used by task_exec() to populate AT_PHDR/AT_PHENT/AT_PHNUM in the
 * auxv after exec_load() has already loaded the segments via the format
 * dispatcher (which discards everything except the entry point).
 *
 * On success, fills *phdr_vaddr / *phnum / *phent with the values that
 * elf_load() would have computed for the same image, and returns 1.
 * On failure (data too small, bad magic, headers don't fit, no PT_LOAD
 * containing e_phoff), returns 0 and leaves *phdr_vaddr=0 / *phnum=0 /
 * *phent=0.
 *
 * Out parameters may not be NULL. The implementation shares its program-
 * header walk with elf_load() so the two paths cannot diverge. */
int elf_extract_phdr_info(const uint8_t *data, uint64_t size,
                          uint64_t *phdr_vaddr, uint16_t *phnum,
                          uint16_t *phent);
