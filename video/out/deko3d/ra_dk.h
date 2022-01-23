/*
 * Copyright (C) 2021 averne <averne381@gmail.com>
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
    DkImageFormat fmt;
    DkFence fence;

    int descriptor_idx;
};

struct ra_buf_dk {
    DkMemBlock memblock;
};

struct ra_rpass_dk {
    DkMemBlock shader_memblock;
    DkShader *shaders;
    int num_shaders;

    DkMemBlock vao_memblock;
    DkVtxAttribState *vao_attribs;
    DkVtxBufferState vao_state;

    DkRasterizerState rasterizer_state;
    DkColorState color_state;
    DkColorWriteState color_write_state;
    DkBlendState blend_state;
};

struct ra *ra_create_dk(mp_dk_ctx *dk, struct mp_log *log);
