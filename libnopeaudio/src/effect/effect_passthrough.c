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

/*
 * Passthrough effect, usable as a placeholder in an effect chain.
 */

#include <string.h>

#include "nopeaudio/nopeaudio.h"
#include "effect/effect.h"

static void passthrough_render(void *state, const float *params, const struct nau_block *chunk)
{
    memcpy(chunk->output, chunk->inputs[0], (size_t)chunk->frames * chunk->channels * sizeof(*chunk->output));
}

const struct nau_effect_class nau_effect_passthrough = {
    .info = {.name = "passthrough"},
    .render = passthrough_render,
};
