/* ============================================================================
 * nt_misc.c -- Atom table, locale, and miscellaneous NT syscalls
 *
 * SSDT 0x00D8-0x00E2. See nt_misc.h for the surface. Trust-boundary rule:
 * every user pointer is validated with ProbeFor{Read,Write}IfUser and moved
 * through copy_{from,to}_user before use -- integer atoms (name pointer
 * < NT_MAXINTATOM) are never dereferenced.
 *
 * Global atom table: a fixed NT_ATOM_TABLE_CAP array under one leaf spinlock
 * (s_atom_lock). Each occupied slot's atom value is NT_STRING_ATOM_BASE +
 * slot, so the value is stable while the atom lives and reverse lookup is
 * O(1). String atoms are refcounted (NtAddAtom bumps, NtDeleteAtom drops,
 * slot freed at zero). Name matching is case-insensitive through the invariant
 * fold authority (rtl_upcase_char: ASCII + Latin-1); a per-slot case-folded
 * hash gates the full name compare so the linear scan stays cheap. Full-BMP /
 * locale-specific casing remains out of scope. Local (per-process) atom tables
 * are user-mode (ntdll RtlAtomTable), not here.
 * ============================================================================ */

#include "kernel/nt/nt_misc.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/nt_rtlstr.h"   /* rtl_upcase_char -- the NLS case-fold authority */
#include "kernel/nt/nls_locale.h"  /* nls_locale_get_system -- system-locale query */
#include "kernel/nt/nls.h"             /* nls_get_version -- MUI/version snapshot */
#include "kernel/nt/nls_syscall_info.h" /* MUI_REGISTRY_INFO ABI */
#include "kernel/nt/service_numbers.h"
#include "kernel/ob/peb.h"
#include "kernel/sched/spinlock.h"
#include "kernel/cpu_security.h"
#include "kernel/mm/heap.h"
#include "kernel/klog.h"

/* ---- Global atom table --------------------------------------------------- */

typedef struct atom_slot {
    uint16_t atom;                       /* 0 = free; else NT_STRING_ATOM_BASE+slot */
    uint16_t name_len;                   /* chars, excluding NUL */
    uint32_t refcount;                   /* usage count; every add is accounted */
    uint32_t hash;                       /* case-folded name hash; lookup prefilter */
    uint64_t generation;                 /* bumped on each allocation; ID-reuse guard (64-bit: never wraps) */
    uint16_t name[NT_MAX_ATOM_LEN + 1];  /* NUL-terminated wide name */
} atom_slot_t;

static atom_slot_t     s_atoms[NT_ATOM_TABLE_CAP];
static DEFINE_SPINLOCK(s_atom_lock);

/* ---- Locale / UI-language globals ---------------------------------------- */

static uint32_t        s_default_locale   = NT_DEFAULT_LCID;
static uint16_t        s_default_ui_lang  = NT_DEFAULT_LANGID;
/* Install language is fixed at build/deploy time -- read-only. */
static const uint16_t  s_install_ui_lang  = NT_DEFAULT_LANGID;
static DEFINE_SPINLOCK(s_locale_lock);

/* ---- Case-insensitive name matching (NLS invariant fold authority) ------- */

/* Compare two wide names of equal char length, case-insensitive, through the
 * invariant fold authority (ASCII + Latin-1); replaces the old ASCII-only fold
 * so atom matching agrees with the kernel's single case-fold policy. */
static int atom_name_eq(const uint16_t *a, const uint16_t *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (rtl_upcase_char(a[i]) != rtl_upcase_char(b[i]))
            return 0;
    }
    return 1;
}

/* Case-folded FNV-1a over a wide name -- the per-slot lookup prefilter. Each
 * code unit is folded through rtl_upcase_char so a case-insensitive match
 * hashes identically regardless of the stored case. */
static uint32_t atom_name_hash(const uint16_t *name, uint32_t len)
{
    uint32_t h = 2166136261u;            /* FNV-1a offset basis */
    for (uint32_t i = 0; i < len; i++) {
        h ^= (uint32_t)rtl_upcase_char(name[i]);
        h *= 16777619u;                  /* FNV-1a prime */
    }
    return h;
}

