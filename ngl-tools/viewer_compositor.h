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

#ifndef VIEWER_COMPOSITOR_H
#define VIEWER_COMPOSITOR_H

#include <stdint.h>

struct viewer_ctx;
struct ngpu_ctx;

int viewer_compositor_init(struct viewer_ctx *s,
                           struct ngpu_ctx *shared_gpu_ctx,
                           uintptr_t wsi_window);

void viewer_compositor_close(struct viewer_ctx *s);

/*
 * Render one frame:
 *  1. Convert Nuklear draw commands.
 *  2. Pick up the latest frame from the scene thread (if any) and replace
 *     the held one, returning the old frame + its blit_done fence to ngl.
 *  3. Open a render pass on the default rendertarget.
 *  4. Submit Nuklear UI commands.
 *  5. Blit the scene texture inside the preview panel bounds. No
 *     letterboxing — the scene is rendered at exactly preview_w × preview_h.
 *  6. Close the render pass and submit; stash the new blit_done fence for
 *     the next iteration (queue ordering makes the previous one redundant).
 */
void viewer_compositor_render(struct viewer_ctx *s);

#endif
