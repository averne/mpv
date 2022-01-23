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
