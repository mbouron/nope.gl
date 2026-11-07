/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "nae/nae.h"

struct state {
    struct nae_engine *engine;
    _Atomic int stop;
    pthread_t control;
    unsigned destroyed;
};
struct source {
    struct state *owner;
    float value;
};

static int process(void *user, const struct nae_process *p)
{
    const struct source *s = user;
    for (unsigned i = 0; i < p->frames * p->channels; i++)
        p->output[i] = s->value;
    return NAE_OK;
}

static void destroy(void *user)
{
    struct source *s = user;
    assert(pthread_equal(pthread_self(), s->owner->control));
    s->owner->destroyed++;
    free(s);
}

static void *render(void *user)
{
    struct state *s = user;
    int64_t frame = 0;
    float out[514];
    while (!atomic_load(&s->stop)) {
        assert(nae_engine_render(s->engine, frame, 257, out) == NAE_OK);
        for (unsigned i = 0; i < 514; i++)
            assert(out[i] >= 0 && out[i] <= 2);
        frame += 257;
    }
    return NULL;
}

int main(void)
{
    struct state s = {.control = pthread_self()};
    atomic_init(&s.stop, 0);
    const struct nae_config config = {48000, 2, 64};
    assert(nae_engine_create(&config, &s.engine) == NAE_OK);
    struct nae_node *gain = NULL;
    assert(nae_gain_create(s.engine, 1, 240, &gain) == NAE_OK);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, render, &s));
    for (unsigned i = 0; i < 5000; i++) {
        struct source *source = malloc(sizeof(*source));
        assert(source);
        *source = (struct source){.owner = &s, .value = 1 + (float)(i & 1)};
        struct nae_node *node = NULL;
        const struct nae_processor p = {.user = source, .process = process, .destroy = destroy};
        assert(nae_node_create(s.engine, &p, &node) == NAE_OK);
        uint32_t inputs[] = {0};
        const struct nae_route routes[] = {{node, NULL}, {gain, inputs}};
        int ret;
        do {
            ret = nae_engine_commit(s.engine, routes, 2, 1);
            if (ret == NAE_AGAIN)
                sched_yield();
        } while (ret == NAE_AGAIN);
        assert(ret == NAE_OK);
        assert(nae_gain_set(gain, (i & 1) ? 0.25f : 1.f) == NAE_OK);
        nae_node_unrefp(&node);
    }
    atomic_store(&s.stop, 1);
    assert(!pthread_join(thread, NULL));
    nae_node_unrefp(&gain);
    nae_engine_freep(&s.engine);
    assert(s.destroyed == 5000);
    return 0;
}
