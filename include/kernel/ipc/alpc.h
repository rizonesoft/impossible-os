/* ============================================================================
 * alpc.h -- Advanced Local Procedure Call (ALPC) on-the-wire ABI
 *
 * Defines the message header, port attributes, message type codes, and
 * flag bitmasks for the ALPC subsystem.
 *
 * ABI scope and compatibility
 *     Field names and constant values follow the Windows ALPC convention
 *     (winternl.h PORT_MESSAGE, MSDN ALPC reference) so code written
 *     against that naming reads naturally here. Binary wire layout is
 *     Impossible OS native and matches the canonical Windows x64 widths
 *     for the structurally shared types:
 *       - CLIENT_ID is the canonical 16-byte Impossible OS type from
 *         `kernel/ob/teb.h` (two uint64_t: UniqueProcess, UniqueThread).
 *         Do NOT redefine it here.
 *       - SECURITY_IMPERSONATION_LEVEL is the canonical enum from
 *         `kernel/security/token.h`. Do NOT redefine it here.
 *       - PORT_MESSAGE is 40 bytes. Windows x64 PORT_MESSAGE is 48 bytes
 *         and uses unions at offsets 32..47 for ClientViewSize /
 *         section transfer metadata; Impossible OS defers those 8
 *         bytes until the ALPC view-transfer work wires view transfers,
 *         at which point PORT_MESSAGE may grow or gain a trailing union.
 *     Consumers compile against THIS header, not Windows headers. A
 *     user-mode translation layer is out of scope for this section.
 *
 * Layout is locked by _Static_assert lines on sizeof and per-field
 * offsets for every ABI-visible struct. A mismatch between this header
 * and a future implementation, or between this header and a user-mode
 * client that also builds against this header, will fail at compile
 * time.
 *
 * Recommended threshold for switching from inline message payload to a
 * port-section view-mapped transfer is 512 bytes; the hard ceiling on
 * inline message length is ALPC_MAX_ALLOWED_MESSAGE_LENGTH (65528 bytes,
 * i.e. 64 KiB minus 8 bytes of overhead reserve).
 *
 * This header does not declare the ALPC_PORT object or any syscall
 * entry points. Those ship in alpc_port.h and alpc_syscalls.h as part
 * of the ALPC completion roadmap.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"
#include "kernel/boot_init.h"
#include "kernel/ob/teb.h"            /* canonical CLIENT_ID */
#include "kernel/security/token.h"    /* canonical SECURITY_IMPERSONATION_LEVEL */

/* ---- CLIENT_ID ---------------------------------------------------------- */
/* Sourced from kernel/ob/teb.h (two uint64_t: UniqueProcess/UniqueThread).
 * Re-asserted here so any drift in the canonical definition trips this
 * compile-time check in the ALPC ABI translation unit too. */

_Static_assert(sizeof(CLIENT_ID) == 16, "CLIENT_ID must be 16 bytes");
_Static_assert(__builtin_offsetof(CLIENT_ID, UniqueProcess) == 0,
               "CLIENT_ID.UniqueProcess at offset 0");
_Static_assert(__builtin_offsetof(CLIENT_ID, UniqueThread) == 8,
               "CLIENT_ID.UniqueThread at offset 8");

/* ---- PORT_MESSAGE ------------------------------------------------------- */
/*
 * The 40-byte on-the-wire message header. Field order and offsets match
 * the Windows winternl.h PORT_MESSAGE layout for LPC/ALPC interop.
 *
 * TotalLength is the full message size including this header.
 * DataLength  is the inline payload length (0 if payload is in a section).
 * Type        is one of ALPC_MSG_TYPE_*.
 * DataInfoOffset points to an optional attribute record (0 = none).
 * ClientId    is the sender's process/thread pair.
 * MessageId   is monotonically assigned by the port owner.
 * CallbackId  is used to correlate a reply with its original request.
 */
typedef struct {
    uint16_t  TotalLength;      /* offset  0 */
    uint16_t  DataLength;       /* offset  2 */
    uint16_t  Type;             /* offset  4 */
    uint16_t  DataInfoOffset;   /* offset  6 */
    CLIENT_ID ClientId;         /* offset  8 (16 bytes) */
    uint64_t  MessageId;        /* offset 24 */
    uint64_t  CallbackId;       /* offset 32 */
} PORT_MESSAGE;

