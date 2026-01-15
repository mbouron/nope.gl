/*
 * Copyright 2018-2022 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "config.h"

#include <string.h>

#if defined(HAVE_VAAPI_X11)
#include <X11/Xlib.h>
#include <va/va_x11.h>
#endif

#if defined(HAVE_VAAPI_WAYLAND)
#include <wayland-client.h>
#include <va/va_wayland.h>
#endif

#include "log.h"
#include <ngpu/ngpu.h>
#include "utils/utils.h"
#include "vaapi_ctx.h"

int ngli_vaapi_ctx_init(struct ngpu_ctx *gpu_ctx, struct vaapi_ctx *s)
{
    const uint64_t features = ngpu_ctx_get_features(gpu_ctx);
    if (features & NGPU_FEATURE_SOFTWARE_BIT)
        return -1;

    if (!NGLI_HAS_ALL_FLAGS(features, NGPU_FEATURE_IMPORT_DMA_BUF_BIT))
        return -1;

    VADisplay va_display = NULL;

    const enum ngpu_platform_type platform_type = ngpu_ctx_get_platform_type(gpu_ctx);
    if (platform_type == NGPU_PLATFORM_XLIB) {
#if defined(HAVE_VAAPI_X11)
        Display *x11_display = XOpenDisplay(NULL);
        if (!x11_display) {
            LOG(ERROR, "could not initialize X11 display");
            return -1;
        }
        s->x11_display = x11_display;

        va_display = vaGetDisplay(x11_display);
#endif
    } else if (platform_type == NGPU_PLATFORM_WAYLAND) {
#if defined(HAVE_VAAPI_WAYLAND)
        struct wl_display *wl_display = (struct wl_display *)ngpu_ctx_get_display(gpu_ctx);
        if (!wl_display) {
            wl_display = wl_display_connect(NULL);
            if (!wl_display) {
                LOG(ERROR, "could not connect to Wayland display");
                return -1;
            }
            s->wl_display = wl_display;
        }

        va_display = vaGetDisplayWl(wl_display);
#endif
    }
    if (!va_display) {
        LOG(ERROR, "could not get va display");
        return -1;
    }

    int major_version;
    int minor_version;
    VAStatus va_status = vaInitialize(va_display, &major_version, &minor_version);
    if (va_status != VA_STATUS_SUCCESS) {
        LOG(ERROR, "could not initialize va display: %s", vaErrorStr(va_status));
        return -1;
    }

    s->va_display = va_display;
    s->va_version = major_version * 100 + minor_version;

    LOG(INFO, "VAAPI version: %d.%d", major_version, minor_version);

    return 0;
}

void ngli_vaapi_ctx_reset(struct vaapi_ctx *s)
{
    if (s->va_display)
        vaTerminate(s->va_display);
#if defined(HAVE_VAAPI_X11)
    if (s->x11_display)
        XCloseDisplay(s->x11_display);
#endif
#if defined(HAVE_VAAPI_WAYLAND)
    if (s->wl_display)
        wl_display_disconnect(s->wl_display);
#endif
    memset(s, 0, sizeof(*s));
}
