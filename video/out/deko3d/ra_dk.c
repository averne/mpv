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

static struct ra_fns ra_fns_dk;

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

static int ra_init_dk(struct ra *ra, mp_dk_ctx *dk) {
    struct priv *priv = ra->priv = talloc_zero(NULL, struct priv);
    priv->dk = dk;

    ra->fns = &ra_fns_dk;
    ra->glsl_version = 460;
    ra->glsl_deko3d = true;

    ra->caps = RA_CAP_TEX_1D        |
               RA_CAP_TEX_3D        |
               RA_CAP_BLIT          |
               // TODO
            //    RA_CAP_COMPUTE       |
               RA_CAP_DIRECT_UPLOAD |
               RA_CAP_BUF_RO        |
               RA_CAP_BUF_RW        |
               RA_CAP_NESTED_ARRAY  |
               RA_CAP_GATHER        |
               RA_CAP_FRAGCOORD     |
               RA_CAP_NUM_GROUPS;

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

    // Should be enough to hold all descriptors (max. 128)
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, DK_MEMBLOCK_ALIGNMENT);
    priv->image_descriptors = dkMemBlockCreate(&memblock_maker);

    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, DK_MEMBLOCK_ALIGNMENT);
    priv->sampler_descriptors = dkMemBlockCreate(&memblock_maker);

    priv->num_descriptors = 0;

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, priv->dk->device);
    priv->dk->cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    dkCmdBufAddMemory(priv->dk->cmdbuf, priv->dk->cmdbuf_memblock, 0, CMD_MEM_SIZE);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, priv->dk->device);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute | DkQueueFlags_DisableZcull;
    priv->dk->queue = dkQueueCreate(&queueMaker);

    return 0;
}

struct ra *ra_create_dk(mp_dk_ctx *dk, struct mp_log *log) {
    struct ra *ra = talloc_zero(NULL, struct ra);
    ra->log = log;
    if (ra_init_dk(ra, dk) < 0) {
        talloc_free(ra);
        return NULL;
    }
    return ra;
}

static void dk_destroy(struct ra *ra) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    dkQueueFlush(priv->dk->queue);
    dkQueueWaitIdle(priv->dk->queue);
    dkQueueDestroy(priv->dk->queue);

    dkCmdBufDestroy(priv->dk->cmdbuf);
    dkMemBlockDestroy(priv->dk->cmdbuf_memblock);

    dkMemBlockDestroy(priv->image_descriptors);
    dkMemBlockDestroy(priv->sampler_descriptors);

    for (int i = 0; i < priv->dk->num_tmp_memblocks; ++i)
        dkMemBlockDestroy(priv->dk->tmp_memblocks[i]);

    talloc_free(priv);
}

static struct ra_tex *dk_tex_create(struct ra *ra, const struct ra_tex_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "Creating texture (%s %dx%dx%d)\n",
        params->format->name, params->w, params->h, params->d);

    struct ra_tex *tex = talloc_zero(NULL, struct ra_tex);
    tex->params = *params;
    tex->params.initial_data = NULL;

    struct ra_tex_dk *tex_priv = tex->priv = talloc_zero(tex, struct ra_tex_dk);

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, priv->dk->device);

    layout_maker.format = ((struct dk_format *)params->format->priv)->fmt;
    layout_maker.dimensions[0] = params->w;
    layout_maker.dimensions[1] = params->h;
    layout_maker.dimensions[2] = params->d;
    layout_maker.flags =
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
    memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    tex_priv->memblock = dkMemBlockCreate(&memblock_maker);

    dkImageInitialize(&tex_priv->image, &tex_layout, tex_priv->memblock, 0);

    DkImageDescriptor   *image_descs   = dkMemBlockGetCpuAddr(priv->image_descriptors);
    DkSamplerDescriptor *sampler_descs = dkMemBlockGetCpuAddr(priv->sampler_descriptors);

    DkImageView image_view;
    dkImageViewDefaults(&image_view, &tex_priv->image);

    dkImageDescriptorInitialize(&image_descs[priv->num_descriptors], &image_view, params->storage_dst, false);

    DkSampler sampler;
    dkSamplerDefaults(&sampler);

    sampler.compareEnable = false;
    sampler.compareOp = DkCompareOp_Never;
    sampler.wrapMode[0] = sampler.wrapMode[1] = sampler.wrapMode[2] =
        params->src_repeat ? DkWrapMode_Repeat : DkWrapMode_Clamp;

    if (params->src_linear)
        sampler.minFilter = sampler.magFilter = DkFilter_Linear,
            sampler.mipFilter = DkMipFilter_Linear;

    dkSamplerDescriptorInitialize(&sampler_descs[priv->num_descriptors], &sampler);

    tex_priv->descriptor_idx = priv->num_descriptors++;

    dkQueueSignalFence(priv->dk->queue, &tex_priv->fence, false);

    return tex;
}

