/* Freestanding <math.h> shim -- redirects to kernel kmath functions */
#ifndef _FREESTANDING_MATH_H
#define _FREESTANDING_MATH_H
/* stb_image uses pow() and ldexp() when STBI_NO_LINEAR and STBI_NO_HDR
 * are not both defined. We define both, so this header is effectively empty.
 * If math functions are needed, use kernel/kmath.h. */
#endif
