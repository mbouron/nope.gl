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

#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "viewer.h"
#include "viewer_python.h"

void scene_load_result_freep(struct scene_load_result **rp)
{
    struct scene_load_result *r = *rp;
    if (!r)
        return;
    SDL_free(r->error);
    SDL_free(r);
    *rp = NULL;
}

static void push_load_result_event(struct viewer_ctx *s, struct scene_load_result **rp)
{
     /* Take ownership of *r. */
    struct scene_load_result *r = *rp;
    *rp = NULL;

    SDL_Event ev = {0};
    ev.type = s->scene_loaded_event;
    ev.user.data1 = r;
    if (!SDL_PushEvent(&ev))
        scene_load_result_freep(&r);
}

/*
 * Release ownership of any heap state attached to a command. For SNAPSHOT
 * this also wakes the waiter with a NULL result, which is the right thing
 * whenever the command is dropped without being served: queue full at post
 * time, queue destroyed at shutdown.
 */
static void scene_cmd_release(struct scene_cmd *cmd)
{
    if (cmd->type == SCENE_CMD_LOAD) {
        SDL_free(cmd->load.request);
        cmd->load.request = NULL;
    } else if (cmd->type == SCENE_CMD_SNAPSHOT) {
        if (cmd->snapshot.out)
            *cmd->snapshot.out = NULL;
        if (cmd->snapshot.done)
            SDL_SignalSemaphore(cmd->snapshot.done);
        cmd->snapshot.out  = NULL;
        cmd->snapshot.done = NULL;
    } else if (cmd->type == SCENE_CMD_CALLBACK) {
        if (cmd->callback.free_fn)
            cmd->callback.free_fn(cmd->callback.arg);
        cmd->callback.fn      = NULL;
        cmd->callback.free_fn = NULL;
        cmd->callback.arg     = NULL;
    }
}

int scene_cmd_queue_init(struct scene_cmd_queue *q)
{
    q->lock = SDL_CreateMutex();
    q->cond = SDL_CreateCondition();
    if (!q->lock || !q->cond)
        return -1;
    q->head  = 0;
    q->count = 0;
    q->quit  = 0;
    return 0;
}

void scene_cmd_queue_destroy(struct scene_cmd_queue *q)
{
    if (!q->lock)
        return;
    for (size_t i = 0; i < q->count; i++)
        scene_cmd_release(&q->ring[(q->head + i) % SCENE_CMD_QUEUE_CAP]);
    q->count = 0;
    SDL_DestroyMutex(q->lock);
    SDL_DestroyCondition(q->cond);
    q->lock = NULL;
    q->cond = NULL;
}

/**
 * Post a command to the queue, taking ownership of any heap state it carries.
 *
 * Commands of the same type are coalesced: if an entry of cmd.type is already
 * queued, it is dropped (and its heap state released) and the new cmd is
 * re-enqueued at the tail. "Latest-intent wins" matters for sequences like
 * LOAD A → UNLOAD → LOAD B, where in-place replacement would yield
 * [LOAD B, UNLOAD] and end up unloaded, the wrong final state.
 *
 * SCENE_CMD_SNAPSHOT is excluded from coalescing because each request carries
 * its own waiter (semaphore + out slot), so dropping a previous snapshot
 * would orphan it.
 *
 * SCENE_CMD_QUIT is a special posting: it sets the quit flag, releases the
 * cmd immediately, and wakes any waiter. The queue contents are left as-is
 * so consumers drain queued commands before observing the synthesized QUIT
 * from scene_cmd_pop_timed().
 *
 * The queue has a fixed capacity (SCENE_CMD_QUEUE_CAP). If it is full when a
 * non-coalescing post arrives, the oldest entry is dropped to make room.
 *
 * @param q   pointer to the command queue
 * @param cmd command to post; any heap state it carries is taken over by the
 *            queue (released either when popped, when coalesced, or when the
 *            queue is destroyed)
 */
