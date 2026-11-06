/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nae/nopemd.h"
#include "../examples/wait.h"

static const char *fixture, *fixture_44100, *fixture_index, *fixture_mono, *fixture_aac;

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.00001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

/* The index of a frame of the index fixture, -1 for silence */
static int64_t index_of(const float *frame)
{
    const long left = lroundf(frame[0] * 32768.f), right = lroundf(frame[1] * 32768.f);
    if (!left && !right)
        return -1;
    return (left - 1) | (right - 1) << 14;
}

struct media {
    struct nae_engine *engine;
    struct nae_media *media;
    struct nae_node *source;
};

static struct media open_media(const char *filename, int blocking, int normalize,
                               int64_t timeline_start, int64_t source_start, int64_t duration)
{
    struct media m = {0};
    const struct nae_config c = {48000, 2, 64};
    assert(nae_engine_create(&c, &m.engine) == NAE_OK);
    const struct nae_media_params params = {.start = source_start, .blocking = blocking, .normalize = normalize};
    assert(nae_media_create(m.engine, filename, &params, &m.media) == NAE_OK);
    assert(nae_media_source(m.media, timeline_start, source_start, duration, &m.source) == NAE_OK);
    const struct nae_route r = {m.source, NULL};
    assert(nae_engine_commit(m.engine, &r, 1, 0) == NAE_OK);
    return m;
}

static struct nae_media_stats stats_of(const struct media *m)
{
    struct nae_media_stats stats;
    nae_media_get_stats(m->media, &stats);
    return stats;
}

static void close_media(struct media *m)
{
    assert(stats_of(m).errors == 0);
    nae_node_unrefp(&m->source);
    nae_engine_freep(&m->engine);
    nae_media_unrefp(&m->media);
}

/* The fixture's frame `frame` of the media */
static void check_fixture(const float *out, int64_t frame, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        const float sample = frame + i < 4096 ? (float)(1000 + frame + i) / 32768.f : 0;
        close_to(out[2 * i], sample);
        close_to(out[2 * i + 1], -sample);
    }
}

static void test_read_and_seek(void)
{
    /* Offline: every read waits for its data */
    struct media m = open_media(fixture, 1, 0, 17, 101, 10000);
    assert(nae_media_get_length(m.media) == 4096);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 4224; frame += 64) {
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
        for (unsigned i = 0; i < 64; i++) {
            if (frame + i < 17)
                close_to(out[2 * i], 0);
            else
                check_fixture(out + 2 * i, 101 + frame + i - 17, 1);
        }
    }
    /* Past the end of the media: silence, without asking the decoder again */
    assert(stats_of(&m).requests == 0);
    for (int64_t frame = 4224; frame < 6400; frame += 64)
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
    for (unsigned i = 0; i < 128; i++) close_to(out[i], 0);
    assert(stats_of(&m).requests == 0);
    /* A jump back: a seek inside a chunk of the decoder, sample-exact */
    assert(nae_engine_render(m.engine, 17, 17, out) == NAE_OK);
    check_fixture(out, 101, 17);
    assert(stats_of(&m).requests == 1);
    /* To the very start of the media */
    assert(nae_source_set_range(m.source, 0, 0, 10000) == NAE_OK);
    const struct nae_route r = {m.source, NULL};
    assert(nae_engine_commit(m.engine, &r, 1, 0) == NAE_OK);
    for (int64_t frame = 0; frame < 4608; frame += 64) {
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
        check_fixture(out, frame, 64);
    }
    close_media(&m);
}

/* A loop of the project the media goes round ahead, read without a single seek */
static void test_loop(void)
{
    struct media m = open_media(fixture_index, 1, 0, 0, 0, 48000);
    const struct nae_transport_config tc = {0};
    struct nae_transport *t = NULL;
    assert(nae_transport_create(m.engine, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000) == NAE_OK);
    nae_transport_set_looping(t, 1);
    assert(nae_transport_set_loop_range(t, 12000, 24000) == NAE_OK);
    assert(nae_media_set_loop(m.media, 12000, 24000) == NAE_OK);
    nae_transport_play(t);
    float out[2 * 256];
    int64_t expected = 0;
    for (int i = 0; i < 300; i++) {
        assert(nae_transport_render(t, 0, 256, out) == NAE_OK);
        for (unsigned f = 0; f < 256; f++) {
            assert(index_of(out + 2 * f) == expected);
            if (++expected == 24000)
                expected = 12000;
        }
    }
    assert(stats_of(&m).requests == 0 && stats_of(&m).skipped_frames == 0);
    nae_transport_freep(&t);
    close_media(&m);
}

/*
 * A short media looped as a whole: nope.media reads it to its end as soon
 * as it is opened, before it knows of the loop, set right away as a player
 * does, or later on: it goes round it all the same
 */
