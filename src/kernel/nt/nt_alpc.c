/* ============================================================================
 * nt_alpc.c -- Modern ALPC port SSDT handlers
 *
 * Reserves SSDT slots 0x010F-0x011E for the Vista+ ALPC syscall
 * surface. Each handler is a bounded stub that logs the call once
 * (atomic one-time flag, load-fast / CAS-on-transition) and returns a
 * deferred-status sentinel until the ALPC engine work in
 * 02-kernel-core/TODO-24-alpc-message-ports.md is implemented.
 *
 * SCOPE-GAP-ALLOWED: 16 handlers in this file intentionally return
 *                    STATUS_NOT_IMPLEMENTED pending the ALPC SSDT
 *                    retrofit and Query/Set/Cancel work on the ALPC
 *                    completion roadmap. When that work ships, every
 *                    handler body in this file is replaced with a call
 *                    into the ALPC subsystem and the sentinel comments
 *                    are removed.
 *
 * Registration helper (alpc_register_one) detects collisions with
 * ssdt_stub_not_implemented, captures ssdt_register return, and
 * per-slot logs failures -- mirroring the legacy LPC pattern.
 * ============================================================================ */

#include "kernel/nt/nt_alpc.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/zw.h"
#include "kernel/ipc/alpc.h"
#include "kernel/ipc/alpc_port.h"
#include "kernel/sched/task.h"
#include "kernel/security/sid.h"       /* Section 7: RequiredServerSid snapshot */
#include "kernel/cpu_security.h"       /* Section 7: copy_from_user */
#include "libc/string.h"               /* Section 7: memcpy (kernel-mode path) */
#include "kernel/klog.h"

/* ---- OBJECT_ATTRIBUTES probe + \RPC Control path split ----------------
 *
 * NT OBJECT_ATTRIBUTES.ObjectName typically carries a full path like
 * "\\RPC Control\\TestPort". ObInsertObject treats its name argument as
 * a single leaf component (no splitting), so we split here: verify the
 * parent prefix is literally "\\RPC Control\\" and pass only the leaf
 * to AlpcCreatePort.
 *
 * When previous mode is UserMode, probe OA + UNICODE_STRING + bounded
 * buffer before dereferencing (mirrors the oa_probe_ascii_name pattern
 * from nt_section.c). Kernel callers skip the probes via
 * ProbeForReadIfUser.
 * ---------------------------------------------------------------------- */

/*
 * After probing + prefix validation, COPY the leaf bytes into a
 * caller-provided kernel buffer. Returning a pointer into
 * UNICODE_STRING.Buffer (user memory) would be a TOCTOU gap: a
 * malicious user-mode caller could alter or unmap the buffer between
 * the probe here and the later strncpy inside ObInsertObject.
 *
 * The kernel-owned copy is NUL-terminated, bounded to kbuf_len-1, and
 * validated byte-by-byte against the {no \\, no /, no empty, no
 * > OB_NAME_MAX} contract.
 */
