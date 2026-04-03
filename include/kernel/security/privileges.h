/* ============================================================================
 * privileges.h -- Privilege LUID constants, LUID_AND_ATTRIBUTES, PRIVILEGE_SET
 *
 * Each privilege is identified by a well-known LUID (HighPart=0,
 * LowPart=<n>).  These match the Windows NT privilege numbering so
 * that token dumps are comparable across systems.
 * ============================================================================ */

#pragma once

#include "kernel/security/luid.h"

/* --- Privilege LUID constants -------------------------------------------- */

#define SE_PRIVILEGE_LUID(n)  { (n), 0 }

#define SE_CREATE_TOKEN_PRIVILEGE           SE_PRIVILEGE_LUID(2)
#define SE_ASSIGNPRIMARYTOKEN_PRIVILEGE     SE_PRIVILEGE_LUID(3)
#define SE_LOCK_MEMORY_PRIVILEGE            SE_PRIVILEGE_LUID(4)
#define SE_INCREASE_QUOTA_PRIVILEGE         SE_PRIVILEGE_LUID(5)
#define SE_TCB_PRIVILEGE                    SE_PRIVILEGE_LUID(7)
#define SE_SECURITY_PRIVILEGE               SE_PRIVILEGE_LUID(8)
#define SE_TAKE_OWNERSHIP_PRIVILEGE         SE_PRIVILEGE_LUID(9)
#define SE_LOAD_DRIVER_PRIVILEGE            SE_PRIVILEGE_LUID(10)
#define SE_SYSTEM_PROFILE_PRIVILEGE         SE_PRIVILEGE_LUID(11)
#define SE_SYSTEMTIME_PRIVILEGE             SE_PRIVILEGE_LUID(12)
#define SE_PROF_SINGLE_PROCESS_PRIVILEGE    SE_PRIVILEGE_LUID(13)
#define SE_INC_BASE_PRIORITY_PRIVILEGE      SE_PRIVILEGE_LUID(14)
#define SE_CREATE_PAGEFILE_PRIVILEGE        SE_PRIVILEGE_LUID(15)
#define SE_BACKUP_PRIVILEGE                 SE_PRIVILEGE_LUID(17)
#define SE_RESTORE_PRIVILEGE                SE_PRIVILEGE_LUID(18)
#define SE_SHUTDOWN_PRIVILEGE               SE_PRIVILEGE_LUID(19)
#define SE_DEBUG_PRIVILEGE                  SE_PRIVILEGE_LUID(20)
#define SE_AUDIT_PRIVILEGE                  SE_PRIVILEGE_LUID(21)
#define SE_CHANGE_NOTIFY_PRIVILEGE          SE_PRIVILEGE_LUID(23)
#define SE_UNDOCK_PRIVILEGE                 SE_PRIVILEGE_LUID(25)
#define SE_MANAGE_VOLUME_PRIVILEGE          SE_PRIVILEGE_LUID(28)
#define SE_IMPERSONATE_PRIVILEGE            SE_PRIVILEGE_LUID(29)
#define SE_CREATE_GLOBAL_PRIVILEGE          SE_PRIVILEGE_LUID(30)
#define SE_CREATE_SYMBOLIC_LINK_PRIVILEGE   SE_PRIVILEGE_LUID(35)

/* Highest well-known privilege LowPart (for iteration bounds) */
#define SE_MAX_WELL_KNOWN_PRIVILEGE         35

/* Named LUID globals for runtime lookup */
extern const LUID SeCreateTokenPrivilege;
extern const LUID SeAssignPrimaryTokenPrivilege;
extern const LUID SeLockMemoryPrivilege;
extern const LUID SeIncreaseQuotaPrivilege;
extern const LUID SeTcbPrivilege;
extern const LUID SeSecurityPrivilege;
extern const LUID SeTakeOwnershipPrivilege;
extern const LUID SeLoadDriverPrivilege;
extern const LUID SeSystemProfilePrivilege;
extern const LUID SeSystemtimePrivilege;
extern const LUID SeProfileSingleProcessPrivilege;
extern const LUID SeIncreaseBasePriorityPrivilege;
extern const LUID SeCreatePagefilePrivilege;
extern const LUID SeBackupPrivilege;
extern const LUID SeRestorePrivilege;
extern const LUID SeShutdownPrivilege;
extern const LUID SeDebugPrivilege;
extern const LUID SeAuditPrivilege;
extern const LUID SeChangeNotifyPrivilege;
extern const LUID SeUndockPrivilege;
extern const LUID SeManageVolumePrivilege;
extern const LUID SeImpersonatePrivilege;
extern const LUID SeCreateGlobalPrivilege;
extern const LUID SeCreateSymbolicLinkPrivilege;

/* --- LUID_AND_ATTRIBUTES ------------------------------------------------ */

/* Privilege attribute flags */
#define SE_PRIVILEGE_ENABLED_BY_DEFAULT  0x00000001
#define SE_PRIVILEGE_ENABLED             0x00000002
#define SE_PRIVILEGE_REMOVED             0x00000004
#define SE_PRIVILEGE_USED_FOR_ACCESS     0x80000000

typedef struct {
    LUID     Luid;
    uint32_t Attributes;
} LUID_AND_ATTRIBUTES;

/* --- PRIVILEGE_SET ------------------------------------------------------- */

/* Control flag: all privileges in the set must be held */
#define PRIVILEGE_SET_ALL_NECESSARY  1

typedef struct {
    uint32_t            PrivilegeCount;
    uint32_t            Control;
    LUID_AND_ATTRIBUTES Privilege[];
} PRIVILEGE_SET;

/* --- TOKEN_PRIVILEGES ---------------------------------------------------- */

typedef struct {
    uint32_t            PrivilegeCount;
    LUID_AND_ATTRIBUTES Privileges[];
} TOKEN_PRIVILEGES;

/* --- Debug helper -------------------------------------------------------- */

/*
 * RtlPrivilegeSetToString -- format a PRIVILEGE_SET as a human-readable
 * string: "SeShutdownPrivilege(E) SeDebugPrivilege(D) ..."
 * E=Enabled, D=Disabled, R=Removed.
 * Returns chars written (excluding NUL), or -1 on error.
 */
int RtlPrivilegeSetToString(const PRIVILEGE_SET *ps, char *buf, uint32_t len);

/*
 * RtlPrivilegeLuidToName -- return the name string for a well-known
 * privilege LUID, or NULL if not recognized.
 */
const char *RtlPrivilegeLuidToName(const LUID *luid);
