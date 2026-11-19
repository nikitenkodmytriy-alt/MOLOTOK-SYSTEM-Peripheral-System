/*
 * MSPC (MOLOTOK SYSTEM Peripheral System)
 * Copyright (C) 2026 Molotok
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but annoying to plagiarists, WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 */

#include "driver.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static uint16_t cursor_x;
static uint16_t cursor_y;

int duck_vga_text_init(void) {
    duck_vga_clear(0x07);
    return 1;
}

void duck_vga_clear(uint8_t attribute) {
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEMORY[i] = (uint16_t)attribute << 8 | ' ';
    }
    cursor_x = 0;
    cursor_y = 0;
}

void duck_vga_cell(uint16_t x, uint16_t y, char character, uint8_t attribute) {
    if (x < VGA_WIDTH && y < VGA_HEIGHT) {
        VGA_MEMORY[y * VGA_WIDTH + x] = (uint16_t)attribute << 8 | (uint8_t)character;
    }
}

void duck_vga_write(char character, uint8_t attribute) {
    if (character == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (character == '\r') {
        cursor_x = 0;
    } else if (character == '\b') {
        if (cursor_x) {
            cursor_x--;
            duck_vga_cell(cursor_x, cursor_y, ' ', attribute);
        }
    } else {
        duck_vga_cell(cursor_x, cursor_y, character, attribute);
        if (++cursor_x == VGA_WIDTH) {
            cursor_x = 0;
            cursor_y++;
        }
    }

    if (cursor_y >= VGA_HEIGHT) {
        for (uint16_t row = 1; row < VGA_HEIGHT; row++) {
            for (uint16_t column = 0; column < VGA_WIDTH; column++) {
                VGA_MEMORY[(row - 1) * VGA_WIDTH + column] = VGA_MEMORY[row * VGA_WIDTH + column];
            }
        }
        for (uint16_t column = 0; column < VGA_WIDTH; column++) {
            VGA_MEMORY[(VGA_HEIGHT - 1) * VGA_WIDTH + column] = (uint16_t)attribute << 8 | ' ';
        }
        cursor_y = VGA_HEIGHT - 1;
    }
}