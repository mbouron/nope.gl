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
 * The create()/init() contract stated in nopeaudio.h: uninitialized objects
 * reject the operations returning a status with NAU_ERROR_INVALID and read as
 * zero, freep()/unrefp() accept them, init() rejects an initialized object,
 * and a failed init() can be retried on the same allocation.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "nopeaudio/nopeaudio.h"

static const struct nau_engine_config engine_config = {.sample_rate = 48000, .channels = 2, .block_frames = 256};

static void test_engine(void)
{
    struct nau_engine *e = nau_engine_create();
    assert(e);
    assert(!nau_engine_get_config(e));
    assert(nau_engine_collect(e) == NAU_ERROR_INVALID);
    assert(nau_engine_commit(e, NULL, 0, 0) == NAU_ERROR_INVALID);
    float out[2 * 16];
    assert(nau_engine_render(e, 0, 16, out) == NAU_ERROR_INVALID);

    struct nau_node *node = nau_node_create();
    assert(node);
    assert(nau_gain_init(node, e, 1.f, 0) == NAU_ERROR_INVALID);
    assert(nau_fader_init(node, e) == NAU_ERROR_INVALID);
    assert(nau_mix_init(node, e, 2) == NAU_ERROR_INVALID);
    assert(nau_effect_init(node, e, NAU_EFFECT_PASSTHROUGH, 0) == NAU_ERROR_INVALID);
    assert(nau_node_set_origin(node, 0) == NAU_ERROR_INVALID);

    /* Failed initializations, then a retry on the same allocation */
    assert(nau_engine_init(e, NULL) == NAU_ERROR_INVALID);
    const struct nau_engine_config bad = {.sample_rate = 48000, .channels = 0, .block_frames = 256};
    assert(nau_engine_init(e, &bad) == NAU_ERROR_INVALID);
    assert(!nau_engine_get_config(e));
    assert(nau_engine_init(e, &engine_config) == NAU_OK);
    assert(nau_engine_get_config(e)->sample_rate == 48000);
    assert(nau_engine_init(e, &engine_config) == NAU_ERROR_INVALID);

    /* The node rejected above can still be initialized */
    assert(nau_gain_init(node, e, 1.f, 0) == NAU_OK);
    assert(nau_gain_init(node, e, 1.f, 0) == NAU_ERROR_INVALID);
    const struct nau_route route = {node, (const uint32_t[]){NAU_INPUT_NONE}};
    assert(nau_engine_commit(e, &route, 1, 0) == NAU_OK);
    assert(nau_engine_render(e, 0, 16, out) == NAU_OK);
    nau_node_unrefp(&node);
    nau_engine_freep(&e);
    assert(!e);

    /* Uninitialized objects are released */
    e = nau_engine_create();
    node = nau_node_create();
    nau_node_unrefp(&node);
    nau_engine_freep(&e);
    assert(!e && !node);
    nau_engine_freep(NULL);
    nau_node_unrefp(NULL);
}

