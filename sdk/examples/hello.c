/**
 * hello.c — Minimal Impossible OS application
 *
 * Build:
 *   clang-19 --target=x86_64-elf -I sdk/include -ffreestanding -nostdlib \
 *       -o hello.exe hello.c -L sdk/lib -limpossible
 */

#include "../include/impossible/windows.h"

int WinMain(void) {
    // TODO: Call CreateFile + WriteFile once Win32 API is implemented
    return 0;
}
