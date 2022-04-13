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

#include <libavutil/intreadwrite.h>
#include <switch.h>
#include <libuam/libuam.h>

#include "common/msg.h"

#include "ra_dk.h"

#define CMD_MEM_SIZE MP_ALIGN_UP(0x10000, DK_MEMBLOCK_ALIGNMENT)

// See deko3d image_formats.inc
const struct dk_format formats[] = {
    { "r8",       1,  1, { 8},             DkImageFormat_R8_Unorm,      RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "rg8",      2,  2, { 8,  8},         DkImageFormat_RG8_Unorm,     RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "rgba8",    4,  4, { 8,  8,  8,  8}, DkImageFormat_RGBA8_Unorm,   RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "r16",      1,  2, {16},             DkImageFormat_R16_Unorm,     RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "rg16",     2,  4, {16, 16},         DkImageFormat_RG16_Unorm,    RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "rgba16",   4,  8, {16, 16, 16, 16}, DkImageFormat_RGBA16_Unorm,  RA_CTYPE_UNORM, true,  true,  true,  true  },

    { "r32ui",    1,  4, {32},             DkImageFormat_R32_Uint,      RA_CTYPE_UINT,  true,  false, true,  true  },
    { "rg32ui",   2,  8, {32, 32},         DkImageFormat_RG32_Uint,     RA_CTYPE_UINT,  true,  false, true,  true  },
    { "rgb32ui",  3, 12, {32, 32, 32},     DkImageFormat_RGB32_Uint,    RA_CTYPE_UINT,  false, false, false, true  },
    { "rgba32ui", 4, 16, {32, 32, 32, 32}, DkImageFormat_RGBA32_Uint,   RA_CTYPE_UINT,  true,  false, true,  true  },

    { "r16f",     1,  2, {16},             DkImageFormat_R16_Float,     RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "rg16f",    2,  4, {16, 16},         DkImageFormat_RG16_Float,    RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "rgba16f",  4,  8, {16, 16, 16, 16}, DkImageFormat_RGBA16_Float,  RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "r32f",     1,  4, {32},             DkImageFormat_R32_Float,     RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "rg32f",    2,  8, {32, 32},         DkImageFormat_RG32_Float,    RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "rgb32f",   3, 12, {32, 32, 32},     DkImageFormat_RGB32_Float,   RA_CTYPE_FLOAT, false, false, false, true  },
    { "rgba32f",  4, 16, {32, 32, 32, 32}, DkImageFormat_RGBA32_Float,  RA_CTYPE_FLOAT, true,  true,  true,  true  },

    { "rgb10_a2", 4,  4, {10, 10, 10,  2}, DkImageFormat_RGB10A2_Unorm, RA_CTYPE_UNORM, true,  true,  true,  true  },
    { "bgra8",    4,  4, { 8,  8,  8,  8}, DkImageFormat_BGRA8_Unorm,   RA_CTYPE_UNORM, true,  true,  true,  false },
    { "bgrx8",    3,  4, { 8,  8,  8},     DkImageFormat_BGRX8_Unorm,   RA_CTYPE_UNORM, true,  true,  false, false },
};

struct priv {
    mp_dk_ctx *dk;

    DkMemBlock image_descriptors;
    DkMemBlock sampler_descriptors;
    size_t num_descriptors;
};

static const char dk_shadercache_magic[] = "DKCH";
static const int dk_shadercache_version = 1;

struct dk_shadercache_hdr {
    uint32_t magic;
    int cache_version;
    uint32_t vertex_offset, vertex_size;
    uint32_t fragment_offset, fragment_size;
    uint32_t compute_offset, compute_size;
};

static void dk_destroy(struct ra *ra);
static void dk_tex_destroy(struct ra *ra, struct ra_tex *tex);
static struct ra_tex *dk_tex_create(struct ra *ra, const struct ra_tex_params *params);
static bool dk_tex_upload(struct ra *ra, const struct ra_tex_upload_params *params);
static bool dk_tex_download(struct ra *ra, struct ra_tex_download_params *params);
static void dk_buf_destroy(struct ra *ra, struct ra_buf *buf);
static struct ra_buf *dk_buf_create(struct ra *ra, const struct ra_buf_params *params);
static void dk_buf_update(struct ra *ra, struct ra_buf *buf, ptrdiff_t offset,
                          const void *data, size_t size);
static bool dk_buf_poll(struct ra *ra, struct ra_buf *buf);
static void dk_clear(struct ra *ra, struct ra_tex *dst, float color[4], struct mp_rect *scissor);
static void dk_blit(struct ra *ra, struct ra_tex *dst, struct ra_tex *src,
                    struct mp_rect *dst_rc, struct mp_rect *src_rc);
static int dk_desc_namespace(struct ra *ra, enum ra_vartype type);
static void dk_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass);
static struct ra_renderpass *dk_renderpass_create(struct ra *ra,
                                                  const struct ra_renderpass_params *params);
static void dk_renderpass_run(struct ra *ra, const struct ra_renderpass_run_params *params);
static void dk_debug_marker(struct ra *ra, const char *msg);