static void test_short_loop(int set_late)
{
    struct media m = open_media(fixture, 1, 0, 0, 0, 4096);
    if (set_late)
        wait_ms(200);
    const struct nae_transport_config tc = {0};
    struct nae_transport *t = NULL;
    assert(nae_transport_create(m.engine, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 4096) == NAE_OK);
    nae_transport_set_looping(t, 1);
    assert(nae_media_set_loop(m.media, 0, 4096) == NAE_OK);
    nae_transport_play(t);
    float out[2 * 256];
    for (int i = 0; i < 64; i++) {
        assert(nae_transport_render(t, 0, 256, out) == NAE_OK);
        check_fixture(out, i * 256 % 4096, 256);
    }
    assert(stats_of(&m).requests == 0);
    nae_transport_freep(&t);
    close_media(&m);
}

/* A position asked for as soon as the media is created, before its decoding thread runs: served all the same */
static void test_request_at_once(void)
{
    for (int i = 0; i < 20; i++) {
        struct media m = open_media(fixture_index, 1, 0, 0, 0, 48000);
        float out[2 * 64];
        assert(nae_engine_render(m.engine, 30000, 64, out) == NAE_OK);
        for (unsigned f = 0; f < 64; f++)
            assert(index_of(out + 2 * f) == 30000 + f);
        assert(stats_of(&m).requests == 1);
        close_media(&m);
    }
}

/*
 * Scrubbing while paused, a seek after the other, nothing read meanwhile:
 * the audio decoded for each position left does not take the room of the
 * next, which is there when the playback goes on
 */
static void test_paused_seeks(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nae_transport_config tc = {0};
    struct nae_transport *t = NULL;
    assert(nae_transport_create(m.engine, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000) == NAE_OK);
    float out[2 * 480];
    for (int i = 0; i < 30; i++) {
        assert(nae_transport_seek(t, 1000 + 1000 * i) == NAE_OK);
        assert(nae_transport_render(t, 0, 480, out) == NAE_OK);
        wait_ms(10);
    }
    wait_ms(100);
    /* In real time, 10 ms every 10 ms */
    nae_transport_play(t);
    for (int i = 0; i < 20; i++) {
        assert(nae_transport_render(t, 0, 480, out) == NAE_OK);
        for (unsigned f = 0; f < 480; f++)
            assert(index_of(out + 2 * f) == 30000 + 480 * i + f);
        wait_ms(10);
    }
    assert(stats_of(&m).silent_frames == 0);
    nae_transport_freep(&t);
    close_media(&m);
}

/*
 * A source positioned while paused at the end of its loop, as one past the
 * end of its range is, then going round it: its next pass is there from the
 * start, decoded meanwhile though the reader did not take the audio queued
 * for its previous position
 */
static void test_loop_from_end(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nae_transport_config tc = {0};
    struct nae_transport *t = NULL;
    assert(nae_transport_create(m.engine, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000) == NAE_OK);
    nae_transport_set_looping(t, 1);
    assert(nae_transport_set_loop_range(t, 12000, 24000) == NAE_OK);
    assert(nae_media_set_loop(m.media, 12000, 24000) == NAE_OK);
    wait_ms(200);
    assert(nae_transport_seek(t, 24000) == NAE_OK);
    float out[2 * 480];
    assert(nae_transport_render(t, 0, 480, out) == NAE_OK);
    wait_ms(200);
    /* In real time, 10 ms every 10 ms */
    nae_transport_play(t);
    int64_t expected = 12000;
    for (int i = 0; i < 60; i++) {
        assert(nae_transport_render(t, 0, 480, out) == NAE_OK);
        for (unsigned f = 0; f < 480; f++) {
            assert(index_of(out + 2 * f) == expected);
            if (++expected == 24000)
                expected = 12000;
        }
        wait_ms(10);
    }
    assert(stats_of(&m).requests == 1 && stats_of(&m).silent_frames == 0);
    nae_transport_freep(&t);
    close_media(&m);
}

/*
 * Playing in real time, rendering 10 ms every 10 ms: what plays is always
 * at its position, looping or after a seek
 */
