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
};

struct ra *ra_create_dk(mp_dk_ctx *dk, struct mp_log *log);
