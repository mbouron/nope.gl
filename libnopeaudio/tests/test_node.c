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

#include <stdio.h>

#include "nopeaudio/nopeaudio.h"

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        return -1;                                                           \
    }                                                                        \
} while (0)

struct state {
    uint32_t resets;
    uint32_t destroys;
};

static int process(void *user, const struct nau_block *block)
{
    for (uint32_t i = 0; i < block->frames * block->channels; i++)
        block->output[i] = 1.f;
    return NAU_OK;
}

static void reset(void *user, int64_t frame)
{
    struct state *state = user;
    state->resets++;
}

static void destroy(void *user)
{
    struct state *state = user;
    state->destroys++;
}

static int read_source(void *user, int64_t frame, uint32_t frames, float *output)
{
    for (uint32_t i = 0; i < frames * 2; i++)
        output[i] = 1.f;
    return NAU_OK;
}

static int render(struct nau_engine *engine, int64_t frame, float expected)
{
    float output[16];
    CHECK(nau_engine_render(engine, frame, 8, output) == NAU_OK);
    for (uint32_t i = 0; i < 16; i++)
        CHECK(output[i] == expected);
    return 0;
}

static int test_lifetime(struct nau_engine *engine)
{
    struct nau_node *node = nau_node_create();
    CHECK(node);
    struct nau_route route = {.node = node};
    CHECK(nau_engine_commit(engine, &route, 1, 0) == NAU_ERROR_INVALID);
    CHECK(nau_node_set_origin(node, 0) == NAU_ERROR_INVALID);
    nau_node_unrefp(&node);
    CHECK(!node);
    nau_node_unrefp(&node);
    nau_node_unrefp(NULL);

    node = nau_node_create();
    CHECK(node);
    struct state original = {0}, rejected = {0};
    struct nau_processor processor = {
        .user = &original, .reset = reset, .destroy = destroy,
    };
    CHECK(nau_node_init(node, engine, &processor) == NAU_ERROR_INVALID);
    CHECK(original.destroys == 0);
    processor.process = process;
    CHECK(nau_node_init(node, NULL, &processor) == NAU_ERROR_INVALID);
    CHECK(nau_node_init(node, engine, &processor) == NAU_OK);
    processor.user = &rejected;
    CHECK(nau_node_init(node, engine, &processor) == NAU_ERROR_INVALID);
    const struct nau_source source = {
        .user = &rejected, .read = read_source, .destroy = destroy, .duration = 128,
    };
    CHECK(nau_source_init(node, engine, &source) == NAU_ERROR_INVALID);
    CHECK(nau_mix_init(node, engine, 0) == NAU_ERROR_INVALID);
    CHECK(nau_gain_init(node, engine, 2.f, 0) == NAU_ERROR_INVALID);
    CHECK(nau_fader_init(node, engine) == NAU_ERROR_INVALID);
    CHECK(nau_effect_init(node, engine, NAU_EFFECT_PASSTHROUGH, 0) == NAU_ERROR_INVALID);
    CHECK(original.destroys == 0 && rejected.destroys == 0);

    route.node = node;
    CHECK(nau_engine_commit(engine, &route, 1, 0) == NAU_OK);
    nau_node_unrefp(&node);
    CHECK(original.destroys == 0);
    CHECK(render(engine, 0, 1.f) == 0);
    CHECK(original.resets == 1 && rejected.resets == 0);
    CHECK(nau_engine_commit(engine, NULL, 0, 0) == NAU_OK);
    CHECK(original.destroys == 0);
    CHECK(render(engine, 8, 0.f) == 0);
    CHECK(nau_engine_collect(engine) == NAU_OK);
    CHECK(original.destroys == 1 && rejected.destroys == 0);
    return 0;
}