/* Caller must hold s_atom_lock and pass the pre-computed folded hash `qh`
 * (computed OUTSIDE the lock so the per-char fold never lengthens the
 * irq-disabled hold time). Returns slot index of a matching name, or
 * NT_ATOM_TABLE_CAP if none. The folded hash gates the full name compare so a
 * non-matching slot costs a single word compare, not len char folds -- bounding
 * the old O(NT_ATOM_TABLE_CAP * name_len) scan to O(NT_ATOM_TABLE_CAP). */
static uint32_t atom_find_slot_locked(const uint16_t *name, uint32_t len, uint32_t qh)
{
    for (uint32_t i = 0; i < NT_ATOM_TABLE_CAP; i++) {
        if (s_atoms[i].atom != 0 && s_atoms[i].hash == qh &&
            s_atoms[i].name_len == len &&
            atom_name_eq(s_atoms[i].name, name, len)) {
            return i;
        }
    }
    return NT_ATOM_TABLE_CAP;
}

/* Full form: also returns the slot's generation so a caller can later release
 * exactly the reference it acquired even if the slot ID is reused meanwhile. */
static NTSTATUS nt_atom_add_ex(const uint16_t *name, uint32_t len_chars,
                               uint16_t *out_atom, uint64_t *out_gen)
{
    if (!name || !out_atom || len_chars == 0 || len_chars > NT_MAX_ATOM_LEN)
        return STATUS_INVALID_PARAMETER;

    /* Fold + hash the name OUTSIDE the lock so the irq-disabled hold time never
     * covers the per-char fold; reused for the slot's stored hash on insert. */
    uint32_t qh = atom_name_hash(name, len_chars);

    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);

    uint32_t slot = atom_find_slot_locked(name, len_chars, qh);
    if (slot != NT_ATOM_TABLE_CAP) {
        /* Existing atom: every add is accounted. Refuse rather than return
         * an unaccounted reference the caller would later over-delete. */
        if (s_atoms[slot].refcount == 0xFFFFFFFFu) {
            spin_unlock_irqrestore(&s_atom_lock, flags);
            return STATUS_INVALID_PARAMETER;
        }
        s_atoms[slot].refcount++;
        *out_atom = s_atoms[slot].atom;
        if (out_gen)
            *out_gen = s_atoms[slot].generation;
        spin_unlock_irqrestore(&s_atom_lock, flags);
        return STATUS_SUCCESS;
    }

    /* New atom: find a free slot. */
    for (uint32_t i = 0; i < NT_ATOM_TABLE_CAP; i++) {
        if (s_atoms[i].atom == 0) {
            s_atoms[i].generation++;    /* fresh allocation instance */
            s_atoms[i].atom     = (uint16_t)(NT_STRING_ATOM_BASE + i);
            s_atoms[i].refcount = 1;
            s_atoms[i].name_len = (uint16_t)len_chars;
            s_atoms[i].hash     = qh;   /* reuse the hash computed before the lock */
            for (uint32_t j = 0; j < len_chars; j++)
                s_atoms[i].name[j] = name[j];
            s_atoms[i].name[len_chars] = 0;
            *out_atom = s_atoms[i].atom;
            if (out_gen)
                *out_gen = s_atoms[i].generation;
            spin_unlock_irqrestore(&s_atom_lock, flags);
            return STATUS_SUCCESS;
        }
    }

    spin_unlock_irqrestore(&s_atom_lock, flags);
    return STATUS_INSUFFICIENT_RESOURCES;   /* table full */
}

NTSTATUS nt_atom_add(const uint16_t *name, uint32_t len_chars, uint16_t *out_atom)
{
    return nt_atom_add_ex(name, len_chars, out_atom, (uint64_t *)0);
}

/* Release exactly the reference acquired by a matching nt_atom_add_ex: undo
 * only when BOTH the atom value AND the generation still match, so a freed +
 * ID-reused slot is left untouched. */
static void nt_atom_release(uint16_t atom, uint64_t gen)
{
    if (atom < NT_STRING_ATOM_BASE)
        return;
    uint32_t slot = (uint32_t)(atom - NT_STRING_ATOM_BASE);
    if (slot >= NT_ATOM_TABLE_CAP)
        return;

    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);
    if (s_atoms[slot].atom == atom && s_atoms[slot].generation == gen) {
        if (s_atoms[slot].refcount > 1) {
            s_atoms[slot].refcount--;
        } else {
            s_atoms[slot].atom     = 0;
            s_atoms[slot].refcount = 0;
            s_atoms[slot].name_len = 0;
        }
    }
    spin_unlock_irqrestore(&s_atom_lock, flags);
}

