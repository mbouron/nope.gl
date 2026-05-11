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

#include <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include <nopegl/nopegl.h>

#include "wsi.h"

static int get_default_backend(void)
{
    size_t nb_backends;
    struct ngl_backend *backends;
    int ret = ngl_backends_get(NULL, &nb_backends, &backends);
    if (ret < 0)
        return ret;
    for (size_t i = 0; i < nb_backends; i++) {
        if (backends[i].is_default) {
            ret = backends[i].id;
            ngl_backends_freep(&backends);
            return ret;
        }
    }
    ngl_backends_freep(&backends);
    return NGL_ERROR_NOT_FOUND;
}

int wsi_set_ngl_config(struct ngl_config *config, SDL_Window *window)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) {
        fprintf(stderr, "Failed to get window properties: %s\n", SDL_GetError());
        return -1;
    }

    NSWindow *nswindow = (__bridge NSWindow *)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
    if (!nswindow) {
        fprintf(stderr, "Failed to get Cocoa window\n");
        return -1;
    }

    NSView *view = [nswindow contentView];

    if (config->backend == NGL_BACKEND_AUTO) {
        config->backend = get_default_backend();
        if (config->backend < 0)
            return -1;
    }
    if (config->backend == NGL_BACKEND_VULKAN) {
        NSBundle *bundle = [NSBundle bundleWithPath:@"/System/Library/Frameworks/QuartzCore.framework"];
        view.layer = [[bundle classNamed:@"CAMetalLayer"] layer];
        view.wantsLayer = YES;
    }

    config->platform = NGL_PLATFORM_MACOS;
    config->window = (uintptr_t)view;
    return 0;
}