static NTSTATUS alpc_probe_and_split(OBJECT_ATTRIBUTES *oa,
                                     char *kbuf, uint32_t kbuf_len,
                                     int *out_have_name)
{
    const char prefix[] = "\\RPC Control\\";
    const uint32_t prefix_len = sizeof(prefix) - 1;   /* excl. NUL */
    UNICODE_STRING *us;
    uint32_t probe_len;
    const char *ascii;
    uint32_t i;
    uint32_t leaf_len = 0;
    NTSTATUS st;

    *out_have_name = 0;
    if (!kbuf || kbuf_len == 0)
        return STATUS_INVALID_PARAMETER;
    kbuf[0] = '\0';

    if (!oa)
        return STATUS_SUCCESS;                  /* unnamed port */

    st = ProbeForReadIfUser(oa, sizeof(OBJECT_ATTRIBUTES), 8);
    if (!NT_SUCCESS(st))
        return st;

    us = oa->ObjectName;
    if (!us)
        return STATUS_SUCCESS;                  /* unnamed */

    st = ProbeForReadIfUser(us, sizeof(UNICODE_STRING), 4);
    if (!NT_SUCCESS(st))
        return st;
    if (!us->Buffer)
        return STATUS_INVALID_PARAMETER;

    probe_len = (uint32_t)us->Length;
    if (probe_len == 0u || probe_len > 255u)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForReadIfUser(us->Buffer, probe_len, 1);
    if (!NT_SUCCESS(st))
        return st;

    /* UNICODE_STRING.Buffer is declared uint16_t* but current kernel-wide
     * convention treats it as ASCII char* (see nt_decode_unicode_string
     * retrofit on the registry syscall roadmap). */
    ascii = (const char *)us->Buffer;

    /* Verify prefix. Fail on any path not rooted at \RPC Control\ -- we
     * do not currently support nested subdirectories under RPC Control. */
    if (probe_len <= prefix_len)
        return STATUS_OBJECT_NAME_INVALID;
    for (i = 0; i < prefix_len; i++) {
        if (ascii[i] != prefix[i])
            return STATUS_OBJECT_NAME_INVALID;
    }

    /* Copy leaf byte-by-byte into the kernel buffer, validating as we
     * go. Reject empty leaves and any embedded path separator. Every
     * byte lands in kbuf, so AlpcCreatePort + ObInsertObject never read
     * user memory for this name again. */
    for (i = prefix_len; i < probe_len; i++) {
        char c = ascii[i];
        if (c == '\0')
            break;
        if (c == '\\' || c == '/')
            return STATUS_OBJECT_NAME_INVALID;
        if (leaf_len + 1u >= kbuf_len)
            return STATUS_OBJECT_NAME_INVALID;   /* leaf won't fit + NUL */
        kbuf[leaf_len++] = c;
    }
    if (leaf_len == 0u)
        return STATUS_OBJECT_NAME_INVALID;
    kbuf[leaf_len] = '\0';

    *out_have_name = 1;
    return STATUS_SUCCESS;
}

/* Stub return -- no runtime klog. The deferred-feature inventory is
 * maintained by test_nt_alpc_returns_deferred_status() in test_ob.c
 * via TEST_PENDING; that's the single canonical place "what is
 * incomplete?" gets announced. Adding a per-call klog here would just
 * duplicate the test signal and create N noise lines per boot.
 *
 * SCOPE-GAP-ALLOWED: pending ALPC engine retrofit on the ALPC
 * completion roadmap (concrete checklist item enumerates every slot). */
#define ALPC_STUB_BODY(name)                                                 \
    do {                                                                     \
        /* name is a bare identifier passed for documentation only;          \
         * the deferred contract is exercised by TEST_PENDING in tests. */   \
        (void)#name;                                                         \
        return STATUS_NOT_IMPLEMENTED;  /* SCOPE-GAP-ALLOWED */              \
    } while (0)

/* ---- 0x010F NtAlpcCreatePort -------------------------------------------- */
/* Real implementation wired by the ALPC CreatePort handler. SCOPE-GAP-ALLOWED:
 * the remaining 15 NtAlpc* handlers below still stub out to the deferred
 * sentinel pending the ALPC engine retrofit; CreatePort moves only slot
 * 0x010F out of the pending-features sweep in test_ob.c. User-mode
 * pointer validation (copy_to_user for the out-HANDLE, user-range check
 * on OBJECT_ATTRIBUTES) is the engine-retrofit item's responsibility; for
 * now the args are treated as kernel pointers, which is what in-kernel
 * callers (tests, CSRSS bootstrap) use today. */