static void test_transport_and_sink(void)
{
    struct nau_engine *uninit_engine = nau_engine_create();
    struct nau_engine *e = nau_engine_create();
    assert(nau_engine_init(e, &engine_config) == NAU_OK);

    struct nau_transport *t = nau_transport_create();
    assert(t);
    assert(!nau_transport_get_engine(t));
    assert(!nau_transport_is_playing(t) && !nau_transport_is_looping(t));
    assert(nau_transport_get_duration(t) == 0 && nau_transport_get_position(t) == 0);
    nau_transport_play(t);
    nau_transport_set_looping(t, 1);
    assert(!nau_transport_is_playing(t) && !nau_transport_is_looping(t));
    assert(nau_transport_seek(t, 0) == NAU_ERROR_INVALID);
    assert(nau_transport_set_duration(t, 10) == NAU_ERROR_INVALID);
    assert(nau_transport_set_loop_range(t, 0, 10) == NAU_ERROR_INVALID);
    assert(nau_transport_set_loop_hold(t, 0) == NAU_ERROR_INVALID);
    struct nau_clock clock;
    assert(nau_transport_get_clock(t, &clock) == NAU_ERROR_INVALID);
    int64_t position;
    assert(nau_transport_get_position_at(t, 0, &position) == NAU_ERROR_INVALID);
    float out[2 * 16];
    assert(nau_transport_render(t, 0, 16, out) == NAU_ERROR_INVALID);
    nau_transport_sync(t);

    /* A sink on an uninitialized transport is refused, then retried */
    struct nau_sink *s = nau_sink_create();
    assert(s);
    assert(nau_sink_start(s) == NAU_ERROR_INVALID);
    assert(nau_sink_stop(s) == NAU_ERROR_INVALID);
    assert(nau_sink_get_latency(s) == 0 && nau_sink_get_underruns(s) == 0);
    assert(nau_sink_get_status(s) == NAU_ERROR_INVALID);
    const struct nau_sink_config null_config = {.null_device = 1};
    assert(nau_sink_init(s, t, &null_config) == NAU_ERROR_INVALID);
    assert(nau_sink_init(s, NULL, &null_config) == NAU_ERROR_INVALID);

    const struct nau_transport_config config = {.declick_frames = 240, .ready_timeout_frames = 12000};
    assert(nau_transport_init(t, uninit_engine, &config) == NAU_ERROR_INVALID);
    assert(nau_transport_init(t, e, NULL) == NAU_ERROR_INVALID);
    assert(nau_transport_init(t, e, &config) == NAU_OK);
    assert(nau_transport_get_engine(t) == e);
    assert(nau_transport_init(t, e, &config) == NAU_ERROR_INVALID);
    assert(nau_transport_set_duration(t, 48000) == NAU_OK);

    assert(nau_sink_init(s, t, &null_config) == NAU_OK);
    assert(nau_sink_init(s, t, &null_config) == NAU_ERROR_INVALID);
    assert(nau_sink_get_status(s) == NAU_OK);
    assert(nau_sink_start(s) == NAU_OK);
    assert(nau_sink_stop(s) == NAU_OK);
    nau_sink_freep(&s);
    assert(!s);

    s = nau_sink_create();
    nau_sink_freep(&s);
    nau_sink_freep(NULL);
    nau_transport_freep(&t);
    t = nau_transport_create();
    nau_transport_freep(&t);
    nau_transport_freep(NULL);
    assert(!t);
    nau_engine_freep(&e);
    nau_engine_freep(&uninit_engine);
}

#if NAU_HAVE_NOPEMEDIA
static void test_media(const char *fixture)
{
    struct nau_engine *e = nau_engine_create();
    assert(nau_engine_init(e, &engine_config) == NAU_OK);

    struct nau_media *m = nau_media_create();
    assert(m);
    assert(nau_media_get_length(m) == 0);
    struct nau_media_stats stats;
    memset(&stats, 0xff, sizeof(stats));
    nau_media_get_stats(m, &stats);
    assert(!stats.silent_frames && !stats.skipped_frames && !stats.requests && !stats.errors);
    assert(nau_media_set_loop(m, 0, 10) == NAU_ERROR_INVALID);
    struct nau_node *node = nau_node_create();
    assert(nau_media_init_source(m, node, 0, 0, 10) == NAU_ERROR_INVALID);

    /* Failed initializations (no file, then a file which is not media), then
     * a retry on the same allocation */
    assert(nau_media_init(m, e, NULL, NULL) == NAU_ERROR_INVALID);
    assert(nau_media_init(m, e, "/nonexistent/file.wav", NULL) < 0);
    assert(nau_media_get_length(m) == 0);
    assert(nau_media_init(m, e, fixture, NULL) == NAU_OK);
    assert(nau_media_get_length(m) > 0);
    assert(nau_media_init(m, e, fixture, NULL) == NAU_ERROR_INVALID);
    assert(nau_media_set_loop(m, 0, 10) == NAU_OK);
    assert(nau_media_init_source(m, node, 0, 0, 10) == NAU_OK);
    nau_node_unrefp(&node);
    nau_media_unrefp(&m);
    assert(!m);

    m = nau_media_create();
    nau_media_unrefp(&m);
    nau_media_unrefp(NULL);
    nau_engine_freep(&e);
}

