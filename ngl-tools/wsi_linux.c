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

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <nopegl/nopegl.h>

#include "wsi.h"

int wsi_set_ngl_config(struct ngl_config *config, SDL_Window *window)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver) {
        fprintf(stderr, "Failed to get video driver name\n");
        return -1;
    }

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props) {
        fprintf(stderr, "Failed to get window properties: %s\n", SDL_GetError());
        return -1;
    }

    if (!strcmp(driver, "wayland")) {
        void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        void *surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (!display || !surface) {
            fprintf(stderr, "Failed to get Wayland handles\n");
            return -1;
        }
        config->platform = NGL_PLATFORM_WAYLAND;
        config->display = (uintptr_t)display;
        config->window = (uintptr_t)surface;
        return 0;
    } else if (!strcmp(driver, "x11")) {
        void *display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        Sint64 xwindow = SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (!display || !xwindow) {
            fprintf(stderr, "Failed to get X11 handles\n");
            return -1;
        }
        config->platform = NGL_PLATFORM_XLIB;
        config->display = (uintptr_t)display;
        config->window = (uintptr_t)xwindow;
        return 0;
    }

    fprintf(stderr, "Unsupported video driver: %s\n", driver);
    return -1;
}