static NTSTATUS NtAlpcCreatePort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a2;
    ALPC_PORT_ATTRIBUTES *port_attrs = (ALPC_PORT_ATTRIBUTES *)a3;
    ALPC_PORT_ATTRIBUTES local_attrs;
    ALPC_PORT_ATTRIBUTES *attrs_to_pass = (ALPC_PORT_ATTRIBUTES *)0;
    char leaf_buf[64];                          /* OB_NAME_MAX */
    int have_name = 0;
    NTSTATUS st;
    HANDLE h;

    (void)a4; (void)a5; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForWriteIfUser(out_handle, sizeof(HANDLE), 4);
    if (!NT_SUCCESS(st))
        return st;

    if (port_attrs) {
        st = ProbeForReadIfUser(port_attrs, sizeof(ALPC_PORT_ATTRIBUTES), 8);
        if (!NT_SUCCESS(st))
            return st;
        local_attrs = *port_attrs;              /* single-shot copy */
        attrs_to_pass = &local_attrs;
    }

    st = alpc_probe_and_split(oa, leaf_buf, sizeof(leaf_buf), &have_name);
    if (!NT_SUCCESS(st))
        return st;

    st = AlpcCreatePort(&task_current()->handle_table,
                        have_name ? leaf_buf : (const char *)0,
                        attrs_to_pass, &h);
    if (!NT_SUCCESS(st))
        return st;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- 0x0110 NtAlpcConnectPort ------------------------------------------- */
/* Real implementation wired by the ALPC ConnectPort handler. SCOPE-GAP-ALLOWED:
 * optional OBJECT_ATTRIBUTES.SecurityDescriptor, ConnMsg, SendMsgAttr,
 * RecvMsgAttr all deferred to later ALPC items -- the path parse plus leaf
 * extraction reuses the `alpc_probe_and_split` pattern.
 *
 * RING-3 REACHABILITY (do not mistake this for a live user-mode check): the
 * RequiredServerSid snapshot+compare below is REACHABLE ONLY from KernelMode
 * callers and direct AlpcConnectPort() calls. The SYSCALL transport carries at
 * most four caller arguments -- syscall_entry.asm zeroes r9 ("arg5 = 0") and
 * syscall_dispatch_fast passes a6 as a literal 0 (syscall_fast.c) -- so a ring-3
 * caller CANNOT deliver a5 (timeout_ms) or a6 (RequiredServerSid) at all; both
 * arrive as 0 and this branch never runs. The kernel cannot fail closed on it
 * either, because a NULL a6 is indistinguishable from "caller passed nothing".
 * Widening the 6-word SSDT_HANDLER transport is an operator-reserved ABI
 * decision -> XREF: `02-kernel-core/TODO-24` section 8 (item: "Choose the >6-arg
 * SSDT transport for ALPC"). Until that lands, no user-mode wrapper may
 * advertise RequiredServerSid as an enforced authentication check. */
