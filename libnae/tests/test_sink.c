/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "nae/miniaudio.h"
#include "../examples/wait.h"

static int process(void *user, const struct nae_process *p)
{
    (void)p;
    atomic_fetch_add((_Atomic unsigned *)user, 1);
    return NAE_OK;
}

static int ones(void *user, const struct nae_process *p)
{
    (void)user;
    for (uint32_t i = 0; i < p->frames * p->channels; i++)
        p->output[i] = 1;
    return NAE_OK;
}

/* A device playing a transport: it runs on paused, its callbacks timing the clock */
static void test_transport(void)
{
    struct nae_engine *e = NULL;
    struct nae_node *n = NULL;
    struct nae_transport *t = NULL;
    struct nae_sink *sink = NULL;
    const struct nae_config config = {48000, 2, 128};
    const struct nae_processor p = {.process = ones};
    assert(nae_engine_create(&config, &e) == NAE_OK);
    assert(nae_node_create(e, &p, &n) == NAE_OK);
    const struct nae_route route = {n, NULL};
    assert(nae_engine_commit(e, &route, 1, 0) == NAE_OK);
    const struct nae_transport_config tc = {.declick_frames = 240, .ready_timeout_frames = 4800};
    assert(nae_transport_create(e, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000 * 60) == NAE_OK);
    const struct nae_sink_config sink_config = {.null_device = 1, .transport = t, .period_ms = 10};
    assert(nae_sink_create(e, &sink_config, &sink) == NAE_OK);
    assert(nae_sink_latency(sink) > 0);
    assert(nae_sink_seek(sink, 0) == NAE_ERROR_INVALID); /* the transport seeks */
    assert(nae_sink_start(sink) == NAE_OK);
    /* Paused: the device runs, the position stays */
    wait_ms(50);
    assert(nae_transport_get_position(t) == 0);
    nae_transport_play(t);
    const int64_t start = nae_sink_now();
    wait_ms(300);
    const int64_t played = nae_transport_get_position(t);
    const int64_t elapsed = nae_sink_now() - start;
    /* As fast as time goes, within a few periods */
    assert(llabs(played - elapsed * 48 / 1000000) < 48 * 60);
    struct nae_clock clock;
    assert(nae_transport_get_clock(t, &clock) == NAE_OK && clock.playing);
    /* The clock tells what is heard now: behind what was rendered by the latency, within a few periods */
    const int64_t heard = nae_clock_position_at(&clock, nae_sink_now());
    const int64_t rendered = nae_transport_get_position(t);
    assert(heard < rendered && rendered - heard < (nae_sink_latency(sink) + 60000000) * 48 / 1000000);
    nae_transport_pause(t);
    wait_ms(50);
    const int64_t paused = nae_transport_get_position(t);
    wait_ms(50);
    assert(nae_transport_get_position(t) == paused);
    /* A seek while paused, applied by the running device */
    assert(nae_transport_seek(t, 48000 * 10) == NAE_OK);
    wait_ms(50);
    assert(nae_transport_get_position(t) == 48000 * 10);
    assert(nae_sink_status(sink) == NAE_OK);
    assert(nae_sink_stop(sink) == NAE_OK);
    nae_sink_freep(&sink);
    nae_transport_freep(&t);
    nae_node_unrefp(&n);
    nae_engine_freep(&e);
}

static void test_foreign_transport(void)
{
    struct nae_engine *mono = NULL;
    struct nae_engine *stereo = NULL;
    const struct nae_config mono_config = {48000, 1, 256};
    const struct nae_config stereo_config = {48000, 2, 256};
    assert(nae_engine_create(&mono_config, &mono) == NAE_OK);
    assert(nae_engine_create(&stereo_config, &stereo) == NAE_OK);

    struct nae_transport *transport = NULL;
    const struct nae_transport_config transport_config = {0};
    assert(nae_transport_create(stereo, &transport_config, &transport) == NAE_OK);
    assert(nae_transport_get_engine(transport) == stereo);

    struct nae_sink *sink = NULL;
    const struct nae_sink_config sink_config = {.null_device = 1, .transport = transport};
    assert(nae_sink_create(mono, &sink_config, &sink) == NAE_ERROR_INVALID);
    assert(!sink);

    nae_transport_freep(&transport);
    nae_engine_freep(&mono);
    nae_engine_freep(&stereo);
}

int main(void)
{
    _Atomic unsigned calls = 0;
    struct nae_engine *e = NULL;
    struct nae_node *n = NULL;
    struct nae_sink *sink = NULL;
    const struct nae_config config = {48000, 2, 128};
    const struct nae_sink_config sink_config = {.null_device = 1};
    const struct nae_processor p = {.user = &calls, .process = process};
    assert(nae_engine_create(&config, &e) == NAE_OK);
    assert(nae_node_create(e, &p, &n) == NAE_OK);
    const struct nae_route route = {n, NULL};
    assert(nae_engine_commit(e, &route, 1, 0) == NAE_OK);
    assert(nae_sink_create(e, &sink_config, &sink) == NAE_OK);
    assert(nae_sink_start(sink) == NAE_OK);
    for (unsigned i = 0; i < 100 && !atomic_load(&calls); i++)
        wait_ms(10);
    assert(nae_sink_stop(sink) == NAE_OK);
    unsigned stopped_calls = atomic_load(&calls);
    assert(stopped_calls > 0 && nae_sink_underruns(sink) == 0);
    wait_ms(20);
    assert(stopped_calls == atomic_load(&calls));
    assert(nae_sink_seek(sink, 1000) == NAE_OK);
    assert(nae_sink_start(sink) == NAE_OK);
    assert(nae_sink_stop(sink) == NAE_OK);
    nae_sink_freep(&sink);
    nae_node_unrefp(&n);
    nae_engine_freep(&e);
    test_transport();
    test_foreign_transport();
    return 0;
}
