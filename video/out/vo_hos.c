/*
 * video output driver for deko3d
 *
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

#include "input/input.h"
#include "input/keycodes.h"
#include "common/msg.h"
#include "options/m_config.h"
#include "options/options.h"

#include "config.h"
#include "vo.h"
#include "video/mp_image.h"

#define MAX_WIDTH     1920
#define MAX_HEIGHT    1080
#define NUM_FB_IMAGES 2
#define CMDBUF_SIZE   DK_MEMBLOCK_ALIGNMENT

struct priv {
    DkDevice    device;
    DkMemBlock  fb_memblock, frame_memblock,
        transfer_memblock, cmdbuf_memblock;
    DkSwapchain swapchain;
    DkCmdBuf    render_cmdbuf, transfer_cmdbuf;
    DkQueue     queue;

    DkImage frame;
    DkImage framebuffer_images[NUM_FB_IMAGES];

    DkCmdList transfer_cmdlist;
    DkCmdList render_cmdlists[NUM_FB_IMAGES];
    DkCmdList bind_fb_cmdlists[NUM_FB_IMAGES];

    int frame_width,  frame_height;
    int screen_width, screen_height;

    AppletHookCookie hook_cookie;
};

static void get_current_resolution(int *width, int *height) {
    switch (appletGetOperationMode()) {
        default:
        case AppletOperationMode_Handheld:
            *width = 1280, *height = 720;
            break;
        case AppletOperationMode_Console:
            *width = 1920, *height = 1080;
            break;
    }
}

static void regen_cmdlists(struct priv *priv) {
    dkCmdBufClear(priv->render_cmdbuf);

    int scaled_width = priv->frame_width * priv->screen_height / priv->frame_height;
    int horiz_offset = (priv->screen_width - scaled_width) / 2;

    DkImageView frame_view;
    dkImageViewDefaults(&frame_view, &priv->frame);

    for (int i = 0; i < NUM_FB_IMAGES; ++i) {
        DkImageView fb_view;
        dkImageViewDefaults(&fb_view, &priv->framebuffer_images[i]);

        dkCmdBufBindRenderTarget(priv->render_cmdbuf, &fb_view, NULL);
        priv->bind_fb_cmdlists[i] = dkCmdBufFinishList(priv->render_cmdbuf);

        dkCmdBufSetViewports(priv->render_cmdbuf, 0,
            &(DkViewport){0.0f, 0.0f, priv->screen_width, priv->screen_height, 0.0f, 1.0f}, 1);
        dkCmdBufSetScissors(priv->render_cmdbuf, 0,
            &(DkScissor){0, 0, priv->screen_width, priv->screen_height}, 1);
        dkCmdBufClearColorFloat(priv->render_cmdbuf, 0, DkColorMask_RGBA,
            0.0f, 0.0f, 0.0f, 1.0f);
        dkCmdBufBlitImage(priv->render_cmdbuf,
            &frame_view, &(DkImageRect){0, 0, 0, priv->frame_width, priv->frame_height, 1},
            &fb_view,    &(DkImageRect){horiz_offset, 0, 0, scaled_width, priv->screen_height, 1}, 0, 0);
        priv->render_cmdlists[i] = dkCmdBufFinishList(priv->render_cmdbuf);
    }
}

static void resize(struct vo *vo, int width, int height) {
    struct priv *priv = vo->priv;

    MP_VERBOSE(vo, "Resizing to %dx%d\n", width, height);

    dkQueueWaitIdle(priv->queue);

    if (priv->swapchain)
        dkSwapchainDestroy(priv->swapchain);

    DkImageLayoutMaker fb_layout_maker;
    dkImageLayoutMakerDefaults(&fb_layout_maker, priv->device);
    fb_layout_maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent |
        DkImageFlags_HwCompression | DkImageFlags_Usage2DEngine;
    fb_layout_maker.format = DkImageFormat_RGBA8_Unorm;
    fb_layout_maker.dimensions[0] = width;
    fb_layout_maker.dimensions[1] = height;

    DkImageLayout fb_layout;
    dkImageLayoutInitialize(&fb_layout, &fb_layout_maker);

    size_t fb_size = MP_ALIGN_UP(dkImageLayoutGetSize(&fb_layout),
        dkImageLayoutGetAlignment(&fb_layout));

    DkImage const *swapchain_images[NUM_FB_IMAGES];
    for (int i = 0; i < MP_ARRAY_SIZE(priv->framebuffer_images); ++i) {
        swapchain_images[i] = &priv->framebuffer_images[i];
        dkImageInitialize(&priv->framebuffer_images[i], &fb_layout,
            priv->fb_memblock, i * fb_size);
    }

    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, priv->device, nwindowGetDefault(),
        swapchain_images, MP_ARRAY_SIZE(swapchain_images));
    priv->swapchain = dkSwapchainCreate(&swapchain_maker);

    priv->screen_width = width, priv->screen_height = height;

    regen_cmdlists(priv);
}

static void handle_applet_hook(AppletHookType type, void *param) {
    if (type != AppletHookType_OnOperationMode)
        return;

    int width, height;
    get_current_resolution(&width, &height);
    resize(param, width, height);
}

static int preinit(struct vo *vo) {
    struct priv *priv = vo->priv;

    MP_VERBOSE(vo, "Initializing hos video\n");

    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);

    priv->device = dkDeviceCreate(&device_maker);

    DkMemBlockMaker memblock_maker;
    DkCmdBufMaker   cmdbuf_maker;

    DkImageLayoutMaker fb_layout_maker;
    dkImageLayoutMakerDefaults(&fb_layout_maker, priv->device);
    fb_layout_maker.flags = DkImageFlags_UsageRender |
        DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    fb_layout_maker.format = DkImageFormat_RGBA8_Unorm;
    fb_layout_maker.dimensions[0] = MAX_WIDTH;
    fb_layout_maker.dimensions[1] = MAX_HEIGHT;

    DkImageLayout fb_layout;
    dkImageLayoutInitialize(&fb_layout, &fb_layout_maker);

    size_t fb_size = MP_ALIGN_UP(dkImageLayoutGetSize(&fb_layout),
        dkImageLayoutGetAlignment(&fb_layout));

    dkMemBlockMakerDefaults(&memblock_maker, priv->device, NUM_FB_IMAGES * fb_size);
    memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    priv->fb_memblock = dkMemBlockCreate(&memblock_maker);

    dkMemBlockMakerDefaults(&memblock_maker, priv->device, 2 * CMDBUF_SIZE);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    priv->cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);

    dkCmdBufMakerDefaults(&cmdbuf_maker, priv->device);
    priv->render_cmdbuf = dkCmdBufCreate(&cmdbuf_maker);

    dkCmdBufAddMemory(priv->render_cmdbuf, priv->cmdbuf_memblock, 0,
        dkMemBlockGetSize(priv->cmdbuf_memblock));

    dkCmdBufMakerDefaults(&cmdbuf_maker, priv->device);
    priv->transfer_cmdbuf = dkCmdBufCreate(&cmdbuf_maker);

    dkCmdBufAddMemory(priv->transfer_cmdbuf, priv->cmdbuf_memblock, CMDBUF_SIZE,
        dkMemBlockGetSize(priv->cmdbuf_memblock));

    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, priv->device);
    queue_maker.flags = DkQueueFlags_Graphics;
    priv->queue = dkQueueCreate(&queue_maker);

    DkImageLayoutMaker frame_layout_maker;
    dkImageLayoutMakerDefaults(&frame_layout_maker, priv->device);
    frame_layout_maker.flags = DkImageFlags_Usage2DEngine;
    frame_layout_maker.format = DkImageFormat_RGBA8_Unorm;
    frame_layout_maker.dimensions[0] = MAX_WIDTH;
    frame_layout_maker.dimensions[1] = MAX_HEIGHT;

    DkImageLayout frame_layout;
    dkImageLayoutInitialize(&frame_layout, &frame_layout_maker);

    size_t frame_size = MP_ALIGN_UP(dkImageLayoutGetSize(&frame_layout),
        dkImageLayoutGetAlignment(&frame_layout));

    dkMemBlockMakerDefaults(&memblock_maker, priv->device, frame_size);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    priv->frame_memblock = dkMemBlockCreate(&memblock_maker);

    dkImageInitialize(&priv->frame, &frame_layout, priv->frame_memblock, 0);

    priv->frame_width = MAX_WIDTH, priv->frame_height = MAX_HEIGHT;

    int width, height;
    get_current_resolution(&width, &height);
    resize(vo, width, height);

    appletHook(&priv->hook_cookie, handle_applet_hook, vo);

    return 0;
}

static int query_format(struct vo *vo, int format) {
    return format == IMGFMT_RGBA;
}

static int reconfig2(struct vo *vo, struct mp_image *img) {
    struct priv *priv = vo->priv;

    if (priv->frame_memblock)
        dkMemBlockDestroy(priv->frame_memblock);

    DkImageLayoutMaker frame_layout_maker;
    dkImageLayoutMakerDefaults(&frame_layout_maker, priv->device);
    frame_layout_maker.flags = DkImageFlags_Usage2DEngine;
    frame_layout_maker.format = DkImageFormat_RGBA8_Unorm;
    frame_layout_maker.dimensions[0] = img->w;
    frame_layout_maker.dimensions[1] = img->h;

    DkImageLayout frame_layout;
    dkImageLayoutInitialize(&frame_layout, &frame_layout_maker);

    size_t frame_size = MP_ALIGN_UP(dkImageLayoutGetSize(&frame_layout),
        dkImageLayoutGetAlignment(&frame_layout));

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->device, frame_size);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached |
        DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    priv->frame_memblock = dkMemBlockCreate(&memblock_maker);

    dkImageInitialize(&priv->frame, &frame_layout, priv->frame_memblock, 0);

    priv->frame_width = img->w, priv->frame_height = img->h;

    regen_cmdlists(priv);

    return 0;
}

static int control(struct vo *vo, uint32_t request, void *data) {
    // MP_VERBOSE(vo, "control\n");
    return VO_NOTIMPL;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame) {
    struct mp_image *img = frame->current;
    struct priv    *priv = vo->priv;

    dkCmdBufClear(priv->transfer_cmdbuf);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, priv->device,
        MP_ALIGN_UP(img->stride[0] * img->h, DK_MEMBLOCK_ALIGNMENT));
    memblock_maker.storage = img->planes[0];
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    priv->transfer_memblock = dkMemBlockCreate(&memblock_maker);

    DkImageView frame_view;
    dkImageViewDefaults(&frame_view, &priv->frame);

    dkCmdBufCopyBufferToImage(priv->transfer_cmdbuf,
        &(DkCopyBuf){dkMemBlockGetGpuAddr(priv->transfer_memblock), img->stride[0], img->h},
        &frame_view, &(DkImageRect){0, 0, 0, img->w, img->h, 1}, 0);
    priv->transfer_cmdlist = dkCmdBufFinishList(priv->transfer_cmdbuf);

    dkQueueSubmitCommands(priv->queue, priv->transfer_cmdlist);
}

static void flip_page(struct vo *vo) {
    struct priv *priv = vo->priv;

    // Wait for the frame transfer to complete
    dkQueueWaitIdle(priv->queue);
    dkMemBlockDestroy(priv->transfer_memblock);

    int slot = dkQueueAcquireImage(priv->queue, priv->swapchain);
    dkQueueSubmitCommands(priv->queue, priv->bind_fb_cmdlists[slot]);
    dkQueueSubmitCommands(priv->queue, priv->render_cmdlists[slot]);
    dkQueuePresentImage(priv->queue, priv->swapchain, slot);
}

static void uninit(struct vo *vo) {
    struct priv *priv = vo->priv;

    MP_VERBOSE(vo, "Deinitializing hos video\n");

    dkQueueWaitIdle(priv->queue);

    dkQueueDestroy(priv->queue);
    dkCmdBufDestroy(priv->render_cmdbuf);
    dkMemBlockDestroy(priv->transfer_memblock);
    dkMemBlockDestroy(priv->frame_memblock);
    dkMemBlockDestroy(priv->fb_memblock);
    dkMemBlockDestroy(priv->cmdbuf_memblock);
    dkSwapchainDestroy(priv->swapchain);
    dkDeviceDestroy(priv->device);

    appletUnhook(&priv->hook_cookie);
}

#define OPT_BASE_STRUCT struct priv

const struct vo_driver video_out_hos = {
    .description    = "deko3d Renderer",
    .name           = "hos",
    .priv_size      = sizeof(struct priv),
    .priv_defaults  = &(const struct priv) {

    },
    .options        = NULL,
    .preinit        = preinit,
    .query_format   = query_format,
    .reconfig2      = reconfig2,
    .control        = control,
    .draw_frame     = draw_frame,
    .flip_page      = flip_page,
    .uninit         = uninit,
    .options_prefix = "vo-hos",
};
