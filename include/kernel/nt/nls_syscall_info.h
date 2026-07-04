/* ============================================================================
 * nls_syscall_info.h -- SYSTEM_NLS_INFORMATION + MUI_REGISTRY_INFO ABI
 *
 * Shared contract for the read-only NLS/locale queries exposed through
 * NtQuerySystemInformation(SystemNlsInformation) and NtGetMUIRegistryInfo
 * (SSDT 0x0263). Both the kernel marshallers (nt_syscall.c / nt_misc.c) and the
 * unit tests consume this single definition so the on-the-wire layout cannot
 * drift. Impossible OS extension class value 0x1001 sits above the Windows
 * SYSTEM_INFORMATION_CLASS range (0x1000 is SystemKernelConfigInformation).
 *
 * These are READ-ONLY snapshots of already-published NLS state (code-page
 * policy, locale/LCID policy, UI-language, active NLS/collation version). The
 * SET/commit path (NtSetDefaultLocale/UILanguage, NtFlushInstallUILanguage) is
 * fail-closed until the security reference monitor's privilege checks exist.
 * ============================================================================ */
#ifndef KERNEL_NT_NLS_SYSCALL_INFO_H
#define KERNEL_NT_NLS_SYSCALL_INFO_H

#include "kernel/types.h"

/* SYSTEM_INFORMATION_CLASS extension for NtQuerySystemInformation. There is no
 * real Windows SystemNlsInformation class value to collide with; this is a
 * deliberate Impossible OS extension in the 0x1000+ range. */
#define SystemNlsInformation  0x1001

/* Read-only snapshot of the active NLS/locale policy. NlsVersion is the same
 * value GetNLSVersionEx reports (nls_get_version()); the full NLSVERSIONINFOEX
 * tuple -- DefinedVersion / EffectiveId / GuidCustomVersion -- is deferred until
 * the NLS ABI can carry those fields honestly (the public GetNLSVersionEx export
 * is owned by the NLS native-syscall layer). */
typedef struct _SYSTEM_NLS_INFORMATION {
    uint32_t AnsiCodePage;      /* nls_cp_get_acp() */
    uint32_t OemCodePage;       /* nls_cp_get_oemcp() */
    uint32_t SystemLcid;        /* nls_locale_get_system() */
    uint32_t UserLcid;          /* nls_locale_get_user() */
    uint32_t NlsVersion;        /* nls_get_version() (GetNLSVersionEx backing) */
    uint16_t UiLangId;          /* nt_locale_get_ui_language() */
    uint16_t InstallUiLangId;   /* nt_locale_get_install_ui_language() */
} __attribute__((packed)) SYSTEM_NLS_INFORMATION;

_Static_assert(sizeof(SYSTEM_NLS_INFORMATION) == 24,
    "SYSTEM_NLS_INFORMATION ABI size pinned at 24 bytes");
_Static_assert(__builtin_offsetof(SYSTEM_NLS_INFORMATION, NlsVersion) == 16,
    "SYSTEM_NLS_INFORMATION.NlsVersion ABI offset pinned at 16");
_Static_assert(__builtin_offsetof(SYSTEM_NLS_INFORMATION, UiLangId) == 20,
    "SYSTEM_NLS_INFORMATION.UiLangId ABI offset pinned at 20");

/* Read-only MUI registry snapshot returned by NtGetMUIRegistryInfo (SSDT 0x0263)
 * from HKLM\SYSTEM\Nls. The in/out size argument is the caller buffer capacity
 * on entry and the required byte count on exit (NOT the NtQuerySystemInformation
 * ReturnLength contract). */
typedef struct _MUI_REGISTRY_INFO {
    uint32_t SystemLocale;      /* HKLM\SYSTEM\Nls SystemLocale (LCID) */
    uint32_t UserLocale;        /* HKLM\SYSTEM\Nls UserLocale (LCID) */
    uint32_t UILanguage;        /* HKLM\SYSTEM\Nls UILanguage (LANGID) */
    uint32_t NlsVersion;        /* active NLS/collation version */
} __attribute__((packed)) MUI_REGISTRY_INFO;

_Static_assert(sizeof(MUI_REGISTRY_INFO) == 16,
    "MUI_REGISTRY_INFO ABI size pinned at 16 bytes");

#endif /* KERNEL_NT_NLS_SYSCALL_INFO_H */
