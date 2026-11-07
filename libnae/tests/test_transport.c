/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nae/nae.h"

/* A mono source whose samples are their frame numbers in the media */
struct numbers {
    int seeks, wraps;
    int64_t last_seek;
    int again;      /* reads left answering AGAIN */
    float value;    /* nonzero: a constant rather than the frame numbers */
};

static int numbers_read(void *user, int64_t frame, uint32_t frames, float *out)
{
    struct numbers *n = user;
    for (uint32_t i = 0; i < frames; i++)
        out[i] = n->value != 0.f ? n->value : (float)(frame + i);
    if (n->again > 0) {
        n->again--;
        return NAE_AGAIN;
    }
    return NAE_OK;
}

static void numbers_seek(void *user, int64_t frame, int wrap)
{
    struct numbers *n = user;
    n->seeks++;
    n->wraps += !!wrap;
    n->last_seek = frame;
}

struct setup {
    struct nae_engine *engine;
    struct nae_node *source;
    struct nae_transport *transport;
};

static struct setup setup(struct numbers *n, int64_t source_start, int64_t duration,
                          uint32_t declick, uint32_t ready_timeout)
{
    struct setup s = {0};
    const struct nae_config c = {48000, 1, 16};
    assert(nae_engine_create(&c, &s.engine) == NAE_OK);
    const struct nae_source source = {
        .user = n, .read = numbers_read, .seek = numbers_seek,
        .timeline_start = 0, .source_start = source_start, .duration = duration,
    };
    assert(nae_source_create(s.engine, &source, &s.source) == NAE_OK);
    const struct nae_route r = {s.source, NULL};
    assert(nae_engine_commit(s.engine, &r, 1, 0) == NAE_OK);
    const struct nae_transport_config tc = {.declick_frames = declick, .ready_timeout_frames = ready_timeout};
    assert(nae_transport_create(s.engine, &tc, &s.transport) == NAE_OK);
    assert(nae_transport_set_duration(s.transport, duration) == NAE_OK);
    return s;
}

static void teardown(struct setup *s)
{
    nae_transport_freep(&s->transport);
    nae_node_unrefp(&s->source);
    nae_engine_freep(&s->engine);
}

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.0001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

static void test_play_to_end(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[80];
    /* Not playing: silence, nothing read, the source positioned for its data to be there on play */
    assert(nae_transport_render(s.transport, 0, 8, out) == NAE_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], 0);
    assert(n.seeks == 1 && n.last_seek == 1000 && n.wraps == 0);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 40, out) == NAE_OK);
    for (int i = 0; i < 40; i++)
        close_to(out[i], (float)(1000 + i));
    assert(n.seeks == 1); /* not positioned twice */
    assert(nae_transport_get_position(s.transport) == 40);
    /* The end: back to the start, stopped, the source positioned there */
    assert(nae_transport_render(s.transport, 0, 80, out) == NAE_OK);
    for (int i = 0; i < 60; i++)
        close_to(out[i], (float)(1040 + i));
    for (int i = 60; i < 80; i++)
        close_to(out[i], 0);
    assert(!nae_transport_is_playing(s.transport));
    assert(nae_transport_get_position(s.transport) == 0);
    assert(n.seeks == 2 && n.last_seek == 1000);
    teardown(&s);
}

static void test_seek(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[16];
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 10, out) == NAE_OK);
    assert(nae_transport_seek(s.transport, 50) == NAE_OK);
    assert(nae_transport_get_position(s.transport) == 50); /* before the renderer applies it */
    assert(nae_transport_render(s.transport, 0, 10, out) == NAE_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], (float)(1050 + i));
    assert(n.last_seek == 1050 && n.wraps == 0);
    assert(nae_transport_get_position(s.transport) == 60);
    /* Past the duration: cut to it */
    assert(nae_transport_seek(s.transport, 1000) == NAE_OK);
    assert(nae_transport_get_position(s.transport) == 100);
    /* Paused, a seek positions the source at once, for its data to be there on play */
    nae_transport_pause(s.transport);
    assert(nae_transport_seek(s.transport, 30) == NAE_OK);
    const int seeks = n.seeks;
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    for (int i = 0; i < 16; i++)
        close_to(out[i], 0);
    assert(n.seeks == seeks + 1 && n.last_seek == 1030);
    assert(nae_transport_get_position(s.transport) == 30);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 4, out) == NAE_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1030 + i));
    assert(n.seeks == seeks + 1); /* no second seek */
    /* Nothing rendering: the control thread applies the seek itself */
    assert(nae_transport_seek(s.transport, 70) == NAE_OK);
    nae_transport_sync(s.transport);
    assert(n.last_seek == 1070 && nae_transport_get_position(s.transport) == 70);
    assert(nae_transport_render(s.transport, 0, 4, out) == NAE_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1070 + i));
    assert(nae_transport_seek(s.transport, -1) == NAE_ERROR_INVALID);
    assert(nae_transport_seek(s.transport, INT64_C(1) << 48) == NAE_ERROR_INVALID);
    teardown(&s);
}

