/*
 * Copyright 2019-2022 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <string.h>

#include "drawutils.h"
#include "nopegl.h"

#define BYTES_PER_PIXEL 4 /* RGBA */

#define FONT_OFFSET 32

static const uint8_t font8[128 - FONT_OFFSET][8] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x08, 0x08, 0x08, 0x08, 0x00, 0x08, 0x00},
    {0x00, 0x14, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x14, 0x3e, 0x14, 0x14, 0x3e, 0x14, 0x00},
    {0x08, 0x3c, 0x0a, 0x1c, 0x28, 0x28, 0x1e, 0x08}, {0x00, 0x00, 0x22, 0x10, 0x08, 0x04, 0x22, 0x00},
    {0x00, 0x0c, 0x12, 0x0c, 0x52, 0x32, 0x6c, 0x00}, {0x00, 0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x18, 0x04, 0x04, 0x04, 0x04, 0x18, 0x00}, {0x00, 0x0c, 0x10, 0x10, 0x10, 0x10, 0x0c, 0x00},
    {0x08, 0x2a, 0x1c, 0x7f, 0x1c, 0x2a, 0x08, 0x00}, {0x00, 0x00, 0x08, 0x08, 0x3e, 0x08, 0x08, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02}, {0x00, 0x00, 0x00, 0x00, 0x3e, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00}, {0x00, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00},
    {0x00, 0x1c, 0x32, 0x2a, 0x2a, 0x26, 0x1c, 0x00}, {0x00, 0x08, 0x0c, 0x08, 0x08, 0x08, 0x1c, 0x00},
    {0x00, 0x1c, 0x22, 0x20, 0x1c, 0x02, 0x3e, 0x00}, {0x00, 0x1c, 0x22, 0x18, 0x20, 0x22, 0x1c, 0x00},
    {0x00, 0x18, 0x14, 0x12, 0x3e, 0x10, 0x38, 0x00}, {0x00, 0x3e, 0x02, 0x1e, 0x20, 0x22, 0x1c, 0x00},
    {0x00, 0x3c, 0x02, 0x1e, 0x22, 0x22, 0x1c, 0x00}, {0x00, 0x3e, 0x20, 0x10, 0x08, 0x04, 0x04, 0x00},
    {0x00, 0x1c, 0x22, 0x1c, 0x22, 0x22, 0x1c, 0x00}, {0x00, 0x1c, 0x22, 0x22, 0x3c, 0x20, 0x1e, 0x00},
    {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x00}, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x04, 0x02},
    {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10, 0x00}, {0x00, 0x00, 0x3e, 0x00, 0x00, 0x3e, 0x00, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02, 0x00}, {0x00, 0x1c, 0x22, 0x20, 0x18, 0x04, 0x00, 0x04},
    {0x00, 0x3c, 0x22, 0x3a, 0x1a, 0x42, 0x3c, 0x00}, {0x00, 0x1c, 0x22, 0x22, 0x3e, 0x22, 0x22, 0x00},
    {0x00, 0x1e, 0x22, 0x1e, 0x22, 0x22, 0x1e, 0x00}, {0x00, 0x3c, 0x02, 0x02, 0x02, 0x02, 0x3c, 0x00},
    {0x00, 0x1e, 0x22, 0x22, 0x22, 0x22, 0x1e, 0x00}, {0x00, 0x3e, 0x02, 0x1e, 0x02, 0x02, 0x3e, 0x00},
    {0x00, 0x3e, 0x02, 0x1e, 0x02, 0x02, 0x02, 0x00}, {0x00, 0x1c, 0x22, 0x02, 0x32, 0x22, 0x3c, 0x00},
    {0x00, 0x22, 0x22, 0x3e, 0x22, 0x22, 0x22, 0x00}, {0x00, 0x1c, 0x08, 0x08, 0x08, 0x08, 0x1c, 0x00},
    {0x00, 0x20, 0x20, 0x20, 0x20, 0x22, 0x1c, 0x00}, {0x00, 0x12, 0x0a, 0x06, 0x0a, 0x12, 0x22, 0x00},
    {0x00, 0x02, 0x02, 0x02, 0x02, 0x02, 0x3e, 0x00}, {0x00, 0x22, 0x36, 0x2a, 0x22, 0x22, 0x22, 0x00},
    {0x00, 0x22, 0x26, 0x2a, 0x32, 0x22, 0x22, 0x00}, {0x00, 0x1c, 0x22, 0x22, 0x22, 0x22, 0x1c, 0x00},
    {0x00, 0x1e, 0x22, 0x22, 0x1e, 0x02, 0x02, 0x00}, {0x00, 0x1c, 0x22, 0x22, 0x22, 0x1c, 0x30, 0x00},
    {0x00, 0x1e, 0x22, 0x22, 0x1e, 0x22, 0x22, 0x00}, {0x00, 0x3c, 0x02, 0x1c, 0x20, 0x20, 0x1e, 0x00},
    {0x00, 0x3e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, {0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1c, 0x00},
    {0x00, 0x22, 0x22, 0x22, 0x14, 0x14, 0x08, 0x00}, {0x00, 0x22, 0x22, 0x22, 0x2a, 0x36, 0x22, 0x00},
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22, 0x00}, {0x00, 0x22, 0x22, 0x22, 0x14, 0x08, 0x08, 0x00},
    {0x00, 0x3e, 0x10, 0x08, 0x04, 0x02, 0x3e, 0x00}, {0x00, 0x1c, 0x04, 0x04, 0x04, 0x04, 0x1c, 0x00},
    {0x00, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00}, {0x00, 0x1c, 0x10, 0x10, 0x10, 0x10, 0x1c, 0x00},
    {0x00, 0x08, 0x14, 0x22, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00},
    {0x00, 0x02, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x3c, 0x22, 0x22, 0x22, 0x5c, 0x00},
    {0x02, 0x02, 0x1e, 0x22, 0x22, 0x22, 0x1e, 0x00}, {0x00, 0x00, 0x1c, 0x22, 0x02, 0x02, 0x3c, 0x00},
    {0x20, 0x20, 0x3c, 0x22, 0x22, 0x22, 0x3c, 0x00}, {0x00, 0x00, 0x1c, 0x22, 0x3e, 0x02, 0x3c, 0x00},
    {0x00, 0x38, 0x04, 0x1e, 0x04, 0x04, 0x04, 0x00}, {0x00, 0x00, 0x3c, 0x22, 0x22, 0x3c, 0x20, 0x1e},
    {0x02, 0x02, 0x02, 0x1e, 0x22, 0x22, 0x22, 0x00}, {0x00, 0x04, 0x00, 0x06, 0x04, 0x04, 0x0c, 0x00},
    {0x00, 0x10, 0x00, 0x10, 0x10, 0x10, 0x12, 0x0c}, {0x02, 0x02, 0x12, 0x0a, 0x06, 0x0a, 0x12, 0x00},
    {0x0c, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1c, 0x00}, {0x00, 0x00, 0x1e, 0x2a, 0x2a, 0x2a, 0x2a, 0x00},
    {0x00, 0x00, 0x1a, 0x26, 0x22, 0x22, 0x22, 0x00}, {0x00, 0x00, 0x1c, 0x22, 0x22, 0x22, 0x1c, 0x00},
    {0x00, 0x00, 0x1e, 0x22, 0x22, 0x1e, 0x02, 0x02}, {0x00, 0x00, 0x3c, 0x22, 0x22, 0x3c, 0x20, 0x20},
    {0x00, 0x00, 0x1a, 0x26, 0x02, 0x02, 0x02, 0x00}, {0x00, 0x00, 0x3c, 0x02, 0x1c, 0x20, 0x1e, 0x00},
    {0x04, 0x04, 0x1e, 0x04, 0x04, 0x04, 0x18, 0x00}, {0x00, 0x00, 0x22, 0x22, 0x22, 0x32, 0x2c, 0x00},
    {0x00, 0x00, 0x22, 0x22, 0x14, 0x14, 0x08, 0x00}, {0x00, 0x00, 0x22, 0x2a, 0x2a, 0x2a, 0x14, 0x00},
    {0x00, 0x00, 0x22, 0x14, 0x08, 0x14, 0x22, 0x00}, {0x00, 0x00, 0x22, 0x22, 0x22, 0x3c, 0x20, 0x1e},
    {0x00, 0x00, 0x3e, 0x10, 0x08, 0x04, 0x3e, 0x00}, {0x18, 0x04, 0x04, 0x02, 0x04, 0x04, 0x18, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00}, {0x0c, 0x10, 0x10, 0x20, 0x10, 0x10, 0x0c, 0x00},
    {0x00, 0x00, 0x00, 0x2c, 0x1a, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
};