void scene_cmd_post(struct scene_cmd_queue *q, struct scene_cmd cmd)
{
    SDL_LockMutex(q->lock);
    if (cmd.type == SCENE_CMD_QUIT) {
        q->quit = 1;
        scene_cmd_release(&cmd);
        SDL_SignalCondition(q->cond);
        SDL_UnlockMutex(q->lock);
        return;
    }
    if (cmd.type != SCENE_CMD_SNAPSHOT && cmd.type != SCENE_CMD_CALLBACK) {
        for (size_t i = 0; i < q->count; i++) {
            const size_t idx = (q->head + i) % SCENE_CMD_QUEUE_CAP;
            if (q->ring[idx].type != cmd.type)
                continue;
            scene_cmd_release(&q->ring[idx]);
            for (size_t j = i; j + 1 < q->count; j++) {
                const size_t cur  = (q->head + j)     % SCENE_CMD_QUEUE_CAP;
                const size_t next = (q->head + j + 1) % SCENE_CMD_QUEUE_CAP;
                q->ring[cur] = q->ring[next];
            }
            q->ring[(q->head + q->count - 1) % SCENE_CMD_QUEUE_CAP] = cmd;
            SDL_SignalCondition(q->cond);
            SDL_UnlockMutex(q->lock);
            return;
        }
    }
    if (q->count == SCENE_CMD_QUEUE_CAP) {
        scene_cmd_release(&q->ring[q->head]);
        q->head = (q->head + 1) % SCENE_CMD_QUEUE_CAP;
        q->count--;
    }
    q->ring[(q->head + q->count) % SCENE_CMD_QUEUE_CAP] = cmd;
    q->count++;
    SDL_SignalCondition(q->cond);
    SDL_UnlockMutex(q->lock);
}

/**
 * Pop a command from the queue, with an optional bounded wait.
 *
 * SCENE_CMD_QUIT is synthesized only after the queue has been fully drained
 * so the caller observes every queued command before shutdown.
 *
 * @param q             pointer to the command queue
 * @param out           pointer to the slot that receives the popped command
 *                      (only written when the call returns 1)
 * @param timeout_ms    is the maximum number of milliseconds to wait for a
 *                      command to arrive. If timeout_ms is zero, the function
 *                      does not wait, but simply returns whatever is currently
 *                      in the queue. A negative value is treated as infinity
 *                      and the function will not return until a command
 *                      arrives or quit is set.
 *
 * @return 1 if *out was set (either a real command or a synthesized
 *         SCENE_CMD_QUIT once the queue has drained and quit is set),
 *         0 on timeout with nothing to deliver
 */
static int scene_cmd_pop_timed(struct scene_cmd_queue *q, struct scene_cmd *out, Sint32 timeout_ms)
{
    SDL_LockMutex(q->lock);
    if (q->count == 0 && !q->quit) {
        if (timeout_ms < 0) {
            while (q->count == 0 && !q->quit)
                SDL_WaitCondition(q->cond, q->lock);
        } else if (timeout_ms > 0) {
            SDL_WaitConditionTimeout(q->cond, q->lock, timeout_ms);
        }
    }
    int got = 0;
    if (q->count > 0) {
        *out = q->ring[q->head];
        q->ring[q->head] = (struct scene_cmd){0};
        q->head = (q->head + 1) % SCENE_CMD_QUEUE_CAP;
        q->count--;
        got = 1;
    } else if (q->quit) {
        *out = (struct scene_cmd){.type = SCENE_CMD_QUIT};
        got = 1;
    }
    SDL_UnlockMutex(q->lock);
    return got;
}

Sint64 viewer_scene_get_file_mtime(const char *path)
{
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info))
        return 0;
    return info.modify_time;
}

void viewer_request_load(struct viewer_ctx *s)
{
    if (s->selected_scene < 0 || (size_t)s->selected_scene >= s->nb_scenes)
        return;
    struct scene_load_request *req = SDL_malloc(sizeof(*req));
    if (!req)
        abort();
    snprintf(req->script_path, sizeof(req->script_path), "%s", s->script_path);
    snprintf(req->scene_name,  sizeof(req->scene_name),  "%s", s->scene_names[s->selected_scene]);
    s->script_mtime = viewer_scene_get_file_mtime(s->script_path);
    scene_cmd_post(&s->cmd_q, (struct scene_cmd){
        .type = SCENE_CMD_LOAD,
        .load = {.request = req},
    });
}

