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

#ifndef NAU_EFFECT_H
#define NAU_EFFECT_H

#include <stddef.h>
#include <stdint.h>

#include "nopeaudio/nopeaudio.h"

#define NAU_EFFECT_MAX_PARAMS 8U

/*
 * Exchange the node's automation with *ramps and *ramp_count without copying.
 * The array must be allocated with utils/memory.h. The caller owns the previous
 * array and can swap it back without allocation if the commit fails.
 */
int nau_effect_swap_automation(struct nau_node *node, uint32_t param, struct nau_ramp **ramps, uint32_t *ramp_count);

/* Parameter evaluation interval, in frames relative to the node's origin. */
#define NAU_EFFECT_CHUNK 16

/*
 * An effect type, registered in effect.c. The shared node code handles targets,
 * ramps and configuration snapshots. render() receives chunks with constant
 * parameter values, evaluated at NAU_EFFECT_CHUNK boundaries relative to the
 * node's origin and clamped to the supported range.
 */
struct nau_effect_class {
    struct nau_effect_info info;    /* at most NAU_EFFECT_MAX_PARAMS parameters */
    size_t state_size;              /* size of the state of the effect, allocated zeroed; 0 for none */

    /* Optional. Restrict a parameter's range for this sample rate. */
    void (*limit)(uint32_t param, uint32_t sample_rate, float *min, float *max);

    /* Optional. Reset state after a jump or when resuming from silence. */
    void (*reset)(void *state);

    /*
     * Render a chunk of at most NAU_EFFECT_CHUNK frames, with the values of
     * the parameters. Runs on the render thread, as nau_processor.process.
     */
    void (*render)(void *state, const float *params, const struct nau_block *chunk);
};

extern const struct nau_effect_class nau_effect_passthrough;

#endif
