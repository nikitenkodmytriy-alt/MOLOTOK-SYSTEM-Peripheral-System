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

#ifndef MSPC_GL_H
#define MSPC_GL_H

#include <stddef.h>
#include <stdint.h>

#define MSPC_GL_POINTS 1
#define MSPC_GL_LINES 2
#define MSPC_GL_TRIANGLES 3

void mspc_gl_clear(void);
void mspc_gl_begin(int mode);
void mspc_gl_vertex2(int32_t x, int32_t y);
void mspc_gl_vertex3(int32_t x, int32_t y, int32_t z);
void mspc_gl_end(void);
void mspc_gl_test(void);
void mspc_gl_run(void);

#endif
