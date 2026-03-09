/* Freestanding <assert.h> shim — asserts disabled in kernel mode */
#ifndef _FREESTANDING_ASSERT_H
#define _FREESTANDING_ASSERT_H
#define assert(x) ((void)0)
#endif
