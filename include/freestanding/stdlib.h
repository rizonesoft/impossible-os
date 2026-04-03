/* Freestanding <stdlib.h> shim for stb libraries in kernel mode */
#ifndef _FREESTANDING_STDLIB_H
#define _FREESTANDING_STDLIB_H
/* abs() -- used by stb_image */
static inline int abs(int x) { return x < 0 ? -x : x; }
/* malloc/free/realloc are redirected via STBI_MALLOC etc. */
#endif
