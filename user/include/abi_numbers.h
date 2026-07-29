/* ============================================================================
 * abi_numbers.h -- User-mode ABI numbers (STATIC FACADE, byte-pinned)
 *
 * NOT generated, and NOT free-form: `make check-abi` compares this file
 * BYTE FOR BYTE against CANONICAL_SHIM_USER in scripts/gen-user-abi.py.
 * Change it there, or the build fails. The rationale for every line below --
 * why one generated artifact, why a relative include, why this side defines
 * ABI_CONTRACT_WANT_NUMBERS -- lives in that script's HEADER_TEMPLATE and
 * check_shim_form() docstrings, so it is stated once rather than twice.
 * ============================================================================ */

#pragma once

#define ABI_CONTRACT_WANT_NUMBERS
#include "../../abi/generated/abi_contract.h"
