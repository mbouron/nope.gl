/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "nae/nopemd.h"

struct worker { struct nae_media *media; _Atomic int terminal; };

static void *pump(void *user)
{
    struct worker *w = user;
    for (;;) {
        int ret = nae_media_pump(w->media);
        if (ret == NAE_EOF || ret < 0) {
            atomic_store(&w->terminal, ret);
            return NULL;
        }
        if (ret == NAE_AGAIN) sched_yield();
    }
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct nae_engine *e = NULL;
    struct nae_media *m = NULL;
    struct nae_node *source = NULL;
    const struct nae_config config = {48000, 2, 16};
    assert(nae_engine_create(&config, &e) == NAE_OK);
    assert(nae_media_create(e, argv[1], 2, &m) == NAE_OK);
    assert(nae_media_source(m, 0, 0, 4096, &source) == NAE_OK);
    const struct nae_route route = {source, NULL};
    assert(nae_engine_commit(e, &route, 1, 0) == NAE_OK);
    struct worker w = {.media = m};
    atomic_init(&w.terminal, 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, pump, &w));
    for (int64_t frame = 0; frame < 4096; frame += 16) {
        /* Waiting belongs to the export driver, outside the renderer. */
        while (!nae_media_queued(m) && !atomic_load(&w.terminal)) sched_yield();
        float out[32];
        assert(nae_engine_render(e, frame, 16, out) == NAE_OK);
        for (unsigned i = 0; i < 16; i++) {
            float expected = (float)(1000 + frame + i) / 32768.f;
            assert(fabsf(out[2 * i] - expected) < 0.00001f);
            assert(fabsf(out[2 * i + 1] + expected) < 0.00001f);
        }
    }
    assert(!pthread_join(thread, NULL));
    assert(atomic_load(&w.terminal) == NAE_EOF);
    nae_node_unrefp(&source);
    nae_engine_freep(&e);
    nae_media_unrefp(&m);
    return 0;
}
