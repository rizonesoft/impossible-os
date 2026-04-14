/* ============================================================================
 * nt_section.c -- Section and mapped-file SSDT handlers (TODO-05 §18)
 *
 * SSDT 0x005C-0x0062: NtCreateSection, NtOpenSection, NtMapViewOfSection,
 * NtUnmapViewOfSection, NtExtendSection, NtQuerySection,
 * NtAreMappedFilesTheSame.
 * ============================================================================ */

#include "kernel/nt/nt_section.h"
#include "kernel/nt/ssdt.h"
#include "kernel/nt/zw.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nt_types.h"
#include "kernel/nt/service_numbers.h"
#include "kernel/nt/nt_memory.h"
#include "kernel/sched/task.h"
#include "kernel/ob/ob.h"
#include "kernel/ob/ob_section.h"
#include "kernel/ob/peb.h"
#include "kernel/klog.h"

/* Windows STATUS_NOT_MAPPED_VIEW (local name avoids ntstatus.h churn) */
#define NTSTA_NOT_MAPPED_VIEW ((NTSTATUS)0xC0000019)

static struct task *task_from_process_handle(HANDLE h)
{
    uint32_t pid;

    if (h == CURRENT_PROCESS || h == 0)
        return task_current();
    pid = (uint32_t)(uint64_t)(int32_t)h;
    if (pid >= task_count())
        return (struct task *)0;
    return task_get_by_pid(pid);
}

/* Probe OBJECT_ATTRIBUTES + UNICODE_STRING for user callers; kernel tests
 * skip probing when previous mode is kernel. */
static NTSTATUS oa_probe_ascii_name(OBJECT_ATTRIBUTES *oa, const char **out)
{
    UNICODE_STRING *us;
    uint32_t probe_len;
    NTSTATUS st;

    *out = (const char *)0;
    if (!oa)
        return STATUS_SUCCESS;

    st = ProbeForReadIfUser(oa, sizeof(OBJECT_ATTRIBUTES), 8);
    if (!NT_SUCCESS(st))
        return st;

    us = oa->ObjectName;
    if (!us)
        return STATUS_SUCCESS;

    st = ProbeForReadIfUser(us, sizeof(UNICODE_STRING), 4);
    if (!NT_SUCCESS(st))
        return st;

    if (!us->Buffer)
        return STATUS_INVALID_PARAMETER;

    probe_len = (uint32_t)us->Length;
    if (probe_len > 255u)
        probe_len = 255u;
    if (probe_len == 0u)
        return STATUS_INVALID_PARAMETER;

    st = ProbeForReadIfUser(us->Buffer, probe_len, 1);
    if (!NT_SUCCESS(st))
        return st;

    *out = (const char *)us->Buffer;
    return STATUS_SUCCESS;
}

/* ---- NtCreateSection (0x005C) -------------------------------------------
 * a1 = PHANDLE SectionHandle (out)
 * a2 = ACCESS_MASK DesiredAccess (stored on handle; section body uses a5)
 * a3 = OBJECT_ATTRIBUTES* (optional name)
 * a4 = MaximumSize in bytes (QuadPart when non-pointer migration)
 * a5 = (uint64_t)SectionPageProtection | ((uint64_t)AllocationAttributes << 32)
 * a6 = HANDLE FileHandle (0 or INVALID_HANDLE_VALUE = none)
 * ----------------------------------------------------------------------- */
static NTSTATUS NtCreateSection_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t desired_access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    uint64_t max_size = a4;
    uint32_t page_prot = (uint32_t)a5;
    uint32_t alloc_attrs = (uint32_t)(a5 >> 32);
    HANDLE file_h = (HANDLE)(int32_t)a6;
    const char *name = (const char *)0;
    HANDLE h;
    NTSTATUS prn;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    prn = oa_probe_ascii_name(oa, &name);
    if (!NT_SUCCESS(prn))
        return prn;

    if (page_prot == 0)
        page_prot = PAGE_READWRITE;

    h = ObCreateSectionEx(&task_current()->handle_table, max_size, page_prot,
                          alloc_attrs, file_h, name, desired_access);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_NO_MEMORY;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtOpenSection (0x005D) --------------------------------------------- */

