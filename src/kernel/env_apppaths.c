/* ============================================================================
 * env_apppaths.c -- App Paths registry-based executable lookup (TODO-22 s17)
 *
 * See include/kernel/env_apppaths.h for the contract and security posture.
 * Arch-neutral: depends only on env (elevation gate) + registry; no
 * gdt/idt/msr/cpuid. Post-Phase-3 code (no POST16). Holds no locks and does no
 * allocation -- all state lives in caller buffers and the registry. The registry
 * (registry.c) is deliberately LOCK-FREE, so these functions inherit its
 * registry-wide SMP-synchronization gap: concurrent key/value mutation can race,
 * and the two-value register (default + "Path") is not written or read as an
 * atomic pair. App Paths is as SMP-safe as every other RegXxx consumer today;
 * registry-wide locking + batched multi-value writes are owned by TODO-14 s14.
 * ============================================================================ */

#include "kernel/env_apppaths.h"
#include "kernel/env.h"        /* env_is_secure_context */
#include "registry.h"

/* Base container key, relative to the HKLM/HKCU root sentinel. */
#define APP_PATHS_BASE  "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths"

/* Longest subkey path this module assembles: base + '\\' + name + ".exe" + NUL.
 * APP_PATHS_BASE is 51 chars; APP_PATHS_NAME_MAX is 255; 51+1+255+4+1 = 312. */
#define APP_PATHS_SUBKEY_MAX  320u

static uint32_t ap_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Build "SOFTWARE\...\App Paths\{name}" into `out` (size APP_PATHS_SUBKEY_MAX),
 * appending ".exe" when `name` carries no '.'. Rejects an empty / over-long name
 * or one containing a path separator (a single subkey component only -- anything
 * with '\' or '/' would traverse out of the App Paths container). Returns
 * ERROR_SUCCESS or ERROR_INVALID_PARAMETER. */
static long ap_build_subkey(const char *name, char *out, uint32_t out_size)
{
    uint32_t nlen = 0;
    int has_dot = 0;
    uint32_t blen, need, o, i;
    const char *base = APP_PATHS_BASE;

    if (!name || !out)
        return ERROR_INVALID_PARAMETER;

    for (const char *p = name; *p; ++p) {
        char c = *p;
        if (c == '\\' || c == '/')
            return ERROR_INVALID_PARAMETER;
        if (c == '.')
            has_dot = 1;
        nlen++;
        if (nlen > APP_PATHS_NAME_MAX)
            return ERROR_INVALID_PARAMETER;
    }
    if (nlen == 0)
        return ERROR_INVALID_PARAMETER;

    blen = ap_strlen(base);
    need = blen + 1u + nlen + (has_dot ? 0u : 4u) + 1u;   /* base '\' name [.exe] NUL */
    if (need > out_size)
        return ERROR_INVALID_PARAMETER;

    o = 0;
    for (i = 0; i < blen; ++i)
        out[o++] = base[i];
    out[o++] = '\\';
    for (i = 0; i < nlen; ++i)
        out[o++] = name[i];
    if (!has_dot) {
        out[o++] = '.';
        out[o++] = 'e';
        out[o++] = 'x';
        out[o++] = 'e';
    }
    out[o] = '\0';
    return ERROR_SUCCESS;
}

/* Read a REG_SZ / REG_EXPAND_SZ value (default value when value_name is NULL)
 * from root\subkey into `out`. Guarantees NUL-termination on success even for a
 * malformed non-terminated stored value. Reports the required size incl NUL via
 * *out_needed. A value present but not a string type is reported as
 * ERROR_FILE_NOT_FOUND (App Paths values are strings). */
static long ap_read_str(HKEY root, const char *subkey, const char *value_name,
                        char *out, uint32_t out_size, uint32_t *out_needed)
{
    uint32_t type = 0;
    uint32_t cb;
    long rc;

    if (out_needed)
        *out_needed = 0;

    /* No output buffer: probe existence + size only. RegQueryValueEx returns the
     * raw stored size in cb (ERROR_SUCCESS when lpData is NULL) but cannot report
     * whether the value is NUL-terminated. Report cb+1 so the size ALWAYS fits the
     * follow-up read: a malformed non-terminated value needs room for the
     * terminator this layer appends (matching the exact-fit read branch below); a
     * well-formed value already includes its NUL, so the extra byte is harmless
     * upper-bound slack. */
    if (!out || out_size == 0) {
        cb = 0;
        rc = RegReadKeyValue(root, subkey, value_name, &type, (uint8_t *)0, &cb);
        if (rc != ERROR_SUCCESS)
            return rc;
        if (type != REG_SZ && type != REG_EXPAND_SZ)
            return ERROR_FILE_NOT_FOUND;
        if (out_needed)
            *out_needed = cb + 1u;
        return ERROR_MORE_DATA;
    }

    cb = out_size;
    rc = RegReadKeyValue(root, subkey, value_name, &type, (uint8_t *)out, &cb);
    if (rc == ERROR_MORE_DATA) {
        if (out_needed)
            *out_needed = cb;                 /* required size incl NUL */
        return ERROR_MORE_DATA;
    }
    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_SZ && type != REG_EXPAND_SZ)
        return ERROR_FILE_NOT_FOUND;

    /* Guarantee NUL-termination against a malformed non-terminated value. When
     * such a value exactly fills the buffer it cannot be terminated in place, so
     * report MORE_DATA with room for the terminator rather than truncating a
     * different path and falsely claiming success. */
    if (cb == 0) {
        out[0] = '\0';
        cb = 1;
    } else if (out[cb - 1] != '\0') {
        if (cb < out_size) {
            out[cb++] = '\0';
        } else {
            if (out_needed)
                *out_needed = cb + 1u;
            return ERROR_MORE_DATA;
        }
    }
    if (out_needed)
        *out_needed = cb;
    return ERROR_SUCCESS;
}

