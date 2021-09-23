/*
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

#pragma once

#include <libplacebo/deko3d.h>

struct mpdk_ctx {
    struct mp_log       *pl_log;
    struct pl_context   *ctx;
    struct pl_deko3d    *dk;
    struct pl_gpu       *gpu;
    struct pl_swapchain *swapchain;
};
