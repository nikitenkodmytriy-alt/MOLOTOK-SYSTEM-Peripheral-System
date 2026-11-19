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

BITS 16
ORG 0x7c00

%ifndef KERNEL_SECTORS
%error "KERNEL_SECTORS is required"
%endif
%ifndef KERNEL_ENTRY
%error "KERNEL_ENTRY is required"
%endif

%define KERNEL_LBA 4

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov [boot_drive], dl
    xor bp, bp
    mov word [load_segment], 0x1000

read_kernel:
    cmp bp, KERNEL_SECTORS
    jae enter_protected_mode

    mov ax, bp
    add ax, KERNEL_LBA
    xor dx, dx
    mov bx, 36
    div bx
    mov ch, al
    mov ax, dx
    xor dx, dx
    mov bx, 18
    div bx
    mov dh, al
    mov cl, dl
    inc cl

    mov ax, [load_segment]
    mov es, ax
    xor bx, bx
    mov ax, 0x0201
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    add word [load_segment], 0x20
    inc bp
    jmp read_kernel

disk_error:
    mov si, error_message
.print_error:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0e
    int 0x10
    jmp .print_error
.halt:
    cli
    hlt
    jmp .halt

enter_protected_mode:
    cli
    in al, 0x92
    or al, 2
    out 0x92, al
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_mode

BITS 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov esi, 0x10000
    mov edi, 0x100000
    mov ecx, (KERNEL_SECTORS * 512) / 4
    rep movsd

    mov eax, 0x36d76289
    xor ebx, ebx
    jmp KERNEL_ENTRY

BITS 16
align 8
gdt_start:
    dq 0
    dq 0x00cf9a000000ffff
    dq 0x00cf92000000ffff
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

boot_drive db 0
load_segment dw 0
error_message db "MSPC: disk read error", 0

times 510 - ($ - $$) db 0
dw 0xaa55