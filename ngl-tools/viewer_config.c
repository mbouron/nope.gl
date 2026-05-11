/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <string.h>

#include <SDL3/SDL.h>

#include "viewer_config.h"

#define CONFIG_KEY_SCRIPT      "script"
#define CONFIG_KEY_SCENE       "scene"
#define CONFIG_KEY_PANEL_RATIO "panel_ratio"

static int config_file_path(char *out, size_t out_size)
{
    char *pref = SDL_GetPrefPath("nopeforge", "ngl-viewer");
    if (!pref)
        return -1;
    int r = SDL_snprintf(out, out_size, "%slast", pref);
    SDL_free(pref);
    if (r < 0 || (size_t)r >= out_size)
        return -1;
    return 0;
}

void config_save(const struct config_data *cfg)
{
    char path[2560];
    if (config_file_path(path, sizeof(path)) < 0)
        return;

    SDL_IOStream *io = SDL_IOFromFile(path, "w");
    if (!io)
        return;

    const struct { const char *key; const char *value; } fields[] = {
        {CONFIG_KEY_SCRIPT, cfg->script_path},
        {CONFIG_KEY_SCENE,  cfg->scene_name},
    };

    for (size_t i = 0; i < SDL_arraysize(fields); i++)
        if (fields[i].value[0])
            SDL_IOprintf(io, "%s=%s\n", fields[i].key, fields[i].value);

    if (cfg->panel_ratio > 0.0f)
        SDL_IOprintf(io, "%s=%.4f\n", CONFIG_KEY_PANEL_RATIO, cfg->panel_ratio);

    SDL_CloseIO(io);
}

static int config_read_kv(const char *line, const char *key, char *out, size_t out_size)
{
    const size_t klen = strlen(key);
    if (strncmp(line, key, klen) != 0 || line[klen] != '=')
        return 0;
    SDL_snprintf(out, out_size, "%s", line + klen + 1);
    return 1;
}

void config_load(struct config_data *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    char path[2560];
    if (config_file_path(path, sizeof(path)) < 0)
        return;

    size_t size = 0;
    char *data = SDL_LoadFile(path, &size);
    if (!data)
        return;

    const struct { const char *key; char *out; size_t out_size; } fields[] = {
        {CONFIG_KEY_SCRIPT, cfg->script_path, sizeof(cfg->script_path)},
        {CONFIG_KEY_SCENE,  cfg->scene_name,  sizeof(cfg->scene_name)},
    };

    char *line = data;
    while (*line) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            /* Tolerate CRLF line endings on configs round-tripped through Windows. */
            if (nl > line && nl[-1] == '\r')
                nl[-1] = '\0';
        }

        for (size_t i = 0; i < SDL_arraysize(fields); i++)
            if (config_read_kv(line, fields[i].key, fields[i].out, fields[i].out_size))
                break;

        /* Non-string keys handled separately. */
        char buf[32];
        if (config_read_kv(line, CONFIG_KEY_PANEL_RATIO, buf, sizeof(buf)))
            cfg->panel_ratio = (float)SDL_atof(buf);

        if (!nl)
            break;
        line = nl + 1;
    }

    SDL_free(data);
}
