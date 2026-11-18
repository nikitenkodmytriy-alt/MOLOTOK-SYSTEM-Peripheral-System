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

#ifndef MSPC_DRIVER_H
#define MSPC_DRIVER_H

#include <stddef.h>
#include <stdint.h>

int duck_vga_text_init(void);
void duck_vga_clear(uint8_t attribute);
void duck_vga_write(char character, uint8_t attribute);
void duck_vga_cell(uint16_t x, uint16_t y, char character, uint8_t attribute);

int duck_ps2_keyboard_init(void);
int duck_ps2_read_char(void);
int duck_ps2_read_key(uint8_t *scan_code, uint8_t *extended, uint8_t *released);
char duck_ps2_translate(uint8_t scan_code);

int duck_pcnet_init(void);
const uint8_t *duck_pcnet_mac(void);
int duck_pcnet_send(const uint8_t *frame, size_t length);
int duck_pcnet_receive(uint8_t *frame, size_t capacity, size_t *length);

int duck_network_init(void);
int duck_network_ping(const char *address);
int duck_network_resolve(const char *hostname, uint8_t address[4]);
void duck_network_get_config(uint8_t address[4], uint8_t mask[4], uint8_t gateway[4], uint8_t dns[4]);
int duck_network_http_get(const char *host, const char *path, uint8_t *output, size_t capacity, size_t *received);

#endif