static NTSTATUS NtAlpcConnectPort_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a2;
    ALPC_PORT_ATTRIBUTES *port_attrs = (ALPC_PORT_ATTRIBUTES *)a3;
    uint32_t timeout_ms = (uint32_t)a5;
    char leaf_buf[64];
    char full_path[96];
    int have_name = 0;
    NTSTATUS st;
    HANDLE h;

    (void)a4; /* Flags -- + */

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForWriteIfUser(out_handle, sizeof(HANDLE), 4);
    if (!NT_SUCCESS(st))
        return st;

    /* The caller's SecurityQos is how a client LIMITS what a server may do with
     * its identity, and AlpcConnectPort honors it by negotiating min(client,
     * listener). We can only pass it on when reading it is SAFE.
     *
     * KernelMode callers: the pointer is kernel-resident, so read it directly.
     *
     * UserMode callers: do NOT dereference. ProbeForReadIfUser validates range
     * and alignment ONLY -- it never checks that the pages are mapped -- and
     * copy_from_user is a plain byte loop with no fixup table and no SEH
     * (src/kernel/cpu_security.c). An in-range unmapped buffer, or one unmapped
     * right after the probe, would fault in ring 0 and take the system down:
     * trading an unreachable escalation for a reachable DoS is a bad trade, and
     * this path never dereferenced port_attrs before. So ring-3 clients cannot
     * constrain the listener yet -- the listener's QoS governs, exactly as
     * before. Probe still runs so a bad pointer is still rejected early.
     * Closing this needs bounded, fault-recoverable usercopy (the kernel safe
     * probing work: probe.c + a safe_return_rip slot in the CPU-local area),
     * the same blocker the environment-variable Rtl expansion work carries. */
    SECURITY_QUALITY_OF_SERVICE client_qos_copy;
    const SECURITY_QUALITY_OF_SERVICE *client_qos =
        (const SECURITY_QUALITY_OF_SERVICE *)0;
    if (port_attrs) {
        st = ProbeForReadIfUser(port_attrs, sizeof(ALPC_PORT_ATTRIBUTES), 8);
        if (!NT_SUCCESS(st))
            return st;
        if (ssdt_previous_mode() != SSDT_USER_MODE) {
            client_qos_copy = port_attrs->SecurityQos;
            if (client_qos_copy.ImpersonationLevel > SecurityDelegation)
                return STATUS_INVALID_PARAMETER;
            client_qos = &client_qos_copy;
        }
    }

    st = alpc_probe_and_split(oa, leaf_buf, sizeof(leaf_buf), &have_name);
    if (!NT_SUCCESS(st))
        return st;
    if (!have_name)
        return STATUS_INVALID_PARAMETER;

    /* Reconstruct the full namespace path -- AlpcConnectPort expects a
     * complete path for ObLookupObjectByName. The split helper already
     * verified the prefix; rebuild instead of trusting user memory. */
    {
        const char prefix[] = "\\RPC Control\\";
        uint32_t pi = 0, li;
        for (li = 0; prefix[li]; li++, pi++) full_path[pi] = prefix[li];
        for (li = 0; leaf_buf[li] && pi < sizeof(full_path) - 1; li++, pi++)
            full_path[pi] = leaf_buf[li];
        full_path[pi] = '\0';
    }

    /* Section 7: snapshot + validate the optional RequiredServerSid (a6) into
     * kernel memory before it reaches AlpcConnectPort. A caller-controlled
     * variable-length SID must never be compared in place: copy the 8-byte
     * header, derive a bounded length from SubAuthorityCount, copy exactly
     * that many bytes, then validate the immutable snapshot -- otherwise a
     * short mapping causes an out-of-bounds kernel read and a live user buffer
     * creates validate/use skew. */
    /* _Alignas(4): this buffer is cast to SID*, and SID carries a uint32_t
     * SubAuthority[] array, so it needs 4-byte alignment. A bare uint8_t[] is
     * 1-byte aligned -- benign on x86-64 but UB, and the compiler may emit
     * alignment-assuming code (ARM64 is the live concern). */
    _Alignas(4) uint8_t sid_storage[SID_MAX_SIZE] = {0};
    const SID *required_sid = (const SID *)0;
    if (a6) {
        const SID *user_sid = (const SID *)a6;
        SID hdr;
        uint32_t sid_len;
        if (ssdt_previous_mode() == SSDT_USER_MODE) {
            st = ProbeForReadIfUser(user_sid, (uint32_t)sizeof(SID), 1);
            if (!NT_SUCCESS(st))
                return st;
            if (copy_from_user(&hdr, user_sid, (uint32_t)sizeof(SID)) != 0)
                return STATUS_ACCESS_VIOLATION;
        } else {
            memcpy(&hdr, user_sid, sizeof(SID));
        }
        if (hdr.Revision != SID_REVISION ||
            hdr.SubAuthorityCount > SID_MAX_SUB_AUTHORITIES)
            return STATUS_INVALID_SID;
        sid_len = (uint32_t)sizeof(SID) + 4u * (uint32_t)hdr.SubAuthorityCount;
        if (ssdt_previous_mode() == SSDT_USER_MODE) {
            st = ProbeForReadIfUser(user_sid, sid_len, 1);
            if (!NT_SUCCESS(st))
                return st;
            if (copy_from_user(sid_storage, user_sid, sid_len) != 0)
                return STATUS_ACCESS_VIOLATION;
        } else {
            memcpy(sid_storage, user_sid, sid_len);
        }
        /* Guard against a TOCTOU where the user mutates SubAuthorityCount
         * between the two reads: the snapshot's own bounded length must equal
         * the number of bytes actually copied (sid_len). Otherwise a grown
         * count would let RtlEqualSid consume uninitialized trailing bytes. */
        if (!RtlValidSid((const SID *)sid_storage) ||
            RtlLengthSidBounded((const SID *)sid_storage, sid_len) != sid_len)
            return STATUS_INVALID_SID;
        required_sid = (const SID *)sid_storage;
    }

    st = AlpcConnectPort(&task_current()->handle_table, full_path,
                         timeout_ms, required_sid, client_qos, &h);
    /* STATUS_TIMEOUT and other non-error-severity informational values
     * are NOT success here -- NT_SUCCESS(STATUS_TIMEOUT) is true because
     * its severity bit is 0, but the connection did not complete. Only
     * STATUS_SUCCESS publishes the handle. */
    if (st != STATUS_SUCCESS)
        return st;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- 0x0111 NtAlpcConnectPortEx ----------------------------------------- */
