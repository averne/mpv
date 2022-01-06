#pragma once

#include "video/out/gpu/context.h"
#include "common.h"

struct ra_dk_ctx_params {
    const struct ra_swapchain_fns *external_swapchain;
};

bool ra_dk_ctx_init(struct ra_ctx *ctx, mp_dk_ctx *dk, struct ra_dk_ctx_params *params);
void ra_dk_ctx_uninit(struct ra_ctx *ctx);