static inline void set_color(uint8_t *p, uint32_t rgba)
{
    p[0] = (uint8_t)(rgba >> 24);
    p[1] = rgba >> 16 & 0xff;
    p[2] = rgba >>  8 & 0xff;
    p[3] = rgba       & 0xff;
}

static inline uint8_t *get_pixel_pos_buf(const struct canvas *canvas, int px, int py)
{
    return canvas->buf + (py * canvas->w + px) * BYTES_PER_PIXEL;
}

void ngli_drawutils_draw_rect(struct canvas *canvas, const struct rect *rect, uint32_t color)
{
    uint8_t *buf = get_pixel_pos_buf(canvas, rect->x, rect->y);
    const int stride = canvas->w * BYTES_PER_PIXEL;
    for (int y = 0; y < rect->h; y++) {
        for (int x = 0; x < rect->w; x++)
            set_color(buf + x * BYTES_PER_PIXEL, color);
        buf += stride;
    }
}

void ngli_drawutils_print(struct canvas *canvas, int x, int y, const char *str, uint32_t color)
{
    int px = 0, py = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] == '\n') {
            py++;
            px = 0;
            continue;
        }

        const uint8_t *c = str[i] < FONT_OFFSET || str[i] > 127 ? font8[0] : font8[str[i] - FONT_OFFSET];
        for (int char_y = 0; char_y < NGLI_FONT_H; char_y++) {
            for (int char_x = 0; char_x < NGLI_FONT_W; char_x++) {
                const int pix_x = x + px * NGLI_FONT_W + char_x;
                const int pix_y = y + py * NGLI_FONT_H + char_y;
                if (pix_x < 0 || pix_y < 0 || pix_x >= canvas->w || pix_y >= canvas->h)
                    continue;
                uint8_t *p = get_pixel_pos_buf(canvas, pix_x, pix_y);
                if (c[char_y] & (1 << char_x))
                    set_color(p, color);
            }
        }
        px++;
    }
}