_Static_assert(sizeof(PORT_MESSAGE) == 40, "PORT_MESSAGE must be 40 bytes");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, TotalLength)    ==  0, "PORT_MESSAGE.TotalLength offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, DataLength)     ==  2, "PORT_MESSAGE.DataLength offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, Type)           ==  4, "PORT_MESSAGE.Type offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, DataInfoOffset) ==  6, "PORT_MESSAGE.DataInfoOffset offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, ClientId)       ==  8, "PORT_MESSAGE.ClientId offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, MessageId)      == 24, "PORT_MESSAGE.MessageId offset");
_Static_assert(__builtin_offsetof(PORT_MESSAGE, CallbackId)     == 32, "PORT_MESSAGE.CallbackId offset");

/* ---- ALPC_MESSAGE ------------------------------------------------------- */
/*
 * Variable-length wrapper: fixed header followed by an inline payload
 * whose length is carried in Header.DataLength. Callers allocate
 * sizeof(PORT_MESSAGE) + payload_len bytes and write into Body[].
 */
typedef struct {
    PORT_MESSAGE Header;
    uint8_t      Body[];
} ALPC_MESSAGE;

_Static_assert(__builtin_offsetof(ALPC_MESSAGE, Body) == sizeof(PORT_MESSAGE),
               "ALPC_MESSAGE.Body immediately follows Header");

/* ---- Message type codes (Header.Type) ----------------------------------- */

#define ALPC_MSG_TYPE_REQUEST             1u
#define ALPC_MSG_TYPE_REPLY               2u
#define ALPC_MSG_TYPE_DATAGRAM            3u
#define ALPC_MSG_TYPE_LOST_REPLY          4u
#define ALPC_MSG_TYPE_PORT_CLOSED         5u
#define ALPC_MSG_TYPE_CLIENT_DIED         6u
#define ALPC_MSG_TYPE_EXCEPTION           7u
#define ALPC_MSG_TYPE_DEBUG_EVENT         8u
#define ALPC_MSG_TYPE_ERROR_EVENT         9u
#define ALPC_MSG_TYPE_CONNECTION_REQUEST 10u

/* ---- Size limits -------------------------------------------------------- */
/*
 * Hard ceiling for the inline payload. Messages larger than this must
 * use a port-section view transfer. 65528 = 64 KiB minus an 8-byte
 * overhead reserve; the reserve accommodates alignment padding the
 * transport may apply internally.
 *
 * Recommended threshold for switching from inline to port-section
 * transfer is 512 bytes (documented here; enforced by policy, not by
 * this header).
 */
#define ALPC_MAX_ALLOWED_MESSAGE_LENGTH 65528u

/* ---- SECURITY_QUALITY_OF_SERVICE --------------------------------------- */
/* SECURITY_IMPERSONATION_LEVEL is owned by kernel/security/token.h and
 * pulled in via this header's top-level includes. Do NOT redefine it. */

#define SECURITY_CONTEXT_TRACKING_STATIC  0u
#define SECURITY_CONTEXT_TRACKING_DYNAMIC 1u

typedef struct {
    uint32_t                     Length;
    SECURITY_IMPERSONATION_LEVEL ImpersonationLevel;
    uint8_t                      ContextTrackingMode;
    uint8_t                      EffectiveOnly;
    uint8_t                      Padding[2];
} SECURITY_QUALITY_OF_SERVICE;

_Static_assert(sizeof(SECURITY_QUALITY_OF_SERVICE) == 12,
               "SECURITY_QUALITY_OF_SERVICE must be 12 bytes");

/* ---- ALPC_PORT_ATTRIBUTES ---------------------------------------------- */
/*
 * Field names match MSDN ALPC_PORT_ATTRIBUTES. This is NOT a Windows x64
 * binary-compatible layout: the Impossible OS CLIENT_ID width (see file
 * header) propagates here via the Flags/SecurityQos prefix. sizeof and
 * per-field offsets are locked below; any future extension must be
 * reflected in the asserts so consumers cannot silently disagree.
 */
