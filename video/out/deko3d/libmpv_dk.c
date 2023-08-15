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

#include "common/msg.h"
#include "options/m_config.h"
#include "libmpv/render_dk3d.h"
#include "video/out/gpu/libmpv_gpu.h"
#include "video/out/gpu/context.h"
#include "video/out/gpu/ra.h"
#include "video/out/deko3d/ra_dk.h"
#include "common.h"
#include "context.h"

struct priv {
    struct ra_ctx *ra_ctx;
    mp_dk_ctx *dk;

    struct ra_tex *cur_fbo;

    bool first_frame;

    DkFence *client_done_fence;
};

static int init(struct libmpv_gpu_context *ctx, mpv_render_param *params) {
    MP_VERBOSE(ctx, "Creating libmpv deko3d context\n");

    struct priv *priv = ctx->priv = talloc_zero(ctx, struct priv);

    mpv_deko3d_init_params *init_params =
        get_mpv_render_param(params, MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, NULL);
    if (!init_params)
        return MPV_ERROR_INVALID_PARAMETER;

    priv->ra_ctx         = talloc_zero(priv, struct ra_ctx);
    priv->ra_ctx->log    = ctx->log;
    priv->ra_ctx->global = ctx->global;
    priv->ra_ctx->opts   = (struct ra_ctx_opts){
        .probing = false,
    };

    priv->dk  = talloc_zero(priv, mp_dk_ctx);
    *priv->dk = (mp_dk_ctx){
        .device = init_params->device,
    };

    struct ra_dk_ctx_params dk_params = {0};

    if (!ra_dk_ctx_init(priv->ra_ctx, priv->dk, &dk_params))
        return MPV_ERROR_UNSUPPORTED;

    ctx->ra = priv->ra_ctx->ra;

    priv->cur_fbo       = talloc_zero(priv, struct ra_tex);
    priv->cur_fbo->priv = talloc_zero(priv, struct ra_tex_dk);

    priv->first_frame = true;

    return 0;
}

static int wrap_fbo(struct libmpv_gpu_context *ctx, mpv_render_param *params, struct ra_tex **out) {
    struct priv *priv = ctx->priv;

    mpv_deko3d_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_DEKO3D_FBO, NULL);

    struct ra_format *fmt = NULL;
    for (int i = 0; i < ctx->ra->num_formats; ++i) {
        fmt = ctx->ra->formats[i];
        if (((struct dk_format *)fmt->priv)->fmt == fbo->format)
            break;
    }

    if (!fmt)
        return MPV_ERROR_INVALID_PARAMETER;

    priv->cur_fbo->params = (struct ra_tex_params){
        .w          = fbo->w,
        .h          = fbo->h,
        .d          = 1,
        .format     = fmt,
        .render_dst = true,
        .blit_src   = true,
        .blit_dst   = true,
    };

    struct ra_tex_dk *priv_tex = priv->cur_fbo->priv;
    priv_tex->image = *fbo->tex;

    *out = priv->cur_fbo;

    return 0;
}

static void begin_frame(struct libmpv_gpu_context *ctx, mpv_render_param *params, struct ra_tex *tex) {
    struct priv *priv = ctx->priv;

    MP_TRACE(ctx, "%s\n", __func__);

    // Wait for the queue operations submitted during initialization to complete
    if (priv->first_frame) {
        dkQueueWaitIdle(priv->dk->queue);
        priv->first_frame = false;
    }

    // Cycle through the command buffer memory
    priv->dk->cur_cmdbuf_slice = (priv->dk->cur_cmdbuf_slice + 1) % RA_DK_NUM_CMDBUFS;
    dkCmdBufClear(priv->dk->cmdbuf);
    dkCmdBufAddMemory(priv->dk->cmdbuf, priv->dk->cmdbuf_memblock,
        priv->dk->cur_cmdbuf_slice * RA_DK_CMDBUF_SIZE, RA_DK_CMDBUF_SIZE);

    // Starting a new render cycle would overwrite the command buffer for the in-flight frame
    // Despite the gpu-side wait inserted before queuing the frame, the rendering is not guaranteed
    // to have completed when the dequeue operation returns, when using triple+ buffering
    dkFenceWait(&priv->dk->cmdbuf_fences[priv->dk->cur_cmdbuf_slice], -1);

    mpv_deko3d_fbo *fbo =
        get_mpv_render_param(params, MPV_RENDER_PARAM_DEKO3D_FBO, NULL);

    priv->client_done_fence = fbo->done_fence;

    // Wait for the framebuffer to be free to write to
    if (fbo->ready_fence)
        dkQueueWaitFence(priv->dk->queue, fbo->ready_fence);
}

static void done_frame(struct libmpv_gpu_context *ctx, bool ds) {
    struct priv *priv = ctx->priv;

    MP_TRACE(ctx, "%s\n", __func__);

    // Signal that all the rendering tasks have completed
    if (priv->client_done_fence)
        dkQueueSignalFence(priv->dk->queue, priv->client_done_fence, false);
    dkQueueSignalFence(priv->dk->queue,
        &priv->dk->cmdbuf_fences[priv->dk->cur_cmdbuf_slice], false);
    dkQueueFlush(priv->dk->queue);
}

static void destroy(struct libmpv_gpu_context *ctx) {
    struct priv *p = ctx->priv;

    MP_VERBOSE(ctx, "Destroying libmpv deko3d context\n");

    if (p && p->ra_ctx)
        ra_dk_ctx_uninit(p->ra_ctx);
}

const struct libmpv_gpu_context_fns libmpv_gpu_context_dk = {
    .api_name    = MPV_RENDER_API_TYPE_DEKO3D,
    .init        = init,
    .wrap_fbo    = wrap_fbo,
    .begin_frame = begin_frame,
    .done_frame  = done_frame,
    .destroy     = destroy,
};