static struct ra_fns ra_fns_dk = {
    .destroy            = dk_destroy,
    .tex_create         = dk_tex_create,
    .tex_destroy        = dk_tex_destroy,
    .tex_upload         = dk_tex_upload,
    .tex_download       = dk_tex_download,
    .buf_create         = dk_buf_create,
    .buf_destroy        = dk_buf_destroy,
    .buf_update         = dk_buf_update,
    .buf_poll           = dk_buf_poll,
    .clear              = dk_clear,
    .blit               = dk_blit,
    .uniform_layout     = std140_layout,
    .desc_namespace     = dk_desc_namespace,
    .renderpass_create  = dk_renderpass_create,
    .renderpass_destroy = dk_renderpass_destroy,
    .renderpass_run     = dk_renderpass_run,
    // deko3d does not provide timestamp information
    // .timer_create       = dk_timer_create,
    // .timer_destroy      = dk_timer_destroy,
    // .timer_start        = dk_timer_start,
    // .timer_stop         = dk_timer_stop,
    .debug_marker       = dk_debug_marker,
};

static inline DkBlendFactor map_blend_factor(enum ra_blend factor) {
    switch (factor) {
        case RA_BLEND_ZERO:
            return DkBlendFactor_Zero;
        case RA_BLEND_ONE:
            return DkBlendFactor_One;
        case RA_BLEND_SRC_ALPHA:
            return DkBlendFactor_SrcAlpha;
        case RA_BLEND_ONE_MINUS_SRC_ALPHA:
            return DkBlendFactor_InvSrcAlpha;
        default:
            return -1;
    }
}

static DkVtxAttribType map_vertex_attrib_type(enum ra_vartype type) {
    switch (type) {
        case RA_VARTYPE_INT:
            return DkVtxAttribType_Sint;
        case RA_VARTYPE_FLOAT:
            return DkVtxAttribType_Float;
        case RA_VARTYPE_BYTE_UNORM:
            return DkVtxAttribType_Unorm;
        default:
            return -1;
    }
}

static DkVtxAttribSize map_vertex_attrib_size(enum ra_vartype type, int dim_v, int dim_m) {
    // Matrix types not supported
    switch (type) {
        case RA_VARTYPE_INT:
        case RA_VARTYPE_FLOAT:
            switch (dim_v) {
                case 1:
                    return DkVtxAttribSize_1x32;
                case 2:
                    return DkVtxAttribSize_2x32;
                case 3:
                    return DkVtxAttribSize_3x32;
                case 4:
                    return DkVtxAttribSize_4x32;
            }
            break;
        case RA_VARTYPE_BYTE_UNORM:
            switch (dim_v) {
                case 1:
                    return DkVtxAttribSize_1x8;
                case 2:
                    return DkVtxAttribSize_2x8;
                case 3:
                    return DkVtxAttribSize_3x8;
                case 4:
                    return DkVtxAttribSize_4x8;
            }
            break;
    }
    return -1;
}

mp_dk_ctx *ra_dk_get_ctx(struct ra *ra) {
    struct priv *priv = ra->priv;
    return priv->dk;
}

static int ra_init_dk(struct ra *ra, mp_dk_ctx *dk) {
    struct priv *priv = ra->priv = talloc_zero(NULL, struct priv);
    priv->dk = dk;

    ra->fns = &ra_fns_dk;
    ra->glsl_version = 460;
    ra->glsl_deko3d = true;

    ra->caps = RA_CAP_TEX_1D        |
               RA_CAP_TEX_3D        |
               RA_CAP_BLIT          |
               RA_CAP_COMPUTE       |
               RA_CAP_DIRECT_UPLOAD |
               RA_CAP_BUF_RO        |
               RA_CAP_BUF_RW        |
               RA_CAP_NESTED_ARRAY  |
               RA_CAP_GATHER        |
               RA_CAP_FRAGCOORD     |
               RA_CAP_NUM_GROUPS;

    // Values reported by the opengl driver
    ra->max_texture_wh            = 16384;
    ra->max_shmem                 = 98304;
    ra->max_compute_group_threads = 1024;

    for (int i = 0; i < MP_ARRAY_SIZE(formats); ++i) {
        const struct dk_format *dkfmt = &formats[i];

        struct ra_format *fmt = talloc_zero(ra, struct ra_format);
        *fmt = (struct ra_format){
            .name           = dkfmt->name,
            .priv           = (void *)dkfmt,
            .ctype          = dkfmt->ctype,
            .ordered        = dkfmt->ordered,
            .num_components = dkfmt->components,
            .pixel_size     = dkfmt->bytes,
            .linear_filter  = dkfmt->linear_filter,
            .renderable     = dkfmt->renderable,
            .storable       = dkfmt->storable,
        };

        for (int j = 0; j < dkfmt->components; j++)
            fmt->component_size[j] = fmt->component_depth[j] = dkfmt->bits[j];

        fmt->glsl_format = ra_fmt_glsl_format(fmt);

        MP_TARRAY_APPEND(ra, ra->formats, ra->num_formats, fmt);
    }

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, CMD_MEM_SIZE);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    priv->dk->cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);
    if (!priv->dk->cmdbuf_memblock)
        return -1;

    // Should be enough to hold all descriptors (max. 128)
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, DK_MEMBLOCK_ALIGNMENT);
    priv->image_descriptors = dkMemBlockCreate(&memblock_maker);
    if (!priv->image_descriptors)
        return -1;

    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, DK_MEMBLOCK_ALIGNMENT);
    priv->sampler_descriptors = dkMemBlockCreate(&memblock_maker);
    if (!priv->sampler_descriptors)
        return -1;

    priv->num_descriptors = 0;

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, priv->dk->device);
    priv->dk->cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    if (!priv->dk->cmdbuf)
        return -1;
    dkCmdBufAddMemory(priv->dk->cmdbuf, priv->dk->cmdbuf_memblock, 0, CMD_MEM_SIZE);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, priv->dk->device);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_DisableZcull;
    priv->dk->queue = dkQueueCreate(&queueMaker);
    if (!priv->dk->queue)
        return -1;

    priv->dk->can_clear_cmdbuf = true;

    return 0;
}