static void test_loop(void)
{
    struct numbers n = {0};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    float out[80];
    nae_transport_set_looping(s.transport, 1);
    assert(nae_transport_set_loop_range(s.transport, 20, 60) == NAE_OK);
    nae_transport_play(s.transport);
    /* Before the range, playback goes on to its end, then round it */
    assert(nae_transport_render(s.transport, 0, 70, out) == NAE_OK);
    for (int i = 0; i < 60; i++)
        close_to(out[i], (float)(1000 + i));
    for (int i = 60; i < 70; i++)
        close_to(out[i], (float)(1020 + i - 60));
    assert(n.wraps == 1 && n.last_seek == 1020);
    assert(nae_transport_get_position(s.transport) == 30);
    /* Holding still at the end: silence, the position at the end, the source going round meanwhile */
    assert(nae_transport_set_loop_hold(s.transport, 10) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 35, out) == NAE_OK);
    for (int i = 0; i < 30; i++)
        close_to(out[i], (float)(1030 + i));
    for (int i = 30; i < 35; i++)
        close_to(out[i], 0);
    assert(nae_transport_get_position(s.transport) == 60);
    assert(n.wraps == 2 && n.last_seek == 1020);
    assert(nae_transport_render(s.transport, 0, 10, out) == NAE_OK);
    for (int i = 0; i < 5; i++)
        close_to(out[i], 0);
    for (int i = 5; i < 10; i++)
        close_to(out[i], (float)(1020 + i - 5));
    assert(n.wraps == 2); /* went round once */
    /* Not looping: the whole duration, to its end */
    nae_transport_set_looping(s.transport, 0);
    assert(nae_transport_render(s.transport, 0, 80, out) == NAE_OK);
    for (int i = 0; i < 75; i++)
        close_to(out[i], (float)(1025 + i));
    assert(!nae_transport_is_playing(s.transport));
    /* A loop range past the duration loops over the whole duration */
    nae_transport_set_looping(s.transport, 1);
    assert(nae_transport_set_loop_hold(s.transport, 0) == NAE_OK);
    assert(nae_transport_set_loop_range(s.transport, 200, 300) == NAE_OK);
    assert(nae_transport_seek(s.transport, 90) == NAE_OK);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 20, out) == NAE_OK);
    for (int i = 0; i < 10; i++)
        close_to(out[i], (float)(1090 + i));
    for (int i = 10; i < 20; i++)
        close_to(out[i], (float)(1000 + i - 10));
    teardown(&s);
}

/* Gain k/8 after k frames of an 8-frame ramp from silence */
static void check_ramp_in(const float *out, int count)
{
    for (int i = 0; i < count; i++)
        close_to(out[i], i < 8 ? (float)(i + 1) / 8 : 1);
}

static void check_ramp_out(const float *out)
{
    for (int i = 0; i < 8; i++)
        close_to(out[i], (float)(7 - i) / 8);
}

static void test_declick(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 100, 8, 1000);
    float out[64];
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    check_ramp_in(out, 16);
    /* A seek fades out, jumps, and fades back in */
    assert(nae_transport_seek(s.transport, 50) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 24, out) == NAE_OK);
    check_ramp_out(out);
    check_ramp_in(out + 8, 16);
    assert(n.last_seek == 50);
    assert(nae_transport_get_position(s.transport) == 66); /* 8 frames faded out before the jump */
    /* Pausing fades out, playing fades in */
    nae_transport_pause(s.transport);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    check_ramp_out(out);
    for (int i = 8; i < 16; i++)
        close_to(out[i], 0);
    assert(nae_transport_get_position(s.transport) == 74);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 8, out) == NAE_OK);
    check_ramp_in(out, 8);
    /* A loop fades out into its end, and in from its start */
    nae_transport_set_looping(s.transport, 1);
    assert(nae_transport_set_loop_range(s.transport, 0, 96) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 30, out) == NAE_OK); /* 82..95, then 0..15 */
    for (int i = 0; i < 6; i++)
        close_to(out[i], 1);
    check_ramp_out(out + 6);
    check_ramp_in(out + 14, 16);
    /* Edited as it pauses: the fade out plays the graph it started on, the
     * edit adopted once silent, and playing fades the new graph in */
    struct numbers m = {.value = 2};
    const struct nae_source source = {
        .user = &m, .read = numbers_read, .seek = numbers_seek, .timeline_start = 0, .duration = 100,
    };
    struct nae_node *other = NULL;
    assert(nae_source_create(s.engine, &source, &other) == NAE_OK);
    const struct nae_route to_other = {other, NULL};
    nae_transport_pause(s.transport);
    assert(nae_engine_commit(s.engine, &to_other, 1, 0) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    check_ramp_out(out);
    for (int i = 8; i < 16; i++)
        close_to(out[i], 0);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 8, out) == NAE_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], 2.f * (float)(i + 1) / 8);
    nae_node_unrefp(&other);
    teardown(&s);
}

