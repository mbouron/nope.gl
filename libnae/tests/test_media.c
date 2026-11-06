/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "nae/nopemd.h"

static int fill(struct nae_media *m)
{
    int ret;
    unsigned attempts = 0;
    do {
        ret = nae_media_pump(m);
        assert(++attempts < 10000);
    } while (ret == NAE_OK);
    assert(ret == NAE_AGAIN || ret == NAE_EOF);
    return ret;
}

static void check(const float *out, int64_t first, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        float sample = first + i < 4096 ? (float)(1000 + first + i) / 32768.f : 0;
        assert(fabsf(out[2 * i] - sample) < 0.00001f);
        assert(fabsf(out[2 * i + 1] + sample) < 0.00001f);
    }
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    struct nae_engine *e = NULL;
    struct nae_media *m = NULL;
    struct nae_node *source = NULL;
    const struct nae_config c = {48000, 2, 64};
    assert(nae_engine_create(&c, &e) == NAE_OK);
    assert(nae_media_create(e, argv[1], 16, &m) == NAE_OK);
    assert(nae_media_seek(m, INT64_MAX) == NAE_ERROR_INVALID);
    assert(nae_media_source(m, 17, 101, 10000, &source) == NAE_OK);
    const struct nae_route r = {source, NULL};
    assert(nae_engine_commit(e, &r, 1, 0) == NAE_OK);
    float out[512];
    /* Starvation must not shift the timeline when old PCM finally arrives. */
    assert(nae_engine_render(e, 0, 64, out) == NAE_AGAIN);
    for (unsigned i = 0; i < 128; i++) assert(out[i] == 0);
    fill(m);
    assert(nae_engine_render(e, 64, 197, out) == NAE_OK);
    check(out, 148, 197);
    /* Sample-level seek, including a decoder frame containing the target. */
    assert(nae_media_seek(m, 101) == NAE_OK);
    fill(m);
    assert(nae_engine_render(e, 17, 17, out) == NAE_OK);
    check(out, 101, 17);
    assert(nae_source_set_range(source, 0, 0, 10000) == NAE_OK);
    assert(nae_engine_commit(e, &r, 1, 0) == NAE_OK);
    assert(nae_media_seek(m, 0) == NAE_OK);
    for (int64_t frame = 0; frame < 4608; frame += 64) {
        fill(m);
        assert(nae_engine_render(e, frame, 64, out) == NAE_OK);
        check(out, frame, 64);
    }
    assert(nae_media_pump(m) == NAE_EOF);
    assert(nae_media_pump(m) == NAE_EOF); /* Never implicitly restart nmd. */
    assert(nae_media_queued(m) == 0);
    nae_node_unrefp(&source);
    nae_engine_freep(&e);
    nae_media_unrefp(&m);
    /* Source-rate conversion must not introduce holes/duplicates between
     * decoder frames, and must drain the resampler at EOF. */
    assert(nae_engine_create(&c, &e) == NAE_OK);
    assert(nae_media_create(e, argv[2], 16, &m) == NAE_OK);
    assert(nae_media_source(m, 0, 0, 10000, &source) == NAE_OK);
    const struct nae_route resampled = {source, NULL};
    assert(nae_engine_commit(e, &resampled, 1, 0) == NAE_OK);
    for (int64_t frame = 0; frame < 5120; frame += 64) {
        fill(m);
        assert(nae_engine_render(e, frame, 64, out) == NAE_OK);
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
    nae_node_unrefp(&source);
    nae_engine_freep(&e);
    nae_media_unrefp(&m);
    puts("nae media: OK");
    return 0;
}