NTSTATUS nt_atom_find(const uint16_t *name, uint32_t len_chars, uint16_t *out_atom)
{
    if (!name || !out_atom || len_chars == 0 || len_chars > NT_MAX_ATOM_LEN)
        return STATUS_INVALID_PARAMETER;

    uint32_t qh = atom_name_hash(name, len_chars);   /* fold+hash outside the lock */

    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);
    uint32_t slot = atom_find_slot_locked(name, len_chars, qh);
    if (slot == NT_ATOM_TABLE_CAP) {
        spin_unlock_irqrestore(&s_atom_lock, flags);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    *out_atom = s_atoms[slot].atom;
    spin_unlock_irqrestore(&s_atom_lock, flags);
    return STATUS_SUCCESS;
}

NTSTATUS nt_atom_delete(uint16_t atom)
{
    /* Integer atoms have no table entry -- deletion is a no-op success
     * (Win32 GlobalDeleteAtom on MAKEINTATOM). */
    if (atom != 0 && atom < NT_MAXINTATOM)
        return STATUS_SUCCESS;
    if (atom < NT_STRING_ATOM_BASE)
        return STATUS_INVALID_HANDLE;   /* atom == 0 */

    uint32_t slot = (uint32_t)(atom - NT_STRING_ATOM_BASE);
    if (slot >= NT_ATOM_TABLE_CAP)
        return STATUS_INVALID_HANDLE;

    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);
    if (s_atoms[slot].atom != atom) {
        spin_unlock_irqrestore(&s_atom_lock, flags);
        return STATUS_INVALID_HANDLE;
    }
    if (s_atoms[slot].refcount > 1) {
        s_atoms[slot].refcount--;
    } else {
        s_atoms[slot].atom     = 0;
        s_atoms[slot].refcount = 0;
        s_atoms[slot].name_len = 0;
    }
    spin_unlock_irqrestore(&s_atom_lock, flags);
    return STATUS_SUCCESS;
}

NTSTATUS nt_atom_query_basic(uint16_t atom, uint16_t *out_usage,
                             uint16_t *name_out, uint32_t name_cap_chars,
                             uint32_t *out_name_len_chars)
{
    if (!out_usage || !out_name_len_chars)
        return STATUS_INVALID_PARAMETER;

    /* Integer atom: usage 1, no name. */
    if (atom != 0 && atom < NT_MAXINTATOM) {
        *out_usage = 1;
        *out_name_len_chars = 0;
        return STATUS_SUCCESS;
    }
    if (atom < NT_STRING_ATOM_BASE)
        return STATUS_INVALID_HANDLE;

    uint32_t slot = (uint32_t)(atom - NT_STRING_ATOM_BASE);
    if (slot >= NT_ATOM_TABLE_CAP)
        return STATUS_INVALID_HANDLE;

    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);
    if (s_atoms[slot].atom != atom) {
        spin_unlock_irqrestore(&s_atom_lock, flags);
        return STATUS_INVALID_HANDLE;
    }
    uint32_t nlen = s_atoms[slot].name_len;
    /* UsageCount is a 16-bit ABI field; clamp the true internal count. */
    uint32_t rc = s_atoms[slot].refcount;
    *out_usage = (rc > 0xFFFFu) ? 0xFFFFu : (uint16_t)rc;
    *out_name_len_chars = nlen;
    if (name_out) {
        uint32_t copy = (nlen < name_cap_chars) ? nlen : name_cap_chars;
        for (uint32_t i = 0; i < copy; i++)
            name_out[i] = s_atoms[slot].name[i];
        if (copy < name_cap_chars)
            name_out[copy] = 0;
    }
    spin_unlock_irqrestore(&s_atom_lock, flags);
    return STATUS_SUCCESS;
}

void nt_atom_reset_for_test(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_atom_lock, &flags);
    for (uint32_t i = 0; i < NT_ATOM_TABLE_CAP; i++) {
        s_atoms[i].atom     = 0;
        s_atoms[i].refcount = 0;
        s_atoms[i].name_len = 0;
    }
    spin_unlock_irqrestore(&s_atom_lock, flags);
}

/* ---- Locale accessors ---------------------------------------------------- */