static int test_builtin_nodes(struct nau_engine *engine)
{
    struct nau_node *nodes[5];
    for (uint32_t i = 0; i < 5; i++) {
        nodes[i] = nau_node_create();
        CHECK(nodes[i]);
    }
    struct state source_state = {0};
    struct nau_source source = {
        .user = &source_state, .read = read_source, .destroy = destroy, .duration = -1,
    };
    CHECK(nau_source_init(nodes[0], engine, &source) == NAU_ERROR_INVALID);
    CHECK(source_state.destroys == 0);
    source.duration = 128;
    CHECK(nau_source_init(nodes[0], engine, &source) == NAU_OK);
    CHECK(nau_gain_init(nodes[1], engine, -1.f, 0) == NAU_ERROR_INVALID);
    CHECK(nau_gain_init(nodes[1], engine, 2.f, 0) == NAU_OK);
    CHECK(nau_fader_init(nodes[2], NULL) == NAU_ERROR_INVALID);
    CHECK(nau_fader_init(nodes[2], engine) == NAU_OK);
    CHECK(nau_effect_init(nodes[3], NULL, NAU_EFFECT_PASSTHROUGH, 0) == NAU_ERROR_INVALID);
    CHECK(nau_effect_init(nodes[3], engine, NAU_EFFECT_PASSTHROUGH, 0) == NAU_OK);
    CHECK(nau_mix_init(nodes[4], engine, 65) == NAU_ERROR_INVALID);
    CHECK(nau_mix_init(nodes[4], engine, 1) == NAU_OK);
    const uint32_t inputs[] = {0, 1, 2, 3};
    const struct nau_route routes[] = {
        {nodes[0], NULL}, {nodes[1], &inputs[0]}, {nodes[2], &inputs[1]},
        {nodes[3], &inputs[2]}, {nodes[4], &inputs[3]},
    };
    struct nau_ramp ramp = {.value = 0.25f};
    CHECK(nau_fader_set_ramps(nodes[2], &ramp, 1) == NAU_OK);
    CHECK(nau_engine_commit(engine, routes, 5, 4) == NAU_OK);

    /* Editing after commit must not change even a snapshot awaiting adoption. */
    ramp.value = 0.5f;
    CHECK(nau_fader_set_ramps(nodes[2], &ramp, 1) == NAU_OK);
    CHECK(nau_source_set_range(nodes[0], 64, 0, 64) == NAU_OK);
    CHECK(render(engine, 0, 0.5f) == 0);

    /* A failed plan must leave the active graph intact and allow a retry. */
    const uint32_t self_input = 0;
    const struct nau_route cycle = {nodes[4], &self_input};
    CHECK(nau_engine_commit(engine, &cycle, 1, 0) == NAU_ERROR_INVALID);
    CHECK(render(engine, 8, 0.5f) == 0);
    CHECK(nau_engine_commit(engine, routes, 5, 4) == NAU_OK);
    CHECK(render(engine, 16, 0.f) == 0);
    CHECK(render(engine, 64, 1.f) == 0);
    CHECK(nau_engine_collect(engine) == NAU_OK);

    /* The active plan retains nodes and their snapshots after caller release. */
    for (uint32_t i = 0; i < 5; i++)
        nau_node_unrefp(&nodes[i]);
    CHECK(source_state.destroys == 0);
    CHECK(render(engine, 72, 1.f) == 0);
    CHECK(nau_engine_commit(engine, NULL, 0, 0) == NAU_OK);
    CHECK(render(engine, 80, 0.f) == 0);
    CHECK(nau_engine_collect(engine) == NAU_OK);
    CHECK(source_state.destroys == 1);
    return 0;
}

int main(void)
{
    const struct nau_engine_config config = {48000, 2, 8};
    struct nau_engine *engine = nau_engine_create();
    CHECK(engine);
    CHECK(nau_engine_init(engine, &config) == NAU_OK);
    int ret = test_lifetime(engine);
    if (!ret)
        ret = test_builtin_nodes(engine);
    nau_engine_freep(&engine);
    return ret != 0;
}
