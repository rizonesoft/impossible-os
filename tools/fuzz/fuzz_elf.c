/* libFuzzer harness: ELF64 header and program-header validation.
 *
 * Input is a whole executable as exec reads it. elf_parse
 * (src/kernel/elf_parse.c) makes every decision the loader takes from the
 * file; elf_load then copies what it accepted. The contract under test: no
 * input makes the parse read outside the buffer, and every PT_LOAD of an
 * accepted image is inside the file and the user ELF range, which is what the
 * copy trusts.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "kernel/elf.h"
#include "kernel/mm/user_range.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    struct elf_load_result r = elf_parse(data, size);
    struct elf64_header h;
    struct elf64_phdr ph;
    uint64_t phdr_vaddr;
    uint16_t phnum, phent;

    (void)elf_extract_phdr_info(data, size, &phdr_vaddr, &phnum, &phent);
    if (!r.success)
        return 0;

    __builtin_memcpy(&h, data, sizeof(h));
    for (uint16_t i = 0; i < h.e_phnum; i++) {
        elf_phdr_read(data, &h, i, &ph);
        if (ph.p_type != PT_LOAD)
            continue;
        if (ph.p_offset > size || ph.p_filesz > size - ph.p_offset ||
            ph.p_filesz > ph.p_memsz || ph.p_vaddr < USER_ELF_BASE ||
            ph.p_vaddr > USER_ELF_END || ph.p_memsz > USER_ELF_END - ph.p_vaddr)
            abort();
    }
    if (r.entry < r.load_base || r.entry >= r.load_end)
        abort();
    return 0;
}