static void test_waiting(void)
{
    /* After a jump, silent until the sources have their data */
    struct numbers n = {.value = 1, .again = 3};
    struct setup s = setup(&n, 0, 1000, 4, 1000);
    float out[32];
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 12, out) == NAE_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    assert(nae_transport_render(s.transport, 0, 8, out) == NAE_OK);
    for (int i = 0; i < 8; i++)
        close_to(out[i], i < 4 ? (float)(i + 1) / 4 : 1);
    assert(nae_transport_get_position(s.transport) == 20); /* the position went on meanwhile */
    /* Playing, a source missing its data does not silence the others */
    n.again = 1;
    assert(nae_transport_render(s.transport, 0, 4, out) == NAE_AGAIN);
    for (int i = 0; i < 4; i++)
        close_to(out[i], 1);
    teardown(&s);
    /* A source never ready: the output comes back after the timeout */
    struct numbers late = {.value = 1, .again = 1000};
    s = setup(&late, 0, 1000, 4, 12);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 20, out) == NAE_AGAIN);
    for (int i = 0; i < 12; i++)
        close_to(out[i], 0);
    for (int i = 12; i < 20; i++)
        close_to(out[i], i < 16 ? (float)(i - 11) / 4 : 1);
    teardown(&s);
}

static void test_clock(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 48000 * 10, 0, 0);
    struct nae_clock clock;
    assert(nae_transport_get_clock(s.transport, &clock) == NAE_AGAIN);
    float out[480];
    nae_transport_play(s.transport);
    /* Renders of 10 ms, called with up to 2 ms of jitter */
    const int64_t start = INT64_C(1000000000);
    int64_t max_error = 0;
    for (int i = 0; i < 200; i++) {
        const int64_t jitter = (i * 7919 % 41 - 20) * 100000;
        const int64_t time = start + i * INT64_C(10000000) + jitter;
        assert(nae_transport_render(s.transport, time, 480, out) == NAE_OK);
        assert(nae_transport_get_clock(s.transport, &clock) == NAE_OK);
        assert(clock.playing && clock.sample_rate == 48000 && clock.position == 480 * i);
        /* Once settled, the clock follows the true times rather than the jittered ones */
        const int64_t error = clock.time - (start + i * INT64_C(10000000));
        if (i >= 100 && llabs(error) > max_error)
            max_error = llabs(error);
    }
    assert(max_error < 1000000);
    assert(nae_clock_position_at(&clock, clock.time + 5000000) == clock.position + 240);
    assert(nae_clock_position_at(&clock, clock.time - 5000000) == clock.position - 240);
    nae_transport_pause(s.transport);
    assert(nae_transport_render(s.transport, start + INT64_C(2000000000), 480, out) == NAE_OK);
    assert(nae_transport_get_clock(s.transport, &clock) == NAE_OK);
    assert(!clock.playing && nae_clock_position_at(&clock, clock.time + 5000000) == clock.position);
    teardown(&s);
}

/* Paused or holding still, the renders adopt the commits all the same: the
 * control thread commits again without waiting for the playback to resume */
static void test_commit_while_silent(void)
{
    struct numbers n = {0}, m = {.value = 1};
    struct setup s = setup(&n, 1000, 100, 0, 0);
    const struct nae_source source = {
        .user = &m, .read = numbers_read, .seek = numbers_seek, .timeline_start = 0, .duration = 100,
    };
    struct nae_node *other = NULL;
    assert(nae_source_create(s.engine, &source, &other) == NAE_OK);
    const struct nae_route to_other = {other, NULL}, to_source = {s.source, NULL};
    float out[16];
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 10, out) == NAE_OK);
    nae_transport_pause(s.transport);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    assert(nae_engine_commit(s.engine, &to_other, 1, 0) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    for (int i = 0; i < 16; i++)
        close_to(out[i], 0);
    assert(m.seeks == 1 && m.last_seek == 10); /* the new node where the playback resumes */
    assert(nae_engine_commit(s.engine, &to_source, 1, 0) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 0, 4, out) == NAE_OK);
    for (int i = 0; i < 4; i++)
        close_to(out[i], (float)(1010 + i));
    assert(n.seeks == 1); /* went on from where it was */
    /* Holding still at the end of a loop */
    nae_transport_set_looping(s.transport, 1);
    assert(nae_transport_set_loop_range(s.transport, 0, 20) == NAE_OK);
    assert(nae_transport_set_loop_hold(s.transport, 1000) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    for (int i = 6; i < 16; i++)
        close_to(out[i], 0);
    assert(nae_engine_commit(s.engine, &to_other, 1, 0) == NAE_OK);
    assert(nae_transport_render(s.transport, 0, 16, out) == NAE_OK);
    assert(nae_engine_commit(s.engine, &to_source, 1, 0) == NAE_OK);
    nae_node_unrefp(&other);
    teardown(&s);
}