typedef struct {
    uint32_t                    Flags;               /* offset  0; ALPC_PORTFLG_* */
    uint32_t                    Pad0;                /* offset  4; reserved, must be 0 */
    SECURITY_QUALITY_OF_SERVICE SecurityQos;         /* offset  8; 12 bytes */
    uint32_t                    Pad1;                /* offset 20; reserved, must be 0 */
    uint64_t                    MaxMessageLength;    /* offset 24 */
    uint64_t                    MemoryBandwidth;     /* offset 32 */
    uint64_t                    MaxPoolUsage;        /* offset 40 */
    uint64_t                    MaxSectionSize;      /* offset 48 */
    uint64_t                    MaxViewSize;         /* offset 56 */
    uint64_t                    MaxTotalSectionSize; /* offset 64 */
    uint32_t                    DupObjectTypes;      /* offset 72 */
    uint32_t                    Pad2;                /* offset 76; trailing pad to 8-align */
} ALPC_PORT_ATTRIBUTES;

_Static_assert(sizeof(ALPC_PORT_ATTRIBUTES) == 80,
               "ALPC_PORT_ATTRIBUTES must be 80 bytes");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, Flags)               ==  0, "PortAttr.Flags offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, SecurityQos)         ==  8, "PortAttr.SecurityQos offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxMessageLength)    == 24, "PortAttr.MaxMessageLength offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MemoryBandwidth)     == 32, "PortAttr.MemoryBandwidth offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxPoolUsage)        == 40, "PortAttr.MaxPoolUsage offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxSectionSize)      == 48, "PortAttr.MaxSectionSize offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxViewSize)         == 56, "PortAttr.MaxViewSize offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, MaxTotalSectionSize) == 64,
               "PortAttr.MaxTotalSectionSize offset");
_Static_assert(__builtin_offsetof(ALPC_PORT_ATTRIBUTES, DupObjectTypes)      == 72, "PortAttr.DupObjectTypes offset");

/* ---- ALPC_PORTFLG_* (ALPC_PORT_ATTRIBUTES.Flags) ----------------------- */

#define ALPC_PORTFLG_LPC_MODE         0x00020000u
#define ALPC_PORTFLG_WAITABLE_PORT    0x00040000u
#define ALPC_PORTFLG_ALLOW_DUP_OBJECT 0x00080000u
#define ALPC_PORTFLG_SYSTEM_PROCESS   0x00100000u

/* Non-overlap contract: pairwise-disjoint bitmasks sum to their OR. */
_Static_assert((ALPC_PORTFLG_LPC_MODE
              | ALPC_PORTFLG_WAITABLE_PORT
              | ALPC_PORTFLG_ALLOW_DUP_OBJECT
              | ALPC_PORTFLG_SYSTEM_PROCESS)
            == (ALPC_PORTFLG_LPC_MODE
              + ALPC_PORTFLG_WAITABLE_PORT
              + ALPC_PORTFLG_ALLOW_DUP_OBJECT
              + ALPC_PORTFLG_SYSTEM_PROCESS),
               "ALPC_PORTFLG_* bits must be non-overlapping");

/* ---- ALPC_MSGFLG_* (NtAlpcSendWaitReceivePort flags) ------------------- */

#define ALPC_MSGFLG_REPLY_MESSAGE          0x00000001u
#define ALPC_MSGFLG_LPC_MODE               0x00000002u
#define ALPC_MSGFLG_RELEASE_MESSAGE        0x00000010u
#define ALPC_MSGFLG_SYNC_REQUEST           0x00020000u
#define ALPC_MSGFLG_WAIT_USER_MODE         0x00100000u
#define ALPC_MSGFLG_WAIT_PENDING_CALLBACKS 0x00200000u

_Static_assert((ALPC_MSGFLG_REPLY_MESSAGE
              | ALPC_MSGFLG_LPC_MODE
              | ALPC_MSGFLG_RELEASE_MESSAGE
              | ALPC_MSGFLG_SYNC_REQUEST
              | ALPC_MSGFLG_WAIT_USER_MODE
              | ALPC_MSGFLG_WAIT_PENDING_CALLBACKS)
            == (ALPC_MSGFLG_REPLY_MESSAGE
              + ALPC_MSGFLG_LPC_MODE
              + ALPC_MSGFLG_RELEASE_MESSAGE
              + ALPC_MSGFLG_SYNC_REQUEST
              + ALPC_MSGFLG_WAIT_USER_MODE
              + ALPC_MSGFLG_WAIT_PENDING_CALLBACKS),
               "ALPC_MSGFLG_* bits must be non-overlapping");

