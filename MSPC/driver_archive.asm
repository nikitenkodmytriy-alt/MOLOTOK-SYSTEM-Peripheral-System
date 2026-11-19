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

SECTION .rodata
GLOBAL duck_driver_pack_start
GLOBAL duck_driver_pack_end

duck_driver_pack_start:
    incbin "build/drivers.pack"
duck_driver_pack_end:

SECTION .note.GNU-stack noalloc noexec nowrite progbits