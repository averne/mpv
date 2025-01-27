/*
 * Copyright (c) 2024 averne <averne381@gmail.com>
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

#include <deko3d.h>
#include <switch.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_envideo.h>

#include "config.h"

#include "common/common.h"
#include "options/m_config.h"
#include "video/hwdec.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/deko3d/ra_dk.h"

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
};

struct priv {
    mp_dk_ctx *dk;

    bool has_calculated_layouts;
    int num_planes;

    DkImageLayout dklayouts[3];
    bool is_linear;

    struct cached_texture {
        AVHWFramesContext *frames_ctx;

        uint32_t handle;
        DkMemBlock memblock;
        struct ra_tex_dk *tex[3];
    } *cached_textures;
    int num_cached_textures;
};

// NVDEC can render to NV12 and YV12 surfaces, the FFmpeg backend hardcodes for NV12
// NVJPG can decode to grayscale surfaces
// Some filters will output YUV420P and upload that to a hardware surface
static const int supported_formats[] = {
    IMGFMT_Y8,
    IMGFMT_NV12,
    IMGFMT_P010,
    IMGFMT_420P,
    IMGFMT_NONE,
};

static int init(struct ra_hwdec *hw) {
    struct priv_owner *priv = hw->priv;

    MP_VERBOSE(hw, "%s\n", __func__);

    AVBufferRef *hw_device_ctx = NULL;
    if ((av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_ENVIDEO, NULL, NULL, 0) < 0)
            || (hw_device_ctx == NULL))
        goto error;

    priv->hwctx = (struct mp_hwdec_ctx) {
        .driver_name       = hw->driver->name,
        .av_device_ref     = hw_device_ctx,
        .supported_formats = supported_formats,
        .hw_imgfmt         = IMGFMT_ENVIDEO,
    };
    hwdec_devices_add(hw->devs, &priv->hwctx);

    return 0;

 error:
    av_buffer_unref(&hw_device_ctx);
    return -1;
}

static void uninit(struct ra_hwdec *hw) {
    struct priv_owner *priv = hw->priv;

    MP_VERBOSE(hw, "%s\n", __func__);

    hwdec_devices_remove(hw->devs, &priv->hwctx);
    av_buffer_unref(&priv->hwctx.av_device_ref);
}

static int mapper_init(struct ra_hwdec_mapper *mapper) {
    struct priv *priv = mapper->priv;

    MP_VERBOSE(mapper, "%s\n", __func__);

    mapper->dst_params           = mapper->src_params;
    mapper->dst_params.imgfmt    = mapper->src_params.hw_subfmt;
    mapper->dst_params.hw_subfmt = 0;

    struct mp_image layout;
    mp_image_set_params(&layout, &mapper->dst_params);

    struct ra_imgfmt_desc desc;
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc)) {
        MP_ERR(mapper, "Unsupported format: %s\n", mp_imgfmt_to_name(mapper->dst_params.imgfmt));
        return -1;
    }

    priv->dk                     = ra_dk_get_ctx(mapper->ra);
    priv->num_planes             = desc.num_planes;
    priv->has_calculated_layouts = false;

    for (int i = 0; i < priv->num_planes; ++i) {
        mapper->tex[i] = talloc_zero(mapper, struct ra_tex);
        if (!mapper->tex[i])
            return -1;

        mapper->tex[i]->params = (struct ra_tex_params){
            .dimensions = 2,
            .w          = mp_image_plane_w(&layout, i),
            .h          = mp_image_plane_h(&layout, i),
            .d          = 1,
            .format     = desc.planes[i],
            .render_src = true,
            .src_linear = true,
        };
    }

    return 0;
}

static void destroy_cache_entry(struct ra_hwdec_mapper *mapper, struct cached_texture *e) {
    struct priv *priv = mapper->priv;

    for (int i = 0; i < priv->num_planes; ++i)
        ra_dk_unregister_texture(mapper->ra, e->tex[i]);

    if (e->memblock)
        dkMemBlockDestroy(e->memblock);
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper) {
    struct priv *priv = mapper->priv;

    MP_VERBOSE(mapper, "%s\n", __func__);

    for (int i = 0; i < priv->num_cached_textures; ++i)
        destroy_cache_entry(mapper, &priv->cached_textures[i]);
}

static int mapper_map(struct ra_hwdec_mapper *mapper) {
    struct priv *priv = mapper->priv;
    AVEnvideoFrame *frame = (AVEnvideoFrame *)mapper->src->bufs[0]->data;
    EnvideoMap *map = (EnvideoMap *)frame->map;

    if ((priv->is_linear != frame->is_pitch) || !priv->has_calculated_layouts) {
        priv->is_linear = frame->is_pitch;

        for (int i = 0; i < priv->num_planes; ++i) {
            struct ra_tex_params *params = &mapper->tex[i]->params;

            // If the width (aligned to relevant boundaries) is not equal to the stride
            // (for example because of cropping), set it to the latter
            int align = ENVIDEO_WIDTH_ALIGN(mapper->tex[i]->params.format->pixel_size);
            int texel_stride = mapper->src->stride[i] / mapper->tex[i]->params.format->pixel_size;
            if (MP_ALIGN_UP(params->w, align) != texel_stride)
                params->w = texel_stride;

            DkImageLayoutMaker layout_maker;
            dkImageLayoutMakerDefaults(&layout_maker, priv->dk->device);
            layout_maker.type          = DkImageType_2D;
            layout_maker.format        = ((struct dk_format *)params->format->priv)->fmt;
            layout_maker.dimensions[0] = params->w;
            layout_maker.dimensions[1] = params->h;
            layout_maker.dimensions[2] = 1;

            layout_maker.flags = DkImageFlags_UsageLoadStore | DkImageFlags_Usage2DEngine;

            if (priv->is_linear) {
                layout_maker.flags      |= DkImageFlags_PitchLinear;
                layout_maker.pitchStride = mapper->src->stride[i];
            } else {
                layout_maker.flags      |= DkImageFlags_UsageVideo;
            }

            dkImageLayoutInitialize(&priv->dklayouts[i], &layout_maker);
        }

        priv->has_calculated_layouts = true;
    }

    // Clean up stale cached frames
    AVHWFramesContext *hwctx = (AVHWFramesContext *)mapper->src->hwctx->data;
    for (int i = priv->num_cached_textures - 1; i >= 0; --i) {
        if (priv->cached_textures[i].frames_ctx != hwctx) {
            destroy_cache_entry(mapper, &priv->cached_textures[i]);
            MP_TARRAY_REMOVE_AT(priv->cached_textures, priv->num_cached_textures, i);
        }
    }

    // Wait for the decode to complete
    // Submit immediately so that deko3d doesn't keep a dangling pointer to the decode fence
    DkFence fence;
    dkFenceImport(&fence, frame->fence >> 32, frame->fence >> 0);
    dkCmdBufWaitFence(priv->dk->cmdbuf, &fence);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));

    for (int i = 0; i < priv->num_cached_textures; ++i) {
        struct cached_texture *e = &priv->cached_textures[i];
        if (e->handle == envideo_map_get_handle(map)) {
            for (int j = 0; j < priv->num_planes; ++j)
                mapper->tex[j]->priv = e->tex[j];

            // Invalidate texture cache
            dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_None, DkInvalidateFlags_Image);
            dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
            return 0;
        }
    }

    struct cached_texture e = {0};
    e.frames_ctx = hwctx;
    e.handle     = envideo_map_get_handle(map);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, envideo_map_get_size(map));
    memblock_maker.flags   = DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    memblock_maker.storage = envideo_map_get_cpu_addr(map);
    e.memblock = dkMemBlockCreate(&memblock_maker);
    if (!e.memblock)
        return -1;

    for (int i = 0; i < priv->num_planes; ++i) {
        DkImage image;
        dkImageInitialize(&image, &priv->dklayouts[i], e.memblock,
            (uintptr_t)(mapper->src->planes[i] - mapper->src->planes[0]));

        struct ra_tex_dk *tex_priv = mapper->tex[i]->priv = talloc_zero(mapper->tex[i], struct ra_tex_dk);
        if (!tex_priv) {
            dkMemBlockDestroy(e.memblock);
            return -1;
        }

        tex_priv->image    = image;
        tex_priv->memblock = e.memblock;

        ra_dk_register_texture(mapper->ra, mapper->tex[i]);
        e.tex[i] = mapper->tex[i]->priv;
    }

    MP_TARRAY_APPEND(mapper, priv->cached_textures, priv->num_cached_textures, e);

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper) {
    // Do nothing
}

const struct ra_hwdec_driver ra_hwdec_envideo = {
    .name          = "envideo",
    .priv_size     = sizeof(struct priv_owner),
    .imgfmts       = {IMGFMT_ENVIDEO, 0},
    .init          = init,
    .uninit        = uninit,
    .mapper        = &(const struct ra_hwdec_mapper_driver){
        .priv_size = sizeof(struct priv),
        .init      = mapper_init,
        .uninit    = mapper_uninit,
        .map       = mapper_map,
        .unmap     = mapper_unmap,
    },
};