/* ---- ALPC_COMPLETION_LIST -----------------------------
 *
 * Lock-free single-producer / multi-consumer ring buffer for async
 * message notifications. Windows maps this structure into the server's
 * address space so the consumer can check for incoming messages without
 * a syscall. Impossible OS defines the on-the-wire layout now; the
 * full shared-VA mapping path lives in (port sections), and's
 * IOCP bridge (io_completion_post) implements the kernel-side wake
 * while leaving actual message bodies on MessageQueue.
 *
 * Consumers read ConsumerHead, atomically increment to claim a slot,
 * then read Items[slot]. Producers (the kernel ALPC engine) increment
 * ProducerHead. Callers that only care about IOCP-based delivery can
 * ignore this struct.
 */
typedef struct {
    void     *Message;        /* PORT_MESSAGE * into the port's section */
    uint64_t  PortContext;
    uint32_t  MessageFlags;
    uint32_t  _pad;
} ALPC_COMPLETION_LIST_ITEM;

typedef struct {
    uint32_t  TotalSize;          /* total bytes, including header */
    uint32_t  UserVirtualAddr;    /* offset from mapping base */
    uint32_t  KernelOffset;       /* offset from kernel-side pointer */
    uint32_t  Capacity;           /* number of Items[] slots */
    uint32_t  ProducerHead;       /* producer cursor -- atomic ops only */
    uint32_t  ConsumerHead;       /* consumer cursor -- atomic ops only */
    uint32_t  _pad[2];            /* align Items[] to 32 bytes */
    ALPC_COMPLETION_LIST_ITEM Items[];   /* variable length, Capacity entries */
} ALPC_COMPLETION_LIST;

_Static_assert(sizeof(ALPC_COMPLETION_LIST_ITEM) == 24,
               "ALPC_COMPLETION_LIST_ITEM must be 24 bytes");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, Message) == 0,
               "ALPC_COMPLETION_LIST_ITEM.Message at offset 0");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, PortContext) == 8,
               "ALPC_COMPLETION_LIST_ITEM.PortContext at offset 8");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST_ITEM, MessageFlags) == 16,
               "ALPC_COMPLETION_LIST_ITEM.MessageFlags at offset 16");

/* ALPC_COMPLETION_LIST is an ABI contract for port sections. Frozen
 * now so consumers read the same layout the test suite locks. The
 * header (without flexible tail) is 32 bytes -- one cache line on x86-64
 * -- so the first Items[] element starts on a 32-byte boundary and SPMC
 * atomic updates to ProducerHead/ConsumerHead do not share a line with
 * Items[0]. */
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, TotalSize) == 0,
               "ALPC_COMPLETION_LIST.TotalSize at offset 0");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, UserVirtualAddr) == 4,
               "ALPC_COMPLETION_LIST.UserVirtualAddr at offset 4");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, KernelOffset) == 8,
               "ALPC_COMPLETION_LIST.KernelOffset at offset 8");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, Capacity) == 12,
               "ALPC_COMPLETION_LIST.Capacity at offset 12");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, ProducerHead) == 16,
               "ALPC_COMPLETION_LIST.ProducerHead at offset 16");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, ConsumerHead) == 20,
               "ALPC_COMPLETION_LIST.ConsumerHead at offset 20");
_Static_assert(__builtin_offsetof(ALPC_COMPLETION_LIST, Items) == 32,
               "ALPC_COMPLETION_LIST.Items[] at offset 32");

/* ---- Boot-time announcement -------------------------------------------- */
/*
 * alpc_init() emits a single klog line confirming the ABI is compiled
 * in. It does not allocate, touch hardware, or register any objects --
 * the ALPC_PORT object type and syscall surface ship in later sections.
 * Safe to call once from Phase 3 init.
 */
boot_result_t alpc_init(void);