static NTSTATUS NtOpenSection_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE *out_handle = (HANDLE *)a1;
    uint32_t desired_access = (uint32_t)a2;
    OBJECT_ATTRIBUTES *oa = (OBJECT_ATTRIBUTES *)a3;
    const char *name = (const char *)0;
    HANDLE h;
    NTSTATUS prn;

    (void)a4; (void)a5; (void)a6;

    if (!out_handle)
        return STATUS_INVALID_PARAMETER;

    prn = oa_probe_ascii_name(oa, &name);
    if (!NT_SUCCESS(prn))
        return prn;
    if (!name)
        return STATUS_INVALID_PARAMETER;

    h = ObOpenSection(&task_current()->handle_table, desired_access, name);
    if (h == INVALID_HANDLE_VALUE)
        return STATUS_OBJECT_NAME_NOT_FOUND;

    *out_handle = h;
    return STATUS_SUCCESS;
}

/* ---- NtMapViewOfSection (0x005E) ----------------------------------------
 * a1 = SectionHandle, a2 = ProcessHandle, a3 = PVOID* BaseAddress,
 * a4 = ZeroBits (ignored),
 * a5 = NT_MAPVIEW_ARGS* optional (NULL = legacy: map full section),
 * a6 = reserved (0).
 * ----------------------------------------------------------------------- */
static NTSTATUS NtMapViewOfSection_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE section_handle = (HANDLE)(int32_t)a1;
    HANDLE proc_handle = (HANDLE)(int32_t)a2;
    void **base_ptr = (void **)a3;
    NT_MAPVIEW_ARGS *ex = (NT_MAPVIEW_ARGS *)a5;
    struct task *target = task_from_process_handle(proc_handle);
    HANDLE_TABLE_ENTRY *ent;
    SECTION_OBJECT *so;
    uintptr_t addr;
    uint64_t section_off = 0;
    uint32_t view_sz = 0;
    NTSTATUS pr;

    (void)a4; (void)a6;

    if (!base_ptr)
        return STATUS_INVALID_PARAMETER;

    if (!target)
        return STATUS_INVALID_HANDLE;

    if (target != task_current())
        return STATUS_ACCESS_DENIED;

    ent = ObpLookupHandle(&task_current()->handle_table, section_handle);
    if (!ent || OB_HEADER_FROM_BODY(ent->object)->type != ObpSectionType)
        return STATUS_INVALID_HANDLE;

    so = (SECTION_OBJECT *)ent->object;

    if (ex) {
        pr = ProbeForReadIfUser(ex, sizeof(NT_MAPVIEW_ARGS), 4);
        if (!NT_SUCCESS(pr))
            return pr;
        if (ex->SectionOffsetQuad < 0)
            return STATUS_INVALID_PARAMETER;
        section_off = (uint64_t)ex->SectionOffsetQuad;
        if (ex->ViewSize) {
            pr = ProbeForWriteIfUser(ex->ViewSize, sizeof(uint64_t), 8);
            if (!NT_SUCCESS(pr))
                return pr;
            if (*ex->ViewSize != 0)
                view_sz = (uint32_t)(*ex->ViewSize > (uint64_t)0xFFFFFFFFu
                                          ? 0xFFFFFFFFu
                                          : *ex->ViewSize);
        }
    }

    addr = ObMapViewOfSectionFull(&task_current()->handle_table,
                                  section_handle, target->pid, section_off,
                                  view_sz);
    if (addr == 0)
        return STATUS_INVALID_HANDLE;

    *base_ptr = (void *)addr;

    if (ex && ex->ViewSize) {
        uint64_t span = (uint64_t)so->size - section_off;

        if (view_sz != 0)
            span = (uint64_t)view_sz;
        *ex->ViewSize = span;
    }
    return STATUS_SUCCESS;
}

/* ---- NtUnmapViewOfSection (0x005F) -------------------------------------- */

static NTSTATUS NtUnmapViewOfSection_handler(uint64_t a1, uint64_t a2,
                                             uint64_t a3, uint64_t a4,
                                             uint64_t a5, uint64_t a6)
{
    HANDLE proc_handle = (HANDLE)(int32_t)a1;
    uintptr_t base = (uintptr_t)a2;
    struct task *target = task_from_process_handle(proc_handle);

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!target)
        return STATUS_INVALID_HANDLE;

    if (ObUnmapViewOfSectionByBase(&target->handle_table, target->pid, base)
        != 0)
        return NTSTA_NOT_MAPPED_VIEW;
    return STATUS_SUCCESS;
}

