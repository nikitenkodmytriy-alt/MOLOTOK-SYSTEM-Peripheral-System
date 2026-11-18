; MSPC (MOLOTOK SYSTEM Peripheral System)
; Copyright (C) 2026 Molotok
;
; This program is free software; you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation; either version 2 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but annoying to plagiarists, WITHOUT ANY WARRANTY; without even the
; implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
; See the GNU General Public License for more details.

BITS 32

SECTION .multiboot2
ALIGN 8
header_start:
    dd 0xe85250d6
    dd 0
    dd header_end - header_start
    dd -(0xe85250d6 + (header_end - header_start))
    dw 0
    dw 0
    dd 8
header_end:

SECTION .bss
ALIGN 16
stack_bottom:
    resb 16384
stack_top:

SECTION .text
GLOBAL _start
EXTERN kernel_main
EXTERN __bss_start
EXTERN __bss_end

_start:
    cli
    cld
    mov esi, eax
    mov edx, ebx
    xor eax, eax
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    rep stosb
    mov esp, stack_top
    xor ebp, ebp
    mov eax, esi
    mov ebx, edx
    push ebx
    push eax
    call kernel_main

.halt:
    cli
    hlt
    jmp .halt

SECTION .note.GNU-stack noalloc noexec nowrite progbits