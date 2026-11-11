/* ============================================================================
 * attest_syscall_info.h -- SystemAttestationReportInformation
 *
 * NtQuerySystemInformation(SystemAttestationReportInformation) returns this
 * boot's measured-boot attestation report: the same UTF-8 JSON document the
 * kernel writes to X:\Diag\attestation.json (schema 3, no trailing NUL), so a
 * user-mode reader and an offline reader parse one format. Impossible OS
 * extension class 0x1004, after 0x1000 kernel config, 0x1001 NLS, 0x1002
 * notification and 0x1003 resource pressure.
 *
 * ACCESS. Only a caller whose effective token is a member of BUILTIN\
 * Administrators (S-1-5-32-544) with the group ENABLED may read it; a filtered
 * administrator token (UAC split, the group deny-only) and a standard user get
 * STATUS_ACCESS_DENIED before any size is revealed. SYSTEM carries the group.
 * Decision recorded in todo/answers.md Q13. This is group membership, not a
 * privilege: no privilege an administrator holds enabled by default means
 * "administrator", and the evidence (EK certificate, AK, quote) identifies the
 * machine.
 *
 * SIZE. ReturnLength (when given) always receives the full document length.
 * A buffer smaller than that returns STATUS_INFO_LENGTH_MISMATCH and writes
 * nothing to it, so a caller queries the size first and then the bytes. When
 * no report was published this boot (no export yet, a build failure, or an
 * oversized report) the class returns STATUS_NOT_FOUND.
 *
 * FRESHNESS. The report is built once per boot with a kernel CSPRNG nonce. It
 * is evidence of how this boot was measured, not a fresh challenge-response;
 * a verifier-supplied nonce belongs to remote attestation (todo/answers.md
 * Q14, parked until a platform service needs it).
 * ========================================================================== */
#ifndef KERNEL_NT_ATTEST_SYSCALL_INFO_H
#define KERNEL_NT_ATTEST_SYSCALL_INFO_H

#include "kernel/types.h"

/* SYSTEM_INFORMATION_CLASS extension for NtQuerySystemInformation. */
#define SystemAttestationReportInformation  0x1004

#endif /* KERNEL_NT_ATTEST_SYSCALL_INFO_H */
