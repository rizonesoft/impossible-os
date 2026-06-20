/* ============================================================================
 * kcodec.h -- Kernel base64 / hex codec dispatch
 *
 * One kernel-owned base64 + hex layer so TLS PEM, SSH, SMTP/MIME, registry
 * .reg binary values, PDF ASCIIHex, and BSOD-QR consumers stop reinventing
 * incompatible variants. All functions write into a CALLER-provided buffer,
 * bounds-checked against `dst_cap`, and return the byte count written or -1 on
 * invalid input / insufficient destination. No allocation.
 *
 * XREF: 02-kernel-core/TODO-03-kernel-libraries.md
 * ============================================================================ */
#ifndef KERNEL_KCODEC_H
#define KERNEL_KCODEC_H

#include "kernel/types.h"

/* base64 (RFC 4648). encode emits standard '+'/'/' alphabet with '=' padding;
 * the encoded length is 4*((src_len+2)/3). Returns bytes written, or -1 if
 * dst_cap is too small or src_len is too large for an int result. */
int base64_encode(char *dst, size_t dst_cap, const void *src, size_t src_len);

/* base64 decode. mime==0: STRICT -- every input byte must be alphabet or the
 * trailing '='; length must be a multiple of 4; padding placement validated.
 * mime!=0: MIME -- ASCII whitespace (space/tab/CR/LF) is skipped first, then
 * the same strict rules apply to the compacted stream. Returns bytes written,
 * or -1 on any malformed input / dst overflow. */
int base64_decode(void *dst, size_t dst_cap, const char *src, size_t src_len, int mime);

/* hex. encode emits lowercase, 2*src_len chars. decode accepts lower+upper,
 * rejects odd length and any non-hex byte. Returns bytes written, or -1 on
 * invalid input / dst overflow. */
int hex_encode(char *dst, size_t dst_cap, const void *src, size_t src_len);
int hex_decode(void *dst, size_t dst_cap, const char *src, size_t src_len);

#endif /* KERNEL_KCODEC_H */