/* Stands for an effect the player does not have: the player only compares it */
static max_align_t foreign_effect;

static void test_player(const char *fixture, const char *export_path)
{
    struct nau_player *p = nau_player_create();
    assert(p);
    assert(nau_player_get_sample_rate(p) == 0);
    assert(nau_player_get_volume(p) == 0.f);
    assert(nau_player_get_position(p) == 0 && nau_player_get_duration(p) == 0);
    assert(!nau_player_is_playing(p) && !nau_player_is_looping(p));
    assert(nau_player_play(p) == NAU_ERROR_INVALID);
    assert(nau_player_pause(p) == NAU_ERROR_INVALID);
    assert(nau_player_stop(p) == NAU_ERROR_INVALID);
    assert(nau_player_seek(p, 0) == NAU_ERROR_INVALID);
    assert(nau_player_set_duration(p, 10) == NAU_ERROR_INVALID);
    assert(nau_player_set_volume(p, 1.f) == NAU_ERROR_INVALID);
    assert(nau_player_clear_tracks(p) == NAU_ERROR_INVALID);
    struct nau_track_effect *effect = (struct nau_track_effect *)&foreign_effect;
    assert(nau_player_set_track_effect_sidechain(p, effect, NULL, 0) == NAU_ERROR_INVALID);
    assert(nau_player_export(p, export_path, NAU_EXPORT_S16) == NAU_ERROR_INVALID);
    struct nau_track *t = (struct nau_track *)1;
    const struct nau_track_params params = {.filename = fixture, .duration = -1, .volume = 1.f};
    assert(nau_player_add_track(p, &params, &t) == NAU_ERROR_INVALID && !t);
    struct nau_player_stats stats;
    memset(&stats, 0xff, sizeof(stats));
    nau_player_get_stats(p, &stats);
    assert(!stats.underruns && !stats.silent_frames && !stats.skipped_frames && !stats.requests && !stats.errors);

    /* Failed initializations, including one failing midway (a device player
     * without nau_sink_backend_init() on SDL), then a retry */
    const struct nau_player_config bad = {.mode = (enum nau_player_mode)42};
    assert(nau_player_init(p, &bad) == NAU_ERROR_INVALID);
    assert(nau_player_init(p, NULL) == NAU_ERROR_INVALID);
    const struct nau_player_config device = {.mode = NAU_PLAYER_DEVICE};
    const int device_ret = nau_player_init(p, &device);
    if (device_ret == NAU_OK) {
        nau_player_freep(&p);
        p = nau_player_create();
    } else {
        assert(nau_player_get_sample_rate(p) == 0);
        assert(nau_player_play(p) == NAU_ERROR_INVALID);
    }
    const struct nau_player_config offline = {.mode = NAU_PLAYER_OFFLINE};
    assert(nau_player_init(p, &offline) == NAU_OK);
    assert(nau_player_init(p, &offline) == NAU_ERROR_INVALID);
    assert(nau_player_get_sample_rate(p) == 48000);
    assert(nau_player_get_volume(p) == 1.f);
    assert(nau_player_add_track(p, &params, &t) == NAU_OK && t);
    assert(nau_player_set_duration(p, 4800) == NAU_OK);
    assert(nau_player_export(p, export_path, NAU_EXPORT_S16) == NAU_OK);
    nau_player_freep(&p);
    assert(!p);

    p = nau_player_create();
    nau_player_freep(&p);
    nau_player_freep(NULL);
}
#endif

int main(int argc, char **argv)
{
    test_engine();
    test_transport_and_sink();
#if NAU_HAVE_NOPEMEDIA
    assert(argc >= 3);
    test_media(argv[1]);
    test_player(argv[1], argv[2]);
#else
    (void)argc;
    (void)argv;
#endif
    return 0;
}