static void dk_destroy(struct ra *ra) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    if (priv->dk->queue) {
        dkQueueWaitIdle(priv->dk->queue);
        dkQueueDestroy(priv->dk->queue);
    }

    if (priv->dk->cmdbuf)
        dkCmdBufDestroy(priv->dk->cmdbuf);
    if (priv->dk->cmdbuf_memblock)
        dkMemBlockDestroy(priv->dk->cmdbuf_memblock);

    if (priv->image_descriptors)
        dkMemBlockDestroy(priv->image_descriptors);
    if (priv->sampler_descriptors)
        dkMemBlockDestroy(priv->sampler_descriptors);

    for (int i = 0; i < priv->dk->num_tmp_memblocks; ++i) {
        if (priv->dk->tmp_memblocks[i].blk)
            dkMemBlockDestroy(priv->dk->tmp_memblocks[i].blk);
    }

    talloc_free(priv);
}

struct ra *ra_create_dk(mp_dk_ctx *dk, struct mp_log *log) {
    struct ra *ra = talloc_zero(NULL, struct ra);
    ra->log = log;
    if (ra_init_dk(ra, dk) < 0) {
        dk_destroy(ra);
        talloc_free(ra);
        return NULL;
    }
    return ra;
}

void ra_dk_register_texture(struct ra *ra, struct ra_tex *tex) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = tex->priv;

    tex_priv->descriptor_idx = priv->num_descriptors++;

    DkImageDescriptor   *image_descs   = dkMemBlockGetCpuAddr(priv->image_descriptors);
    DkSamplerDescriptor *sampler_descs = dkMemBlockGetCpuAddr(priv->sampler_descriptors);

    DkImageView image_view;
    dkImageViewDefaults(&image_view, &tex_priv->image);

    dkImageDescriptorInitialize(&image_descs[tex_priv->descriptor_idx],
        &image_view, tex->params.storage_dst, false);

    DkSampler sampler;
    dkSamplerDefaults(&sampler);

    sampler.compareEnable = false;
    sampler.compareOp = DkCompareOp_Never;
    sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] =
        tex->params.src_repeat ? DkWrapMode_Repeat  : DkWrapMode_ClampToEdge;
    sampler.minFilter = sampler.magFilter =
        tex->params.src_linear ? DkFilter_Linear    : DkFilter_Nearest;
    sampler.mipFilter =
        tex->params.src_linear ? DkMipFilter_Linear : DkMipFilter_Nearest;

    dkSamplerDescriptorInitialize(&sampler_descs[tex_priv->descriptor_idx], &sampler);

    dkQueueSignalFence(priv->dk->queue, &tex_priv->fence, false);

    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_None, DkInvalidateFlags_Descriptors);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
}

static void dk_tex_destroy(struct ra *ra, struct ra_tex *tex) {
    struct ra_tex_dk *tex_priv = tex->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    if (tex_priv->memblock)
        dkMemBlockDestroy(tex_priv->memblock);

    talloc_free(tex);
}

static struct ra_tex *dk_tex_create(struct ra *ra, const struct ra_tex_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s (%s %dx%dx%d)\n", __func__,
        params->format->name, params->w, params->h, params->d);

    struct ra_tex *tex = talloc_zero(NULL, struct ra_tex);
    if (!tex) {
        dk_tex_destroy(ra, tex);
        return NULL;
    }
    tex->params = *params;
    tex->params.initial_data = NULL;

    struct ra_tex_dk *tex_priv = tex->priv = talloc_zero(tex, struct ra_tex_dk);
    if (!tex_priv) {
        dk_tex_destroy(ra, tex);
        return NULL;
    }

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, priv->dk->device);

    layout_maker.format        = ((struct dk_format *)params->format->priv)->fmt;
    layout_maker.dimensions[0] = params->w;
    layout_maker.dimensions[1] = params->h;
    layout_maker.dimensions[2] = params->d;
    layout_maker.flags         = DkImageFlags_Usage2DEngine | // Always allow 2D engine usage for screenshots (see dk_tex_download)
        ((params->render_src || params->render_dst) ? DkImageFlags_UsageRender    : 0) |
        ( params->storage_dst                       ? DkImageFlags_UsageLoadStore : 0) |
        ((params->blit_src   || params->blit_dst)   ? DkImageFlags_Usage2DEngine  : 0);

    switch (params->dimensions) {
        case 1:
            layout_maker.type = DkImageType_1D;
            break;
        case 2:
            layout_maker.type = DkImageType_2D;
            break;
        case 3:
            layout_maker.type = DkImageType_3D;
            break;
        default:
            dk_tex_destroy(ra, tex);
            return NULL;
    }

    DkImageLayout tex_layout;
    dkImageLayoutInitialize(&tex_layout, &layout_maker);

    uint32_t tex_size  = dkImageLayoutGetSize(&tex_layout);
    uint32_t tex_align = dkImageLayoutGetAlignment(&tex_layout);
    tex_size = MP_ALIGN_UP(tex_size, tex_align);

    // This is supposed to be a rare operation so allocating a memblock for each texture is probably fine
    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(tex_size, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags = DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    tex_priv->memblock = dkMemBlockCreate(&memblock_maker);
    if (!tex_priv->memblock) {
        dk_tex_destroy(ra, tex);
        return NULL;
    }

    dkImageInitialize(&tex_priv->image, &tex_layout, tex_priv->memblock, 0);

    ra_dk_register_texture(ra, tex);

    return tex;
}

