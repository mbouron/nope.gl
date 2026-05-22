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

#ifndef VIEWER_SCENE_H
#define VIEWER_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <SDL3/SDL.h>

#include <nopegl/nopegl.h>

struct viewer_ctx;

struct scene_load_request {
    char script_path[2048];
    char scene_name[2048];
};

struct scene_load_result {
    bool success;
    char *error;
    double duration;
    int32_t framerate[2];
};

void scene_load_result_freep(struct scene_load_result **rp);

enum scene_cmd_type {
    SCENE_CMD_LOAD,
    SCENE_CMD_RESIZE,
    SCENE_CMD_UNLOAD,
    /*
     * Synchronous snapshot request: the scene thread ngl_scene_duplicate's
     * its currently-loaded scene, stores the result in *snapshot.out, and
     * signals snapshot.done. *snapshot.out is set to NULL if no scene is
     * loaded or if the command is dropped (queue full, queue destroyed)
     * — the waiter always wakes. Typically posted from the export worker
     * thread, which blocks on snapshot.done.
     */
    SCENE_CMD_SNAPSHOT,
    /*
     * Fire-and-forget callback dispatched onto the scene thread. Used by the
     * UI thread to mutate the live scene graph on the thread that owns the
     * rendering context.
     */
    SCENE_CMD_CALLBACK,
    /*
     * Render request: the scene thread calls ngl_draw for the given
     * target_time and publishes the resulting frame into latest_frame.
     */
    SCENE_CMD_RENDER,
    SCENE_CMD_QUIT,
};

struct scene_cmd {
    enum scene_cmd_type type;
    union {
        struct {
            struct scene_load_request *request; /* Owned. */
        } load;
        struct {
            uint32_t width;
            uint32_t height;
        } resize;
        struct {
            struct ngl_scene **out;
            SDL_Semaphore *done;
        } snapshot;
        struct {
            void (*fn)(struct viewer_ctx *s, void *arg);
            void (*free_fn)(void *arg); /* Optional. */
            void *arg;
        } callback;
        struct {
            double target_time;
        } render;
    };
};

#define SCENE_CMD_QUEUE_CAP 16

struct scene_cmd_queue {
    SDL_Mutex *lock;
    SDL_Condition *cond;
    struct scene_cmd ring[SCENE_CMD_QUEUE_CAP];
    size_t head;
    size_t count;
    int    quit;
};

int  scene_cmd_queue_init(struct scene_cmd_queue *q);
void scene_cmd_queue_destroy(struct scene_cmd_queue *q);
void scene_cmd_post(struct scene_cmd_queue *q, struct scene_cmd cmd);

int scene_thread_func(void *arg);

void viewer_request_load(struct viewer_ctx *s);
void viewer_load_script(struct viewer_ctx *s, const char *path);

Sint64 viewer_scene_get_file_mtime(const char *path);

int viewer_scene_init(struct viewer_ctx *s, uint32_t width, uint32_t height);
void viewer_scene_close(struct viewer_ctx *s);

#endif
