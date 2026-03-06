/* ============================================================================
 * hello.c — User-mode test program
 *
 * Linked against libc.a. Uses printf() and main() convention.
 * crt0.asm calls main() and passes the return value to sys_exit().
 * ============================================================================ */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void)
{
    /* Test printf with various format specifiers */
    printf("  [ELF] Hello from user-mode ELF binary!\n");
    printf("  [ELF] printf works: %d + %d = %d\n", 17, 25, 17 + 25);
    printf("  [ELF] hex: 0x%x, string: \"%s\"\n", 0xDEAD, "Impossible OS");

    /* Test string functions */
    char buf[64];
    strcpy(buf, "libc ");
    strcat(buf, "is working!");
    printf("  [ELF] strlen(\"%s\") = %d\n", buf, (int)strlen(buf));

    /* Test itoa */
    char numbuf[32];
    itoa(12345, numbuf, 10);
    printf("  [ELF] itoa(12345) = \"%s\"\n", numbuf);

    printf("  [ELF] About to return 42 from main()\n");
    return 42;
}