static bool dk_tex_upload(struct ra *ra, const struct ra_tex_upload_params *params) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = params->tex->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkImageRect tex_rect;
    if (params->rc) {
        tex_rect = (DkImageRect){
            params->rc->x0, params->rc->y0, 0,
            mp_rect_w(*params->rc), mp_rect_h(*params->rc), 1,
        };
    } else {
        tex_rect = (DkImageRect){
            0, 0, 0,
            params->tex->params.w,
            params->tex->params.h,
            params->tex->params.d,
        };
    }

    DkCopyBuf tex_copy;
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        tex_copy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(buf_priv->memblock) + params->buf_offset,
            params->stride, tex_rect.height * tex_rect.depth,
        };
    } else {
        // Map the provided buffer into the GPU address space and mark it to be removed from it later
        size_t memblk_off  = (uintptr_t)params->src & (DK_MEMBLOCK_ALIGNMENT - 1);
        size_t memblk_size = params->stride * tex_rect.height * tex_rect.depth + memblk_off;

        DkMemBlockMaker memblock_maker;
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(memblk_size, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        memblock_maker.storage = (uint8_t *)params->src - memblk_off;

        DkMemBlock memblock = dkMemBlockCreate(&memblock_maker);
        if (!memblock)
            return false;

        MP_TARRAY_APPEND(ra, priv->dk->tmp_memblocks, priv->dk->num_tmp_memblocks,
            (struct ra_dk_tmp_memblock){memblock});

        tex_copy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(memblock) + memblk_off,
            params->stride, tex_rect.height * tex_rect.depth,
        };
    }

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_priv->fence);
    dkCmdBufCopyBufferToImage(priv->dk->cmdbuf, &tex_copy, &tex_view, &tex_rect, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        dkCmdBufSignalFence(priv->dk->cmdbuf, &buf_priv->fence, false);
    } else {
        struct ra_dk_tmp_memblock *tmp =
            &priv->dk->tmp_memblocks[priv->dk->num_tmp_memblocks - 1];
        dkCmdBufSignalFence(priv->dk->cmdbuf, &tmp->fence, false);
    }
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);

    return true;
}

static bool dk_tex_download(struct ra *ra, struct ra_tex_download_params *params) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = params->tex->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkImageRect tex_rect = (DkImageRect){
        0, 0, 0,
        params->tex->params.w,
        params->tex->params.h,
        1,
    };

    // Map the provided buffer into the GPU address space and mark it to be removed from it later
    // The buffer might not be aligned correctly so map the range containing it,
    // and it an offset to the copy command
    size_t memblk_off  = (uintptr_t)params->dst & (DK_MEMBLOCK_ALIGNMENT - 1);
    size_t memblk_size = memblk_off + params->stride * params->tex->params.h;

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(memblk_size, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags   = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    memblock_maker.storage = (uint8_t *)params->dst - memblk_off;

    DkMemBlock memblock = dkMemBlockCreate(&memblock_maker);
    if (!memblock)
        return false;

    // TODO: Use dkCmdBufCopyImageToBuffer, not implemented yet
    // In the meantime a pitch linear texture is created over the provided memory
    // and used to copy the source data to

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, priv->dk->device);
    layout_maker.type          = DkImageType_2D;
    layout_maker.format        = ((struct dk_format *)params->tex->params.format->priv)->fmt;
    layout_maker.dimensions[0] = params->tex->params.w;
    layout_maker.dimensions[1] = params->tex->params.h;
    layout_maker.flags         = DkImageFlags_Usage2DEngine | DkImageFlags_PitchLinear;
    layout_maker.pitchStride   = params->stride;

    DkImageLayout tex_layout;
    dkImageLayoutInitialize(&tex_layout, &layout_maker);

    DkImage image;
    dkImageInitialize(&image, &tex_layout, memblock, memblk_off);

    DkImageView dst_view;
    dkImageViewDefaults(&dst_view, &image);

    DkFence fence;

    priv->dk->can_clear_cmdbuf = false;

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_priv->fence);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    dkCmdBufBlitImage(priv->dk->cmdbuf, &tex_view, &tex_rect, &dst_view, &tex_rect,
        DkBlitFlag_ModeBlit, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &fence, true); // Flush GPU cache
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);

    // Wait here to avoid the caller functions reading while the command is being processed
    // This is an uncommon operation anyways (only used for screenshots)
    // Wait 10ms at most
    bool ret;
    if (dkFenceWait(&fence, -1) != DkResult_Success) {
        ret = false;
        goto done;
    }

    priv->dk->can_clear_cmdbuf = true;
    ret = true;

done:
    dkMemBlockDestroy(memblock);
    return ret;
}

static void dk_buf_destroy(struct ra *ra, struct ra_buf *buf) {
    struct ra_buf_dk *buf_priv = buf->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    if (buf_priv->memblock)
        dkMemBlockDestroy(buf_priv->memblock);

    talloc_free(buf);
}