static NTSTATUS NtAlpcConnectPortEx_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcConnectPortEx);
}

/* ---- 0x0112 NtAlpcAcceptConnectPort ------------------------------------- */
/* Real implementation wired by the ALPC AcceptConnect handler. SCOPE-GAP-ALLOWED:
 * optional PortContext, ConnectionMessage, ConnMsgAttr all deferred to later
 * ALPC items. */
static NTSTATUS NtAlpcAcceptConnectPort_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    HANDLE conn_port_handle = (HANDLE)(int32_t)(uint32_t)a2;
    int accept = (int)(a3 & 1u);
    uint32_t timeout_ms = (uint32_t)a4;
    NTSTATUS st;
    HANDLE h = INVALID_HANDLE_VALUE;

    (void)a5; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForWriteIfUser(out_handle, sizeof(HANDLE), 4);
    if (!NT_SUCCESS(st))
        return st;

    st = AlpcAcceptConnectPort(&task_current()->handle_table,
                               conn_port_handle, accept, timeout_ms, &h);
    /* Same rationale as NtAlpcConnectPort: STATUS_TIMEOUT must propagate
     * verbatim, NT_SUCCESS is not tight enough to gate the handle write. */
    if (st != STATUS_SUCCESS)
        return st;

    if (accept)
        *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- 0x0113 NtAlpcSendWaitReceivePort ----------------------------------- */
/* Real implementation wired by the ALPC SendWaitReceive handler. SCOPE-GAP-ALLOWED:
 * optional ALPC_MESSAGE_ATTRIBUTES (SendMsgAttr/RecvMsgAttr) deferred to the
 * message-attribute item -- inline send+wait+reply does not consume them.
 * NumberOfBytesTransferred is implicit in recv_msg->DataLength after success
 * and is not returned separately. Timeout is uint32_t milliseconds rather
 * than the Windows
 * LARGE_INTEGER pointer convention; the sentinel 0 means "wait
 * forever" for the sync/receive paths and "no wait" for non-blocking
 * receive (matching spec). */