uint32_t nt_locale_get_default(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_locale_lock, &flags);
    uint32_t v = s_default_locale;
    spin_unlock_irqrestore(&s_locale_lock, flags);
    return v;
}

void nt_locale_set_default(uint32_t lcid)
{
    uint64_t flags;
    spin_lock_irqsave(&s_locale_lock, &flags);
    s_default_locale = lcid;
    spin_unlock_irqrestore(&s_locale_lock, flags);
}

uint16_t nt_locale_get_ui_language(void)
{
    uint64_t flags;
    spin_lock_irqsave(&s_locale_lock, &flags);
    uint16_t v = s_default_ui_lang;
    spin_unlock_irqrestore(&s_locale_lock, flags);
    return v;
}

void nt_locale_set_ui_language(uint16_t langid)
{
    uint64_t flags;
    spin_lock_irqsave(&s_locale_lock, &flags);
    s_default_ui_lang = langid;
    spin_unlock_irqrestore(&s_locale_lock, flags);
}

uint16_t nt_locale_get_install_ui_language(void)
{
    return s_install_ui_lang;   /* immutable */
}

/* ---- User marshalling helpers -------------------------------------------- */

/* Copy a user wide buffer (len bytes) into kbuf (chars = len/2). Validates
 * length is non-empty, even, and within the atom-name cap. Returns an
 * NTSTATUS; on success *out_chars holds the char count. */
