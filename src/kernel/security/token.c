/* ============================================================================
 * token.c — ACCESS_TOKEN Ob type registration
 *
 * Implements TODO-11 §4.1: token object type with lifecycle callbacks.
 * ============================================================================ */

#include "kernel/security/token.h"
#include "kernel/ob/ob.h"
#include "kernel/klog.h"

/* --- Callbacks ----------------------------------------------------------- */

static void token_on_delete(void *body)
{
    (void)body;
    /* Token SID/ACL pointers reference either static well-known SIDs
     * or kmalloc'd copies.  Deep-free of dynamic allocations will be
     * wired when SeCreateUserToken is implemented (§4.2).  For now,
     * static SID pointers (SeLocalSystemSid etc.) must not be freed. */
}

/* --- Type registration --------------------------------------------------- */

void ob_token_type_init(void)
{
    extern const OBJECT_TYPE *ObpTokenType;

    ObpTokenType = ob_create_type(&(OBJECT_TYPE){
        .name      = "Token",
        .body_size = sizeof(ACCESS_TOKEN),
        .on_close  = NULL,
        .on_delete = token_on_delete,
        .on_open   = NULL,
        .on_parse  = NULL,
    });

    if (!ObpTokenType)
        klog(LOG_ERROR, "security", "Failed to register ObpTokenType");
}