static void test_realtime(void)
{
    struct media m = open_media(fixture_index, 0, 0, 0, 0, 48000);
    const struct nae_transport_config tc = {0};
    struct nae_transport *t = NULL;
    assert(nae_transport_create(m.engine, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000) == NAE_OK);
    nae_transport_set_looping(t, 1);
    assert(nae_transport_set_loop_range(t, 12000, 24000) == NAE_OK);
    assert(nae_media_set_loop(m.media, 12000, 24000) == NAE_OK);
    nae_transport_play(t);
    float out[2 * 480];
    int64_t position = 0, silent = 0, started = -1;
    for (int i = 0; i < 150; i++) {
        if (i == 100) {
            /* A seek away from the loop, then playing on from there */
            nae_transport_set_looping(t, 0);
            assert(nae_media_set_loop(m.media, 0, 0) == NAE_OK);
            assert(nae_transport_seek(t, 30000) == NAE_OK);
            position = 30000;
        }
        nae_transport_render(t, 0, 480, out);
        for (unsigned f = 0; f < 480; f++) {
            const int64_t index = index_of(out + 2 * f);
            if (index < 0) {
                silent += started >= 0 && position < 48000; /* not past the end */
            } else {
                assert(index == position);
                if (started < 0)
                    started = i;
            }
            if (++position == 24000 && i < 100)
                position = 12000;
        }
        wait_ms(10);
    }
    const struct nae_media_stats stats = stats_of(&m);
    printf("realtime: started after %" PRId64 " renders, %" PRId64 " frames silent then (seek included), %" PRId64 " requests\n",
           started, silent, stats.requests);
    assert(started >= 0);
    assert(stats.requests == 2); /* the seek and the end going back to the start: going round the loop asks for nothing */
    nae_transport_freep(&t);
    close_media(&m);
}

/* Source-rate conversion must not introduce holes/duplicates between decoder
 * frames, and must drain the resampler at EOF. */
static void test_resample(void)
{
    struct media m = open_media(fixture_44100, 1, 0, 0, 0, 10000);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 5120; frame += 64) {
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
        for (unsigned i = 0; i < 64; i++) {
            int64_t index = frame + i;
            /* Ignore the resampler's edge transients. A one-sample drift
             * in the body is much larger than this quantization tolerance. */
            if (index < 100 || (index >= 4700 && index < 4800)) continue;
            float expected = index < 4800 ? (float)(6000. / 32768. * sin(2 * 3.141592653589793 * 1000 * (double)index / 48000)) : 0.f;
            assert(fabsf(out[2 * i] - expected) < 0.0001f);
            assert(fabsf(out[2 * i + 1] + expected) < 0.0001f);
        }
    }
    close_media(&m);
}

static void test_mono(void)
{
    struct media m = open_media(fixture_mono, 1, 0, 0, 0, 4800);
    float out[2 * 64];
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
        for (unsigned i = 0; i < 64; i++) {
            const float sample = (float)(1000 + frame + i) / 32768.f;
            assert(out[2 * i] == out[2 * i + 1] && out[2 * i] > 0);
            /* Mono to stereo: as is, or each side at -3 dB */
            assert(fabsf(out[2 * i] - sample) < 0.0001f || fabsf(out[2 * i] - sample * 0.70710678f) < 0.0001f);
        }
    }
    close_media(&m);
}

/* Back to the start of an AAC media, its first frame plays as when starting from there */
static void test_aac_start(void)
{
    struct media m = open_media(fixture_aac, 1, 0, 0, 0, 48000);
    float first[2 * 4800], again[2 * 4800];
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        const uint32_t n = 4800 - frame < 64 ? (uint32_t)(4800 - frame) : 64;
        assert(nae_engine_render(m.engine, frame, n, first + 2 * frame) == NAE_OK);
    }
    float peak = 0;
    for (int i = 0; i < 2 * 1024; i++)
        peak = fmaxf(peak, fabsf(first[i]));
    assert(peak > 0.01f); /* not silent from the start */
    for (int64_t frame = 0; frame < 4800; frame += 64) {
        const uint32_t n = 4800 - frame < 64 ? (uint32_t)(4800 - frame) : 64;
        assert(nae_engine_render(m.engine, frame, n, again + 2 * frame) == NAE_OK);
    }
    assert(!memcmp(first, again, sizeof(first)));
    close_media(&m);
}

static void test_normalize(void)
{
    struct media m = open_media(fixture_index, 1, 1, 0, 0, 48000);
    float out[2 * 64];
    float peak = 0;
    for (int64_t frame = 0; frame < 48000; frame += 64) {
        assert(nae_engine_render(m.engine, frame, 64, out) == NAE_OK);
        for (unsigned i = 0; i < 128; i++)
            peak = fmaxf(peak, fabsf(out[i]));
    }
    assert(peak > 0);
    close_media(&m);
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    fixture = argv[1];
    fixture_44100 = argv[2];
    fixture_index = argv[3];
    fixture_mono = argv[4];
    fixture_aac = argv[5];
    test_read_and_seek();
    test_loop();
    test_short_loop(0);
    test_short_loop(1);
    test_request_at_once();
    test_paused_seeks();
    test_loop_from_end();
    test_realtime();
    test_resample();
    test_mono();
    test_aac_start();
    test_normalize();
    puts("nae media: OK");
    return 0;
}