static void dk_tex_destroy(struct ra *ra, struct ra_tex *tex) {
    MP_VERBOSE(ra, "Destroying texture\n");

    struct ra_tex_dk *tex_priv = tex->priv;
    dkMemBlockDestroy(tex_priv->memblock);

    talloc_free(tex);
}

static bool dk_tex_upload(struct ra *ra, const struct ra_tex_upload_params *params) {
    struct priv          *priv = ra->priv;
    struct ra_tex_dk *tex_priv = params->tex->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    DkImageRect dkrect;
    if (params->rc) {
        dkrect = (DkImageRect){
            params->rc->x0, params->rc->y0, 0,
            mp_rect_w(*params->rc), mp_rect_h(*params->rc), 0,
        };
    } else {
        dkrect = (DkImageRect){
            0, 0, 0,
            params->tex->params.w,
            params->tex->params.h,
            params->tex->params.d,
        };
    }

    DkCopyBuf dkcopy;
    if (params->buf) {
        struct ra_buf_dk *buf_priv = params->buf->priv;
        dkcopy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(buf_priv->memblock) + params->buf_offset,
            params->buf->params.size, 1,
        };
    } else {
        // Map the provided buffer into the GPU address space and mark it to be removed from it later
        size_t memblk_size = params->stride * params->tex->params.h * params->tex->params.d;

        DkMemBlockMaker memblock_maker;
        dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(memblk_size, DK_MEMBLOCK_ALIGNMENT));
        memblock_maker.storage = (void *)params->src;

        DkMemBlock memblock = dkMemBlockCreate(&memblock_maker);

        dkcopy = (DkCopyBuf){
            dkMemBlockGetGpuAddr(memblock),
            params->stride, params->tex->params.h * params->tex->params.d,
        };

        MP_TARRAY_APPEND(ra, priv->dk->tmp_memblocks, priv->dk->num_tmp_memblocks, memblock);
    }

    dkCmdBufWaitFence(priv->dk->cmdbuf, &tex_priv->fence);
    dkCmdBufCopyBufferToImage(priv->dk->cmdbuf, &dkcopy, &tex_view, &dkrect, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);

    return true;
}

static bool dk_tex_download(struct ra *ra, struct ra_tex_download_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return false;
}

static struct ra_buf *dk_buf_create(struct ra *ra, const struct ra_buf_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    struct ra_buf *buf = talloc_zero(NULL, struct ra_buf);
    buf->params = *params;
    buf->params.initial_data = NULL;

    struct ra_buf_dk *buf_priv = buf->priv = talloc_zero(buf, struct ra_buf_dk);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device, MP_ALIGN_UP(params->size, DK_MEMBLOCK_ALIGNMENT));

    buf_priv->memblock = dkMemBlockCreate(&memblock_maker);

    if (params->host_mapped)
        buf->data = dkMemBlockGetCpuAddr(buf_priv->memblock);

    if (params->initial_data)
        memcpy(dkMemBlockGetCpuAddr(buf_priv->memblock), params->initial_data, params->size);

    return buf;
}

static void dk_buf_destroy(struct ra *ra, struct ra_buf *buf) {
    struct ra_buf_dk *buf_priv = buf->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    dkMemBlockDestroy(buf_priv->memblock);

    talloc_free(buf);
}

