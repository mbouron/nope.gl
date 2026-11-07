/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE  /* RUSAGE_THREAD */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include "nae/miniaudio.h"
#include "nae/player.h"
#include "../examples/wait.h"

#ifdef HAVE_VALGRIND_H
#include <valgrind/valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif
#if defined(__SANITIZE_THREAD__)
#define THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define THREAD_SANITIZER 1
#endif
#endif
#ifndef THREAD_SANITIZER
#define THREAD_SANITIZER 0
#endif

#define CHECK_EDIT(call) do {                         \
    int edit_ret;                                    \
    while ((edit_ret = (call)) == NAE_AGAIN)           \
        wait_ms(1);                                  \
    assert(edit_ret == NAE_OK);                       \
} while (0)

static const char *fixture, *fixture_index, *output_dir;

static void close_to(float a, float b)
{
    if (fabsf(a - b) > 0.0001f) {
        fprintf(stderr, "%f != %f\n", a, b);
        assert(0);
    }
}

static int64_t index_of(const float *frame)
{
    const long left = lroundf(frame[0] * 32768.f), right = lroundf(frame[1] * 32768.f);
    if (!left && !right)
        return -1;
    return (left - 1) | (right - 1) << 14;
}

/* The frames of a stereo float WAV file written by the player */
static float *read_wav(const char *filename, int64_t *nb_frames)
{
    FILE *f = fopen(filename, "rb");
    assert(f);
    char header[44];
    assert(fread(header, 1, 44, f) == 44);
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8) && !memcmp(header + 36, "data", 4));
    uint32_t size;
    memcpy(&size, header + 40, 4);
    float *frames = malloc(size);
    assert(frames && fread(frames, 1, size, f) == size);
    fclose(f);
    *nb_frames = size / 8;
    return frames;
}

static struct nae_player *offline_player(int64_t duration)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_OFFLINE};
    assert(nae_player_create(&config, &p) == NAE_OK);
    assert(nae_player_get_sample_rate(p) == 48000);
    assert(nae_player_set_duration(p, duration) == NAE_OK);
    return p;
}

static void test_record(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/record.wav", output_dir);
    struct nae_player *p = offline_player(15000);
    struct nae_track *t = NULL;
    const struct nae_track_params params = {.filename = fixture_index, .start = 1000, .trim = 2000, .duration = 10000, .volume = 1};
    assert(nae_player_add_track(p, &params, &t) == NAE_OK);
    assert(nae_track_get_length(t) == 48000 && nae_track_get_duration(t) == 10000);
    assert(nae_player_record(p, path, NAE_RECORD_F32) == NAE_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == 15000);
    for (int64_t i = 0; i < n; i++)
        assert(index_of(frames + 2 * i) == (i >= 1000 && i < 11000 ? 2000 + i - 1000 : -1));
    free(frames);

    /* Moved and cut while laid: the rest of the media from its trim */
    assert(nae_player_set_track_timings(p, t, 500, 40000, -1) == NAE_OK);
    assert(nae_track_get_duration(t) == 8000);
    assert(nae_player_set_loop_range(p, 2000, 4000) == NAE_OK);
    assert(nae_player_set_loop_hold(p, 2400) == NAE_OK);
    assert(nae_player_set_looping(p, 1) == NAE_OK);
    assert(nae_player_record(p, path, NAE_RECORD_F32) == NAE_OK);
    assert(nae_player_is_looping(p));
    frames = read_wav(path, &n);
    for (int64_t i = 0; i < n; i++)
        assert(index_of(frames + 2 * i) == (i >= 500 && i < 8500 ? 40000 + i - 500 : -1));
    free(frames);
    assert(nae_player_remove_track(p, &t) == NAE_OK && !t);
    nae_player_freep(&p);
}

/* The fixture's sample at a frame of its media */
static float fixture_at(int64_t frame)
{
    return frame < 4096 ? (float)(1000 + frame) / 32768.f : 0.f;
}