void viewer_load_script(struct viewer_ctx *s, const char *path)
{
    if (!path || !path[0])
        return;
    snprintf(s->script_path, sizeof(s->script_path), "%s", path);
    viewer_python_free_scenes(s->scene_names, s->nb_scenes);
    s->scene_names = NULL;
    s->nb_scenes = 0;
    s->selected_scene = -1;
    s->scene_loaded = 0;
    if (s->selected_node)
        ngl_node_unrefp(&s->selected_node);
    scene_cmd_post(&s->cmd_q, (struct scene_cmd){.type = SCENE_CMD_UNLOAD});
    s->last_error[0] = '\0';
    viewer_python_list_scenes(s->script_path, &s->scene_names, &s->nb_scenes,
                              s->last_error, sizeof(s->last_error));
    if (s->nb_scenes > 0) {
        s->selected_scene = 0;
        viewer_request_load(s);
    }
}

static struct ngl_scene *do_scene_load(struct viewer_ctx *s, const struct scene_load_request *req)
{
    struct scene_load_result *r = SDL_calloc(1, sizeof(*r));
    if (!r)
        abort();

    char err[4096] = {0};
    struct ngl_scene *scene = viewer_python_build_scene(
        req->script_path, req->scene_name, err, sizeof(err));

    if (!scene) {
        r->error = SDL_strdup(err);
        if (!r->error)
            abort();
        push_load_result_event(s, &r);
        return NULL;
    }

    int set_ret = ngl_set_scene(s->ngl_ctx, scene);
    if (set_ret < 0) {
        if (SDL_asprintf(&r->error, "ngl_set_scene failed (ret=%d)", set_ret) < 0)
            abort();
        ngl_scene_unrefp(&scene);
        push_load_result_event(s, &r);
        return NULL;
    }

    const struct ngl_scene_params *p = ngl_scene_get_params(scene);
    r->success      = true;
    r->duration     = p->duration;
    r->framerate[0] = p->framerate[0];
    r->framerate[1] = p->framerate[1];

    push_load_result_event(s, &r);
    return scene;
}

#define SCENE_DEFAULT_FPS_NUM 60
#define SCENE_DEFAULT_FPS_DEN 1

int scene_thread_func(void *arg)
{
    struct viewer_ctx *s = arg;

    bool is_active = false;
    uint32_t scene_width = 0;
    uint32_t scene_height = 0;
    struct ngl_scene *active_scene = NULL;
    int32_t framerate_num = SCENE_DEFAULT_FPS_NUM;
    int32_t framerate_den = SCENE_DEFAULT_FPS_DEN;

    for (;;) {
        struct scene_cmd cmd;
        if (!scene_cmd_pop_timed(&s->cmd_q, &cmd, -1))
            continue; /* Defensive: timeout=-1 should always deliver something. */

        switch (cmd.type) {
        case SCENE_CMD_QUIT:
            scene_cmd_release(&cmd);
            ngl_scene_unrefp(&active_scene);
            return 0;
        case SCENE_CMD_LOAD:
            if (cmd.load.request) {
                struct ngl_scene *loaded = do_scene_load(s, cmd.load.request);
                if (loaded) {
                    ngl_scene_unrefp(&active_scene);
                    active_scene = loaded;
                    is_active = true;
                    const struct ngl_scene_params *p = ngl_scene_get_params(loaded);
                    framerate_num = p->framerate[0];
                    framerate_den = p->framerate[1];
                }
            }
            break;
        case SCENE_CMD_RESIZE:
            scene_width  = cmd.resize.width;
            scene_height = cmd.resize.height;
            break;
        case SCENE_CMD_UNLOAD:
            is_active = false;
            ngl_scene_unrefp(&active_scene);
            break;
        case SCENE_CMD_SNAPSHOT: {
            struct ngl_scene *snap = NULL;
            if (active_scene) {
                const Uint64 t0 = SDL_GetTicksNS();
                snap = ngl_scene_duplicate(active_scene);
                const Uint64 t1 = SDL_GetTicksNS();
                SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                             "ngl_scene_duplicate: %.3f ms",
                             (double)(t1 - t0) / 1.0e6);
            }
            if (cmd.snapshot.out)
                *cmd.snapshot.out = snap;
            if (cmd.snapshot.done)
                SDL_SignalSemaphore(cmd.snapshot.done);
            cmd.snapshot.out  = NULL;
            cmd.snapshot.done = NULL;
            break;
        }
        case SCENE_CMD_CALLBACK:
            if (cmd.callback.fn)
                cmd.callback.fn(s, cmd.callback.arg);
            break;
        case SCENE_CMD_RENDER: {
            if (!is_active)
                break;

            double t = cmd.render.target_time;
            if (framerate_num > 0 && framerate_den > 0) {
                const int64_t idx = (int64_t)(t * (double)framerate_num / (double)framerate_den);
                t = (double)idx * (double)framerate_den / (double)framerate_num;
            }

            if (scene_width >= 2 && scene_height >= 2) {
                uint32_t rt_width = 0;
                uint32_t rt_height = 0;
                struct ngpu_ctx *gpu_ctx = ngl_get_gpu_ctx(s->ngl_ctx);
                if (gpu_ctx)
                    ngpu_ctx_get_default_rendertarget_size(gpu_ctx, &rt_width, &rt_height);
                if (rt_width != scene_width || rt_height != scene_height)
                    ngl_resize(s->ngl_ctx, scene_width, scene_height);
            }

            struct ngl_draw_output output = {0};
            int draw_ret = ngl_draw(s->ngl_ctx, t, &output);
            if (draw_ret < 0)
                break;

            SDL_LockMutex(s->frame_lock);
            struct ngl_frame *previous_frame = s->latest_frame;
            s->latest_frame = output.frame;
            SDL_UnlockMutex(s->frame_lock);

            if (previous_frame)
                ngl_frame_release(previous_frame, NULL);
            break;
        }
        }

        scene_cmd_release(&cmd);
    }
}

