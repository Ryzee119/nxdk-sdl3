/*
 * QEMU texture swizzling routines
 *
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2013 espes
 * Copyright (c) 2007-2010 The Nouveau Project.
 * Copyright (c) 2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "swizzle.h"

/*
 * Helpers for converting to and from swizzled (Z-ordered) texture formats.
 * Swizzled textures store pixels in a more cache-friendly layout for rendering
 * than linear textures.
 * Width, height, and depth must be powers of two.
 * See also:
 * https://en.wikipedia.org/wiki/Z-order_curve
 */

/*
 * Create masks representing the interleaving of each linear texture dimension (x, y, z).
 * These can be used to map linear texture coordinates to a swizzled "Z" offset.
 * For example, a 2D 8x32 texture needs 3 bits for x, and 5 bits for y:
 * mask_x:  00010101
 * mask_y:  11101010
 * mask_z:  00000000
 * for "Z": yyyxyxyx
 */
static void generate_swizzle_masks(unsigned int width,
                                   unsigned int height,
                                   unsigned int depth,
                                   uint32_t *mask_x,
                                   uint32_t *mask_y,
                                   uint32_t *mask_z)
{
    uint32_t x = 0, y = 0, z = 0;
    uint32_t bit = 1;
    uint32_t mask_bit = 1;
    bool done;
    do {
        done = true;
        if (bit < width) {
            x |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < height) {
            y |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        if (bit < depth) {
            z |= mask_bit;
            mask_bit <<= 1;
            done = false;
        }
        bit <<= 1;
    } while (!done);
    assert((x ^ y ^ z) == (mask_bit - 1)); /* masks are mutually exclusive */
    *mask_x = x;
    *mask_y = y;
    *mask_z = z;
}

static void swizzle_box_internal(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel)
{
    uint32_t mask_x, mask_y, mask_z;
    generate_swizzle_masks(width, height, depth, &mask_x, &mask_y, &mask_z);

    uint32_t x_offsets_stack[2048];
    uint32_t *x_lut = (width <= 2048) ? x_offsets_stack : (uint32_t *)malloc(width * sizeof(uint32_t));
    if (x_lut == NULL) {
        return;
    }

    uint32_t off_x = 0;
    for (unsigned int x = 0; x < width; x++) {
        x_lut[x] = off_x;
        off_x = (off_x - mask_x) & mask_x;
    }

    uint32_t off_z = 0;
    for (unsigned int z = 0; z < depth; z++) {
        uint32_t off_y = 0;
        for (unsigned int y = 0; y < height; y++) {
            const uint8_t *src_row = src_buf + y * row_pitch;
            const uint32_t base_dst_idx = off_y + off_z;

            if (bytes_per_pixel == 4) {
                const uint32_t *src32 = (const uint32_t *)src_row;
                uint32_t *dst32 = (uint32_t *)dst_buf + base_dst_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst32[x_lut[x]] = src32[x];
                }
            } else if (bytes_per_pixel == 2) {
                const uint16_t *src16 = (const uint16_t *)src_row;
                uint16_t *dst16 = (uint16_t *)dst_buf + base_dst_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst16[x_lut[x]] = src16[x];
                }
            } else if (bytes_per_pixel == 1) {
                const uint8_t *src8 = src_row;
                uint8_t *dst8 = dst_buf + base_dst_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst8[x_lut[x]] = src8[x];
                }
            } else {
                uint8_t *dst_tmp = dst_buf + base_dst_idx * bytes_per_pixel;
                for (unsigned int x = 0; x < width; x++) {
                    memcpy(dst_tmp + x_lut[x] * bytes_per_pixel, src_row + x * bytes_per_pixel, bytes_per_pixel);
                }
            }

            off_y = (off_y - mask_y) & mask_y;
        }
        src_buf += slice_pitch;
        off_z = (off_z - mask_z) & mask_z;
    }

    if (x_lut != x_offsets_stack) {
        free(x_lut);
    }
}

static void unswizzle_box_internal(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel)
{
    uint32_t mask_x, mask_y, mask_z;
    generate_swizzle_masks(width, height, depth, &mask_x, &mask_y, &mask_z);

    uint32_t x_offsets_stack[2048];
    uint32_t *x_lut = (width <= 2048) ? x_offsets_stack : (uint32_t *)malloc(width * sizeof(uint32_t));
    if (x_lut == NULL) {
        return;
    }

    uint32_t off_x = 0;
    for (unsigned int x = 0; x < width; x++) {
        x_lut[x] = off_x;
        off_x = (off_x - mask_x) & mask_x;
    }

    uint32_t off_z = 0;
    for (unsigned int z = 0; z < depth; z++) {
        uint32_t off_y = 0;
        for (unsigned int y = 0; y < height; y++) {
            uint8_t *dst_row = dst_buf + y * row_pitch;
            const uint32_t base_src_idx = off_y + off_z;

            if (bytes_per_pixel == 4) {
                uint32_t *dst32 = (uint32_t *)dst_row;
                const uint32_t *src32 = (const uint32_t *)src_buf + base_src_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst32[x] = src32[x_lut[x]];
                }
            } else if (bytes_per_pixel == 2) {
                uint16_t *dst16 = (uint16_t *)dst_row;
                const uint16_t *src16 = (const uint16_t *)src_buf + base_src_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst16[x] = src16[x_lut[x]];
                }
            } else if (bytes_per_pixel == 1) {
                uint8_t *dst8 = dst_row;
                const uint8_t *src8 = src_buf + base_src_idx;
                for (unsigned int x = 0; x < width; x++) {
                    dst8[x] = src8[x_lut[x]];
                }
            } else {
                const uint8_t *src_tmp = src_buf + base_src_idx * bytes_per_pixel;
                for (unsigned int x = 0; x < width; x++) {
                    memcpy(dst_row + x * bytes_per_pixel, src_tmp + x_lut[x] * bytes_per_pixel, bytes_per_pixel);
                }
            }

            off_y = (off_y - mask_y) & mask_y;
        }
        dst_buf += slice_pitch;
        off_z = (off_z - mask_z) & mask_z;
    }

    if (x_lut != x_offsets_stack) {
        free(x_lut);
    }
}

void swizzle_box(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel)
{
    swizzle_box_internal(src_buf, width, height, depth, dst_buf, row_pitch, slice_pitch, bytes_per_pixel);
}

void unswizzle_box(
    const uint8_t *src_buf,
    unsigned int width,
    unsigned int height,
    unsigned int depth,
    uint8_t *dst_buf,
    unsigned int row_pitch,
    unsigned int slice_pitch,
    unsigned int bytes_per_pixel)
{
    unswizzle_box_internal(src_buf, width, height, depth, dst_buf, row_pitch, slice_pitch, bytes_per_pixel);
}