static struct ra_buf *dk_buf_create(struct ra *ra, const struct ra_buf_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s, buffer type %d\n", __func__, params->type);

    struct ra_buf *buf = talloc_zero(NULL, struct ra_buf);
    if (!buf) {
        dk_buf_destroy(ra, buf);
        return NULL;
    }
    buf->params = *params;
    buf->params.initial_data = NULL;

    struct ra_buf_dk *buf_priv = buf->priv = talloc_zero(buf, struct ra_buf_dk);
    if (!buf_priv) {
        dk_buf_destroy(ra, buf);
        return NULL;
    }

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(params->size, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;

    buf_priv->memblock = dkMemBlockCreate(&memblock_maker);
    if (!buf_priv->memblock) {
        dk_buf_destroy(ra, buf);
        return NULL;
    }

    if (params->host_mapped)
        buf->data = dkMemBlockGetCpuAddr(buf_priv->memblock);

    if (params->initial_data)
        memcpy(dkMemBlockGetCpuAddr(buf_priv->memblock), params->initial_data, params->size);

    dkQueueSignalFence(priv->dk->queue, &buf_priv->fence, false);

    return buf;
}

static void dk_buf_update(struct ra *ra, struct ra_buf *buf, ptrdiff_t offset,
                          const void *data, size_t size) {
    struct ra_buf_dk *buf_priv = buf->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    memcpy((uint8_t *)dkMemBlockGetCpuAddr(buf_priv->memblock) + offset, data, size);

    // Not needed since buffers are currently all cpu-uncached
    // dkMemBlockFlushCpuCache(buf_priv->memblock, offset, size);
}

static bool dk_buf_poll(struct ra *ra, struct ra_buf *buf) {
    struct ra_buf_dk *buf_priv = buf->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    return dkFenceWait(&buf_priv->fence, 0) == DkResult_Success;
}

static void dk_clear(struct ra *ra, struct ra_tex *dst, float color[4], struct mp_rect *scissor) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = dst->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkScissor dkscissor = {
        scissor->x0, scissor->y0,
        mp_rect_w(*scissor), mp_rect_h(*scissor),
    };

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_priv->fence);
    dkCmdBufBindRenderTarget(priv->dk->cmdbuf, &tex_view, NULL);
    dkCmdBufSetScissors(priv->dk->cmdbuf, 0, &dkscissor, 1);
    dkCmdBufClearColorFloat(priv->dk->cmdbuf, 0, DkColorMask_RGBA, color[0], color[1], color[2], color[3]);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    // dkQueueFlush(priv->dk->queue);
}

static void dk_blit(struct ra *ra, struct ra_tex *dst, struct ra_tex *src,
                    struct mp_rect *dst_rc, struct mp_rect *src_rc) {
    struct priv              *priv = ra->priv;
    struct ra_tex_dk *tex_src_priv = src->priv;
    struct ra_tex_dk *tex_dst_priv = dst->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkImageView src_view, dst_view;
    dkImageViewDefaults(&src_view, &tex_src_priv->image);
    dkImageViewDefaults(&dst_view, &tex_dst_priv->image);

    DkImageRect src_rect = (DkImageRect){
        src_rc->x0, src_rc->y0, 0,
        mp_rect_w(*src_rc),
        mp_rect_h(*src_rc),
        1
    };

    DkImageRect dst_rect = (DkImageRect){
        dst_rc->x0, dst_rc->y0, 0,
        mp_rect_w(*dst_rc),
        mp_rect_h(*dst_rc),
        1
    };


    uint32_t flags = DkBlitFlag_ModeBlit;

    // Handle y-flipping here, since deko3d doesn't flip blits based on coordinates
    if (dst_rc->y0 > dst_rc->y1) {
        flags |= DkBlitFlag_FlipY;
        dst_rect.y = dst_rc->y1;
        dst_rect.height = dst_rc->y0 - dst_rc->y1;
    }

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_src_priv->fence);
    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_dst_priv->fence);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_src_priv->fence, false);
    dkCmdBufBlitImage(priv->dk->cmdbuf, &src_view, &src_rect, &dst_view, &dst_rect,
        flags, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_dst_priv->fence, false);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
}

static int dk_desc_namespace(struct ra *ra, enum ra_vartype type) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return type;
}

static void dk_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass) {
    MP_VERBOSE(ra, "%s\n", __func__);

    struct ra_rpass_dk *pass_priv = pass->priv;

    if (pass_priv->shader_memblock)
        dkMemBlockDestroy(pass_priv->shader_memblock);
    if (pass_priv->vao_memblock)
        dkMemBlockDestroy(pass_priv->vao_memblock);

    talloc_free(pass);
}

static void save_shader_code(struct ra *ra, struct ra_renderpass *pass,
                             bstr vert_dat, bstr frag_dat, bstr comp_dat) {
    struct dk_shadercache_hdr header = {
        .magic         = AV_RN32(dk_shadercache_magic),
        .cache_version = dk_shadercache_version,
    };

    uint32_t offset = sizeof(struct dk_shadercache_hdr);
    if (vert_dat.len)
        header.vertex_offset   = offset,                 header.vertex_size   = vert_dat.len;
    if (frag_dat.len)
        header.fragment_offset = offset += vert_dat.len, header.fragment_size = frag_dat.len;
    if (comp_dat.len)
        header.compute_offset  = offset += frag_dat.len, header.compute_size  = comp_dat.len;

    struct bstr *prog = &pass->params.cached_program;
    bstr_xappend(pass, prog, (bstr){(char *)&header, sizeof(struct dk_shadercache_hdr)});
    bstr_xappend(pass, prog, vert_dat);
    bstr_xappend(pass, prog, frag_dat);
    bstr_xappend(pass, prog, comp_dat);
}

