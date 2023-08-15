/*
 * Copyright (c) 2023 averne <averne381@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "video/out/gpu/ra.h"
#include "video/out/gpu/utils.h"
#include "common.h"

struct dk_format {
    const char *name;
    int components;
    int bytes;
    int bits[4];
    DkImageFormat fmt;
    enum ra_ctype ctype;
    bool renderable, linear_filter, storable, ordered;
};

struct ra_tex_dk {
    DkMemBlock memblock;
    DkImage image;
    int descriptor_idx;
};

struct ra_buf_dk {
    DkMemBlock memblock;
    DkFence fence;
    bool is_cpu_cached;
    bool dirty;
};

struct ra_rpass_dk {
    DkMemBlock shader_memblock;
    DkShader *shaders;

    DkMemBlock vao_memblock;
    DkVtxAttribState *vao_attribs;
    DkVtxBufferState vao_state;

    DkRasterizerState rasterizer_state;
    DkColorState color_state;
    DkColorWriteState color_write_state;
    DkBlendState blend_state;
    DkDepthStencilState depth_state;
};

#define RA_DK_MAX_DESCRIPTORS 128
#define RA_DK_MAX_QUERIES     128

#define RA_DK_NUM_QUERIES 2
struct ra_dk_timer {
    int query_idx[RA_DK_NUM_QUERIES];
    int cur_idx;
    uint64_t result;
};

struct ra *ra_create_dk(mp_dk_ctx *dk, struct mp_log *log);
mp_dk_ctx *ra_dk_get_ctx(struct ra *ra);
void ra_dk_register_texture(struct ra *ra, struct ra_tex *tex);
void ra_dk_unregister_texture(struct ra *ra, struct ra_tex_dk *tex);