static void test_mix(void)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/mix.wav", output_dir);
    struct nae_player *p = offline_player(6000);
    struct nae_track *a = NULL, *b = NULL;
    const struct nae_track_params pa = {.filename = fixture, .start = 0, .duration = -1, .volume = 0.5f};
    const struct nae_track_params pb = {.filename = fixture, .start = 1000, .trim = 100, .duration = 2000, .volume = 1};
    assert(nae_player_add_track(p, &pa, &a) == NAE_OK);
    assert(nae_player_add_track(p, &pb, &b) == NAE_OK);
    /* From the start of the track: 0.25 from its frame 100, then a ramp to 1 over its frames [200, 300) */
    const struct nae_keyframe keyframes[] = {{100, 0, 0.25f}, {200, 100, 1}};
    assert(nae_player_set_track_fader(p, b, keyframes, 2) == NAE_OK);
    assert(nae_player_set_volume(p, 0.8f) == NAE_OK);
    close_to(nae_player_get_volume(p), 0.8f);
    assert(nae_player_record(p, path, NAE_RECORD_F32) == NAE_OK);
    int64_t n;
    float *frames = read_wav(path, &n);
    assert(n == 6000);
    for (int64_t i = 0; i < n; i++) {
        float expected = 0.5f * fixture_at(i);
        if (i >= 1000 && i < 3000) {
            const int64_t local = i - 1000;
            const float fader = local < 100 ? 1 : local < 200 ? 0.25f : local < 300 ? 0.25f + 0.75f * (float)(local - 200) / 100 : 1;
            expected += fader * fixture_at(100 + local);
        }
        close_to(frames[2 * i], 0.8f * expected);
        close_to(frames[2 * i + 1], -0.8f * expected);
    }
    free(frames);
    /* As the application exports: 16 bits */
    assert(nae_player_record(p, path, NAE_RECORD_S16) == NAE_OK);
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    assert(ftell(f) == 44 + 6000 * 4);
    fclose(f);
    assert(nae_player_clear_tracks(p) == NAE_OK);
    nae_player_freep(&p);
}

/*
 * The audio thread never waits, whatever the application does meanwhile:
 * a thread renders 10 ms every 10 ms, its voluntary context switches
 * counted in each render, while the control thread seeks, loops, holds,
 * adds and removes tracks, and moves their timings, faders and volumes.
 */
struct renderer {
    struct nae_player *player;
    _Atomic int stop;
    _Atomic int64_t renders;
    _Atomic int64_t waits;       /* voluntary context switches in renders that did not fault */
    _Atomic int64_t max_ns;
    _Atomic int64_t audible;
    _Atomic long peak;           /* the loudest sample, in 1/32768 */
};

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *run_renderer(void *arg)
{
    struct renderer *r = arg;
    float out[2 * 480];
    int64_t next = now_ns();
    while (!atomic_load(&r->stop)) {
#ifdef RUSAGE_THREAD
        struct rusage before, after;
        getrusage(RUSAGE_THREAD, &before);
#endif
        const int64_t start = now_ns();
        assert(nae_player_render(r->player, start, 480, out) >= 0);
        const int64_t spent = now_ns() - start;
#ifdef RUSAGE_THREAD
        getrusage(RUSAGE_THREAD, &after);
        if (after.ru_minflt == before.ru_minflt && after.ru_majflt == before.ru_majflt)
            atomic_fetch_add(&r->waits, after.ru_nvcsw - before.ru_nvcsw);
#endif
        if (spent > atomic_load(&r->max_ns))
            atomic_store(&r->max_ns, spent);
        for (int i = 0; i < 960; i++)
            if (out[i] != 0.f) {
                atomic_fetch_add(&r->audible, 1);
                break;
            }
        for (int i = 0; i < 960; i++) {
            const long sample = lroundf(fabsf(out[i]) * 32768.f);
            if (sample > atomic_load(&r->peak))
                atomic_store(&r->peak, sample);
        }
        atomic_fetch_add(&r->renders, 1);
        next += 10000000;
        const int64_t left = next - now_ns();
        if (left > 0)
            wait_ms((unsigned)(left / 1000000));
    }
    return NULL;
}