long app_paths_lookup(struct task *caller, const char *name,
                      char *out_path, uint32_t out_path_size,
                      uint32_t *out_path_needed,
                      char *out_additional, uint32_t out_add_size,
                      uint32_t *out_add_needed)
{
    char subkey[APP_PATHS_SUBKEY_MAX];
    HKEY roots[2];
    int nroots, i, hklm_only;
    long rc;

    if (out_path_needed)
        *out_path_needed = 0;
    if (out_add_needed)
        *out_add_needed = 0;

    rc = ap_build_subkey(name, subkey, sizeof subkey);
    if (rc != ERROR_SUCCESS)
        return rc;

    /* Root precedence bound to caller elevation. A NULL caller has no identity to
     * trust, so it fails closed to HKLM-only alongside genuinely elevated callers
     * (env_is_secure_context(NULL) returns 0, hence the explicit NULL test). A
     * normal <= Medium caller gets the Windows HKCU-first order. */
    hklm_only = (caller == (struct task *)0) || env_is_secure_context(caller);
    if (hklm_only) {
        roots[0] = HKEY_LOCAL_MACHINE;
        nroots = 1;
    } else {
        roots[0] = HKEY_CURRENT_USER;
        roots[1] = HKEY_LOCAL_MACHINE;
        nroots = 2;
    }

    for (i = 0; i < nroots; ++i) {
        uint32_t need = 0;
        rc = ap_read_str(roots[i], subkey, (const char *)0 /* default value */,
                         out_path, out_path_size, &need);
        if (rc == ERROR_FILE_NOT_FOUND)
            continue;                         /* not in this root -- try next */

        /* Found, or a hard result (MORE_DATA / ACCESS_DENIED / ...). A present
         * entry that will not fit, or an inaccessible one, is returned as-is --
         * never downgraded to a lower-precedence root or to not-found. */
        if (out_path_needed)
            *out_path_needed = need;

        if (rc == ERROR_SUCCESS && (out_additional || out_add_needed)) {
            uint32_t anp = 0;
            long arc = ap_read_str(roots[i], subkey, "Path",
                                   out_additional, out_add_size, &anp);
            if (out_add_needed)
                *out_add_needed = (arc == ERROR_FILE_NOT_FOUND) ? 0u : anp;
            /* The optional Path is independent: absence or a too-small/failed
             * read must not erase the valid executable result. */
            if (out_additional && out_add_size > 0 && arc != ERROR_SUCCESS)
                out_additional[0] = '\0';
        }
        return rc;
    }

    return ERROR_FILE_NOT_FOUND;
}

long app_paths_register(struct task *caller, HKEY root, const char *name,
                        const char *full_path, const char *additional_path)
{
    char subkey[APP_PATHS_SUBKEY_MAX];
    HKEY hKey = (HKEY)0;
    uint32_t disp = 0;
    long rc;

    if (!full_path)
        return ERROR_INVALID_PARAMETER;
    if (root != HKEY_LOCAL_MACHINE && root != HKEY_CURRENT_USER)
        return ERROR_INVALID_PARAMETER;

    rc = ap_build_subkey(name, subkey, sizeof subkey);
    if (rc != ERROR_SUCCESS)
        return rc;

    /* Machine-wide (HKLM) registration requires a PROVEN elevated caller; this is
     * the API-level authorization boundary until registry DACL enforcement lands
     * (reg_check_access is currently always-grant, and predefined roots carry
     * implicit full access, so nothing below this gate would refuse the write).
     * env_is_proven_elevated (NOT env_is_secure_context) is the authorization
     * predicate: it requires the token's authoritative IsElevated flag AND a
     * High+ integrity level, denying a NULL, identity-less, malformed-token,
     * IsElevated-clear, or below-High caller -- a fail-closed authorization gate,
     * so an inconsistent token cannot be authorized to write machine-wide state. */
    if (root == HKEY_LOCAL_MACHINE && !env_is_proven_elevated(caller))
        return ERROR_ACCESS_DENIED;

    rc = RegCreateKeyEx(root, subkey, 0, (const char *)0, 0, KEY_WRITE,
                        (void *)0, &hKey, &disp);
    if (rc != ERROR_SUCCESS)
        return rc;

    rc = RegSetString(hKey, (const char *)0 /* default value */, full_path);
    if (rc != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return rc;
    }

    /* Deterministic Path replacement: set when provided, else DELETE any existing
     * value so a repair re-registration cannot retain a stale search directory. */
    if (additional_path) {
        rc = RegSetString(hKey, "Path", additional_path);
    } else {
        rc = RegDeleteValue(hKey, "Path");
        if (rc == ERROR_FILE_NOT_FOUND)
            rc = ERROR_SUCCESS;               /* nothing to delete is success */
    }

    RegCloseKey(hKey);
    return rc;
}
