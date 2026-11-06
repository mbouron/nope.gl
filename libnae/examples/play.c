/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nae/nopemd.h"
#include "nae/miniaudio.h"
#include "wait.h"

#define MAX_TRACKS 32
static volatile sig_atomic_t interrupted;
static void interrupt_handler(int signal_number) { (void)signal_number; interrupted = 1; }

int main(int argc, char **argv)
{
    int arg = 1;
    struct nae_sink_config sink_config = {0};
    if (arg < argc && !strcmp(argv[arg], "--null")) {
        sink_config.null_device = 1;
        arg++;
    }
    const int count = argc - arg;
    if (count < 1 || count > MAX_TRACKS) {
        fprintf(stderr, "Usage: %s [--null] audio-file [audio-file ...] (up to %d tracks)\n", argv[0], MAX_TRACKS);
        return 1;
    }
    struct nae_engine *engine = NULL;
    struct nae_sink *sink = NULL;
    struct nae_media *media[MAX_TRACKS] = {0};
    struct nae_node *nodes[2 * MAX_TRACKS + 2] = {0};
    struct nae_route routes[2 * MAX_TRACKS + 2] = {0};
    uint32_t gain_inputs[MAX_TRACKS], mix_inputs[MAX_TRACKS], master_input;
    int64_t length = 0;
    const struct nae_config config = {48000, 2, 256};
    int ret = nae_engine_create(&config, &engine);
    if (ret < 0) goto end;
    for (int i = 0; i < count; i++) {
        ret = nae_media_create(engine, argv[arg + i], NULL, &media[i]);
        if (ret < 0) goto end;
        ret = nae_media_source(media[i], 0, 0, INT64_MAX, &nodes[2 * i]);
        if (ret < 0) goto end;
        ret = nae_gain_create(engine, 1.f / (float)count, 240, &nodes[2 * i + 1]);
        if (ret < 0) goto end;
        gain_inputs[i] = (uint32_t)(2 * i);
        mix_inputs[i] = (uint32_t)(2 * i + 1);
        routes[2 * i] = (struct nae_route){nodes[2 * i], NULL};
        routes[2 * i + 1] = (struct nae_route){nodes[2 * i + 1], &gain_inputs[i]};
        if (nae_media_get_length(media[i]) > length) length = nae_media_get_length(media[i]);
    }
    ret = nae_mix_create(engine, (uint32_t)count, &nodes[2 * count]);
    if (ret < 0) goto end;
    ret = nae_gain_create(engine, 1, 240, &nodes[2 * count + 1]);
    if (ret < 0) goto end;
    master_input = (uint32_t)(2 * count);
    routes[2 * count] = (struct nae_route){nodes[2 * count], mix_inputs};
    routes[2 * count + 1] = (struct nae_route){nodes[2 * count + 1], &master_input};
    ret = nae_engine_commit(engine, routes, (uint32_t)(2 * count + 2), (uint32_t)(2 * count + 1));
    if (ret != NAE_OK) goto end;
    ret = nae_sink_create(engine, &sink_config, &sink);
    if (ret < 0) goto end;
    signal(SIGINT, interrupt_handler);
    ret = nae_sink_start(sink);
    if (ret < 0) goto end;
    /* The media decode on threads of their own: wait for the longest to play */
    for (int64_t waited = 0; !interrupted && waited < length * 1000 / config.sample_rate + 200; waited += 10) {
        if (nae_sink_status(sink) != NAE_OK) {
            ret = NAE_ERROR_IO;
            goto end;
        }
        nae_engine_collect(engine);
        wait_ms(10);
    }
    fprintf(stderr, "source underrun callbacks: %u\n", nae_sink_underruns(sink));
    ret = NAE_OK;
end:
    nae_sink_freep(&sink);
    for (unsigned i = 0; i < 2 * MAX_TRACKS + 2; i++) nae_node_unrefp(&nodes[i]);
    nae_engine_freep(&engine);
    for (unsigned i = 0; i < MAX_TRACKS; i++) nae_media_unrefp(&media[i]);
    if (ret < 0) fprintf(stderr, "nae-play failed: %d\n", ret);
    return ret < 0 ? 1 : 0;
}
