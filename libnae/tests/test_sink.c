/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdatomic.h>
#include "nae/miniaudio.h"
#include "../examples/wait.h"

static int process(void *user, const struct nae_process *p)
{
    (void)p;
    atomic_fetch_add((_Atomic unsigned *)user, 1);
    return NAE_OK;
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
    for (unsigned i = 0; i < 100 && !atomic_load(&calls); i++) wait_ms(10);
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
    return 0;
}