static NTSTATUS marshal_user_wide(uint64_t user_ptr, uint32_t byte_len,
                                  uint16_t *kbuf, uint32_t cap_chars,
                                  uint32_t *out_chars)
{
    if (!user_ptr || byte_len == 0 || (byte_len & 1) != 0)
        return STATUS_INVALID_PARAMETER;
    uint32_t chars = byte_len / 2;
    if (chars > cap_chars)
        return STATUS_INVALID_PARAMETER;
    if (ProbeForReadIfUser((const void *)user_ptr, byte_len, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(kbuf, (const void *)user_ptr, byte_len) != 0)
        return STATUS_ACCESS_VIOLATION;
    *out_chars = chars;
    return STATUS_SUCCESS;
}

/* Write a uint16_t atom value to a user out-pointer. */
static NTSTATUS write_user_atom(uint64_t out_ptr, uint16_t atom)
{
    if (!out_ptr)
        return STATUS_INVALID_PARAMETER;
    if (ProbeForWriteIfUser((void *)out_ptr, sizeof(uint16_t), sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)out_ptr, &atom, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- Atom syscalls ------------------------------------------------------- */

/* NtAddAtom(PWSTR AtomName, ULONG Length(bytes), PRTL_ATOM Atom).
 * A name pointer < NT_MAXINTATOM is an integer atom (MAKEINTATOM). */
static NTSTATUS NtAddAtom_handler(uint64_t name_ptr, uint64_t byte_len,
                                  uint64_t atom_out, uint64_t a4,
                                  uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!atom_out)
        return STATUS_INVALID_PARAMETER;

    /* Integer atom: value 0x0001-0xBFFF passes through, no table entry. */
    if (name_ptr != 0 && name_ptr < NT_MAXINTATOM)
        return write_user_atom(atom_out, (uint16_t)(name_ptr & 0xFFFFu));

    uint16_t kname[NT_MAX_ATOM_LEN + 1];
    uint32_t chars = 0;
    NTSTATUS st = marshal_user_wide(name_ptr, (uint32_t)byte_len, kname,
                                    NT_MAX_ATOM_LEN, &chars);
    if (st != STATUS_SUCCESS)
        return st;

    /* Probe the output before mutating the table so a bad Atom pointer fails
     * without leaking a slot or inflating a refcount. */
    if (ProbeForWriteIfUser((void *)atom_out, sizeof(uint16_t), sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;

    uint16_t atom = 0;
    uint64_t gen = 0;
    st = nt_atom_add_ex(kname, chars, &atom, &gen);
    if (st != STATUS_SUCCESS)
        return st;

    /* If the (already-probed) copy still faults, undo exactly the one
     * reference this call acquired. The generation guard makes the release a
     * no-op if the slot was freed and its ID reused meanwhile. */
    if (copy_to_user((void *)atom_out, &atom, sizeof(uint16_t)) != 0) {
        nt_atom_release(atom, gen);
        return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

/* NtFindAtom(PWSTR AtomName, ULONG Length(bytes), PRTL_ATOM Atom). */
static NTSTATUS NtFindAtom_handler(uint64_t name_ptr, uint64_t byte_len,
                                   uint64_t atom_out, uint64_t a4,
                                   uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!atom_out)
        return STATUS_INVALID_PARAMETER;

    /* Integer atom: GlobalFindAtom returns the integer itself. */
    if (name_ptr != 0 && name_ptr < NT_MAXINTATOM)
        return write_user_atom(atom_out, (uint16_t)(name_ptr & 0xFFFFu));

    uint16_t kname[NT_MAX_ATOM_LEN + 1];
    uint32_t chars = 0;
    NTSTATUS st = marshal_user_wide(name_ptr, (uint32_t)byte_len, kname,
                                    NT_MAX_ATOM_LEN, &chars);
    if (st != STATUS_SUCCESS)
        return st;

    uint16_t atom = 0;
    st = nt_atom_find(kname, chars, &atom);
    if (st != STATUS_SUCCESS)
        return st;
    return write_user_atom(atom_out, atom);
}

/* NtDeleteAtom(RTL_ATOM Atom). */
static NTSTATUS NtDeleteAtom_handler(uint64_t atom, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return nt_atom_delete((uint16_t)(atom & 0xFFFFu));
}

/* NtQueryInformationAtom(RTL_ATOM Atom, ATOM_INFORMATION_CLASS Class,
 *                        PVOID Buffer, ULONG Length, PULONG ReturnLength). */
static NTSTATUS NtQueryInformationAtom_handler(uint64_t atom, uint64_t info_class,
                                               uint64_t buffer, uint64_t buf_len,
                                               uint64_t retlen_out, uint64_t a6)
{
    (void)a6;
    if (info_class != AtomBasicInformation)
        return STATUS_INVALID_PARAMETER;   /* AtomTableInformation: no owner yet */

    uint16_t usage = 0;
    uint32_t nlen = 0;
    uint16_t kname[NT_MAX_ATOM_LEN + 1];
    NTSTATUS st = nt_atom_query_basic((uint16_t)(atom & 0xFFFFu), &usage,
                                      kname, NT_MAX_ATOM_LEN, &nlen);
    if (st != STATUS_SUCCESS)
        return st;

    /* Length is a 32-bit ULONG in the NT ABI; narrow the raw 64-bit syscall
     * arg so a high-bit-set value cannot pass the size check with a low-32
     * capacity that is actually too small for the record. */
    uint32_t length = (uint32_t)buf_len;

    /* Required bytes: header + (name chars + NUL) wide chars. */
    uint32_t required = (uint32_t)__builtin_offsetof(ATOM_BASIC_INFORMATION, Name)
                        + (nlen + 1) * sizeof(uint16_t);
    if (retlen_out) {
        if (ProbeForWriteIfUser((void *)retlen_out, sizeof(uint32_t),
                                sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
        if (copy_to_user((void *)retlen_out, &required, sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    /* A grow-and-retry caller passes Buffer=NULL/Length=0 to learn the size;
     * answer with the required length + BUFFER_TOO_SMALL, not a hard error. */
    if (!buffer || length < required)
        return STATUS_BUFFER_TOO_SMALL;

    /* Build the record in a bounded kernel buffer, then copy out. */
    uint8_t staging[__builtin_offsetof(ATOM_BASIC_INFORMATION, Name)
                    + (NT_MAX_ATOM_LEN + 1) * sizeof(uint16_t)];
    ATOM_BASIC_INFORMATION *abi = (ATOM_BASIC_INFORMATION *)staging;
    uint16_t query_atom = (uint16_t)(atom & 0xFFFFu);
    abi->UsageCount = usage;
    /* Flags: 1 = integer/pinned atom, 0 = string atom (Win ABI). */
    abi->Flags = (query_atom != 0 && query_atom < NT_MAXINTATOM) ? 1u : 0u;
    /* NameLength is a byte count (UNICODE_STRING convention), not chars. */
    abi->NameLength = (uint16_t)(nlen * sizeof(uint16_t));
    for (uint32_t i = 0; i < nlen; i++)
        abi->Name[i] = kname[i];
    abi->Name[nlen] = 0;

    if (ProbeForWriteIfUser((void *)buffer, required, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)buffer, staging, required) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- Locale syscalls ----------------------------------------------------- */

/* NtQueryDefaultLocale(BOOLEAN UserProfile, PLCID DefaultLocaleId).
 * UserProfile TRUE returns the (registry-backed) user locale, FALSE the system
 * locale. Both come from the nls_locale policy (user locale is machine-wide
 * until per-user hives exist). */
static NTSTATUS NtQueryDefaultLocale_handler(uint64_t user_profile, uint64_t lcid_out,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!lcid_out)
        return STATUS_INVALID_PARAMETER;
    /* UserProfile is a BOOLEAN (UCHAR): narrow before branching so high bits of
     * the raw syscall argument (e.g. 0x100 with a FALSE low byte) don't flip it. */
    uint8_t profile = (uint8_t)user_profile;
    uint32_t lcid = profile ? nt_locale_get_default() : nls_locale_get_system();
    if (ProbeForWriteIfUser((void *)lcid_out, sizeof(uint32_t), sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)lcid_out, &lcid, sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtSetDefaultLocale(BOOLEAN UserProfile, LCID DefaultLocaleId). */
static NTSTATUS NtSetDefaultLocale_handler(uint64_t user_profile, uint64_t lcid,
                                           uint64_t a3, uint64_t a4,
                                           uint64_t a5, uint64_t a6)
{
    (void)user_profile; (void)lcid; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Fail closed: this writes a machine-wide global with no privilege check
     * or per-user hive yet, so an unprivileged caller must not corrupt
     * system locale state. Privileged/per-user locale writes are owned by
     * the locale/NLS subsystem (registry policy chooses system vs user). */
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* NtQueryDefaultUILanguage(LANGID *DefaultUILanguageId). */
static NTSTATUS NtQueryDefaultUILanguage_handler(uint64_t langid_out, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!langid_out)
        return STATUS_INVALID_PARAMETER;
    uint16_t langid = nt_locale_get_ui_language();
    if (ProbeForWriteIfUser((void *)langid_out, sizeof(uint16_t), sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)langid_out, &langid, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtSetDefaultUILanguage(LANGID DefaultUILanguageId). */
static NTSTATUS NtSetDefaultUILanguage_handler(uint64_t langid, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)langid; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Fail closed: machine-wide UI-language write is privileged and has no
     * per-user hive owner yet (see NtSetDefaultLocale_handler). */
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* NtQueryInstallUILanguage(LANGID *InstallUILanguageId). */
static NTSTATUS NtQueryInstallUILanguage_handler(uint64_t langid_out, uint64_t a2,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!langid_out)
        return STATUS_INVALID_PARAMETER;
    uint16_t langid = nt_locale_get_install_ui_language();
    if (ProbeForWriteIfUser((void *)langid_out, sizeof(uint16_t), sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_to_user((void *)langid_out, &langid, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- MUI / UI-language syscalls (SSDT 0x0263-0x0265) --------------------- */

/* NtIsUILanguageComitted(LANGID LanguageId, PBOOLEAN Committed) -- reports
 * whether LanguageId is the active (committed) UI language. Read-only; the
 * active UI language is the atomic policy cache, so no lock is held. */
static NTSTATUS NtIsUILanguageComitted_handler(uint64_t langid, uint64_t committed_out,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!committed_out)
        return STATUS_INVALID_PARAMETER;
    /* Probe the output before the (lock-free) comparison so a bad user pointer
     * fails without any work. */
    if (ProbeForWriteIfUser((void *)committed_out, sizeof(uint8_t), sizeof(uint8_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    uint8_t committed = ((uint16_t)langid == nls_locale_get_ui_language()) ? 1 : 0;
    if (copy_to_user((void *)committed_out, &committed, sizeof(uint8_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* NtFlushInstallUILanguage(LANGID InstallUILanguage, ULONG SetComittedFlag) --
 * commits the install-time UI language machine-wide. Fail closed: this mutates
 * global boot policy with no SRM privilege check or per-user hive yet, exactly
 * like NtSetDefaultLocale/NtSetDefaultUILanguage. */
static NTSTATUS NtFlushInstallUILanguage_handler(uint64_t langid, uint64_t flags,
                                                 uint64_t a3, uint64_t a4,
                                                 uint64_t a5, uint64_t a6)
{
    (void)langid; (void)flags; (void)a3; (void)a4; (void)a5; (void)a6;
    return STATUS_PRIVILEGE_NOT_HELD;
}

/* NtGetMUIRegistryInfo(ULONG Flags, PULONG Size, PVOID Buffer) -- read-only
 * snapshot of HKLM\SYSTEM\Nls (system/user locale + UI language + NLS version).
 * Size is an IN/OUT arg: caller buffer capacity on entry, required bytes on exit
 * (NOT the NtQuerySystemInformation ReturnLength contract -- the required size
 * must be proven against the caller's own capacity before any buffer copy). */
static NTSTATUS NtGetMUIRegistryInfo_handler(uint64_t flags, uint64_t size_ptr,
                                             uint64_t buffer, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    (void)flags; (void)a4; (void)a5; (void)a6;
    MUI_REGISTRY_INFO info;
    const uint32_t need = (uint32_t)sizeof(info);
    uint32_t cap = 0;

    if (!size_ptr)
        return STATUS_INVALID_PARAMETER;
    /* One writable probe covers both the capacity read and the required-size
     * write-back (the size word is in/out, so it must be writable; a writable
     * user range is readable). */
    if (ProbeForWriteIfUser((void *)size_ptr, sizeof(uint32_t), sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&cap, (void *)size_ptr, sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    /* Write the required size back on every outcome (two-pass caller). */
    if (copy_to_user((void *)size_ptr, &need, sizeof(uint32_t)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (!buffer || cap < need)
        return STATUS_INFO_LENGTH_MISMATCH;

    /* Probe the output buffer first (bounds it below MM_USER_PROBE_ADDRESS for a
     * user caller), THEN reject any overlap between the in/out size word and the
     * output buffer: an aliasing caller (size_ptr inside [buffer, buffer+need))
     * would otherwise have the blob copy clobber the required-size word we just
     * returned, so a SUCCESS would leave *size holding blob bytes instead of 16.
     * Both pointers are now probe-bounded, so the +need/+sizeof arithmetic cannot
     * wrap. */
    if (ProbeForWriteIfUser((void *)buffer, need, 4) != 0)
        return STATUS_ACCESS_VIOLATION;
    {
        uintptr_t s0 = (uintptr_t)size_ptr, b0 = (uintptr_t)buffer;
        if (s0 < b0 + need && b0 < s0 + (uintptr_t)sizeof(uint32_t))
            return STATUS_INVALID_PARAMETER;
    }

    info.SystemLocale = nls_locale_get_system();
    info.UserLocale   = nls_locale_get_user();
    info.UILanguage   = nls_locale_get_ui_language();
    info.NlsVersion   = nls_get_version();

    if (copy_to_user((void *)buffer, &info, need) != 0)
        return STATUS_ACCESS_VIOLATION;
    return STATUS_SUCCESS;
}

/* ---- Misc syscalls ------------------------------------------------------- */

/* NtDisplayString(PUNICODE_STRING String) -- writes text to the kernel log
 * (the boot-time text channel). UTF-16 is truncated to ASCII (high byte
 * dropped); non-printable bytes become '?'. */
static NTSTATUS NtDisplayString_handler(uint64_t str_ptr, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* NtDisplayString is TCB-privileged (kernel/session-manager only); an
     * unprivileged caller must not write the shared kernel log. */
    ASSERT_KERNEL_CALLER();
    if (!str_ptr)
        return STATUS_INVALID_PARAMETER;

    UNICODE_STRING us;
    if (ProbeForReadIfUser((const void *)str_ptr, sizeof(us), 8) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (copy_from_user(&us, (const void *)str_ptr, sizeof(us)) != 0)
        return STATUS_ACCESS_VIOLATION;
    if (us.Length == 0 || (us.Length & 1) != 0 || !us.Buffer)
        return STATUS_INVALID_PARAMETER;

    /* Bound the printed length to a single line. */
    uint32_t chars = us.Length / 2;
    if (chars > 255)
        chars = 255;
    uint32_t bytes = chars * 2;
    if (ProbeForReadIfUser(us.Buffer, bytes, sizeof(uint16_t)) != 0)
        return STATUS_ACCESS_VIOLATION;

    uint16_t wbuf[256];
    if (copy_from_user(wbuf, us.Buffer, bytes) != 0)
        return STATUS_ACCESS_VIOLATION;

    char abuf[257];
    for (uint32_t i = 0; i < chars; i++) {
        uint16_t c = wbuf[i];
        abuf[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    abuf[chars] = 0;

    klog(LOG_INFO, "nt", "%s", abuf);
    return STATUS_SUCCESS;
}

/* NtRaiseHardError(NTSTATUS ErrorStatus, ULONG NumberOfParameters,
 *                  PUNICODE_STRING UnicodeStringParameterMask,
 *                  PVOID *Parameters, ULONG ValidResponseOptions,
 *                  PULONG Response).
 *
 * No session-manager/CSRSS exists, so ordinary hard errors are logged and
 * answered ResponseNotHandled. OptionShutdownSystem is the kernel-mediated
 * fatal path: it must be privilege-gated (SeShutdownPrivilege) and bugcheck,
 * NOT silently answered -- until token checks are wired here it returns
 * STATUS_PRIVILEGE_NOT_HELD so an unprivileged caller cannot trigger it. */
static NTSTATUS NtRaiseHardError_handler(uint64_t error_status, uint64_t num_params,
                                         uint64_t param_mask, uint64_t parameters,
                                         uint64_t valid_options, uint64_t response_out)
{
    (void)param_mask; (void)parameters;

    /* Shutdown hard errors are the kernel-mediated fatal path: privilege-gated
     * before any side effect (no unprivileged bugcheck/shutdown). */
    if ((valid_options & 0xFFFFFFFFu) == OptionShutdownSystem)
        return STATUS_PRIVILEGE_NOT_HELD;

    /* Only trusted (kernel-mode) callers write the shared diagnostic log; an
     * unprivileged process must not spam LOG_ERROR and evict real diagnostics.
     * User-mode ordinary hard errors are answered ResponseNotHandled (no CSRSS
     * to route to) without a global side effect. */
    if (ssdt_previous_mode() != SSDT_USER_MODE)
        klog(LOG_ERROR, "nt",
             "NtRaiseHardError: status=0x%x params=%u options=%u",
             (uint64_t)(error_status & 0xFFFFFFFFu),
             (uint64_t)(num_params & 0xFFFFFFFFu),
             (uint64_t)(valid_options & 0xFFFFFFFFu));

    if (response_out) {
        uint32_t response = ResponseNotHandled;
        if (ProbeForWriteIfUser((void *)response_out, sizeof(uint32_t),
                                sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
        if (copy_to_user((void *)response_out, &response, sizeof(uint32_t)) != 0)
            return STATUS_ACCESS_VIOLATION;
    }
    return STATUS_SUCCESS;
}

/* ---- SSDT registration --------------------------------------------------- */

void nt_misc_register_ssdt(void)
{
    ssdt_register(SSDT_NtDisplayString,          (SSDT_HANDLER)NtDisplayString_handler);
    ssdt_register(SSDT_NtRaiseHardError,         (SSDT_HANDLER)NtRaiseHardError_handler);
    ssdt_register(SSDT_NtQueryDefaultLocale,     (SSDT_HANDLER)NtQueryDefaultLocale_handler);
    ssdt_register(SSDT_NtSetDefaultLocale,       (SSDT_HANDLER)NtSetDefaultLocale_handler);
    ssdt_register(SSDT_NtQueryDefaultUILanguage, (SSDT_HANDLER)NtQueryDefaultUILanguage_handler);
    ssdt_register(SSDT_NtSetDefaultUILanguage,   (SSDT_HANDLER)NtSetDefaultUILanguage_handler);
    ssdt_register(SSDT_NtQueryInstallUILanguage, (SSDT_HANDLER)NtQueryInstallUILanguage_handler);
    ssdt_register(SSDT_NtGetMUIRegistryInfo,     (SSDT_HANDLER)NtGetMUIRegistryInfo_handler);
    ssdt_register(SSDT_NtIsUILanguageComitted,   (SSDT_HANDLER)NtIsUILanguageComitted_handler);
    ssdt_register(SSDT_NtFlushInstallUILanguage, (SSDT_HANDLER)NtFlushInstallUILanguage_handler);
    ssdt_register(SSDT_NtAddAtom,                (SSDT_HANDLER)NtAddAtom_handler);
    ssdt_register(SSDT_NtFindAtom,               (SSDT_HANDLER)NtFindAtom_handler);
    ssdt_register(SSDT_NtDeleteAtom,             (SSDT_HANDLER)NtDeleteAtom_handler);
    ssdt_register(SSDT_NtQueryInformationAtom,   (SSDT_HANDLER)NtQueryInformationAtom_handler);
}
