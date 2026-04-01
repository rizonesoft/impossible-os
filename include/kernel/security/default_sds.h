/* ============================================================================
 * default_sds.h — Default security descriptors for kernel object types
 *
 * SeCreateDefaultSD() returns a pointer to a static self-relative SD
 * blob appropriate for the given object type name.
 * ============================================================================ */

#pragma once

#include "kernel/types.h"

/* Object type name constants for SeCreateDefaultSD() */
#define SE_SD_TYPE_DEFAULT       0   /* generic: SY+BA=Full, WD=ReadControl */
#define SE_SD_TYPE_PROCESS       1   /* process: restricted Everyone */
#define SE_SD_TYPE_TOKEN         2   /* token: SY=Full, Owner=Query */
#define SE_SD_TYPE_REGISTRY_KEY  3   /* regkey: SY+BA=Full, BU=Read */

/*
 * SeCreateDefaultSD — return a pointer to a static self-relative SD
 * for the given object type.  Returns NULL for unknown types.
 * The returned pointer is to read-only static data — do not free.
 */
const void *SeCreateDefaultSD(uint32_t type);

/*
 * SeGetDefaultSDSize — return the byte size of the SD returned by
 * SeCreateDefaultSD() for the given type.  Returns 0 for unknown types.
 */
uint32_t SeGetDefaultSDSize(uint32_t type);

/*
 * se_default_sds_init — pre-build the static SD blobs at boot time.
 * Called once from ob_init() or security subsystem init.
 */
void se_default_sds_init(void);

/* Forward-declare (uses acl.h types) */
#include "kernel/security/acl.h"

/*
 * SeCreateCreatorSD — build an absolute SD owned by creator_sid with
 * DACL: (A;;GA;;;creator)(A;;GA;;;SY)(A;;GR;;;WD).
 * Writes into sd_out (caller provides storage).
 * dacl_buf/dacl_buf_size: workspace for the DACL.
 * Returns 0 on success, -1 on error.
 */
int SeCreateCreatorSD(SECURITY_DESCRIPTOR *sd_out,
                      const SID *creator_sid,
                      void *dacl_buf, uint32_t dacl_buf_size);