static void test_never_waits(void)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_HOST};
    assert(nae_player_create(&config, &p) == NAE_OK);
    assert(nae_player_set_duration(p, 96000) == NAE_OK);
    struct nae_track *tracks[9] = {0};
    for (int i = 0; i < 4; i++) {
        const struct nae_track_params params = {.filename = fixture_index, .start = 6000 * i, .duration = -1, .volume = 0.25f};
        CHECK_EDIT(nae_player_add_track(p, &params, &tracks[i]));
    }
    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nae_player_start_rendering(p) == NAE_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    assert(nae_player_play(p) == NAE_OK);
    unsigned seed = 7;
    for (int i = 0; i < 120; i++) {
        seed = seed * 1103515245 + 12345;
        const unsigned what = (seed >> 16) % 8;
        const int64_t frame = (seed >> 4) % 90000;
        if (what == 0) {
            assert(nae_player_seek(p, frame) == NAE_OK);
        } else if (what == 1) {
            assert(nae_player_set_looping(p, 1) == NAE_OK);
            assert(nae_player_set_loop_range(p, frame, frame + 12000) == NAE_OK);
            assert(nae_player_set_loop_hold(p, (seed >> 8) % 2 ? 2400 : 0) == NAE_OK);
        } else if (what == 2) {
            assert(nae_player_set_looping(p, 0) == NAE_OK);
            assert(nae_player_play(p) == NAE_OK);
        } else if (what == 3) {
            const int slot = 4 + (int)((seed >> 8) % 5);
            if (tracks[slot]) {
                CHECK_EDIT(nae_player_remove_track(p, &tracks[slot]));
            } else {
                const struct nae_track_params params = {.filename = fixture_index, .start = frame, .trim = 1000, .duration = 24000, .volume = 0.25f, .normalize = slot % 2};
                CHECK_EDIT(nae_player_add_track(p, &params, &tracks[slot]));
            }
        } else if (what == 4) {
            const struct nae_keyframe keyframes[] = {{0, 0, 0.5f}, {(int64_t)(seed % 20000), 4800, 0.1f}};
            CHECK_EDIT(nae_player_set_track_fader(p, tracks[seed % 4], keyframes, 2));
        } else if (what == 5) {
            CHECK_EDIT(nae_player_set_track_timings(p, tracks[seed % 4], frame, (seed >> 3) % 20000, 20000));
        } else if (what == 6) {
            assert(nae_player_set_track_volume(p, tracks[seed % 4], (float)(seed % 100) / 100) == NAE_OK);
            assert(nae_player_set_volume(p, 0.5f + (float)(seed % 50) / 100) == NAE_OK);
        } else {
            assert(nae_player_pause(p) == NAE_OK);
            wait_ms(20);
            assert(nae_player_play(p) == NAE_OK);
        }
        wait_ms(25);
        int64_t heard;
        const int clock_ret = nae_player_get_position_at(p, now_ns(), &heard);
        assert(clock_ret == NAE_OK || clock_ret == NAE_AGAIN);
        if (clock_ret == NAE_OK)
            assert(heard >= 0 && heard <= 96000);
    }
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nae_player_stop_rendering(p) == NAE_OK);
    struct nae_player_stats stats;
    nae_player_get_stats(p, &stats);
    printf("never waits: %" PRId64 " renders, %" PRId64 " waits, longest %.3f ms, %" PRId64 " audible, %" PRId64 " requests, %" PRId64 " frames silent\n",
           atomic_load(&r.renders), atomic_load(&r.waits), atomic_load(&r.max_ns) / 1e6, atomic_load(&r.audible),
           stats.requests, stats.silent_frames);
    assert(stats.errors == 0 && atomic_load(&r.audible) > 0);
    /* Threads switch out of their own accord under Valgrind and TSan, which serialize them */
#ifdef RUSAGE_THREAD
    if (!RUNNING_ON_VALGRIND && !THREAD_SANITIZER)
        assert(atomic_load(&r.waits) == 0);
#endif
    nae_player_freep(&p);
}

/* Edits while paused reach the graph at once, though nothing is heard: the
 * renders adopt them, the control thread not waiting for the playback */
