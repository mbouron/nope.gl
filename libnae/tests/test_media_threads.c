/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include "nae/nopemd.h"
#include "../examples/wait.h"

/*
 * The renderer playing while the control thread seeks and moves the loop
 * around, as an editor does: only valid audio plays, nothing crashes, and
 * the sanitizers find no race.
 */

struct control {
    struct nae_transport *transport;
    struct nae_media *media;
    _Atomic int stop;
};

static void *run_control(void *arg)
{
    struct control *c = arg;
    unsigned seed = 1;
    for (int i = 0; i < 400 && !atomic_load(&c->stop); i++) {
        seed = seed * 1103515245 + 12345;
        const int64_t start = (seed >> 8) % 40000, end = start + 2400 + (seed >> 4) % 8000;
        if (i % 3 == 0) {
            nae_transport_set_looping(c->transport, 1);
            assert(nae_transport_set_loop_range(c->transport, start, end) == NAE_OK);
            assert(nae_media_set_loop(c->media, start, end) == NAE_OK);
        } else if (i % 3 == 1) {
            assert(nae_transport_seek(c->transport, (seed >> 3) % 48000) == NAE_OK);
        } else {
            nae_transport_set_looping(c->transport, 0);
            assert(nae_media_set_loop(c->media, 0, 0) == NAE_OK);
        }
        wait_ms(2);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct nae_engine *e = NULL;
    struct nae_media *m = NULL;
    struct nae_node *source = NULL;
    struct nae_transport *t = NULL;
    const struct nae_config config = {48000, 2, 64};
    assert(nae_engine_create(&config, &e) == NAE_OK);
    assert(nae_media_create(e, argv[1], NULL, &m) == NAE_OK);
    assert(nae_media_source(m, 0, 0, 48000, &source) == NAE_OK);
    const struct nae_route route = {source, NULL};
    assert(nae_engine_commit(e, &route, 1, 0) == NAE_OK);
    const struct nae_transport_config tc = {.declick_frames = 240, .ready_timeout_frames = 4800};
    assert(nae_transport_create(e, &tc, &t) == NAE_OK);
    assert(nae_transport_set_duration(t, 48000) == NAE_OK);
    nae_transport_play(t);

    struct control c = {.transport = t, .media = m};
    atomic_init(&c.stop, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, run_control, &c));
    float out[2 * 256];
    int64_t audible = 0;
    for (int i = 0; i < 2000; i++) {
        if (i % 4 == 0)
            nae_transport_play(t);
        assert(nae_transport_render(t, 0, 256, out) >= 0);
        for (unsigned f = 0; f < 512; f++) {
            assert(isfinite(out[f]) && fabsf(out[f]) <= 1.f);
            audible += out[f] != 0;
        }
        if (i % 8 == 0)
            wait_ms(1);
    }
    atomic_store(&c.stop, 1);
    assert(!pthread_join(thread, NULL));
    struct nae_media_stats stats;
    nae_media_get_stats(m, &stats);
    printf("concurrent: %" PRId64 " audible samples, %" PRId64 " requests, %" PRId64 " errors\n",
           audible, stats.requests, stats.errors);
    assert(audible > 0 && stats.errors == 0);
    nae_transport_freep(&t);
    nae_node_unrefp(&source);
    nae_engine_freep(&e);
    nae_media_unrefp(&m);
    return 0;
}