static bool load_shader_code(struct ra *ra, struct ra_renderpass *pass, bstr data,
                             DkShader *vert_sh, DkShader *frag_sh, DkShader *comp_sh) {
    struct priv              *priv = ra->priv;
    struct ra_rpass_dk  *pass_priv = pass->priv;
    struct dk_shadercache_hdr *hdr = (struct dk_shadercache_hdr *)data.start;

    MP_VERBOSE(ra, "Loading from shadercache\n");

    if (!data.start ||
            (data.len < sizeof(struct dk_shadercache_hdr)))
        return false;

    if ((hdr->magic != AV_RN32(dk_shadercache_magic)) ||
            (hdr->cache_version != dk_shadercache_version))
        return false;

    size_t memblock_size = DK_SHADER_CODE_UNUSABLE_SIZE +
        ((vert_sh && hdr->vertex_size)   ? MP_ALIGN_UP(hdr->vertex_size,   DK_SHADER_CODE_ALIGNMENT) : 0) +
        ((frag_sh && hdr->fragment_size) ? MP_ALIGN_UP(hdr->fragment_size, DK_SHADER_CODE_ALIGNMENT) : 0) +
        ((comp_sh && hdr->compute_size)  ? MP_ALIGN_UP(hdr->compute_size,  DK_SHADER_CODE_ALIGNMENT) : 0);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
        MP_ALIGN_UP(memblock_size, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);
    if (!pass_priv->shader_memblock)
        return false;

    size_t offset = 0;
    DkShaderMaker shader_maker;
    if (vert_sh && hdr->vertex_size) {
        memcpy((uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + offset,
            data.start + hdr->vertex_offset, hdr->vertex_size);
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, offset);
        dkShaderInitialize(vert_sh, &shader_maker);
        offset += MP_ALIGN_UP(hdr->vertex_size, DK_SHADER_CODE_ALIGNMENT);
    }
    if (frag_sh && hdr->fragment_size) {
        memcpy((uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + offset,
            data.start + hdr->fragment_offset, hdr->fragment_size);
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, offset);
        dkShaderInitialize(frag_sh, &shader_maker);
        offset += MP_ALIGN_UP(hdr->fragment_size, DK_SHADER_CODE_ALIGNMENT);
    }
    if (comp_sh && hdr->compute_size) {
        memcpy((uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + offset,
            data.start + hdr->compute_offset, hdr->compute_size);
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, offset);
        dkShaderInitialize(comp_sh, &shader_maker);
        offset += MP_ALIGN_UP(hdr->compute_size, DK_SHADER_CODE_ALIGNMENT);
    }

    return true;
}

static struct ra_renderpass *dk_renderpass_create_raster(struct ra *ra, struct ra_renderpass *pass,
                                                         const struct ra_renderpass_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = pass->priv;
    DkMemBlockMaker memblock_maker;

    if (mp_msg_test(ra->log, MSGL_V)) {
        MP_VERBOSE(ra, "Vertex shader source:\n");
        mp_log_source(ra->log, MSGL_V, params->vertex_shader);
        MP_VERBOSE(ra, "Fragment shader source:\n");
        mp_log_source(ra->log, MSGL_V, params->frag_shader);
    }

    pass_priv->shaders = talloc_array(pass, DkShader, 2);
    if (!pass_priv->shaders) {
        dk_renderpass_destroy(ra, pass);
        ta_free(pass);
        return NULL;
    }

    if (!params->cached_program.len ||
            !load_shader_code(ra, pass, params->cached_program, &pass_priv->shaders[0], &pass_priv->shaders[1], NULL)) {
        uam_compiler *vsh_compiler = uam_create_compiler(DkStage_Vertex),
            *fsh_compiler = uam_create_compiler(DkStage_Fragment);
        if (!vsh_compiler || !fsh_compiler) {
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        if (!uam_compile_dksh(vsh_compiler, params->vertex_shader) ||
                !uam_compile_dksh(fsh_compiler, params->frag_shader)) {
            MP_ERR(ra, "Failed to compile shaders\n");
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        size_t vsh_size = uam_get_code_size(vsh_compiler), fsh_size = uam_get_code_size(fsh_compiler);
        size_t vsh_off = 0, fsh_off = MP_ALIGN_UP(vsh_size, DK_SHADER_CODE_ALIGNMENT);

        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
            MP_ALIGN_UP(vsh_size + fsh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);
        if (!pass_priv->shader_memblock) {
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        uam_write_code(vsh_compiler, (uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + vsh_off);
        uam_write_code(fsh_compiler, (uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + fsh_off);

        save_shader_code(ra, pass,
            (bstr){(uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + vsh_off, vsh_size},
            (bstr){(uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + fsh_off, fsh_size},
            (bstr){0});

        DkShaderMaker shader_maker;
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, vsh_off);
        dkShaderInitialize(&pass_priv->shaders[0], &shader_maker);
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, fsh_off);
        dkShaderInitialize(&pass_priv->shaders[1], &shader_maker);

        goto success;

fail:
        if (vsh_compiler)
            uam_free_compiler(vsh_compiler);
        if (fsh_compiler)
            uam_free_compiler(fsh_compiler);
        return NULL;
    }

success:
    pass_priv->vao_attribs = talloc_array(pass, DkVtxAttribState, params->num_vertex_attribs);
    if (!pass_priv->vao_attribs) {
        dk_renderpass_destroy(ra, pass);
        ta_free(pass);
        return NULL;
    }

    for (int i = 0; i < params->num_vertex_attribs; ++i) {
        struct ra_renderpass_input *inp = &params->vertex_attribs[i];

        pass_priv->vao_attribs[i] = (DkVtxAttribState){
            .offset = inp->offset,
            .type   = map_vertex_attrib_type(inp->type),
            .size   = map_vertex_attrib_size(inp->type, inp->dim_v, inp->dim_m),
        };
    }
    pass_priv->vao_state = (DkVtxBufferState){
        .stride = params->vertex_stride,
    };

    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
        MP_ALIGN_UP(6 * params->vertex_stride, DK_MEMBLOCK_ALIGNMENT)); // 6 vertices to draw a rectangle
	memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    pass_priv->vao_memblock = dkMemBlockCreate(&memblock_maker);
    if (!pass_priv->vao_memblock) {
        dk_renderpass_destroy(ra, pass);
        ta_free(pass);
        return NULL;
    }

    dkRasterizerStateDefaults(&pass_priv->rasterizer_state);
    dkColorStateDefaults(&pass_priv->color_state);
    dkColorWriteStateDefaults(&pass_priv->color_write_state);

    pass_priv->rasterizer_state.cullMode = DkFace_None;

    if (params->enable_blend) {
        dkColorStateSetBlendEnable(&pass_priv->color_state, 0, true);
        dkBlendStateSetOps(&pass_priv->blend_state, DkBlendOp_Add, DkBlendOp_Add);
        dkBlendStateSetFactors(&pass_priv->blend_state,
            map_blend_factor(params->blend_src_rgb),   map_blend_factor(params->blend_dst_rgb),
            map_blend_factor(params->blend_src_alpha), map_blend_factor(params->blend_dst_alpha));
    }

    return pass;
}

static struct ra_renderpass *dk_renderpass_create_compute(struct ra *ra, struct ra_renderpass *pass,
                                                         const struct ra_renderpass_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = pass->priv;
    DkMemBlockMaker memblock_maker;

    if (mp_msg_test(ra->log, MSGL_V)) {
        MP_VERBOSE(ra, "Compute shader source:\n");
        mp_log_source(ra->log, MSGL_V, params->compute_shader);
    }

    pass_priv->shaders = talloc_array(pass, DkShader, 1);
    if (!pass_priv->shaders) {
        dk_renderpass_destroy(ra, pass);
        ta_free(pass);
        return NULL;
    }

    if (!params->cached_program.len ||
            !load_shader_code(ra, pass, params->cached_program, NULL, NULL, &pass_priv->shaders[0])) {
        uam_compiler *sh_compiler = uam_create_compiler(DkStage_Compute);
        if (!sh_compiler) {
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        if (!uam_compile_dksh(sh_compiler, params->compute_shader)) {
            MP_ERR(ra, "Failed to compile shader\n");
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        size_t sh_size = uam_get_code_size(sh_compiler);
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
            MP_ALIGN_UP(sh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);
        if (!pass_priv->shader_memblock) {
            dk_renderpass_destroy(ra, pass);
            ta_free(pass);
            goto fail;
        }

        uam_write_code(sh_compiler, dkMemBlockGetCpuAddr(pass_priv->shader_memblock));
        save_shader_code(ra, pass, (bstr){0}, (bstr){0},
            (bstr){dkMemBlockGetCpuAddr(pass_priv->shader_memblock), sh_size});

        DkShaderMaker shader_maker;
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, 0);
        dkShaderInitialize(&pass_priv->shaders[0], &shader_maker);

        goto success;

fail:
        if (sh_compiler)
            uam_free_compiler(sh_compiler);
        return NULL;
    }

success:
    return pass;
}

static struct ra_renderpass *dk_renderpass_create(struct ra *ra,
                                                  const struct ra_renderpass_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);

    struct ra_renderpass *pass = talloc_zero(NULL, struct ra_renderpass);
    if (!pass)
        return NULL;

    pass->params = *ra_renderpass_params_copy(pass, params);
    pass->params.cached_program = (bstr){0};
    pass->priv = talloc_zero(pass, struct ra_rpass_dk);
    if (!pass->priv) {
        talloc_free(pass);
        return NULL;
    }

    if (params->type == RA_RENDERPASS_TYPE_RASTER)
        return dk_renderpass_create_raster(ra, pass, params);
    else
        return dk_renderpass_create_compute(ra, pass, params);
}

static void dk_renderpass_run_raster(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv                        *priv = ra->priv;
    struct ra_rpass_dk            *pass_priv = params->pass->priv;
    struct ra_renderpass_params *pass_params = &params->pass->params;
    struct ra_tex_dk               *tex_priv = params->target->priv;

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    // Reallocate vao if the data doesn't fit
    if (!pass_priv->vao_memblock || (params->vertex_count * pass_params->vertex_stride >
            dkMemBlockGetSize(pass_priv->vao_memblock))) {
        if (pass_priv->vao_memblock)
            dkMemBlockDestroy(pass_priv->vao_memblock);

        DkMemBlockMaker memblock_maker;
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
            MP_ALIGN_UP(params->vertex_count * pass_params->vertex_stride, DK_MEMBLOCK_ALIGNMENT));
	    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        pass_priv->vao_memblock = dkMemBlockCreate(&memblock_maker);
        if (!pass_priv->vao_memblock)
            return;
    }

    // No CPU cache flush needed
    memcpy(dkMemBlockGetCpuAddr(pass_priv->vao_memblock), params->vertex_data,
        params->vertex_count * pass_params->vertex_stride);

    DkViewport dkviewport = (DkViewport){
        params->viewport.x0, params->viewport.y0,
        mp_rect_w(params->viewport), mp_rect_h(params->viewport),
        0.0f, 1.0f,
    };

    DkScissor dkscissor = (DkScissor){
        params->scissors.x0, params->scissors.y0,
        mp_rect_w(params->scissors), mp_rect_h(params->scissors),
    };

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_priv->fence);

    if (params->pass->params.enable_blend)
        dkCmdBufBindBlendState(priv->dk->cmdbuf, 0, &pass_priv->blend_state);

    dkCmdBufBindRenderTarget(priv->dk->cmdbuf, &tex_view, NULL);
    dkCmdBufSetViewports(priv->dk->cmdbuf, 0, &dkviewport, 1);
    dkCmdBufSetScissors(priv->dk->cmdbuf, 0, &dkscissor, 1);
    dkCmdBufBindShaders(priv->dk->cmdbuf, DkStageFlag_GraphicsMask,
        (DkShader const *[]){&pass_priv->shaders[0], &pass_priv->shaders[1]}, 2);
    dkCmdBufBindRasterizerState(priv->dk->cmdbuf, &pass_priv->rasterizer_state);
    dkCmdBufBindColorState(priv->dk->cmdbuf, &pass_priv->color_state);
    dkCmdBufBindColorWriteState(priv->dk->cmdbuf, &pass_priv->color_write_state);
    dkCmdBufBindImageDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->image_descriptors), priv->num_descriptors);
    dkCmdBufBindSamplerDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->sampler_descriptors), priv->num_descriptors);
    dkCmdBufBindVtxBuffer(priv->dk->cmdbuf, 0, dkMemBlockGetGpuAddr(pass_priv->vao_memblock),
        dkMemBlockGetSize(pass_priv->vao_memblock));
    dkCmdBufBindVtxAttribState(priv->dk->cmdbuf, pass_priv->vao_attribs, pass_params->num_vertex_attribs);
    dkCmdBufBindVtxBufferState(priv->dk->cmdbuf, &pass_priv->vao_state, 1);
    dkCmdBufDraw(priv->dk->cmdbuf, DkPrimitive_Triangles, params->vertex_count, 1, 0, 0);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Fragments, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
}

