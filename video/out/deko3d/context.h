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

#pragma once

#include "video/out/gpu/context.h"
#include "common.h"

struct ra_dk_ctx_params {
    const struct ra_swapchain_fns *external_swapchain;
};

bool ra_dk_ctx_init(struct ra_ctx *ctx, mp_dk_ctx *dk, struct ra_dk_ctx_params *params);
void ra_dk_ctx_uninit(struct ra_ctx *ctx);