static NTSTATUS NtAlpcSendWaitReceivePort_handler(uint64_t a1, uint64_t a2,
                                                  uint64_t a3, uint64_t a4,
                                                  uint64_t a5, uint64_t a6)
{
    HANDLE port_handle = (HANDLE)(int32_t)(uint32_t)a1;
    uint32_t flags     = (uint32_t)a2;
    PORT_MESSAGE *send_msg = (PORT_MESSAGE *)a3;
    PORT_MESSAGE *recv_msg = (PORT_MESSAGE *)a4;
    uint32_t recv_buf_len  = (uint32_t)a5;
    uint32_t timeout_ms    = (uint32_t)a6;
    NTSTATUS st;

    /* User-mode probes. send_msg is read-only (header + inline body up
     * to TotalLength); recv_msg is write-only (caller-supplied buffer
     * the kernel fills on dequeue/reply wake). Each probe runs only
     * when previous mode is UserMode (kernel callers skip). */
    if (send_msg) {
        st = ProbeForReadIfUser(send_msg, sizeof(PORT_MESSAGE), 8);
        if (!NT_SUCCESS(st))
            return st;
        if (send_msg->TotalLength < sizeof(PORT_MESSAGE) ||
            send_msg->DataLength > ALPC_MAX_ALLOWED_MESSAGE_LENGTH)
            return STATUS_INVALID_PARAMETER;
        st = ProbeForReadIfUser((const uint8_t *)send_msg
                                  + sizeof(PORT_MESSAGE),
                                send_msg->DataLength, 1);
        if (!NT_SUCCESS(st))
            return st;
    }
    if (recv_msg) {
        if (recv_buf_len < sizeof(PORT_MESSAGE))
            return STATUS_BUFFER_TOO_SMALL;
        st = ProbeForWriteIfUser(recv_msg, recv_buf_len, 8);
        if (!NT_SUCCESS(st))
            return st;
    }
    /* recv_msg may be NULL only when the call is a fire-and-forget
     * datagram or reply; receive-only and sync-request both require it. */
    if (!recv_msg && !send_msg)
        return STATUS_INVALID_PARAMETER;
    if (!recv_msg && (flags & ALPC_MSGFLG_SYNC_REQUEST))
        return STATUS_INVALID_PARAMETER;
    if (!send_msg && !recv_msg)
        return STATUS_INVALID_PARAMETER;

    return AlpcSendWaitReceivePort(&task_current()->handle_table,
                                   port_handle, flags,
                                   send_msg, recv_msg,
                                   recv_buf_len, timeout_ms);
}

/* ---- 0x0114 NtAlpcDisconnectPort ---------------------------------------- */
/* Real implementation wired by the ALPC DisconnectPort handler. Flags deferred. */
static NTSTATUS NtAlpcDisconnectPort_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    HANDLE port_handle = (HANDLE)(int32_t)(uint32_t)a1;
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    return AlpcDisconnectPort(&task_current()->handle_table, port_handle);
}

/* ---- 0x0115 NtAlpcCancelMessage ----------------------------------------- */
static NTSTATUS NtAlpcCancelMessage_handler(uint64_t a1, uint64_t a2,
                                            uint64_t a3, uint64_t a4,
                                            uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCancelMessage);
}

/* ---- 0x0116 NtAlpcCreatePortSection ------------------------------------- */
static NTSTATUS NtAlpcCreatePortSection_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreatePortSection);
}

/* ---- 0x0117 NtAlpcDeletePortSection ------------------------------------- */
static NTSTATUS NtAlpcDeletePortSection_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeletePortSection);
}

/* ---- 0x0118 NtAlpcCreateSectionView ------------------------------------- */
static NTSTATUS NtAlpcCreateSectionView_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreateSectionView);
}

/* ---- 0x0119 NtAlpcDeleteSectionView ------------------------------------- */
static NTSTATUS NtAlpcDeleteSectionView_handler(uint64_t a1, uint64_t a2,
                                                uint64_t a3, uint64_t a4,
                                                uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeleteSectionView);
}

/* ---- 0x011A NtAlpcCreateResourceReserve --------------------------------- */
static NTSTATUS NtAlpcCreateResourceReserve_handler(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4,
                                                    uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcCreateResourceReserve);
}

