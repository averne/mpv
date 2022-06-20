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

#include <deko3d.h>
#include <switch.h>

#include <libavutil/hwcontext.h>
#include <libavutil/tx1.h>

#include "config.h"

#include "common/common.h"
#include "options/m_config.h"
#include "video/hwdec.h"
#include "video/out/gpu/hwdec.h"
#include "video/out/deko3d/ra_dk.h"

struct priv_owner {
    struct mp_hwdec_ctx hwctx;
};

// Arrays hardcoded to two members since only NV12 is supported
struct priv {
    mp_dk_ctx *dk;
    DkImageLayout dklayouts[2];

    struct cached_texture {
        uint32_t handle;
        DkMemBlock memblock;
        struct ra_dk_tex *tex[2];
    } *cached_textures;
    int num_cached_textures;
};

// Nvdec can render to NV12 and YV12 surfaces, the FFmpeg backend hardcodes for NV12
static const int supported_formats[] = {
    IMGFMT_NV12,
    0,
};

static int init(struct ra_hwdec *hw) {
    struct priv_owner *priv = hw->priv;

    MP_VERBOSE(hw, "%s\n", __func__);

    AVBufferRef *hw_device_ctx = NULL;
    if ((av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_TX1, NULL, NULL, 0) < 0)
            || (hw_device_ctx == NULL))
        goto error;

    priv->hwctx = (struct mp_hwdec_ctx) {
        .driver_name       = hw->driver->name,
        .av_device_ref     = hw_device_ctx,
        .supported_formats = supported_formats,
        .hw_imgfmt         = IMGFMT_TX1,
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

    priv->dk = ra_dk_get_ctx(mapper->ra);

    struct ra_imgfmt_desc desc;
    if (!ra_get_imgfmt_desc(mapper->ra, mapper->dst_params.imgfmt, &desc)) {
        MP_ERR(mapper, "Unsupported format: %s\n", mp_imgfmt_to_name(mapper->dst_params.imgfmt));
        return -1;
    }

    for (int i = 0; i < desc.num_planes; ++i) {
        DkImageLayoutMaker layout_maker;
        dkImageLayoutMakerDefaults(&layout_maker, priv->dk->device);
        layout_maker.type          = DkImageType_2D;
        layout_maker.format        = ((struct dk_format *)desc.planes[i]->priv)->fmt;
        layout_maker.dimensions[0] = mp_image_plane_w(&layout, i);
        layout_maker.dimensions[1] = mp_image_plane_h(&layout, i);
        layout_maker.dimensions[2] = 1;
        layout_maker.flags         = DkImageFlags_UsageLoadStore |
            DkImageFlags_Usage2DEngine | DkImageFlags_UsageVideo;

        dkImageLayoutInitialize(&priv->dklayouts[i], &layout_maker);

        mapper->tex[i] = talloc_zero(mapper, struct ra_tex);
        if (!mapper->tex[i])
            return -1;

        mapper->tex[i]->params = (struct ra_tex_params) {
            .dimensions = 2,
            .w          = mp_image_plane_w(&layout, i),
            .h          = mp_image_plane_h(&layout, i),
            .d          = 1,
            .format     = desc.planes[i],
            .render_src = true,
            .src_linear = desc.planes[i]->linear_filter,
        };
    }

    return 0;
}

static void mapper_uninit(struct ra_hwdec_mapper *mapper) {
    struct priv *priv = mapper->priv;

    MP_VERBOSE(mapper, "%s\n", __func__);

    for (int i = 0; i < priv->num_cached_textures; ++i) {
        if (priv->cached_textures[i].memblock)
            dkMemBlockDestroy(priv->cached_textures[i].memblock);
    }
}

static int mapper_map(struct ra_hwdec_mapper *mapper) {
    struct priv *priv = mapper->priv;
    AVTX1Map     *map = (AVTX1Map *)mapper->src->planes[3];

    for (int i = 0; i < priv->num_cached_textures; ++i) {
        if (priv->cached_textures[i].handle == ff_tx1_map_get_handle(map)) {
            mapper->tex[0]->priv = priv->cached_textures[i].tex[0];
            mapper->tex[1]->priv = priv->cached_textures[i].tex[1];

            // Invalidate texture cache
            dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_None, DkInvalidateFlags_Image);
            dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));

            return 0;
        }
    }

    struct cached_texture cache;
    cache.handle = ff_tx1_map_get_handle(map);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, ff_tx1_map_get_size(map));
    memblock_maker.flags   = DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    memblock_maker.storage = ff_tx1_map_get_addr(map);
    cache.memblock = dkMemBlockCreate(&memblock_maker);
    if (!cache.memblock)
        return -1;

    for (int i = 0; i < MP_ARRAY_SIZE(priv->dklayouts); ++i) {
        DkImage image;
        dkImageInitialize(&image, &priv->dklayouts[i], cache.memblock,
            (uintptr_t)(mapper->src->planes[i] - mapper->src->planes[0]));

        struct ra_tex_dk *tex_priv = mapper->tex[i]->priv = talloc_zero(mapper->tex[i], struct ra_tex_dk);
        if (!tex_priv) {
            dkMemBlockDestroy(cache.memblock);
            return -1;
        }

        tex_priv->image    = image;
        tex_priv->memblock = cache.memblock;

        ra_dk_register_texture(mapper->ra, mapper->tex[i]);
        cache.tex[i] = mapper->tex[i]->priv;
    }

    MP_TARRAY_APPEND(mapper, priv->cached_textures, priv->num_cached_textures, cache);

    return 0;
}

static void mapper_unmap(struct ra_hwdec_mapper *mapper) {
    // Do nothing
}

const struct ra_hwdec_driver ra_hwdec_tx1 = {
    .name          = "tx1",
    .priv_size     = sizeof(struct priv_owner),
    .imgfmts       = {IMGFMT_TX1, 0},
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
