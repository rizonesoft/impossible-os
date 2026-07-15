/* ============================================================================
 * env_searchpath.c -- SearchPathW / SearchPathA kernel32 API (TODO-22 s14)
 *
 * See include/kernel/env_searchpath.h for the contract and security posture.
 * Arch-neutral: depends only on env / task / vfs / pledge / nls_cp / teb; no
 * gdt/idt/msr/cpuid. Post-Phase-3 code (no POST16). All search logic lives in
 * the UTF-8 core env_search_path(); the W/A wrappers only translate encodings.
 * ============================================================================ */

#include "kernel/env_searchpath.h"
#include "kernel/env.h"
#include "kernel/sched/task.h"
#include "kernel/fs/vfs.h"
#include "kernel/nt/pledge.h"
#include "kernel/nt/ntstatus.h"
#include "kernel/nt/nls_cp.h"
#include "kernel/ob/teb.h"
#include "kernel/mm/heap.h"           /* kmalloc / kfree (sp_work scratch) */
#include "registry.h"                 /* ERROR_* Win32 codes */

/* ---- small local string helpers (freestanding) --------------------------- */

static uint32_t sp_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint32_t sp_wcslen(const uint16_t *s)
{
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* This file's view of the shared env buffer allocator (env.h). The size-class rule
 * itself lives there; these adapters keep this file's local contract: a 0-byte
 * request still yields a real 1-byte buffer, because a search-path leg may legally
 * be the empty string and the callers below hand the result to string code that
 * expects a writable NUL slot. env_buf_alloc(0) is NULL by contract, so the
 * normalization must happen HERE -- and identically in both halves, since the count
 * selects the deallocator. */
static char *sp_alloc(uint32_t n)
{
    if (n == 0)
        n = 1;
    return (char *)env_buf_alloc(n);
}

static void sp_free(char *p, uint32_t n)
{
    if (n == 0)
        n = 1;
    env_buf_free(p, n);
}

/* Set the executing thread's TEB LastErrorValue when a live TEB exists (a fixture
 * test task has no TEB -> no-op). Mirrors the syscall dispatcher's propagation. */
static void sp_set_last_error(uint32_t code)
{
    struct thread *thr = thread_current();
    if (thr && thr->teb)
        ((TEB *)thr->teb)->LastErrorValue = code;
}

/* ---- name classification ------------------------------------------------- */

/* A name is "qualified" (probed directly, bypassing directory iteration) ONLY
 * when it carries its own anchor: rooted/UNC (`\...`, `/...`), drive-qualified
 * (`X:...`), or explicitly CWD-relative (`.\`, `./`, `..`, `..\`, `../`). An
 * ORDINARY relative subpath such as `plugins\tool.exe` is NOT qualified -- it is
 * searched BENEATH every configured/default directory, so it cannot bypass an
 * explicit lpPath or the trusted default-search legs (Win32/Wine classification).*/
static int sp_is_qualified(const char *name)
{
    if (name[0] == '\\' || name[0] == '/')          /* rooted or UNC */
        return 1;
    if (name[0] && name[1] == ':')                  /* "X:..." drive-qualified */
        return 1;
    if (name[0] == '.') {                           /* explicitly CWD-relative */
        char c1 = name[1];
        if (c1 == '\\' || c1 == '/')                /* ".\" or "./" */
            return 1;
        if (c1 == '.' &&
            (name[2] == '\\' || name[2] == '/' || name[2] == '\0'))
            return 1;                               /* "..", "..\", or "../" */
    }
    return 0;
}

/* True if any path component of `name` is exactly "..". An unqualified name is
 * searched BENEATH a trusted/explicit directory, and vfs_resolve_path collapses
 * ".." toward the drive root without a descendant check -- so an interior ".."
 * would let the joined candidate canonicalize OUT of the selected directory.
 * Such names are rejected before iteration (a "." component is harmless -- it
 * collapses in place -- so only ".." is caught). */
static int sp_has_parent_component(const char *name)
{
    uint32_t i = 0, cs = 0;
    for (;;) {
        char c = name[i];
        if (c == '\\' || c == '/' || c == '\0') {
            if (i - cs == 2 && name[cs] == '.' && name[cs + 1] == '.')
                return 1;
            cs = i + 1;
            if (c == '\0')
                return 0;
        }
        i++;
    }
}

/* The final path component has an extension when it contains ANY '.', including
 * a leading one: Win32 classifies ".gitignore"/".bashrc" as having extension
 * ".gitignore"/".bashrc", so lpExtension is not appended to such names. */
static int sp_final_has_ext(const char *name)
{
    uint32_t i, comp_start = 0;
    for (i = 0; name[i]; i++)
        if (name[i] == '\\' || name[i] == '/')
            comp_start = i + 1;
    for (i = comp_start; name[i]; i++)
        if (name[i] == '.')
            return 1;
    return 0;
}

/* Build the search name (lpFileName + optional lpExtension) into out[SP_PATH_MAX].
 * The extension is appended only when it starts with '.' and the final component
 * has no extension. Returns the length, or 0 on overflow. */
static uint32_t sp_build_search_name(const char *filename, const char *ext,
                                     char *out)
{
    uint32_t fn = sp_strlen(filename);
    uint32_t el = 0, i;
    int append = 0;

    /* Filename-has-extension check FIRST: lpExtension is consulted only when the
     * final component has none (Win32 contract); the leading '.' is then required
     * (the core rejects a non-dot extension before reaching here). */
    if (!sp_final_has_ext(filename) && ext && ext[0] == '.') {
        el = sp_strlen(ext);
        append = 1;
    }
    if (fn + el + 1 > SP_PATH_MAX)
        return 0;
    for (i = 0; i < fn; i++)
        out[i] = filename[i];
    if (append)
        for (i = 0; i < el; i++)
            out[fn + i] = ext[i];
    out[fn + el] = '\0';
    return fn + el;
}

/* Join a directory (given as pointer + length so a %PATH% substring needs no
 * copy) and a file name into out[SP_PATH_MAX] with exactly one separator.
 * Returns the length, or 0 on overflow. */
static uint32_t sp_join_n(const char *dir, uint32_t dl, const char *name,
                          char *out)
{
    uint32_t nl = sp_strlen(name), pos = 0, i;

    while (dl > 0 && (dir[dl - 1] == '\\' || dir[dl - 1] == '/'))
        dl--;                                   /* drop trailing separators */
    if (dl + 1 + nl + 1 > SP_PATH_MAX)
        return 0;
    for (i = 0; i < dl; i++)
        out[pos++] = dir[i];
    out[pos++] = '\\';
    for (i = 0; i < nl; i++)
        out[pos++] = name[i];
    out[pos] = '\0';
    return pos;
}

static uint32_t sp_join(const char *dir, const char *name, char *out)
{
    return sp_join_n(dir, sp_strlen(dir), name, out);
}

/* Caller-aware existence probe: resolve `candidate` against the caller's cwd,
 * enforce the caller's unveil 'r' policy under pledge, and require a regular
 * file. Denied or missing paths (and directories) read as "not a hit". On a hit,
 * `resolved` (SP_PATH_MAX) holds the canonical absolute path. */
static int sp_probe(struct task *caller, const char *candidate, char *resolved)
{
    struct vfs_stat st;

    if (task_resolve_path_for(caller, candidate, resolved, SP_PATH_MAX) != 0)
        return 0;
    if (pledge_user_mode() &&
        unveil_check(caller, resolved, UNVEIL_R) != STATUS_SUCCESS)
        return 0;                               /* sandbox denial reads as absent */
    if (vfs_stat(resolved, &st) != 0)
        return 0;
    if (st.type != VFS_FILE)
        return 0;                               /* a directory is not a hit */
    return 1;
}

/* ---- %PATH% iteration ----------------------------------------------------- */

/* Search every ';'-delimited directory in the caller's %PATH% (empty elements
 * skipped). candidate/resolved are the shared SP_PATH_MAX scratch buffers.
 * Returns 1 = found, 0 = not found / no PATH, -1 = allocation failure (the
 * caller must NOT fall through to a later leg, or a memory-pressure miss would
 * silently reorder the safe-search CWD leg ahead of the intended PATH hit). */
static int sp_search_path_env(struct task *caller, const char *name,
                              char *candidate, char *resolved)
{
    uint32_t cap = 512;
    char *pathbuf;
    int rc;
    uint32_t i, start;
    int found = 0;

    pathbuf = sp_alloc(cap);
    if (!pathbuf)
        return -1;                              /* OOM: propagate, do not fall through */
    /* Re-fetch until a coherent, NON-truncated snapshot is obtained: a sibling
     * NtSetEnvironmentVariable can grow %PATH% between env_get_copy's length
     * probe and its copy, so a single grow-once refetch could still truncate and
     * silently drop trailing PATH entries. %PATH% is hard-capped at
     * ENV_VALUE_MAX, so cap converges within a few iterations. */
    for (;;) {
        rc = env_get_copy(caller, "PATH", pathbuf, cap);
        if (rc < 0) {                           /* no PATH set */
            sp_free(pathbuf, cap);
            return 0;
        }
        if ((uint32_t)rc < cap)                 /* fit: the snapshot is complete */
            break;
        {                                       /* grew/truncated: enlarge and retry */
            uint32_t need = (uint32_t)rc + 1;
            sp_free(pathbuf, cap);
            cap = need;
            pathbuf = sp_alloc(cap);
            if (!pathbuf)
                return -1;                      /* OOM: propagate */
        }
    }

    start = 0;
    for (i = 0;; i++) {
        char c = pathbuf[i];
        if (c == ';' || c == '\0') {
            uint32_t len = i - start;
            if (len > 0 &&
                sp_join_n(&pathbuf[start], len, name, candidate) &&
                sp_probe(caller, candidate, resolved)) {
                found = 1;
                break;
            }
            start = i + 1;
            if (c == '\0')
                break;
        }
    }

    sp_free(pathbuf, cap);
    return found;
}

/* ---- default (lpPath == NULL) ordered search ----------------------------- */

/* Returns 1 = found, 0 = not found, -1 = allocation failure (propagated from the
 * PATH leg; the caller reports ERROR_OUTOFMEMORY rather than a fail-open miss). */
static int sp_search_default(struct task *caller, const char *name,
                             char *candidate, char *resolved, char *cwd)
{
    uint32_t mode = __atomic_load_n(&caller->search_path_mode, __ATOMIC_ACQUIRE);
    int unsafe = (mode & BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE) ? 1 : 0;
    int have_cwd = 0;
    int r;

    /* SearchPath ALWAYS includes the current directory in its default order;
     * SetSearchPathMode only REORDERS it (safe: after PATH; unsafe: first).
     * NoDefaultCurrentDirectoryInExePath / NeedCurrentDirectoryForExePathW is a
     * SEPARATE policy helper for CreateProcess-style executable resolution and
     * deliberately does NOT gate SearchPath here (Win32 keeps the two distinct). */
    task_get_cwd(caller, cwd, SP_PATH_MAX);
    if (cwd[0] != '\0')
        have_cwd = 1;

    /* Unsafe ordering places the current directory FIRST (DLL-hijack-prone). */
    if (unsafe && have_cwd &&
        sp_join(cwd, name, candidate) && sp_probe(caller, candidate, resolved))
        return 1;

    if (sp_join(ENV_SYSTEM32_DIR, name, candidate) &&
        sp_probe(caller, candidate, resolved))
        return 1;
    if (sp_join(ENV_SYSTEMROOT_DIR, name, candidate) &&
        sp_probe(caller, candidate, resolved))
        return 1;
    r = sp_search_path_env(caller, name, candidate, resolved);
    if (r != 0)                                 /* 1 = found, -1 = OOM: abort before CWD */
        return r;

    /* Safe/unset ordering places the current directory LAST (after PATH). */
    if (!unsafe && have_cwd &&
        sp_join(cwd, name, candidate) && sp_probe(caller, candidate, resolved))
        return 1;

    return 0;
}

/* Search only the ';'-delimited directories in an explicit lpPath. */
static int sp_search_explicit(struct task *caller, const char *lpPath,
                              const char *name, char *candidate, char *resolved)
{
    uint32_t i, start = 0;

    for (i = 0;; i++) {
        char c = lpPath[i];
        if (c == ';' || c == '\0') {
            uint32_t len = i - start;
            if (len > 0 &&
                sp_join_n(&lpPath[start], len, name, candidate) &&
                sp_probe(caller, candidate, resolved))
                return 1;
            start = i + 1;
            if (c == '\0')
                break;
        }
    }
    return 0;
}

/* ---- UTF-8 core ---------------------------------------------------------- */

uint32_t env_search_path(struct task *caller, const char *lpPath,
                         const char *lpFileName, const char *lpExtension,
                         char *out, uint32_t out_size,
                         uint32_t *out_filepart_off, uint32_t *out_err)
{
    uint32_t dummy_err, dummy_fp;
    /* One kmalloc for the shared scratch buffers (4 * SP_PATH_MAX == 4 KiB). */
    struct sp_work {
        char name[SP_PATH_MAX];
        char candidate[SP_PATH_MAX];
        char resolved[SP_PATH_MAX];
        char cwd[SP_PATH_MAX];
    } *w;
    int found;
    uint32_t rl, fp, i;

    if (!out_err)
        out_err = &dummy_err;
    if (!out_filepart_off)
        out_filepart_off = &dummy_fp;
    *out_err = ERROR_SUCCESS;
    *out_filepart_off = 0;

    if (!caller || !lpFileName || lpFileName[0] == '\0') {
        *out_err = ERROR_INVALID_PARAMETER;
        return 0;
    }

    /* lpExtension is consulted ONLY when lpFileName's final component has no
     * extension (Win32 contract; ignored otherwise). When it WILL be used it
     * must be a proper suffix: a leading '.' and no separator/drive char --
     * otherwise it could append traversal to a bare filename and, after
     * canonicalization, escape the trusted/explicit search dirs. */
    if (lpExtension && !sp_final_has_ext(lpFileName)) {
        uint32_t e;
        if (lpExtension[0] != '.') {
            *out_err = ERROR_INVALID_PARAMETER;   /* must include the leading period */
            return 0;
        }
        for (e = 0; lpExtension[e]; e++)
            if (lpExtension[e] == '\\' || lpExtension[e] == '/' ||
                lpExtension[e] == ':') {
                *out_err = ERROR_INVALID_PARAMETER;
                return 0;
            }
    }

    w = (struct sp_work *)kmalloc(sizeof(*w));
    if (!w) {
        *out_err = ERROR_OUTOFMEMORY;
        return 0;
    }

    if (sp_build_search_name(lpFileName, lpExtension, w->name) == 0) {
        *out_err = ERROR_INVALID_PARAMETER;     /* name + extension overflow */
        kfree(w);
        return 0;
    }

    if (sp_is_qualified(lpFileName)) {
        /* Caller-anchored (rooted/drive/`.\`/`..\`): probed directly, and the
         * unveil check still guards the resolved path. */
        found = sp_probe(caller, w->name, w->resolved);
    } else if (sp_has_parent_component(w->name)) {
        /* Unqualified name with an interior ".." would canonicalize out of the
         * trusted/explicit search directory -- refuse it (containment). */
        *out_err = ERROR_INVALID_PARAMETER;
        kfree(w);
        return 0;
    } else if (lpPath) {
        found = sp_search_explicit(caller, lpPath, w->name, w->candidate, w->resolved);
    } else {
        found = sp_search_default(caller, w->name, w->candidate, w->resolved, w->cwd);
    }

    if (found < 0) {                            /* allocation failure in the PATH leg */
        *out_err = ERROR_OUTOFMEMORY;
        kfree(w);
        return 0;
    }
    if (!found) {
        *out_err = ERROR_FILE_NOT_FOUND;
        kfree(w);
        return 0;
    }

    rl = sp_strlen(w->resolved);
    fp = 0;
    for (i = 0; i < rl; i++)
        if (w->resolved[i] == '\\' || w->resolved[i] == '/')
            fp = i + 1;

    if (!out || rl + 1 > out_size) {            /* too small: required incl NUL, untouched */
        kfree(w);
        return rl + 1;
    }
    for (i = 0; i < rl; i++)
        out[i] = w->resolved[i];
    out[rl] = '\0';
    *out_filepart_off = fp;
    kfree(w);
    return rl;                                  /* length excl NUL */
}

int env_need_current_dir_for_exe(struct task *caller, const char *exe_name)
{
    if (exe_name) {
        uint32_t i;
        for (i = 0; exe_name[i]; i++)
            if (exe_name[i] == '\\')            /* a backslash forces CWD participation */
                return 1;
    }
    if (caller) {
        char tmp[1];
        /* rc >= 0 => present (even an empty value); ENV_ERR_NOTFOUND (<0) => absent. */
        if (env_get_copy(caller, "NoDefaultCurrentDirectoryInExePath", tmp,
                         sizeof(tmp)) >= 0)
            return 0;
    }
    return 1;
}

/* ---- Win32 ABI wrappers --------------------------------------------------- */

/* Convert a NUL-terminated UTF-16 string to a freshly sp_alloc'd UTF-8 string.
 * Returns the buffer (NUL-terminated) and stores its allocation size in *out_cap
 * for sp_free. Returns NULL on conversion error or OOM. A NULL src yields an
 * empty string. */
static char *sp_w_to_u8(const uint16_t *src, uint32_t *out_cap, uint32_t *out_err)
{
    uint32_t wlen = src ? sp_wcslen(src) : 0;
    int need;
    char *buf;

    need = nls_cp_utf16_to_utf8(src, wlen, (uint8_t *)0, 0, NLS_CP_STRICT);
    if (need < 0) {
        *out_err = ERROR_INVALID_PARAMETER;     /* bad encoding, not OOM */
        return (char *)0;
    }
    *out_cap = (uint32_t)need + 1;
    buf = sp_alloc(*out_cap);
    if (!buf) {
        *out_err = ERROR_OUTOFMEMORY;
        return (char *)0;
    }
    if (need > 0 &&
        nls_cp_utf16_to_utf8(src, wlen, (uint8_t *)buf, (uint32_t)need,
                             NLS_CP_STRICT) < 0) {
        sp_free(buf, *out_cap);
        *out_err = ERROR_INVALID_PARAMETER;
        return (char *)0;
    }
    buf[need] = '\0';
    return buf;
}

uint32_t SearchPathW(struct task *caller, const uint16_t *lpPath,
                     const uint16_t *lpFileName, const uint16_t *lpExtension,
                     uint32_t nBufferLength, uint16_t *lpBuffer,
                     uint16_t **lpFilePart)
{
    char *u8path = (char *)0, *u8file = (char *)0, *u8ext = (char *)0;
    char *u8out = (char *)0;
    uint32_t cap_path = 0, cap_file = 0, cap_ext = 0;
    uint32_t err = ERROR_SUCCESS, fp8 = 0, rc, ret = 0;
    int req16 = 0, pre16 = 0, wrote = 0;

    if (!caller || !lpFileName) {
        sp_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }

    u8file = sp_w_to_u8(lpFileName, &cap_file, &err);
    if (!u8file) {
        sp_set_last_error(err);
        return 0;
    }
    if (lpPath) {
        u8path = sp_w_to_u8(lpPath, &cap_path, &err);
        if (!u8path) goto done;                 /* err set by sp_w_to_u8 */
    }
    /* Convert lpExtension ONLY when it will actually be used (the filename has no
     * extension). A malformed or oversized extension that the core would IGNORE
     * (filename already has one) must not fail the call -- matching the core's
     * ignore-when-filename-has-extension rule. */
    if (lpExtension && !sp_final_has_ext(u8file)) {
        u8ext = sp_w_to_u8(lpExtension, &cap_ext, &err);
        if (!u8ext) goto done;
    }

    u8out = sp_alloc(SP_PATH_MAX);
    if (!u8out) { err = ERROR_OUTOFMEMORY; goto done; }

    rc = env_search_path(caller, u8path, u8file, u8ext, u8out, SP_PATH_MAX,
                         &fp8, &err);
    if (rc == 0 || rc + 1 > SP_PATH_MAX)        /* not found / error (or would-not-fit) */
        goto done;

    /* Size the found UTF-8 path into UTF-16 code units. */
    req16 = nls_cp_utf8_to_utf16((const uint8_t *)u8out, rc, (uint16_t *)0, 0,
                                 NLS_CP_STRICT);
    if (req16 < 0) { err = ERROR_INVALID_PARAMETER; goto done; }

    if (!lpBuffer || (uint32_t)req16 + 1 > nBufferLength) {
        ret = (uint32_t)req16 + 1;              /* required incl NUL; buffer untouched */
        goto done_no_err;
    }

    wrote = nls_cp_utf8_to_utf16((const uint8_t *)u8out, rc, lpBuffer,
                                 nBufferLength, NLS_CP_STRICT);
    if (wrote < 0) { err = ERROR_INVALID_PARAMETER; goto done; }
    lpBuffer[wrote] = 0;
    if (lpFilePart) {
        pre16 = (fp8 > 0)
            ? nls_cp_utf8_to_utf16((const uint8_t *)u8out, fp8, (uint16_t *)0, 0,
                                   NLS_CP_STRICT)
            : 0;
        *lpFilePart = (pre16 > 0) ? (lpBuffer + pre16) : lpBuffer;
    }
    ret = (uint32_t)wrote;                       /* length excl NUL */

done:
    if (ret == 0 && err != ERROR_SUCCESS)
        sp_set_last_error(err);
done_no_err:
    if (u8path) sp_free(u8path, cap_path);
    if (u8ext)  sp_free(u8ext, cap_ext);
    if (u8file) sp_free(u8file, cap_file);
    if (u8out)  sp_free(u8out, SP_PATH_MAX);
    return ret;
}

/* Convert a NUL-terminated ACP string to UTF-8 (via UTF-16). Same contract as
 * sp_w_to_u8. */
static char *sp_a_to_u8(const char *src, uint32_t *out_cap, uint32_t *out_err)
{
    uint32_t alen = src ? sp_strlen(src) : 0;
    int nu16, nu8;
    uint16_t *u16;
    char *buf;
    uint32_t cap16;

    nu16 = nls_cp_to_utf16(NLS_CP_ACP, (const uint8_t *)src, alen, (uint16_t *)0,
                           0, NLS_CP_STRICT);
    if (nu16 < 0) {
        *out_err = ERROR_INVALID_PARAMETER;
        return (char *)0;
    }
    cap16 = ((uint32_t)nu16 + 1) * (uint32_t)sizeof(uint16_t);
    u16 = (uint16_t *)sp_alloc(cap16);
    if (!u16) {
        *out_err = ERROR_OUTOFMEMORY;
        return (char *)0;
    }
    if (nu16 > 0 &&
        nls_cp_to_utf16(NLS_CP_ACP, (const uint8_t *)src, alen, u16,
                        (uint32_t)nu16, NLS_CP_STRICT) < 0) {
        sp_free((char *)u16, cap16);
        *out_err = ERROR_INVALID_PARAMETER;
        return (char *)0;
    }
    u16[nu16] = 0;

    nu8 = nls_cp_utf16_to_utf8(u16, (uint32_t)nu16, (uint8_t *)0, 0, NLS_CP_STRICT);
    if (nu8 < 0) {
        sp_free((char *)u16, cap16);
        *out_err = ERROR_INVALID_PARAMETER;
        return (char *)0;
    }
    *out_cap = (uint32_t)nu8 + 1;
    buf = sp_alloc(*out_cap);
    if (!buf) {
        sp_free((char *)u16, cap16);
        *out_err = ERROR_OUTOFMEMORY;
        return (char *)0;
    }
    if (nu8 > 0 &&
        nls_cp_utf16_to_utf8(u16, (uint32_t)nu16, (uint8_t *)buf, (uint32_t)nu8,
                             NLS_CP_STRICT) < 0) {
        sp_free(buf, *out_cap);
        sp_free((char *)u16, cap16);
        *out_err = ERROR_INVALID_PARAMETER;
        return (char *)0;
    }
    buf[nu8] = '\0';
    sp_free((char *)u16, cap16);
    return buf;
}

uint32_t SearchPathA(struct task *caller, const char *lpPath,
                     const char *lpFileName, const char *lpExtension,
                     uint32_t nBufferLength, char *lpBuffer, char **lpFilePart)
{
    char *u8path = (char *)0, *u8file = (char *)0, *u8ext = (char *)0;
    char *u8out = (char *)0;
    uint32_t cap_path = 0, cap_file = 0, cap_ext = 0;
    uint32_t err = ERROR_SUCCESS, fp8 = 0, rc, ret = 0;
    int reqA = -1, preA = 0, wroteA = 0;

    if (!caller || !lpFileName) {
        sp_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }

    u8file = sp_a_to_u8(lpFileName, &cap_file, &err);
    if (!u8file) {
        sp_set_last_error(err);
        return 0;
    }
    if (lpPath) {
        u8path = sp_a_to_u8(lpPath, &cap_path, &err);
        if (!u8path) goto done;                 /* err set by sp_a_to_u8 */
    }
    /* Convert lpExtension ONLY when it will be used (filename has no extension),
     * so an ignorable malformed/oversized extension cannot fail the call. */
    if (lpExtension && !sp_final_has_ext(u8file)) {
        u8ext = sp_a_to_u8(lpExtension, &cap_ext, &err);
        if (!u8ext) goto done;
    }

    u8out = sp_alloc(SP_PATH_MAX);
    if (!u8out) { err = ERROR_OUTOFMEMORY; goto done; }

    rc = env_search_path(caller, u8path, u8file, u8ext, u8out, SP_PATH_MAX,
                         &fp8, &err);
    if (rc == 0 || rc + 1 > SP_PATH_MAX)
        goto done;

    /* Size the found UTF-8 path into ACP bytes (UTF-8 -> UTF-16 -> ACP). */
    reqA = -1;
    {
        int nu16 = nls_cp_utf8_to_utf16((const uint8_t *)u8out, rc, (uint16_t *)0,
                                        0, NLS_CP_STRICT);
        uint16_t *u16 = (uint16_t *)0;
        uint32_t cap16 = 0;
        if (nu16 >= 0) {
            cap16 = ((uint32_t)nu16 + 1) * (uint32_t)sizeof(uint16_t);
            u16 = (uint16_t *)sp_alloc(cap16);
        }
        if (u16) {
            if (nu16 == 0 ||
                nls_cp_utf8_to_utf16((const uint8_t *)u8out, rc, u16,
                                     (uint32_t)nu16, NLS_CP_STRICT) >= 0) {
                u16[nu16] = 0;
                reqA = nls_cp_from_utf16(NLS_CP_ACP, u16, (uint32_t)nu16,
                                         (uint8_t *)0, 0, NLS_CP_STRICT);
                /* file-part prefix in ACP bytes */
                if (reqA >= 0 && fp8 > 0) {
                    int pre16 = nls_cp_utf8_to_utf16((const uint8_t *)u8out, fp8,
                                                     (uint16_t *)0, 0, NLS_CP_STRICT);
                    if (pre16 >= 0) {
                        /* reuse u16 prefix already converted; ACP-size it */
                        preA = nls_cp_from_utf16(NLS_CP_ACP, u16, (uint32_t)pre16,
                                                 (uint8_t *)0, 0, NLS_CP_STRICT);
                    } else {
                        preA = 0;
                    }
                } else {
                    preA = 0;
                }

                if (reqA >= 0) {
                    if (!lpBuffer || (uint32_t)reqA + 1 > nBufferLength) {
                        ret = (uint32_t)reqA + 1;   /* required incl NUL; untouched */
                    } else {
                        wroteA = nls_cp_from_utf16(NLS_CP_ACP, u16, (uint32_t)nu16,
                                                   (uint8_t *)lpBuffer,
                                                   nBufferLength, NLS_CP_STRICT);
                        if (wroteA >= 0) {
                            lpBuffer[wroteA] = '\0';
                            if (lpFilePart)
                                *lpFilePart = (preA > 0) ? (lpBuffer + preA)
                                                         : lpBuffer;
                            ret = (uint32_t)wroteA;
                        } else {
                            err = ERROR_INVALID_PARAMETER;
                        }
                    }
                } else {
                    err = ERROR_INVALID_PARAMETER;
                }
            } else {
                err = ERROR_INVALID_PARAMETER;
            }
        } else {
            err = (nu16 < 0) ? ERROR_INVALID_PARAMETER : ERROR_OUTOFMEMORY;
        }
        if (u16)
            sp_free((char *)u16, cap16);
    }

done:
    if (ret == 0 && err != ERROR_SUCCESS)
        sp_set_last_error(err);
    if (u8path) sp_free(u8path, cap_path);
    if (u8ext)  sp_free(u8ext, cap_ext);
    if (u8file) sp_free(u8file, cap_file);
    if (u8out)  sp_free(u8out, SP_PATH_MAX);
    return ret;
}

int NeedCurrentDirectoryForExePathW(struct task *caller, const uint16_t *ExeName)
{
    /* Only the presence of a backslash matters, and '\' is ASCII in UTF-16, so we
     * can inspect the WCHAR string directly without a full UTF-8 conversion. */
    if (ExeName) {
        uint32_t i;
        for (i = 0; ExeName[i]; i++)
            if (ExeName[i] == (uint16_t)'\\')
                return 1;
    }
    return env_need_current_dir_for_exe(caller, "");
}

int SetSearchPathMode(struct task *caller, uint32_t BaseSearchPathMode)
{
    uint32_t enable, disable, perm, cur;

    if (!caller) {
        sp_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    enable  = BaseSearchPathMode & BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE;
    disable = BaseSearchPathMode & BASE_SEARCH_PATH_DISABLE_SAFE_SEARCHMODE;
    perm    = BaseSearchPathMode & BASE_SEARCH_PATH_PERMANENT;

    if ((BaseSearchPathMode & ~BASE_SEARCH_PATH_VALID_MASK) ||  /* unknown bits */
        (enable && disable) ||                                  /* both set */
        (!enable && !disable) ||                                /* neither set */
        (perm && !enable)) {                                    /* PERMANENT w/o ENABLE */
        sp_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }

    cur = __atomic_load_n(&caller->search_path_mode, __ATOMIC_ACQUIRE);
    for (;;) {
        if (cur & BASE_SEARCH_PATH_PERMANENT) {                 /* locked by prior PERMANENT */
            sp_set_last_error(ERROR_ACCESS_DENIED);
            return 0;
        }
        if (__atomic_compare_exchange_n(&caller->search_path_mode, &cur,
                                        BaseSearchPathMode, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
        /* cur reloaded by the CAS; loop re-checks the PERMANENT lock. */
    }
}
