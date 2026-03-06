; =============================================================================
; Multiboot2 Header — Required for GRUB to recognize the kernel
; =============================================================================

section .multiboot2
align 8

MULTIBOOT2_MAGIC    equ 0xE85250D6      ; Multiboot2 magic number
ARCHITECTURE        equ 0               ; 0 = i386 (32-bit protected mode entry)
HEADER_LENGTH       equ multiboot2_header_end - multiboot2_header_start
CHECKSUM            equ -(MULTIBOOT2_MAGIC + ARCHITECTURE + HEADER_LENGTH)

multiboot2_header_start:
    dd MULTIBOOT2_MAGIC
    dd ARCHITECTURE
    dd HEADER_LENGTH
    dd CHECKSUM

    ; --- Framebuffer tag (request GOP framebuffer) ---
    align 8
    dw 5                        ; type = framebuffer tag
    dw 0                        ; flags (not optional)
    dd 20                       ; size of this tag
    dd 1024                     ; preferred width
    dd 768                      ; preferred height
    dd 32                       ; preferred bpp

    ; --- End tag ---
    align 8
    dw 0                        ; type = end tag
    dw 0                        ; flags
    dd 8                        ; size
multiboot2_header_end:
