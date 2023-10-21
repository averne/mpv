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

#include <libavutil/intreadwrite.h>
#include <switch.h>
#include <libuam/libuam.h>

#include "common/msg.h"

#include "ra_dk.h"

// See deko3d format_traits.inc
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
    { "rg11b10f", 3,  4, {11, 11, 10},     DkImageFormat_RG11B10_Float, RA_CTYPE_FLOAT, true,  true,  true,  true  },
    { "bgra8",    4,  4, { 8,  8,  8,  8}, DkImageFormat_BGRA8_Unorm,   RA_CTYPE_UNORM, true,  true,  true,  false },
    { "bgrx8",    3,  4, { 8,  8,  8},     DkImageFormat_BGRX8_Unorm,   RA_CTYPE_UNORM, true,  true,  false, false },
};

struct priv {
    mp_dk_ctx *dk;

    DkMemBlock           descriptors_memblock;
    DkSamplerDescriptor *sampler_descriptors;
    DkImageDescriptor   *image_descriptors;
    uint64_t             allocated_descriptors[2];

    DkMemBlock query_memblock;
    size_t     num_queries;
};
_Static_assert(sizeof(((struct priv *)0)->allocated_descriptors) * __CHAR_BIT__ == RA_DK_MAX_DESCRIPTORS);

static const char dk_shadercache_magic[] = "DKCH";
static const int dk_shadercache_version = 1;

struct dk_shadercache_hdr {
    uint32_t magic;
    int cache_version;
    uint32_t vertex_offset, vertex_size;
    uint32_t fragment_offset, fragment_size;
    uint32_t compute_offset, compute_size;
};
_Static_assert(sizeof(struct dk_shadercache_hdr) == 0x20);

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
static ra_timer *dk_timer_create(struct ra *ra);
static void dk_timer_destroy(struct ra *ra, ra_timer *timer);
static void dk_timer_start(struct ra *ra, ra_timer *timer);
static uint64_t dk_timer_stop(struct ra *ra, ra_timer *timer);
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
    .timer_create       = dk_timer_create,
    .timer_destroy      = dk_timer_destroy,
    .timer_start        = dk_timer_start,
    .timer_stop         = dk_timer_stop,
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
    struct priv *priv = ra->priv = talloc_zero(ra, struct priv);
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
               // Causes most postproc shaders to use compute instead of fragment
               // Works fine here but doesn't seem to cause a significant perf gain
               // RA_CAP_PARALLEL_COMPUTE |
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

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, priv->dk->device);
    queue_maker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_DisableZcull;
    priv->dk->queue = dkQueueCreate(&queue_maker);
    if (!priv->dk->queue)
        return -1;

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, RA_DK_CMDBUF_SIZE * RA_DK_NUM_CMDBUFS);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    priv->dk->cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);
    if (!priv->dk->cmdbuf_memblock)
        return -1;

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, priv->dk->device);
    priv->dk->cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    if (!priv->dk->cmdbuf)
        return -1;

    priv->dk->cur_cmdbuf_slice = 0;
    memset(priv->dk->cmdbuf_fences, 0, sizeof(priv->dk->cmdbuf_fences));
    dkCmdBufAddMemory(priv->dk->cmdbuf, priv->dk->cmdbuf_memblock,
        priv->dk->cur_cmdbuf_slice * RA_DK_CMDBUF_SIZE, RA_DK_CMDBUF_SIZE);

    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
        RA_DK_MAX_DESCRIPTORS * (sizeof(DkSamplerDescriptor) + sizeof(DkImageDescriptor)));
    priv->descriptors_memblock = dkMemBlockCreate(&memblock_maker);
    if (!priv->descriptors_memblock)
        return -1;

    priv->sampler_descriptors = dkMemBlockGetCpuAddr(priv->descriptors_memblock);
    priv->image_descriptors   = (DkImageDescriptor *)(priv->sampler_descriptors + RA_DK_MAX_DESCRIPTORS);

    memset(priv->allocated_descriptors, 0, sizeof(priv->allocated_descriptors));

    // 16 bytes per timestamp (ctr + ts), 2 timestamps per query (start + end)
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, RA_DK_MAX_QUERIES * 16 * 2);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuUncached |
        DkMemBlockFlags_ZeroFillInit;
    priv->query_memblock = dkMemBlockCreate(&memblock_maker);
    if (!priv->query_memblock)
        return -1;

    priv->num_queries = 0;

    dkCmdBufBindSamplerDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->descriptors_memblock), RA_DK_MAX_DESCRIPTORS);
    dkCmdBufBindImageDescriptorSet(priv->dk->cmdbuf,
        dkMemBlockGetGpuAddr(priv->descriptors_memblock) + RA_DK_MAX_DESCRIPTORS * sizeof(DkSamplerDescriptor),
        RA_DK_MAX_DESCRIPTORS);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueWaitIdle(priv->dk->queue);

    return 0;
}

static void dk_destroy(struct ra *ra) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    if (priv->dk->queue)
        dkQueueWaitIdle(priv->dk->queue);

    if (priv->descriptors_memblock)
        dkMemBlockDestroy(priv->descriptors_memblock);

    if (priv->query_memblock)
        dkMemBlockDestroy(priv->query_memblock);

    if (priv->dk->cmdbuf)
        dkCmdBufDestroy(priv->dk->cmdbuf);
    if (priv->dk->cmdbuf_memblock)
        dkMemBlockDestroy(priv->dk->cmdbuf_memblock);

    if (priv->dk->queue)
        dkQueueDestroy(priv->dk->queue);
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

void ra_dk_unregister_texture(struct ra *ra, struct ra_tex_dk *tex) {
    struct priv *priv = ra->priv;

    priv->allocated_descriptors[tex->descriptor_idx / 64] &= ~(1ull << (tex->descriptor_idx % 64));
}

void ra_dk_register_texture(struct ra *ra, struct ra_tex *tex) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = tex->priv;

    tex_priv->descriptor_idx = -1;
    for (int i = 0; i < MP_ARRAY_SIZE(priv->allocated_descriptors); ++i) {
        uint64_t *pos = &priv->allocated_descriptors[i];
        if (*pos == -1ull)
            continue;
        tex_priv->descriptor_idx = __builtin_ctzll(~*pos);
        *pos |= (1ull << tex_priv->descriptor_idx);
        break;
    }

    if (tex_priv->descriptor_idx < 0) {
        MP_ERR(ra, "No more free descriptor slots for texture %dx%dx%d %s\n",
            tex->params.w, tex->params.h, tex->params.d, tex->params.format->name);
        return;
    }

    DkImageView image_view;
    dkImageViewDefaults(&image_view, &tex_priv->image);

    dkImageDescriptorInitialize(&priv->image_descriptors[tex_priv->descriptor_idx],
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

    dkSamplerDescriptorInitialize(&priv->sampler_descriptors[tex_priv->descriptor_idx],
        &sampler);

    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_None, DkInvalidateFlags_Descriptors);
}

static void dk_tex_destroy(struct ra *ra, struct ra_tex *tex) {
    struct ra_tex_dk *tex_priv = tex->priv;

    ra_dk_unregister_texture(ra, tex_priv);

    if (tex_priv->memblock)
        dkMemBlockDestroy(tex_priv->memblock);

    talloc_free(tex);
}

static struct ra_tex *dk_tex_create(struct ra *ra, const struct ra_tex_params *params) {
    struct priv *priv = ra->priv;

    MP_TRACE(ra, "%s (%s %dx%dx%d)\n", __func__,
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
    layout_maker.flags         = DkImageFlags_HwCompression | DkImageFlags_Usage2DEngine | // Always allow 2D engine usage for screenshots (see dk_tex_download)
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

    if (params->initial_data) {
        bool ret = dk_tex_upload(ra, &(struct ra_tex_upload_params){
            .tex    = tex,
            .src    = params->initial_data,
            .stride = params->w * params->format->pixel_size,
        });
        if (!ret) {
            dk_tex_destroy(ra, tex);
            return NULL;
        }
    }

    ra_dk_register_texture(ra, tex);

    return tex;
}

static bool dk_tex_upload(struct ra *ra, const struct ra_tex_upload_params *params) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = params->tex->priv;

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
    DkMemBlock memblock = NULL;
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        tex_copy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(buf_priv->memblock) + params->buf_offset,
            params->stride, tex_rect.height * tex_rect.depth,
        };

        if (params->buf->params.host_mapped)
            dkMemBlockFlushCpuCache(buf_priv->memblock, params->buf_offset,
                params->stride * tex_rect.height * tex_rect.depth);
    } else {
        // Map the provided buffer into the GPU address space
        size_t memblk_off  = (uintptr_t)params->src & (DK_MEMBLOCK_ALIGNMENT - 1);
        size_t memblk_size = params->stride * tex_rect.height * tex_rect.depth + memblk_off;

        DkMemBlockMaker memblock_maker;
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(memblk_size, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
        memblock_maker.storage = (uint8_t *)params->src - memblk_off;

        memblock = dkMemBlockCreate(&memblock_maker);
        if (!memblock)
            return false;

        dkMemBlockFlushCpuCache(memblock, memblk_off, params->stride * tex_rect.height * tex_rect.depth);

        tex_copy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(memblock) + memblk_off,
            params->stride, tex_rect.height * tex_rect.depth,
        };
    }

    DkFence fence, *done_fence;
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        done_fence = &buf_priv->fence;
        dkCmdBufWaitFence(priv->dk->cmdbuf, &buf_priv->fence);
    } else {
        done_fence = &fence;
    }

    dkCmdBufCopyBufferToImage(priv->dk->cmdbuf, &tex_copy, &tex_view, &tex_rect, 0);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_None, DkInvalidateFlags_Image);
    dkCmdBufSignalFence(priv->dk->cmdbuf, done_fence, false);

    // Return early, assuming that the buffer will be kept alive until the transfer is complete
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        if (buf_priv->is_cpu_cached)
            return true;
    }

    // Wait for the copy to finish before returning
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
    bool ret = dkFenceWait(done_fence, -1) == DkResult_Success;

    if (!params->buf)
        dkMemBlockDestroy(memblock);

    return ret;
}

static bool dk_tex_download(struct ra *ra, struct ra_tex_download_params *params) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = params->tex->priv;

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkImageRect tex_rect = (DkImageRect){
        0, 0, 0,
        params->tex->params.w,
        params->tex->params.h,
        1,
    };

    // Map the provided buffer into the GPU address space
    // The buffer might not be aligned correctly so map a range containing it,
    // and pass an offset to the copy command
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

    dkCmdBufBlitImage(priv->dk->cmdbuf, &tex_view, &tex_rect, &dst_view, &tex_rect,
        DkBlitFlag_ModeBlit, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &fence, true); // Flush GPU cache

    // Wait for the copy to finish before returning
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
    bool ret = dkFenceWait(&fence, -1) == DkResult_Success;

    dkMemBlockDestroy(memblock);

    return ret;
}

static void dk_buf_destroy(struct ra *ra, struct ra_buf *buf) {
    struct ra_buf_dk *buf_priv = buf->priv;

    if (buf_priv->memblock)
        dkMemBlockDestroy(buf_priv->memblock);

    talloc_free(buf);
}

static struct ra_buf *dk_buf_create(struct ra *ra, const struct ra_buf_params *params) {
    struct priv *priv = ra->priv;

    MP_TRACE(ra, "%s (type %d)\n", __func__, params->type);

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

    buf_priv->is_cpu_cached = params->type == RA_BUF_TYPE_TEX_UPLOAD;

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(params->size, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags =
        ( buf_priv->is_cpu_cached ? DkMemBlockFlags_CpuCached : DkMemBlockFlags_CpuUncached) |
        (!buf_priv->is_cpu_cached ? DkMemBlockFlags_GpuCached : DkMemBlockFlags_GpuUncached);

    buf_priv->memblock = dkMemBlockCreate(&memblock_maker);
    if (!buf_priv->memblock) {
        dk_buf_destroy(ra, buf);
        return NULL;
    }

    if (params->host_mapped)
        buf->data = dkMemBlockGetCpuAddr(buf_priv->memblock);

    if (params->initial_data)
        dk_buf_update(ra, buf, 0, params->initial_data, params->size);

    return buf;
}

static void dk_buf_update(struct ra *ra, struct ra_buf *buf, ptrdiff_t offset,
                          const void *data, size_t size) {
    struct priv          *priv = ra->priv;
    struct ra_buf_dk *buf_priv = buf->priv;

    if (buf->params.type == RA_BUF_TYPE_UNIFORM) {
        dkCmdBufPushConstants(priv->dk->cmdbuf, dkMemBlockGetGpuAddr(buf_priv->memblock),
            dkMemBlockGetSize(buf_priv->memblock), offset, size, data);
    } else {
        // Wait in case this buffer is currently being used
        dkQueueWaitIdle(priv->dk->queue);

        memcpy((uint8_t *)dkMemBlockGetCpuAddr(buf_priv->memblock) + offset, data, size);
        if (buf_priv->is_cpu_cached)
            dkMemBlockFlushCpuCache(buf_priv->memblock, offset, size);
    }
}

static bool dk_buf_poll(struct ra *ra, struct ra_buf *buf) {
    struct ra_buf_dk *buf_priv = buf->priv;

    return dkFenceWait(&buf_priv->fence, 0) == DkResult_Success;
}

static void dk_clear(struct ra *ra, struct ra_tex *dst, float color[4], struct mp_rect *scissor) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = dst->priv;

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkScissor dkscissor = {
        scissor->x0, scissor->y0,
        mp_rect_w(*scissor), mp_rect_h(*scissor),
    };

    dkCmdBufBindRenderTarget(priv->dk->cmdbuf, &tex_view, NULL);
    dkCmdBufSetScissors(priv->dk->cmdbuf, 0, &dkscissor, 1);

    switch (dst->params.format->ctype) {
        case RA_CTYPE_UNORM:
        case RA_CTYPE_FLOAT:
            dkCmdBufClearColorFloat(priv->dk->cmdbuf, 0, DkColorMask_RGBA,
                color[0], color[1], color[2], color[3]);
            break;
        case RA_CTYPE_UINT:
            dkCmdBufClearColorUint(priv->dk->cmdbuf, 0, DkColorMask_RGBA,
                color[0], color[1], color[2], color[3]);
            break;
        default:
            dkCmdBufClearColor(priv->dk->cmdbuf, 0, DkColorMask_RGBA, (void *)color);
            break;
    }
}

static void dk_blit(struct ra *ra, struct ra_tex *dst, struct ra_tex *src,
                    struct mp_rect *dst_rc, struct mp_rect *src_rc) {
    struct priv              *priv = ra->priv;
    struct ra_tex_dk *tex_src_priv = src->priv;
    struct ra_tex_dk *tex_dst_priv = dst->priv;

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

    dkCmdBufBlitImage(priv->dk->cmdbuf, &src_view, &src_rect, &dst_view, &dst_rect,
        flags, 0);
}

static int dk_desc_namespace(struct ra *ra, enum ra_vartype type) {
    return type;
}

static void dk_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass) {
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

    MP_DBG(ra, "Loading from shadercache\n");

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

    if (mp_msg_test(ra->log, MSGL_DEBUG)) {
        MP_DBG(ra, "Vertex shader source:\n");
        mp_log_source(ra->log, MSGL_DEBUG, params->vertex_shader);
        MP_DBG(ra, "Fragment shader source:\n");
        mp_log_source(ra->log, MSGL_DEBUG, params->frag_shader);
    }

    pass_priv->shaders = talloc_array(pass, DkShader, 2);
    if (!pass_priv->shaders)
        goto fail_1;

    if (!params->cached_program.len ||
            !load_shader_code(ra, pass, params->cached_program, &pass_priv->shaders[0], &pass_priv->shaders[1], NULL)) {
        bool error = false;

        uam_compiler *vsh_compiler = uam_create_compiler(DkStage_Vertex),
            *fsh_compiler = uam_create_compiler(DkStage_Fragment);
        if (!vsh_compiler || !fsh_compiler) {
            error = true; goto fail_2;
        }

        if (!uam_compile_dksh(vsh_compiler, params->vertex_shader) ||
                !uam_compile_dksh(fsh_compiler, params->frag_shader)) {
            MP_ERR(ra, "Failed to compile shaders\n");
            error = true; goto fail_2;
        }

        size_t vsh_size = uam_get_code_size(vsh_compiler), fsh_size = uam_get_code_size(fsh_compiler);
        size_t vsh_off = 0, fsh_off = MP_ALIGN_UP(vsh_size, DK_SHADER_CODE_ALIGNMENT);

        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
            MP_ALIGN_UP(vsh_size + fsh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);
        if (!pass_priv->shader_memblock) {
            error = true; goto fail_2;
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

fail_2:
        if (vsh_compiler)
            uam_free_compiler(vsh_compiler);
        if (fsh_compiler)
            uam_free_compiler(fsh_compiler);

        if (error)
            goto fail_1;
    }

    pass_priv->vao_attribs = talloc_array(pass, DkVtxAttribState, params->num_vertex_attribs);
    if (!pass_priv->vao_attribs)
        goto fail_1;

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
    if (!pass_priv->vao_memblock)
        goto fail_1;

    dkRasterizerStateDefaults(&pass_priv->rasterizer_state);
    dkColorStateDefaults(&pass_priv->color_state);
    dkColorWriteStateDefaults(&pass_priv->color_write_state);
    dkDepthStencilStateDefaults(&pass_priv->depth_state);

    pass_priv->rasterizer_state.cullMode = DkFace_None;

    if (params->enable_blend) {
        dkColorStateSetBlendEnable(&pass_priv->color_state, 0, true);
        dkBlendStateSetOps(&pass_priv->blend_state, DkBlendOp_Add, DkBlendOp_Add);
        dkBlendStateSetFactors(&pass_priv->blend_state,
            map_blend_factor(params->blend_src_rgb),   map_blend_factor(params->blend_dst_rgb),
            map_blend_factor(params->blend_src_alpha), map_blend_factor(params->blend_dst_alpha));
    }

	pass_priv->depth_state.depthTestEnable = pass_priv->depth_state.depthWriteEnable =
	    pass_priv->depth_state.stencilTestEnable = false;

    return pass;

fail_1:
    dk_renderpass_destroy(ra, pass);
    ta_free(pass);
    return NULL;
}

static struct ra_renderpass *dk_renderpass_create_compute(struct ra *ra, struct ra_renderpass *pass,
                                                          const struct ra_renderpass_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = pass->priv;
    DkMemBlockMaker memblock_maker;

    if (mp_msg_test(ra->log, MSGL_DEBUG)) {
        MP_DBG(ra, "Compute shader source:\n");
        mp_log_source(ra->log, MSGL_DEBUG, params->compute_shader);
    }

    pass_priv->shaders = talloc_array(pass, DkShader, 1);
    if (!pass_priv->shaders)
        goto fail_1;

    if (!params->cached_program.len ||
            !load_shader_code(ra, pass, params->cached_program, NULL, NULL, &pass_priv->shaders[0])) {
        bool error = false;

        uam_compiler *sh_compiler = uam_create_compiler(DkStage_Compute);
        if (!sh_compiler) {
            error = true; goto fail_2;
        }

        if (!uam_compile_dksh(sh_compiler, params->compute_shader)) {
            MP_ERR(ra, "Failed to compile shader\n");
            error = true; goto fail_2;
        }

        size_t sh_size = uam_get_code_size(sh_compiler);
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
            MP_ALIGN_UP(sh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);
        if (!pass_priv->shader_memblock) {
            error = true; goto fail_2;
        }

        uam_write_code(sh_compiler, dkMemBlockGetCpuAddr(pass_priv->shader_memblock));
        save_shader_code(ra, pass, (bstr){0}, (bstr){0},
            (bstr){dkMemBlockGetCpuAddr(pass_priv->shader_memblock), sh_size});

        DkShaderMaker shader_maker;
        dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, 0);
        dkShaderInitialize(&pass_priv->shaders[0], &shader_maker);

fail_2:
        if (sh_compiler)
            uam_free_compiler(sh_compiler);

        if (error)
            goto fail_1;
    }

    return pass;

fail_1:
    dk_renderpass_destroy(ra, pass);
    ta_free(pass);
    return NULL;
}

static struct ra_renderpass *dk_renderpass_create(struct ra *ra,
                                                  const struct ra_renderpass_params *params) {
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
        // Wait in case an instance of this pass is already running
        dkQueueWaitIdle(priv->dk->queue);

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

    // If the vertex data cannot fit in the inline engine, perform the copy ourselves
    // XXX: How is this constant calculated?
    size_t vao_size = params->vertex_count * pass_params->vertex_stride;
    if (vao_size < 0x7ffc) {
        dkCmdBufPushData(priv->dk->cmdbuf, dkMemBlockGetGpuAddr(pass_priv->vao_memblock),
            params->vertex_data, vao_size);
    } else {
        dkQueueWaitIdle(priv->dk->queue);
        memcpy(dkMemBlockGetCpuAddr(pass_priv->vao_memblock), params->vertex_data, vao_size);
    }

    DkViewport dkviewport = (DkViewport){
        params->viewport.x0, params->viewport.y0,
        mp_rect_w(params->viewport), mp_rect_h(params->viewport),
        0.0f, 1.0f,
    };

    DkScissor dkscissor = (DkScissor){
        params->scissors.x0, params->scissors.y0,
        mp_rect_w(params->scissors), mp_rect_h(params->scissors),
    };

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
    dkCmdBufBindDepthStencilState(priv->dk->cmdbuf, &pass_priv->depth_state);
    dkCmdBufBindVtxBuffer(priv->dk->cmdbuf, 0, dkMemBlockGetGpuAddr(pass_priv->vao_memblock),
        dkMemBlockGetSize(pass_priv->vao_memblock));
    dkCmdBufBindVtxAttribState(priv->dk->cmdbuf, pass_priv->vao_attribs, pass_params->num_vertex_attribs);
    dkCmdBufBindVtxBufferState(priv->dk->cmdbuf, &pass_priv->vao_state, 1);
    dkCmdBufDraw(priv->dk->cmdbuf, DkPrimitive_Triangles, params->vertex_count, 1, 0, 0);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Fragments, DkInvalidateFlags_Image);
}

static void dk_renderpass_run_compute(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = params->pass->priv;

    dkCmdBufBindShaders(priv->dk->cmdbuf, DkStageFlag_Compute,
        (DkShader const *[]){&pass_priv->shaders[0]}, 1);
    dkCmdBufDispatchCompute(priv->dk->cmdbuf, params->compute_groups[0],
        params->compute_groups[1], params->compute_groups[2]);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Primitives, DkInvalidateFlags_Shader | DkInvalidateFlags_Image);

    for (int i = 0; i < params->num_values; ++i) {
        struct ra_renderpass_input_val *val = &params->values[i];
        struct ra_renderpass_input     *inp = &params->pass->params.inputs[val->index];

        if (inp->type == RA_VARTYPE_BUF_RW) {
            struct ra_buf         *inp_buf = *(struct ra_buf **)val->data;
            struct ra_buf_dk *inp_buf_priv = inp_buf->priv;
            dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_buf_priv->fence, true);
        }
    }
}

static void dk_renderpass_run(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv *priv = ra->priv;

    MP_TRACE(ra, "%s (%s)\n", __func__,
        (params->pass->params.type == RA_RENDERPASS_TYPE_RASTER) ? "raster" : "compute");

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

                if (inp->type == RA_VARTYPE_TEX)
                    dkCmdBufBindTexture(priv->dk->cmdbuf, stage, inp->binding,
                        dkMakeTextureHandle(inp_tex_priv->descriptor_idx, inp_tex_priv->descriptor_idx));
                else
                    dkCmdBufBindImage(priv->dk->cmdbuf, stage, inp->binding,
                        dkMakeImageHandle(inp_tex_priv->descriptor_idx));
                break;
            case RA_VARTYPE_BUF_RO:
            case RA_VARTYPE_BUF_RW:
                struct ra_buf         *inp_buf = *(struct ra_buf **)val->data;
                struct ra_buf_dk *inp_buf_priv = inp_buf->priv;

                // For host-mutable buffers, the cache was flushed in buf_update,
                // and for other buffer types, updating is not possible.
                if (inp_buf->params.host_mapped)
                    dkMemBlockFlushCpuCache(inp_buf_priv->memblock, 0,
                        dkMemBlockGetSize(inp_buf_priv->memblock));

                if (inp->type == RA_VARTYPE_BUF_RO)
                    dkCmdBufBindUniformBuffer(priv->dk->cmdbuf, stage, inp->binding,
                        dkMemBlockGetGpuAddr(inp_buf_priv->memblock), inp_buf->params.size);
                else
                    dkCmdBufBindStorageBuffer(priv->dk->cmdbuf, stage, inp->binding,
                        dkMemBlockGetGpuAddr(inp_buf_priv->memblock), inp_buf->params.size);
                break;
            default:
                break;
        }
    }

    // Note: Here we add a barrier causing WFI, which allows the application-side queue
    // to render the UI smoothly even in performance constrained scenarios
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Primitives, 0);

    if (params->pass->params.type == RA_RENDERPASS_TYPE_RASTER)
        dk_renderpass_run_raster(ra, params);
    else
        dk_renderpass_run_compute(ra, params);
}

static ra_timer *dk_timer_create(struct ra *ra) {
    struct priv *priv = ra->priv;

    if (priv->num_queries + RA_DK_NUM_QUERIES > RA_DK_MAX_QUERIES)
        return NULL;

    struct ra_dk_timer *priv_timer = talloc_zero(ra, struct ra_dk_timer);
    if (!priv_timer)
        return NULL;

    for (int i = 0; i < RA_DK_NUM_QUERIES; ++i)
        priv_timer->query_idx[i] = priv->num_queries++;

    return priv_timer;
}

static void dk_timer_destroy(struct ra *ra, ra_timer *timer) {
    if (timer)
        talloc_free(timer);
}

static void dk_timer_start(struct ra *ra, ra_timer *timer) {
    struct priv *priv              = ra->priv;
    struct ra_dk_timer *priv_timer = timer;

    priv_timer->cur_idx = (priv_timer->cur_idx + 1) % RA_DK_NUM_QUERIES;

    uint64_t *query_data = (uint64_t *)((uint8_t *)dkMemBlockGetCpuAddr(priv->query_memblock) +
        (2 * priv_timer->query_idx[priv_timer->cur_idx]) * 16);

    priv_timer->result = (query_data[3] > query_data[1]) ? dkTimestampToNs(query_data[3] - query_data[1]) : 0;

    dkCmdBufReportCounter(priv->dk->cmdbuf, DkCounter_Timestamp,
        dkMemBlockGetGpuAddr(priv->query_memblock) + (2 * priv_timer->query_idx[priv_timer->cur_idx] + 0) * 16);
}

static uint64_t dk_timer_stop(struct ra *ra, ra_timer *timer) {
    struct priv *priv              = ra->priv;
    struct ra_dk_timer *priv_timer = timer;

    dkCmdBufReportCounter(priv->dk->cmdbuf, DkCounter_Timestamp,
        dkMemBlockGetGpuAddr(priv->query_memblock) + (2 * priv_timer->query_idx[priv_timer->cur_idx] + 1) * 16);

    // Submit here to keep both counter commands in the same submission
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));

    return priv_timer->result;
}

static void dk_debug_marker(struct ra *ra, const char *msg) {
    struct priv *priv = ra->priv;

    if (dkQueueIsInErrorState(priv->dk->queue))
        MP_ERR(ra, "Queue is in error state: %s\n", msg);
}
