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

#include "context.h"
#include "ra_dk.h"

struct priv {
    struct mp_log *log;
    struct ra_swapchain_fns fns;

    mp_dk_ctx *dk;
};

static const struct ra_swapchain_fns ra_dk_swapchain_fns;

bool ra_dk_ctx_init(struct ra_ctx *ctx, mp_dk_ctx *dk, struct ra_dk_ctx_params *params) {
    struct ra_swapchain *sw = ctx->swapchain = talloc_ptrtype(ctx, sw);
    *sw = (struct ra_swapchain) {
        .ctx = ctx,
    };

    struct priv *p = sw->priv = talloc_ptrtype(sw, p);
    *p = (struct priv) {
        .dk  = dk,
        .log = ctx->log,
        .fns = ra_dk_swapchain_fns,
    };

    const struct ra_swapchain_fns *ext = params->external_swapchain;
    if (ext) {
        if (ext->color_depth)
            p->fns.color_depth = ext->color_depth;
        if (ext->start_frame)
            p->fns.start_frame = ext->start_frame;
        if (ext->submit_frame)
            p->fns.submit_frame = ext->submit_frame;
        if (ext->swap_buffers)
            p->fns.swap_buffers = ext->swap_buffers;
    }

    ctx->ra = ra_create_dk(dk, ctx->log);
    return !!ctx->ra;
}

void ra_dk_ctx_uninit(struct ra_ctx *ctx) {
    if (ctx->swapchain)
        TA_FREEP(&ctx->swapchain);
    ra_free(&ctx->ra);
}

static const struct ra_swapchain_fns ra_dk_swapchain_fns = {
    // .color_depth   = ra_dk_ctx_color_depth,
    // .start_frame   = ra_dk_ctx_start_frame,
    // .submit_frame  = ra_dk_ctx_submit_frame,
    // .swap_buffers  = ra_dk_ctx_swap_buffers,
    // .get_vsync     = ra_dk_ctx_get_vsync,
};