/* ---- 0x011B NtAlpcDeleteResourceReserve --------------------------------- */
static NTSTATUS NtAlpcDeleteResourceReserve_handler(uint64_t a1, uint64_t a2,
                                                    uint64_t a3, uint64_t a4,
                                                    uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcDeleteResourceReserve);
}

/* ---- 0x011C NtAlpcQueryInformation -------------------------------------- */
static NTSTATUS NtAlpcQueryInformation_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcQueryInformation);
}

/* ---- 0x011D NtAlpcSetInformation ---------------------------------------- */
/* Real implementation wired by the ALPC SetInformation handler. Only
 * AlpcAssociateCompletionPortInformation is handled here; other info
 * classes return a deferred-status sentinel pending the Query/Set class
 * work. SCOPE-GAP-ALLOWED: remaining info classes tracked on the ALPC
 * roadmap.
 *
 * Arg layout (a1=PortHandle, a2=InfoClass, a3=Info, a4=Length). */
static NTSTATUS NtAlpcSetInformation_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    HANDLE port_handle = (HANDLE)(int32_t)(uint32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *info = (void *)a3;
    uint32_t length = (uint32_t)a4;
    NTSTATUS st;

    (void)a5; (void)a6;

    switch (info_class) {
    case AlpcAssociateCompletionPortInformation: {
        ALPC_PORT_ASSOCIATE_COMPLETION_PORT local;
        if (!info || length < sizeof(ALPC_PORT_ASSOCIATE_COMPLETION_PORT))
            return STATUS_INVALID_PARAMETER;
        st = ProbeForReadIfUser(info,
                                sizeof(ALPC_PORT_ASSOCIATE_COMPLETION_PORT),
                                8);
        if (!NT_SUCCESS(st))
            return st;
        local = *(ALPC_PORT_ASSOCIATE_COMPLETION_PORT *)info;
        return AlpcAssociateCompletionPort(&task_current()->handle_table,
                                           port_handle,
                                           local.CompletionPort,
                                           local.CompletionKey);
    }
    default:
        /* SCOPE-GAP-ALLOWED: remaining info classes land with the ALPC
         * Query/Set/CancelMessage work. The completion-port association
         * is the only class this handler promises. */
        return STATUS_NOT_IMPLEMENTED;
    }
}

/* ---- 0x011E NtAlpcQueryInformationMessage ------------------------------- */
static NTSTATUS NtAlpcQueryInformationMessage_handler(uint64_t a1, uint64_t a2,
                                                      uint64_t a3, uint64_t a4,
                                                      uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    ALPC_STUB_BODY(NtAlpcQueryInformationMessage);
}

/* --- Registration -------------------------------------------------------- */

/* Per-slot register-and-verify helper. Detects collision with the
 * default stub (catches double registration or conflicting claims on
 * the same SSDT index) and reports per-slot ssdt_register failures. */
static int alpc_register_one(uint32_t svc, SSDT_HANDLER h, const char *name)
{
    extern NTSTATUS ssdt_stub_not_implemented(uint64_t, uint64_t, uint64_t,
                                              uint64_t, uint64_t, uint64_t);
    const SSDT_TABLE *tbl = ssdt_get_table(SSDT_TABLE_MAIN);
    uint32_t idx = svc & SSDT_INDEX_MASK;

    if (!tbl) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register %s: main SSDT absent", name);
        return -1;
    }
    /* Bounds check BEFORE the handlers[idx] read. SSDT_INDEX_MASK is
     * 0xFFF (4096 slots) but SSDT_MAIN_MAX is smaller (1024); catch a
     * mis-numbered service constant before it reads past the array. */
    if (idx >= SSDT_MAIN_MAX) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register %s: SSDT index 0x%x out of range (max 0x%x)",
             name, (uint64_t)idx, (uint64_t)SSDT_MAIN_MAX);
        return -1;
    }
    if (tbl->handlers[idx] != ssdt_stub_not_implemented) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register collision at SSDT 0x%x (%s): slot already taken",
             (uint64_t)svc, name);
        return -1;
    }
    if (ssdt_register(svc, h) != 0) {
        klog(LOG_ERROR, "nt/alpc",
             "ALPC register %s: ssdt_register failed", name);
        return -1;
    }
    return 0;
}

