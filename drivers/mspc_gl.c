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

#include "mspc_gl.h"
#include "driver.h"

#define GL_WIDTH 80
#define GL_HEIGHT 25

static int gl_mode;
static int32_t points[128][3];
static size_t point_count;

static int32_t fixed_abs(int32_t value) {
    return value < 0 ? -value : value;
}

static void gl_point(int32_t x, int32_t y, char glyph) {
    if (x >= 0 && x < GL_WIDTH && y >= 0 && y < GL_HEIGHT) {
        duck_vga_cell((uint16_t)x, (uint16_t)y, glyph, 0x0a);
    }
}

static void gl_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t dx = fixed_abs(x1 - x0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t dy = -fixed_abs(y1 - y0);
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;
    for (;;) {
        gl_point(x0, y0, '*');
        if (x0 == x1 && y0 == y1) break;
        int32_t twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void mspc_gl_clear(void) {
    duck_vga_clear(0x07);
    point_count = 0;
}

void mspc_gl_begin(int mode) {
    gl_mode = mode;
    point_count = 0;
}

void mspc_gl_vertex2(int32_t x, int32_t y) {
    if (point_count < 128) {
        points[point_count][0] = x;
        points[point_count][1] = y;
        points[point_count][2] = 0;
        point_count++;
    }
}

void mspc_gl_vertex3(int32_t x, int32_t y, int32_t z) {
    if (point_count < 128) {
        points[point_count][0] = x;
        points[point_count][1] = y;
        points[point_count][2] = z;
        point_count++;
    }
}

static void projected_point(int32_t x, int32_t y, int32_t z, int32_t *screen_x, int32_t *screen_y) {
    int32_t depth = z + 80;
    if (depth < 20) depth = 20;
    *screen_x = 40 + (x * 20) / depth;
    *screen_y = 13 - (y * 10) / depth;
}

void mspc_gl_end(void) {
    if (gl_mode == MSPC_GL_POINTS) {
        for (size_t i = 0; i < point_count; i++) {
            int32_t x, y;
            projected_point(points[i][0], points[i][1], points[i][2], &x, &y);
            gl_point(x, y, '+');
        }
    } else if (gl_mode == MSPC_GL_LINES) {
        for (size_t i = 1; i < point_count; i += 2) {
            int32_t x0, y0, x1, y1;
            projected_point(points[i - 1][0], points[i - 1][1], points[i - 1][2], &x0, &y0);
            projected_point(points[i][0], points[i][1], points[i][2], &x1, &y1);
            gl_line(x0, y0, x1, y1);
        }
    } else if (gl_mode == MSPC_GL_TRIANGLES) {
        for (size_t i = 2; i < point_count; i += 3) {
            int32_t x0, y0, x1, y1, x2, y2;
            projected_point(points[i - 2][0], points[i - 2][1], points[i - 2][2], &x0, &y0);
            projected_point(points[i - 1][0], points[i - 1][1], points[i - 1][2], &x1, &y1);
            projected_point(points[i][0], points[i][1], points[i][2], &x2, &y2);
            gl_line(x0, y0, x1, y1);
            gl_line(x1, y1, x2, y2);
            gl_line(x2, y2, x0, y0);
        }
    }
}

static void cube_line(int32_t ax, int32_t ay, int32_t az,
                      int32_t bx, int32_t by, int32_t bz) {
    mspc_gl_begin(MSPC_GL_LINES);
    mspc_gl_vertex3(ax, ay, az);
    mspc_gl_vertex3(bx, by, bz);
    mspc_gl_end();
}

void mspc_gl_test(void) {
    mspc_gl_clear();

    for (size_t x = 0; x < GL_WIDTH; x++) {
        if (x % 8 == 0) duck_vga_cell((uint16_t)x, 24, '-', 0x08);
    }

    const int32_t points3d[8][3] = {
        { -24, -12, 20 }, { 24, -12, 20 }, { 24, 12, 20 }, { -24, 12, 20 },
        { -24, -12, 40 }, { 24, -12, 40 }, { 24, 12, 40 }, { -24, 12, 40 }
    };
    cube_line(points3d[0][0], points3d[0][1], points3d[0][2], points3d[1][0], points3d[1][1], points3d[1][2]);
    cube_line(points3d[1][0], points3d[1][1], points3d[1][2], points3d[2][0], points3d[2][1], points3d[2][2]);
    cube_line(points3d[2][0], points3d[2][1], points3d[2][2], points3d[3][0], points3d[3][1], points3d[3][2]);
    cube_line(points3d[3][0], points3d[3][1], points3d[3][2], points3d[0][0], points3d[0][1], points3d[0][2]);
    cube_line(points3d[4][0], points3d[4][1], points3d[4][2], points3d[5][0], points3d[5][1], points3d[5][2]);
    cube_line(points3d[5][0], points3d[5][1], points3d[5][2], points3d[6][0], points3d[6][1], points3d[6][2]);
    cube_line(points3d[6][0], points3d[6][1], points3d[6][2], points3d[7][0], points3d[7][1], points3d[7][2]);
    cube_line(points3d[7][0], points3d[7][1], points3d[7][2], points3d[4][0], points3d[4][1], points3d[4][2]);
    for (size_t i = 0; i < 4; i++) {
        cube_line(points3d[i][0], points3d[i][1], points3d[i][2],
                  points3d[i + 4][0], points3d[i + 4][1], points3d[i + 4][2]);
    }

    const char *title = "MSPC Software-GL 1.1 - 3D wireframe test";
    for (size_t i = 0; title[i] && i < GL_WIDTH; i++) duck_vga_cell((uint16_t)i, 0, title[i], 0x1f);
}

static int32_t rotate_cos(int32_t angle) {
    static const int16_t cosine[16] = { 100, 92, 71, 38, 0, -38, -71, -92, -100, -92, -71, -38, 0, 38, 71, 92 };
    if (angle < 0) angle += 16;
    return cosine[angle & 15];
}

static int32_t rotate_sin(int32_t angle) {
    static const int16_t sine[16] = { 0, 38, 71, 92, 100, 92, 71, 38, 0, -38, -71, -92, -100, -92, -71, -38 };
    if (angle < 0) angle += 16;
    return sine[angle & 15];
}

void mspc_gl_run(void) {
    int32_t yaw = 2;
    int32_t pitch = 1;
    for (;;) {
        duck_vga_clear(0x07);
        const char *title = "MSPC Software-GL 1.1 - cube | Arrows rotate | Esc exit";
        for (size_t i = 0; title[i] && i < GL_WIDTH; i++) duck_vga_cell((uint16_t)i, 0, title[i], 0x1f);

        const int32_t cube[8][3] = {
            { -24, -14, -24 }, { 24, -14, -24 }, { 24, 14, -24 }, { -24, 14, -24 },
            { -24, -14, 24 }, { 24, -14, 24 }, { 24, 14, 24 }, { -24, 14, 24 }
        };
        int32_t projected[8][2];
        int32_t cy = rotate_cos(yaw);
        int32_t sy = rotate_sin(yaw);
        int32_t cp = rotate_cos(pitch);
        int32_t sp = rotate_sin(pitch);

        for (size_t i = 0; i < 8; i++) {
            int32_t x = cube[i][0];
            int32_t y = cube[i][1];
            int32_t z = cube[i][2];
            int32_t yaw_x = (x * cy - z * sy) / 100;
            int32_t yaw_z = (x * sy + z * cy) / 100;
            int32_t pitch_y = (y * cp - yaw_z * sp) / 100;
            int32_t pitch_z = (y * sp + yaw_z * cp) / 100;
            int32_t depth = pitch_z + 140;
            if (depth < 40) depth = 40;
            projected[i][0] = 40 + yaw_x * 60 / depth;
            projected[i][1] = 13 - pitch_y * 30 / depth;
        }

        static const uint8_t edges[12][2] = {
            { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
            { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
        };
        for (size_t i = 0; i < 12; i++) {
            gl_line(projected[edges[i][0]][0], projected[edges[i][0]][1],
                    projected[edges[i][1]][0], projected[edges[i][1]][1]);
        }

        uint8_t scan, extended, released;
        if (!duck_ps2_read_key(&scan, &extended, &released) || released) continue;
        if (!extended && scan == 0x01) return;
        if (extended && scan == 0x4b) yaw--;
        else if (extended && scan == 0x4d) yaw++;
        else if (extended && scan == 0x48) pitch--;
        else if (extended && scan == 0x50) pitch++;
    }
}