int viewer_scene_init(struct viewer_ctx *s, uint32_t width, uint32_t height)
{
    s->ngl_cfg.offscreen = 1;
    s->ngl_cfg.width     = width;
    s->ngl_cfg.height    = height;

    s->ngl_ctx = ngl_create();
    if (!s->ngl_ctx)
        return -1;

    int ret = ngl_configure(s->ngl_ctx, &s->ngl_cfg);
    if (ret < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to configure ngl context (offscreen)");
        return ret;
    }

    /* Warm-up draw — fully initializes the offscreen pipeline so the first
     * post-load draw doesn't pay the lazy-init cost. */
    ngl_draw(s->ngl_ctx, 0.0, NULL);

    if (scene_cmd_queue_init(&s->cmd_q) < 0)
        return -1;

    s->frame_lock = SDL_CreateMutex();
    if (!s->frame_lock) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create frame_lock");
        return -1;
    }

    /* Seed the initial render size so the first SET_SCENE draw runs at the
     * right resolution (the queue preserves ordering: RESIZE before LOAD). */
    scene_cmd_post(&s->cmd_q, (struct scene_cmd){
        .type   = SCENE_CMD_RESIZE,
        .resize = {.width = width, .height = height},
    });

    s->scene_thread = SDL_CreateThread(scene_thread_func, "scene", s);
    if (!s->scene_thread) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create scene thread");
        return -1;
    }
    return 0;
}

void viewer_scene_close(struct viewer_ctx *s)
{
    if (s->scene_thread) {
        scene_cmd_post(&s->cmd_q, (struct scene_cmd){.type = SCENE_CMD_QUIT});
        SDL_WaitThread(s->scene_thread, NULL);
        s->scene_thread = NULL;

        SDL_Event drain;
        while (SDL_PeepEvents(&drain, 1, SDL_GETEVENT,
                              s->scene_loaded_event, s->scene_loaded_event) > 0) {
            struct scene_load_result *r = drain.user.data1;
            scene_load_result_freep(&r);
        }

        if (s->latest_frame) {
            ngl_frame_release(s->latest_frame, NULL);
            s->latest_frame = NULL;
        }
        if (s->current_frame) {
            ngl_frame_release(s->current_frame, s->current_blit_done);
            s->current_frame = NULL;
            s->current_blit_done = NULL;
        }
    }

    scene_cmd_queue_destroy(&s->cmd_q);
    if (s->frame_lock) {
        SDL_DestroyMutex(s->frame_lock);
        s->frame_lock = NULL;
    }
    ngl_freep(&s->ngl_ctx);
}
