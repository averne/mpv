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
};

static int init(struct libmpv_gpu_context *ctx, mpv_render_param *params) {
    MP_VERBOSE(ctx, "Creating libmpv deko3d context\n");

    ctx->priv = talloc_zero(NULL, struct priv);
    struct priv *priv = ctx->priv;

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

    priv->cur_fbo->params.w          = fbo->w;
    priv->cur_fbo->params.h          = fbo->h;
    priv->cur_fbo->params.d          = 1;
    priv->cur_fbo->params.format     = fmt;
    priv->cur_fbo->params.render_dst = true;
    priv->cur_fbo->params.blit_src   = true;
    priv->cur_fbo->params.blit_dst   = true;

    struct ra_tex_dk *priv_tex = priv->cur_fbo->priv;
    priv_tex->image = *fbo->tex;

    *out = priv->cur_fbo;

    return 0;
}

static void done_frame(struct libmpv_gpu_context *ctx, bool ds) {
    struct priv *priv = ctx->priv;

    MP_VERBOSE(ctx, "%s\n", __func__);

    // Wait for the rendering to complete before clearing the state
    // TODO: is there a better way than to use dkQueueWaitIdle? (semaphore with DkVariable?)
    // But since frame rendering is sequential it shouldn't matter
    dkQueueWaitIdle(priv->dk->queue);

    dkCmdBufClear(priv->dk->cmdbuf);

    for (int i = 0; i < priv->dk->num_tmp_memblocks; ++i)
        dkMemBlockDestroy(priv->dk->tmp_memblocks[i]);
    priv->dk->num_tmp_memblocks = 0;
}

static void destroy(struct libmpv_gpu_context *ctx) {
    struct priv *p = ctx->priv;

    MP_VERBOSE(ctx, "Destroying libmpv deko3d context\n");

    if (p->ra_ctx)
        ra_dk_ctx_uninit(p->ra_ctx);
}

const struct libmpv_gpu_context_fns libmpv_gpu_context_dk = {
    .api_name   = MPV_RENDER_API_TYPE_DEKO3D,
    .init       = init,
    .wrap_fbo   = wrap_fbo,
    .done_frame = done_frame,
    .destroy    = destroy,
};