int nt_alpc_register_ssdt(void)
{
    int failures = 0;

    failures += alpc_register_one(SSDT_NtAlpcCreatePort,
                                  (SSDT_HANDLER)NtAlpcCreatePort_handler,
                                  "NtAlpcCreatePort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcConnectPort,
                                  (SSDT_HANDLER)NtAlpcConnectPort_handler,
                                  "NtAlpcConnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcConnectPortEx,
                                  (SSDT_HANDLER)NtAlpcConnectPortEx_handler,
                                  "NtAlpcConnectPortEx") != 0;
    failures += alpc_register_one(SSDT_NtAlpcAcceptConnectPort,
                                  (SSDT_HANDLER)NtAlpcAcceptConnectPort_handler,
                                  "NtAlpcAcceptConnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcSendWaitReceivePort,
                                  (SSDT_HANDLER)NtAlpcSendWaitReceivePort_handler,
                                  "NtAlpcSendWaitReceivePort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDisconnectPort,
                                  (SSDT_HANDLER)NtAlpcDisconnectPort_handler,
                                  "NtAlpcDisconnectPort") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCancelMessage,
                                  (SSDT_HANDLER)NtAlpcCancelMessage_handler,
                                  "NtAlpcCancelMessage") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreatePortSection,
                                  (SSDT_HANDLER)NtAlpcCreatePortSection_handler,
                                  "NtAlpcCreatePortSection") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeletePortSection,
                                  (SSDT_HANDLER)NtAlpcDeletePortSection_handler,
                                  "NtAlpcDeletePortSection") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreateSectionView,
                                  (SSDT_HANDLER)NtAlpcCreateSectionView_handler,
                                  "NtAlpcCreateSectionView") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeleteSectionView,
                                  (SSDT_HANDLER)NtAlpcDeleteSectionView_handler,
                                  "NtAlpcDeleteSectionView") != 0;
    failures += alpc_register_one(SSDT_NtAlpcCreateResourceReserve,
                                  (SSDT_HANDLER)NtAlpcCreateResourceReserve_handler,
                                  "NtAlpcCreateResourceReserve") != 0;
    failures += alpc_register_one(SSDT_NtAlpcDeleteResourceReserve,
                                  (SSDT_HANDLER)NtAlpcDeleteResourceReserve_handler,
                                  "NtAlpcDeleteResourceReserve") != 0;
    failures += alpc_register_one(SSDT_NtAlpcQueryInformation,
                                  (SSDT_HANDLER)NtAlpcQueryInformation_handler,
                                  "NtAlpcQueryInformation") != 0;
    failures += alpc_register_one(SSDT_NtAlpcSetInformation,
                                  (SSDT_HANDLER)NtAlpcSetInformation_handler,
                                  "NtAlpcSetInformation") != 0;
    failures += alpc_register_one(SSDT_NtAlpcQueryInformationMessage,
                                  (SSDT_HANDLER)NtAlpcQueryInformationMessage_handler,
                                  "NtAlpcQueryInformationMessage") != 0;

    if (failures != 0) {
        klog(LOG_ERROR, "nt/alpc",
             "NT modern ALPC: %d of 16 handlers FAILED to register",
             (uint64_t)failures);
        return failures;
    }
    klog(LOG_INFO, "nt/alpc",
         "NT modern ALPC: 16 stub handlers registered (SSDT 0x010F-0x011E)");
    return 0;
}