static void test_edits_while_paused(void)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_HOST};
    assert(nae_player_create(&config, &p) == NAE_OK);
    /* Each track at most 0x4000 / 32768 alone */
    assert(nae_player_set_duration(p, 48000) == NAE_OK);
    const struct nae_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    struct nae_track *tracks[4] = {0};
    CHECK_EDIT(nae_player_add_track(p, &params, &tracks[0]));
    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nae_player_start_rendering(p) == NAE_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    assert(nae_player_play(p) == NAE_OK);
    wait_ms(100);
    assert(nae_player_pause(p) == NAE_OK);
    wait_ms(100);
    const int64_t start = now_ns();
    for (int i = 1; i < 4; i++)
        CHECK_EDIT(nae_player_add_track(p, &params, &tracks[i]));
    const struct nae_keyframe keyframe = {0, 0, 1};
    for (int i = 0; i < 4; i++)
        CHECK_EDIT(nae_player_set_track_fader(p, tracks[i], &keyframe, 1));
    /* A render or so each, rather than the 2 s a commit gives up after */
    assert(now_ns() - start < 1000000000);
    atomic_store(&r.peak, 0);
    assert(nae_player_play(p) == NAE_OK);
    wait_ms(200);
    /* More than two tracks heard together */
    assert(atomic_load(&r.peak) > 2 * 0x4000);
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nae_player_stop_rendering(p) == NAE_OK);
    nae_player_freep(&p);
}

static void test_host_stop(void)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_HOST};
    assert(nae_player_create(&config, &p) == NAE_OK);
    assert(nae_player_set_duration(p, 480000) == NAE_OK);
    float output[2];
    assert(nae_player_render(p, 0, 1, output) == NAE_ERROR_INVALID);

    struct renderer r = {.player = p};
    pthread_t thread;
    assert(nae_player_start_rendering(p) == NAE_OK);
    assert(!pthread_create(&thread, NULL, run_renderer, &r));
    while (!atomic_load(&r.renders))
        wait_ms(1);
    for (unsigned i = 0; i < 200; i++) {
        assert(nae_player_play(p) == NAE_OK);
        assert(nae_player_stop(p) == NAE_OK);
        wait_ms(1);
    }
    atomic_store(&r.stop, 1);
    assert(!pthread_join(thread, NULL));
    assert(nae_player_stop_rendering(p) == NAE_OK);
    assert(nae_player_get_position(p) == 0);
    assert(nae_player_render(p, 0, 1, output) == NAE_ERROR_INVALID);
    nae_player_freep(&p);
}

static void test_device(void)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_NULL_DEVICE, .period_ms = 10};
    assert(nae_player_create(&config, &p) == NAE_OK);
    assert(nae_player_set_duration(p, 48000) == NAE_OK);
    struct nae_track *t = NULL;
    const struct nae_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nae_player_add_track(p, &params, &t) == NAE_OK);
    /* Paused, before the device started: the seek applied at once */
    assert(nae_player_seek(p, 4800) == NAE_OK);
    assert(nae_player_get_position(p) == 4800);
    assert(nae_player_play(p) == NAE_OK && nae_player_is_playing(p));
    wait_ms(300);
    const int64_t position = nae_player_get_position(p);
    assert(position > 4800 + 48 * 200 && position < 4800 + 48 * 400);
    int64_t heard = -1;
    for (int i = 0; i < 100; i++) {
        if (nae_player_get_position_at(p, nae_sink_now(), &heard) == NAE_OK)
            break;
        wait_ms(1);
    }
    const int64_t rendered = nae_player_get_position(p);
    assert(heard >= 0 && heard <= rendered && heard > rendered - 48 * 100);
    /* To the end, stopped at its start */
    wait_ms(1000);
    assert(!nae_player_is_playing(p) && nae_player_get_position(p) == 0);
    assert(nae_player_play(p) == NAE_OK);
    wait_ms(100);
    assert(nae_player_stop(p) == NAE_OK);
    assert(!nae_player_is_playing(p) && nae_player_get_position(p) == 0);
    struct nae_player_stats stats;
    nae_player_get_stats(p, &stats);
    assert(stats.errors == 0);
    nae_player_freep(&p);
}

