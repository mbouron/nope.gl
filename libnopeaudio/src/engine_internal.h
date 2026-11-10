/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef NAU_ENGINE_INTERNAL_H
#define NAU_ENGINE_INTERNAL_H

#include <stdint.h>

#include "nopeaudio/nopeaudio.h"

/*
 * A render plan keeps a snapshot of each processor's configuration, such as
 * its automation ramps. The control thread can edit the original while the
 * render thread uses the snapshot. Persistent state, such as filter history,
 * stays with the processor.
 *
 * take() runs on the control thread during commit and returns a copy of the
 * configuration, or NULL on allocation failure. process() receives this copy
 * instead of user; reset() and destroy() still receive the original user.
 * release() frees the copy on the control thread once the plan is no longer
 * used. The callback table must outlive the node.
 */
struct nau_snapshot_callbacks {
    void *(*take)(void *user);
    void (*release)(void *snapshot);
};

/*
 * Same as nau_node_init(), with optional snapshot callbacks. This only
 * registers the callbacks: snapshots are taken when a graph is committed.
 */
int nau_node_init_with_snapshot_callbacks(struct nau_node *node, struct nau_engine *engine,
                                          const struct nau_processor *processor,
                                          const struct nau_snapshot_callbacks *snapshot_callbacks);

/*
 * Return the node's user argument if its process callback matches, NULL
 * otherwise. Built-in processors use this to check the node type.
 */
void *nau_node_get_user(const struct nau_node *node, int (*process)(void *user, const struct nau_block *p));

/*
 * Same as nau_engine_render(). `wrap` tells that a jump to first_frame is a
 * loop going back to its start. A pending commit is only adopted if `adopt`
 * is set.
 */
int nau_engine_render_jump(struct nau_engine *engine, int64_t first_frame, uint32_t frames,
                           float *output, int wrap, int adopt);

/*
 * Adopt the pending commit and position the nodes at frame so the sources
 * can decode ahead. force also resets nodes already at this position, whose
 * state may be stale after a pause. Render thread only.
 */
void nau_engine_prepare(struct nau_engine *engine, int64_t frame, int wrap, int force);

/*
 * Reset processors without seeking the sources. Called before resuming from
 * silence so gain and parameter changes made while idle apply immediately.
 * Render thread only.
 */
void nau_engine_reset_processors(struct nau_engine *engine, int64_t frame);

/*
 * Return NAU_AGAIN if a source playing in [frame, frame + frames) does not
 * have its data yet, NAU_OK otherwise. The nodes must have been positioned.
 * Render thread only.
 */
int nau_engine_ready(struct nau_engine *engine, int64_t frame, uint32_t frames);

#endif