static void dk_renderpass_run_compute(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = params->pass->priv;

    dkCmdBufBindShaders(priv->dk->cmdbuf, DkStageFlag_Compute,
        (DkShader const *[]){&pass_priv->shaders[0]}, 1);
    dkCmdBufBindImageDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->image_descriptors), priv->num_descriptors);
    dkCmdBufBindSamplerDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->sampler_descriptors), priv->num_descriptors);
    dkCmdBufDispatchCompute(priv->dk->cmdbuf, params->compute_groups[0],
        params->compute_groups[1], params->compute_groups[2]);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Primitives, 0);

    for (int i = 0; i < params->num_values; ++i) {
        struct ra_renderpass_input_val *val = &params->values[i];
        struct ra_renderpass_input     *inp = &params->pass->params.inputs[val->index];

        if (inp->type == RA_VARTYPE_IMG_W) {
            struct ra_tex         *inp_tex = *(struct ra_tex **)val->data;
            struct ra_tex_dk *inp_tex_priv = inp_tex->priv;
            dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_tex_priv->fence, false);
        } else if (inp->type == RA_VARTYPE_BUF_RW) {
            struct ra_buf         *inp_buf = *(struct ra_buf **)val->data;
            struct ra_buf_dk *inp_buf_priv = inp_buf->priv;
            dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_buf_priv->fence, false);
        }
    }

    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
}

static void dk_renderpass_run(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkStage stage = (params->pass->params.type == RA_RENDERPASS_TYPE_RASTER) ?
        DkStage_Fragment : DkStage_Compute;

    for (int i = 0; i < params->num_values; ++i) {
        struct ra_renderpass_input_val *val = &params->values[i];
        struct ra_renderpass_input     *inp = &params->pass->params.inputs[val->index];

        switch (inp->type) {
            case RA_VARTYPE_TEX:
            case RA_VARTYPE_IMG_W:
                struct ra_tex         *inp_tex = *(struct ra_tex **)val->data;
                struct ra_tex_dk *inp_tex_priv = inp_tex->priv;
                dkCmdBufWaitFence(priv->dk->cmdbuf, &inp_tex_priv->fence);
                if (inp->type == RA_VARTYPE_TEX) {
                    dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_tex_priv->fence, false);
                    dkCmdBufBindTexture(priv->dk->cmdbuf, stage, inp->binding,
                        dkMakeTextureHandle(inp_tex_priv->descriptor_idx, inp_tex_priv->descriptor_idx));
                } else {
                    dkCmdBufBindImage(priv->dk->cmdbuf, stage, inp->binding,
                        dkMakeImageHandle(inp_tex_priv->descriptor_idx));
                }
                break;
            case RA_VARTYPE_BUF_RO:
            case RA_VARTYPE_BUF_RW:
                struct ra_buf         *inp_buf = *(struct ra_buf **)val->data;
                struct ra_buf_dk *inp_buf_priv = inp_buf->priv;
                dkCmdBufWaitFence(priv->dk->cmdbuf, &inp_buf_priv->fence);
                if (inp->type == RA_VARTYPE_BUF_RO) {
                    dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_buf_priv->fence, false);
                    dkCmdBufBindUniformBuffer(priv->dk->cmdbuf, stage, inp->binding,
                        dkMemBlockGetGpuAddr(inp_buf_priv->memblock), inp_buf->params.size);
                } else {
                    dkCmdBufBindStorageBuffer(priv->dk->cmdbuf, stage, inp->binding,
                        dkMemBlockGetGpuAddr(inp_buf_priv->memblock), inp_buf->params.size);
                }
                break;
            default:
                break;
        }
    }

    if (params->pass->params.type == RA_RENDERPASS_TYPE_RASTER)
        dk_renderpass_run_raster(ra, params);
    else
        dk_renderpass_run_compute(ra, params);
}

static void dk_debug_marker(struct ra *ra, const char *msg) {
    MP_VERBOSE(ra, "%s: %s\n", __func__, msg);
}
