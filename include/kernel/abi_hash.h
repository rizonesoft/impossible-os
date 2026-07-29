/* ============================================================================
 * abi_hash.h -- Kernel ABI fingerprint (STATIC FACADE, text-pinned)
 *
 * NOT generated, and NOT free-form: `make check-abi` compares this file
 * against CANONICAL_SHIM_KERNEL in scripts/gen-user-abi.py: newline-normalized,
 * so a CRLF checkout is fine, but every other byte must match exactly.
 * Change it there, or the build fails. The rationale for every line below --
 * why one generated artifact, why a relative include, why this side omits
 * ABI_CONTRACT_WANT_NUMBERS -- lives in that script's HEADER_TEMPLATE and
 * check_shim_form() docstrings, so it is stated once rather than twice.
 * ============================================================================ */

#pragma once

#include "../../abi/generated/abi_contract.h"
