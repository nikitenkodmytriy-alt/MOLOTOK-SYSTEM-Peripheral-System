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

static uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t extended_pending;
static uint8_t shift_down;
static uint8_t caps_lock;

static const char key_map[128] = {
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
    [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b', [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1a] = '[', [0x1b] = ']', [0x1c] = '\n', [0x1e] = 'a', [0x1f] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' '
};

static char shifted_character(uint8_t scan, char character) {
    static const char shifted_digits[] = "!@#$%^&*()";

    if (character >= 'a' && character <= 'z') {
        return (shift_down != caps_lock) ? (char)(character - 'a' + 'A') : character;
    }

    if (!shift_down) return character;

    if (scan >= 0x02 && scan <= 0x0b) return shifted_digits[scan - 0x02];

    switch (character) {
        case '-': return '_'; case '=': return '+'; case '[': return '{'; case ']': return '}';
        case ';': return ':'; case '\'': return '"'; case '`': return '~'; case '\\': return '|';
        case ',': return '<'; case '.': return '>'; case '/': return '?'; default: return character;
    }
}

char duck_ps2_translate(uint8_t scan) {
    if (scan >= 128) return 0;
    return shifted_character(scan, key_map[scan]);
}

int duck_ps2_keyboard_init(void) {
    return 1;
}

int duck_ps2_read_char(void) {
    uint8_t scan, extended, released;
    if (!duck_ps2_read_key(&scan, &extended, &released) || released || extended || scan >= 128) return -1;
    if (scan == 0x2a || scan == 0x36) {
        shift_down = 1;
        return -1;
    }
    if (scan == 0x3a) {
        caps_lock = !caps_lock;
        return -1;
    }
    char character = duck_ps2_translate(scan);
    return character ? character : -1;
}

int duck_ps2_read_key(uint8_t *scan_code, uint8_t *extended, uint8_t *released) {
    if (!(inb(0x64) & 1)) return 0;
    uint8_t scan = inb(0x60);
    if (scan == 0xe0) {
        extended_pending = 1;
        return 0;
    }
    if (scan == 0x2a || scan == 0x36) {
        shift_down = 1;
        return 0;
    }
    if (scan == 0xaa || scan == 0xb6) {
        shift_down = 0;
        return 0;
    }
    if (scan == 0x3a) {
        caps_lock = !caps_lock;
        return 0;
    }
    *released = (scan & 0x80) != 0;
    *scan_code = scan & 0x7f;
    *extended = extended_pending;
    extended_pending = 0;
    return 1;
}