/* ---- NtExtendSection (0x0060) ------------------------------------------- */

static NTSTATUS NtExtendSection_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE section_handle = (HANDLE)(int32_t)a1;
    LARGE_INTEGER *new_max = (LARGE_INTEGER *)a2;
    HANDLE_TABLE_ENTRY *entry;
    SECTION_OBJECT *so;
    NTSTATUS pr;
    int64_t quad;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (!new_max)
        return STATUS_INVALID_PARAMETER;

    pr = ProbeForReadIfUser(new_max, sizeof(LARGE_INTEGER), 8);
    if (!NT_SUCCESS(pr))
        return pr;

    entry = ObpLookupHandle(&task_current()->handle_table, section_handle);
    if (!entry)
        return STATUS_INVALID_HANDLE;

    if (OB_HEADER_FROM_BODY(entry->object)->type != ObpSectionType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    so = (SECTION_OBJECT *)entry->object;
    quad = new_max->QuadPart;
    if (quad <= 0)
        return STATUS_INVALID_PARAMETER;

    return ObExtendSectionObject(so, (uint64_t)quad);
}

/* ---- NtQuerySection (0x0061) ------------------------------------------- */

static NTSTATUS NtQuerySection_handler(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6)
{
    HANDLE section_handle = (HANDLE)(int32_t)a1;
    uint32_t info_class = (uint32_t)a2;
    void *buffer = (void *)a3;
    uint64_t buf_len = a4;
    uint64_t *ret_len = (uint64_t *)a5;
    HANDLE_TABLE_ENTRY *entry;
    SECTION_OBJECT *so;
    NTSTATUS st;
    NTSTATUS pr;

    (void)a6;

    entry = ObpLookupHandle(&task_current()->handle_table, section_handle);
    if (!entry)
        return STATUS_INVALID_HANDLE;

    if (OB_HEADER_FROM_BODY(entry->object)->type != ObpSectionType)
        return STATUS_OBJECT_TYPE_MISMATCH;

    so = (SECTION_OBJECT *)entry->object;

    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    if (buf_len > 0) {
        pr = ProbeForWriteIfUser(buffer, buf_len, 4);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    if (ret_len) {
        pr = ProbeForWriteIfUser(ret_len, sizeof(uint64_t), 8);
        if (!NT_SUCCESS(pr))
            return pr;
    }

    st = ObQuerySectionObject(so, info_class, buffer, buf_len, ret_len);
    return st;
}

/* ---- NtAreMappedFilesTheSame (0x0062) ----------------------------------- */

static NTSTATUS NtAreMappedFilesTheSame_handler(uint64_t a1, uint64_t a2,
                                               uint64_t a3, uint64_t a4,
                                               uint64_t a5, uint64_t a6)
{
    uintptr_t p1 = (uintptr_t)a1;
    uintptr_t p2 = (uintptr_t)a2;
    uint32_t pid = task_current()->pid;

    (void)a3; (void)a4; (void)a5; (void)a6;

    if (p1 == 0 || p2 == 0)
        return STATUS_INVALID_PARAMETER;

    if (ObAreMappedFilesTheSame(&task_current()->handle_table, pid, p1, p2))
        return STATUS_SUCCESS;
    return STATUS_UNSUCCESSFUL;
}

/* ---- Registration -------------------------------------------------------- */

void nt_section_register_ssdt(void)
{
    ssdt_register(SSDT_NtCreateSection, (SSDT_HANDLER)NtCreateSection_handler);
    ssdt_register(SSDT_NtOpenSection, (SSDT_HANDLER)NtOpenSection_handler);
    ssdt_register(SSDT_NtMapViewOfSection,
                  (SSDT_HANDLER)NtMapViewOfSection_handler);
    ssdt_register(SSDT_NtUnmapViewOfSection,
                  (SSDT_HANDLER)NtUnmapViewOfSection_handler);
    ssdt_register(SSDT_NtExtendSection, (SSDT_HANDLER)NtExtendSection_handler);
    ssdt_register(SSDT_NtQuerySection, (SSDT_HANDLER)NtQuerySection_handler);
    ssdt_register(SSDT_NtAreMappedFilesTheSame,
                  (SSDT_HANDLER)NtAreMappedFilesTheSame_handler);

    klog(LOG_INFO, "nt", "NT section: 7 handlers registered (SSDT 0x005C-0x0062)");
}
