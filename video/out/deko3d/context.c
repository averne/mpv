/*
 * Copyright (C) 2021 averne <averne381@gmail.com>
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <switch.h>
#include <deko3d.h>

#include <libplacebo/deko3d.h>

#include "common/msg.h"
#include "options/m_config.h"

#include "video/out/placebo/ra_pl.h"
#include "video/out/placebo/utils.h"
#include "video/out/gpu/context.h"

struct priv {
    struct pl_log *pl_log;
    struct mp_log *log;
    struct ra     *ra;
    struct mpdk_ctx ctx;

    pl_deko3d dk;
    pl_gpu    gpu;
};

static int color_depth(struct ra_swapchain *sw) {
    MP_VERBOSE(sw->ctx, "color_depth\n");
    return 0;
}

static bool start_frame(struct ra_swapchain *sw, struct ra_fbo *out_fbo) {
    MP_VERBOSE(sw->ctx, "start_frame\n");
    return true;
}

static bool submit_frame(struct ra_swapchain *sw, const struct vo_frame *frame) {
    MP_VERBOSE(sw->ctx, "submit_frame\n");
    return true;
}

static void swap_buffers(struct ra_swapchain *sw) {
    MP_VERBOSE(sw->ctx, "swap_buffers\n");
}

static void get_vsync(struct ra_swapchain *sw, struct vo_vsync_info *info) {
    MP_VERBOSE(sw->ctx, "get_vsync\n");
}

static const struct ra_swapchain_fns deko3d_swapchain = {
    .color_depth  = color_depth,
    .start_frame  = start_frame,
    .submit_frame = submit_frame,
    .swap_buffers = swap_buffers,
    .get_vsync    = get_vsync,
};

static bool reconfig(struct ra_ctx *ctx) {
    MP_VERBOSE(ctx, "reconfig\n");
    return true;
}

static int control(struct ra_ctx *ctx, int *events, int request, void *arg) {
    MP_VERBOSE(ctx, "control\n");
    return VO_TRUE;
}

static void uninit(struct ra_ctx *ctx) {
    struct priv *priv = ctx->priv;

    MP_VERBOSE(ctx, "uninit\n");

    if (ctx->ra) {
        pl_gpu_finish(priv->gpu);
        // pl_swapchain_destroy(&priv->swapchain);
        // ctx->ra->fns->destroy(ctx->ra);
        ctx->ra = NULL;
    }

    pl_deko3d_destroy(&priv->dk);
    pl_context_destroy(&priv->pl_log);
    TA_FREEP(&ctx->swapchain);
    TA_FREEP(&priv->log);
    priv->gpu = NULL;
}

static bool init(struct ra_ctx *ctx) {
    struct priv *priv       = ctx->priv      = talloc_zero(ctx, struct priv);
    struct ra_swapchain *sw = ctx->swapchain = talloc_zero(NULL, struct ra_swapchain);

    MP_VERBOSE(ctx, "init\n");

    priv->pl_log = pl_context_create(PL_API_VER, NULL);
    if (!priv->pl_log)
        goto error;

    priv->log = mp_log_new(ctx, ctx->log, "libplacebo");
    mppl_ctx_set_log(priv->pl_log, priv->log, false);
    MP_VERBOSE(priv, "Initialized libplacebo v%d\n", PL_API_VER);

    priv->dk = pl_deko3d_create(priv->pl_log);
    if (!priv->dk)
        goto error;

    priv->gpu = priv->dk->gpu;

    ctx->ra = ra_create_pl(priv->gpu, ctx->log);
    if (!ctx->ra)
        goto error;

    sw->ctx = ctx;
    sw->fns = &deko3d_swapchain;

    // priv->swapchain = pl_deko3d_create_swapchain(priv->dk);
    // if (!priv->swapchain)
    //     goto error;

    return true;

error:
    uninit(ctx);
    return false;
}

const struct ra_ctx_fns ra_ctx_deko3d = {
    .type     = "deko3d",
    .name     = "deko3d",
    .reconfig = reconfig,
    .control  = control,
    .init     = init,
    .uninit   = uninit,
};