static void dk_buf_update(struct ra *ra, struct ra_buf *buf, ptrdiff_t offset,
                          const void *data, size_t size) {
    struct ra_buf_dk *buf_priv = buf->priv;

    MP_VERBOSE(ra, "%s\n", __func__);

    memcpy((uint8_t *)dkMemBlockGetCpuAddr(buf_priv->memblock) + offset, data, size);

    // Not needed since buffers are currently all cpu-uncached
    // dkMemBlockFlushCpuCache(buf_priv->memblock, offset, size);
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
    MP_VERBOSE(ra, "%s\n", __func__);
}

static int dk_desc_namespace(struct ra *ra, enum ra_vartype type) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return type;
}

static struct ra_renderpass *dk_renderpass_create(struct ra *ra,
                                                  const struct ra_renderpass_params *params) {
    struct priv *priv = ra->priv;

    MP_VERBOSE(ra, "%s, render target format: %s\n", __func__, params->target_format->name);

    if (params->type != RA_RENDERPASS_TYPE_RASTER)
        return NULL;

    DkMemBlockMaker memblock_maker;

    struct ra_renderpass *pass = talloc_zero(NULL, struct ra_renderpass);
    pass->params = *ra_renderpass_params_copy(pass, params);
    pass->params.cached_program = (bstr){0};
    struct ra_rpass_dk *pass_priv = pass->priv = talloc_zero(pass, struct ra_rpass_dk);

    if (mp_msg_test(ra->log, MSGL_V)) {
        MP_VERBOSE(ra, "Vertex shader source:\n");
        mp_log_source(ra->log, MSGL_V, params->vertex_shader);
        MP_VERBOSE(ra, "Fragment shader source:\n");
        mp_log_source(ra->log, MSGL_V, params->frag_shader);
    }

    uam_compiler *vsh_compiler = uam_create_compiler(DkStage_Vertex),
        fsh_compiler = uam_create_compiler(DkStage_Fragment);

    if (!uam_compile_dksh(vsh_compiler, params->vertex_shader) ||
            !uam_compile_dksh(fsh_compiler, params->frag_shader)) {
        MP_ERR(ra, "Failed to compile shaders\n");
        TA_FREEP(&pass);
        goto compile_error;
    }

    size_t vsh_size = uam_get_code_size(vsh_compiler), fsh_size = uam_get_code_size(fsh_compiler);
    size_t vsh_off = 0, fsh_off = MP_ALIGN_UP(vsh_size, DK_SHADER_CODE_ALIGNMENT);

    dkMemBlockMakerDefaults(&memblock_maker, priv->dk->device,
        MP_ALIGN_UP(vsh_size + fsh_size + DK_SHADER_CODE_UNUSABLE_SIZE, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    pass_priv->shader_memblock = dkMemBlockCreate(&memblock_maker);

    uam_write_code(vsh_compiler, (uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + vsh_off);
    uam_write_code(fsh_compiler, (uint8_t *)dkMemBlockGetCpuAddr(pass_priv->shader_memblock) + fsh_off);

    pass_priv->shaders = talloc_array(pass, DkShader, 2);

    DkShaderMaker shader_maker;
    dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, vsh_off);
    dkShaderInitialize(&pass_priv->shaders[0], &shader_maker);
    dkShaderMakerDefaults(&shader_maker, pass_priv->shader_memblock, fsh_off);
    dkShaderInitialize(&pass_priv->shaders[1], &shader_maker);

    pass_priv->vao_attribs = talloc_array(pass, DkVtxAttribState, params->num_vertex_attribs);
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
    pass_priv->vao_memblock = dkMemBlockCreate(&memblock_maker);

    if (params->enable_blend) {
        dkBlendStateSetOps(&pass_priv->blend_state, DkBlendOp_Add, DkBlendOp_Add);
        dkBlendStateSetFactors(&pass_priv->blend_state,
            map_blend_factor(params->blend_src_rgb), map_blend_factor(params->blend_src_rgb),
            map_blend_factor(params->blend_src_alpha), map_blend_factor(params->blend_src_alpha));
    }

    dkRasterizerStateDefaults(&pass_priv->rasterizer_state);
    dkColorStateDefaults(&pass_priv->color_state);
    dkColorWriteStateDefaults(&pass_priv->color_write_state);

    pass_priv->rasterizer_state.cullMode = DkFace_None;

compile_error:
    uam_free_compiler(vsh_compiler), uam_free_compiler(fsh_compiler);
    return pass;
}

static void dk_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass) {
    MP_VERBOSE(ra, "%s\n", __func__);

    struct ra_rpass_dk *pass_priv = pass->priv;

    dkMemBlockDestroy(pass_priv->shader_memblock);
    dkMemBlockDestroy(pass_priv->vao_memblock);

    talloc_free(pass);
}

static void dk_renderpass_run(struct ra *ra, const struct ra_renderpass_run_params *params) {
    struct priv             *priv = ra->priv;
    struct ra_rpass_dk *pass_priv = params->pass->priv;
    struct ra_tex_dk    *tex_priv = params->target->priv;

    MP_VERBOSE(ra, "%s, vertex count: %d\n", __func__, params->vertex_count);

    DkImageView tex_view;
    dkImageViewDefaults(&tex_view, &tex_priv->image);

    // No CPU cache flush needed
    memcpy(dkMemBlockGetCpuAddr(pass_priv->vao_memblock), params->vertex_data,
        params->vertex_count * params->pass->params.vertex_stride);

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

    for (int i = 0; i < params->num_values; ++i) {
        struct ra_renderpass_input_val *val = &params->values[i];
        struct ra_renderpass_input     *inp = &params->pass->params.inputs[val->index];
        switch (inp->type) {
            case RA_VARTYPE_TEX:
                struct ra_tex         *inp_tex = *(struct ra_tex **)val->data;
                struct ra_tex_dk *inp_tex_priv = inp_tex->priv;
                dkCmdBufWaitFence(priv->dk->cmdbuf, &inp_tex_priv->fence);
                dkCmdBufBindTexture(priv->dk->cmdbuf, DkStage_Fragment, inp->binding,
                    dkMakeTextureHandle(inp_tex_priv->descriptor_idx, inp_tex_priv->descriptor_idx));
                dkCmdBufSignalFence(priv->dk->cmdbuf, &inp_tex_priv->fence, false);
                break;
            case RA_VARTYPE_BUF_RO:
                struct ra_buf         *inp_buf = *(struct ra_buf **)val->data;
                struct ra_buf_dk *inp_buf_priv = inp_buf->priv;
                // Can only have an uniform buffer in the fragment stage?
                dkCmdBufBindUniformBuffer(priv->dk->cmdbuf, DkStage_Fragment, inp->binding,
                    dkMemBlockGetGpuAddr(inp_buf_priv->memblock), inp_buf->params.size);
                break;
            default:
                break;
        }
    }

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
        params->vertex_count * params->pass->params.vertex_stride);
    dkCmdBufBindVtxAttribState(priv->dk->cmdbuf, pass_priv->vao_attribs, params->pass->params.num_vertex_attribs);
    dkCmdBufBindVtxBufferState(priv->dk->cmdbuf, &pass_priv->vao_state, 1);
    dkCmdBufDraw(priv->dk->cmdbuf, DkPrimitive_Triangles, params->vertex_count, 1, 0, 0);
    dkCmdBufBarrier(priv->dk->cmdbuf, DkBarrier_Fragments, 0);
    dkCmdBufSignalFence(priv->dk->cmdbuf, &tex_priv->fence, false);
    dkQueueSubmitCommands(priv->dk->queue, dkCmdBufFinishList(priv->dk->cmdbuf));
    dkQueueFlush(priv->dk->queue);
}

static void dk_debug_marker(struct ra *ra, const char *msg) {
    MP_VERBOSE(ra, "%s: %s\n", __func__, msg);
}

static struct ra_fns ra_fns_dk = {
    .destroy            = dk_destroy,
    .tex_create         = dk_tex_create,
    .tex_destroy        = dk_tex_destroy,
    .tex_upload         = dk_tex_upload,
    .tex_download       = dk_tex_download,
    .buf_create         = dk_buf_create,
    .buf_destroy        = dk_buf_destroy,
    .buf_update         = dk_buf_update,
    // Probably not useful to implement (would need to fence buffer usage)
    // .buf_poll          = dk_buf_poll,
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
