/* libFuzzer harness: the bounded SID / ACL / security-descriptor validators.
 *
 * These are the trust boundary for a security descriptor imported from disk,
 * the registry or NtSetSecurityObject (include/kernel/security/acl.h). The
 * contract under test: no call reads past `size` bytes of the input, and an
 * ACL that RtlValidAcl accepts is safe for the walkers that trust it.
 * libFuzzer hands each input in an exact-size heap block, so AddressSanitizer
 * reports the first byte read past it.
 */
#include <stddef.h>
#include <stdint.h>

#include "kernel/security/acl.h"
#include "kernel/security/sid.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t workspace[65536 + 64];
    static char text[4096];
    SECURITY_DESCRIPTOR abs;
    uint32_t avail;

    if (size > 0xFFFFu)
        return 0;
    avail = (uint32_t)size;

    (void)RtlLengthSidBounded((const SID *)data, avail);

    if (RtlValidAcl((const ACL *)data, avail)) {
        const ACL *acl = (const ACL *)data;
        ACE_HEADER *ace;
        for (uint32_t i = 0; i < acl->AceCount; i++)
            (void)RtlGetAceEx(acl, i, &ace, avail);
        (void)RtlAclToCStr(acl, text, sizeof(text));
    }

    (void)RtlSelfRelativeToAbsoluteSD(data, avail, &abs, workspace,
                                      sizeof(workspace));
    return 0;
}