static void check_position_at(struct nae_transport *transport, int64_t time, int64_t expected)
{
    int64_t position = -1;
    assert(nae_transport_get_position_at(transport, time, &position) == NAE_OK);
    assert(position == expected);
}

static void test_clock_history(void)
{
    struct numbers n = {.value = 1};
    struct setup s = setup(&n, 0, 4800, 0, 0);
    float output[960];
    int64_t position = -1;
    assert(nae_transport_get_position_at(s.transport, 1000000000, &position) == NAE_AGAIN);
    assert(position == -1);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 1000000000, 480, output) == NAE_OK);
    assert(nae_transport_render(s.transport, 1010000000, 480, output) == NAE_OK);
    check_position_at(s.transport, 1005000000, 240);
    assert(nae_transport_seek(s.transport, 2400) == NAE_OK);
    assert(nae_transport_render(s.transport, 1020000000, 480, output) == NAE_OK);
    check_position_at(s.transport, 1015000000, 720);
    check_position_at(s.transport, 1025000000, 2640);
    nae_transport_pause(s.transport);
    assert(nae_transport_render(s.transport, 1030000000, 480, output) == NAE_OK);
    check_position_at(s.transport, 1025000000, 2640);
    check_position_at(s.transport, 1035000000, 2880);
    assert(nae_transport_render(s.transport, 1040000000, 0, NULL) == NAE_OK);
    check_position_at(s.transport, 1035000000, 2880);
    teardown(&s);

    s = setup(&n, 0, 960, 0, 0);
    nae_transport_set_looping(s.transport, 1);
    assert(nae_transport_set_loop_range(s.transport, 240, 720) == NAE_OK);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 1000000000, 480, output) == NAE_OK);
    assert(nae_transport_render(s.transport, 1010000000, 480, output) == NAE_OK);
    check_position_at(s.transport, 1012500000, 600);
    check_position_at(s.transport, 1017500000, 360);
    assert(nae_transport_set_loop_hold(s.transport, 240) == NAE_OK);
    assert(nae_transport_render(s.transport, 1020000000, 960, output) == NAE_OK);
    check_position_at(s.transport, 1022500000, 600);
    check_position_at(s.transport, 1027500000, 720);
    check_position_at(s.transport, 1032500000, 360);
    for (int i = 0; i < 300; i++)
        assert(nae_transport_render(s.transport, 1040000000 + (int64_t)i * 10000000, 480, output) == NAE_OK);
    assert(nae_transport_get_position_at(s.transport, 1000000000, &position) == NAE_AGAIN);
    assert(position == -1);
    teardown(&s);

    s = setup(&n, 0, 720, 0, 0);
    nae_transport_play(s.transport);
    assert(nae_transport_render(s.transport, 1000000000, 960, output) == NAE_OK);
    check_position_at(s.transport, 1010000000, 480);
    check_position_at(s.transport, 1017500000, 0);
    teardown(&s);

    struct nae_clock clock = {.position = INT64_MAX - 1, .time = INT64_MIN, .sample_rate = 768000, .playing = 1};
    assert(nae_clock_position_at(&clock, INT64_MAX) == INT64_MAX);
    clock.position = 0;
    clock.time = INT64_MAX;
    assert(nae_clock_position_at(&clock, INT64_MIN) == 0);
}

static void test_short_declick(void)
{
    const int64_t lengths[] = {1, 2, 120, 240};
    for (unsigned i = 0; i < sizeof(lengths) / sizeof(*lengths); i++) {
        struct numbers n = {.value = 1};
        struct setup s = setup(&n, 0, lengths[i], 240, 0);
        nae_transport_set_looping(s.transport, 1);
        nae_transport_play(s.transport);
        float output[480];
        assert(nae_transport_render(s.transport, 0, 480, output) == NAE_OK);
        unsigned audible = 0;
        for (unsigned j = 0; j < 480; j++) {
            assert(output[j] >= 0.f && output[j] <= 1.f);
            audible += output[j] > 0.f;
        }
        assert(audible > 0);
        teardown(&s);
    }
}

int main(void)
{
    test_play_to_end();
    test_seek();
    test_loop();
    test_declick();
    test_short_declick();
    test_waiting();
    test_clock();
    test_clock_history();
    test_commit_while_silent();
    puts("nae transport: OK");
    return 0;
}
