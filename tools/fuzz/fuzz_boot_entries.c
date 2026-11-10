/* libFuzzer harness: the boot entry store parser.
 *
 * Input is the raw bytes of \EFI\...\bootentries.json on the ESP, which the
 * loader reads before any signature is checked, so every byte is attacker
 * controlled. The parser TU is included directly, the same way
 * tools/boot-entries-parser-tests does, so the code under test is the loader's
 * own, not a copy.
 */
#include <stddef.h>
#include <stdint.h>

#include "../../src/boot/uefi/boot_entries_parser.c"

static boot_entries_parse_result_t g_out;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 0xFFFFFFFFu)
        return 0;
    /* Both Secure Boot states: the flag is a grammar gate in the contract. */
    (void)boot_entries_parse(data, (unsigned int)size, 0, 0, &g_out);
    (void)boot_entries_parse(data, (unsigned int)size, 1, 0, &g_out);
    return 0;
}