static void test_time_limits(void)
{
    struct nae_player *p = offline_player(48000);
    struct nae_track *t = NULL;
    const struct nae_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nae_player_add_track(p, &params, &t) == NAE_OK);
    assert(nae_player_set_track_timings(p, t, 0, INT64_MAX, 100) == NAE_OK);
    assert(nae_track_get_duration(t) == 0);
    assert(nae_player_set_track_timings(p, t, INT64_MAX, 0, 100) == NAE_ERROR_INVALID);
    assert(nae_player_set_track_timings(p, t, 100, 0, 100) == NAE_OK);
    const struct nae_keyframe keyframe = {.frame = INT64_MAX, .volume = 1};
    assert(nae_player_set_track_fader(p, t, &keyframe, 1) == NAE_ERROR_INVALID);
    assert(nae_player_set_track_fader(p, t, &keyframe, UINT32_MAX) == NAE_ERROR_INVALID);

    char path[4096];
    snprintf(path, sizeof(path), "%s/too_large.wav", output_dir);
    FILE *file = fopen(path, "wb");
    assert(file && fputs("keep", file) >= 0 && fclose(file) == 0);
    assert(nae_player_set_duration(p, INT64_MAX) == NAE_OK);
    assert(nae_player_set_track_timings(p, t, INT64_MAX - 100, 100, 100) == NAE_OK);
    assert(nae_player_set_looping(p, 1) == NAE_OK);
    assert(nae_player_record(p, path, NAE_RECORD_F32) == NAE_ERROR_UNSUPPORTED);
    file = fopen(path, "rb");
    char data[4];
    assert(file && fread(data, 1, 4, file) == 4 && !memcmp(data, "keep", 4));
    fclose(file);
    remove(path);
    nae_player_freep(&p);
}

static void test_pending_edits(void)
{
    struct nae_player *p = NULL;
    const struct nae_player_config config = {.mode = NAE_PLAYER_HOST};
    assert(nae_player_create(&config, &p) == NAE_OK);
    assert(nae_player_set_duration(p, 48000) == NAE_OK);
    struct nae_track *track = NULL;
    const struct nae_track_params params = {.filename = fixture_index, .duration = -1, .volume = 1};
    assert(nae_player_add_track(p, &params, &track) == NAE_OK);
    const struct nae_keyframe unity = {.volume = 1};
    const struct nae_keyframe mute = {.volume = 0};

    assert(nae_player_start_rendering(p) == NAE_OK);
    assert(nae_player_set_track_fader(p, track, &unity, 1) == NAE_OK);
    assert(nae_player_set_track_timings(p, track, 1000, 2000, 100) == NAE_AGAIN);
    assert(nae_track_get_duration(track) == 48000);
    assert(nae_player_set_track_fader(p, track, &mute, 1) == NAE_AGAIN);
    struct nae_track *other = NULL;
    assert(nae_player_add_track(p, &params, &other) == NAE_AGAIN);
    assert(!other);
    struct nae_track *saved = track;
    assert(nae_player_remove_track(p, &track) == NAE_AGAIN);
    assert(track == saved);
    assert(nae_player_clear_tracks(p) == NAE_AGAIN);
    assert(nae_player_set_track_volume(p, track, 1) == NAE_OK);

    float output[2 * 256];
    assert(nae_player_render(p, 0, 1, output) == NAE_OK);
    assert(nae_player_stop_rendering(p) == NAE_OK);
    struct nae_track_params silent_params = params;
    silent_params.volume = 0;
    assert(nae_player_add_track(p, &silent_params, &other) == NAE_OK);
    assert(nae_player_start_rendering(p) == NAE_OK);
    assert(nae_player_play(p) == NAE_OK);
    int heard = 0;
    for (int i = 0; i < 64; i++) {
        const int ret = nae_player_render(p, 0, 256, output);
        assert(ret >= 0);
        if (ret == NAE_OK) {
            assert(index_of(output + 2 * 255) == 256 * i + 255);
            heard = 1;
            break;
        }
        wait_ms(1);
    }
    assert(heard);
    assert(nae_player_stop_rendering(p) == NAE_OK);
    assert(nae_player_remove_track(p, &track) == NAE_OK && !track);
    nae_player_freep(&p);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    fixture = argv[1];
    fixture_index = argv[2];
    output_dir = argv[3];
    test_time_limits();
    test_pending_edits();
    test_record();
    test_mix();
    test_never_waits();
    test_edits_while_paused();
    test_host_stop();
    test_device();
    puts("nae player: OK");
    return 0;
}
