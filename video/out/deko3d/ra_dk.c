#include "common/msg.h"

#include "ra_dk.h"

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
};

static struct ra_fns ra_fns_dk;

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
    talloc_free(ra->priv);
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

    return tex;
}

static void dk_tex_destroy(struct ra *ra, struct ra_tex *tex) {
    MP_VERBOSE(ra, "Destroying texture\n");

    struct ra_tex_dk *tex_priv = tex->priv;
    dkMemBlockDestroy(tex_priv->memblock);

    talloc_free(tex);
}

static bool dk_tex_upload(struct ra *ra, const struct ra_tex_upload_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return false;
}

static bool dk_tex_download(struct ra *ra, struct ra_tex_download_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return false;
}

static struct ra_buf *dk_buf_create(struct ra *ra, const struct ra_buf_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return NULL;
}

static void dk_buf_destroy(struct ra *ra, struct ra_buf *buf) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static void dk_buf_update(struct ra *ra, struct ra_buf *buf, ptrdiff_t offset,
                          const void *data, size_t size) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static bool dk_buf_poll(struct ra *ra, struct ra_buf *buf) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return false;
}

static void dk_clear(struct ra *ra, struct ra_tex *dst, float color[4], struct mp_rect *scissor) {
    MP_VERBOSE(ra, "%s\n", __func__);

    DkScissor scissor = { scissor->x0, scissor->y0,
                          scissor->x1 - scissor->x0,
                          scissor->y1 - scissor->y0 };


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
    MP_VERBOSE(ra, "%s\n", __func__);
    return NULL;
}

static void dk_renderpass_destroy(struct ra *ra, struct ra_renderpass *pass) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static void dk_renderpass_run(struct ra *ra, const struct ra_renderpass_run_params *params) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static ra_timer *dk_timer_create(struct ra *ra) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return NULL;
}

static void dk_timer_destroy(struct ra *ra, ra_timer *timer) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static void dk_timer_start(struct ra *ra, ra_timer *timer) {
    MP_VERBOSE(ra, "%s\n", __func__);
}

static uint64_t dk_timer_stop(struct ra *ra, ra_timer *timer) {
    MP_VERBOSE(ra, "%s\n", __func__);
    return 0;
}

static void dk_debug_marker(struct ra *ra, const char *msg) {
    MP_VERBOSE(ra, "%s\n", __func__);
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
