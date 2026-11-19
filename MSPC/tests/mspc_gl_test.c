/*
 * MSPC (MOLOTOK SYSTEM Peripheral System)
 * Copyright (C) 2026 Molotok
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../drivers/driver.h"

void duck_vga_clear(uint8_t attribute) {
    (void)attribute;
}

void duck_vga_cell(uint16_t x, uint16_t y, char character, uint8_t attribute) {
    (void)x;
    (void)y;
    (void)character;
    (void)attribute;
}

int duck_ps2_read_key(uint8_t *scan_code, uint8_t *extended, uint8_t *released) {
    *scan_code = 0x01;
    *extended = 0;
    *released = 0;
    return 1;
}

#include "../drivers/mspc_gl.c"

int main(void) {
    mspc_gl_clear();
    mspc_gl_begin(MSPC_GL_LINES);
    mspc_gl_vertex3(-20, -10, 20);
    mspc_gl_vertex3(20, -10, 20);
    mspc_gl_end();
    assert(point_count == 2);

    mspc_gl_test();
    return 0;
}
