/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "nae/nae.h"

struct signal {
    int calls, resets, destroyed;
    float value;
};

static int constant(void *user, const struct nae_process *p)
{
    struct signal *s = user;
    s->calls++;
    for (size_t i = 0; i < (size_t)p->frames * p->channels; i++)
        p->output[i] = s->value;
    return NAE_OK;
}

static void reset(void *user, int64_t frame)
{
    (void)frame;
    ((struct signal *)user)->resets++;
}

static void destroy(void *user)
{
    ((struct signal *)user)->destroyed++;
}

static struct nae_node *signal_node(struct nae_engine *e, struct signal *s)
{
    struct nae_node *n = NULL;
    struct nae_processor p = {.user = s, .process = constant, .reset = reset, .destroy = destroy};
    assert(nae_node_create(e, &p, &n) == NAE_OK);
    return n;
}

static void close_to(float a, float b)
{
    assert(fabsf(a - b) < 0.00001f);
}

static void test_mix_edit(void)
{
    struct nae_engine *e = NULL;
    struct nae_config config = {.sample_rate = 48000, .channels = 2, .block_frames = 4};
    assert(nae_engine_create(&config, &e) == NAE_OK);
    struct signal a = {.value = 1}, b = {.value = 2};
    struct nae_node *na = signal_node(e, &a), *nb = signal_node(e, &b), *gain = NULL, *mix = NULL;
    assert(nae_gain_create(e, 0.5f, 4, &gain) == NAE_OK);
    assert(nae_mix_create(e, 2, &mix) == NAE_OK);
    /* Deliberately not topologically ordered; na fans out twice. */
    uint32_t mix_inputs[] = {1, 2}, gain_inputs[] = {2};
    struct nae_route routes[] = {{mix, mix_inputs}, {gain, gain_inputs}, {na, NULL}, {nb, NULL}};
    assert(nae_engine_commit(e, routes, 4, 0) == NAE_OK);
    assert(nae_engine_commit(e, routes, 4, 0) == NAE_AGAIN);
    float out[22];
    assert(nae_engine_render(e, 0, 3, out) == NAE_OK);
    for (int i = 0; i < 6; i++)
        close_to(out[i], 1.5f);
    assert(a.calls == 1 && b.calls == 0); /* Reachability and fan-out. */
    assert(nae_gain_set(gain, 1) == NAE_OK);
    assert(nae_engine_render(e, 3, 2, out) == NAE_OK);
    close_to(out[0], 1.625f);
    close_to(out[2], 1.75f);
    /* Preserve a half-completed fader ramp across a topology replacement. */
    mix_inputs[1] = 3;
    assert(nae_engine_commit(e, routes, 4, 0) == NAE_OK);
    nae_node_unrefp(&na);
    nae_node_unrefp(&nb);
    nae_node_unrefp(&gain);
    nae_node_unrefp(&mix);
    assert(nae_engine_render(e, 5, 11, out) == NAE_OK);
    close_to(out[0], 2.875f);
    close_to(out[2], 3);
    for (int i = 4; i < 22; i++)
        close_to(out[i], 3);
    assert(a.resets == 1 && b.resets == 1);
    assert(nae_engine_collect(e) == NAE_OK);
    assert(a.destroyed == 0 && b.destroyed == 0);
    assert(nae_engine_render(e, 100, 1, out) == NAE_OK);
    assert(a.resets == 2 && b.resets == 2);
    assert(nae_engine_commit(e, NULL, 0, 0) == NAE_OK);
    assert(nae_engine_render(e, 101, 1, out) == NAE_OK);
    close_to(out[0], 0);
    assert(a.destroyed == 0); /* No destructors on render thread. */
    assert(nae_engine_collect(e) == NAE_OK);
    assert(a.destroyed == 1 && b.destroyed == 1);
    nae_engine_freep(&e);
}

static int sample_numbers(void *user, int64_t first, uint32_t frames, float *out)
{
    (void)user;
    for (uint32_t i = 0; i < frames; i++)
        out[i] = (float)(first + i);
    return NAE_OK;
}

static void test_schedule(void)
{
    struct nae_engine *e = NULL;
    struct nae_config c = {48000, 1, 4};
    assert(nae_engine_create(&c, &e) == NAE_OK);
    struct nae_source s = {.read = sample_numbers, .timeline_start = 3, .source_start = 17, .duration = 5};
    struct nae_node *n = NULL;
    assert(nae_source_create(e, &s, &n) == NAE_OK);
    struct nae_route r = {n, NULL};
    assert(nae_engine_commit(e, &r, 1, 0) == NAE_OK);
    float out[11];
    assert(nae_engine_render(e, 0, 11, out) == NAE_OK);
    const float expected[] = {0, 0, 0, 17, 18, 19, 20, 21, 0, 0, 0};
    assert(!memcmp(out, expected, sizeof(out)));
    assert(nae_source_set_range(n, 0, 21, 11) == NAE_OK);
    /* Editing the authoring range cannot mutate a published plan. */
    assert(nae_engine_render(e, 0, 11, out) == NAE_OK);
    assert(!memcmp(out, expected, sizeof(out)));
    assert(nae_engine_commit(e, &r, 1, 0) == NAE_OK);
    assert(nae_engine_render(e, 0, 11, out) == NAE_OK);
    for (unsigned i = 0; i < 11; i++)
        close_to(out[i], (float)(21 + i));
    nae_node_unrefp(&n);
    nae_engine_freep(&e);
}

static void test_validation(void)
{
    struct nae_engine *e = NULL;
    struct nae_config c = {48000, 2, 4};
    assert(nae_engine_create(&c, &e) == NAE_OK);
    struct nae_node *a = NULL, *b = NULL;
    assert(nae_gain_create(e, NAN, 1, &a) == NAE_ERROR_INVALID);
    assert(nae_gain_create(e, 1, 1, &a) == NAE_OK);
    assert(nae_gain_create(e, 1, 1, &b) == NAE_OK);
    uint32_t ia[] = {1}, ib[] = {0};
    struct nae_route r[] = {{a, ia}, {b, ib}};
    assert(nae_engine_commit(e, r, 2, 0) == NAE_ERROR_INVALID); /* Cycle. */
    ia[0] = 7;
    assert(nae_engine_commit(e, r, 2, 0) == NAE_ERROR_INVALID);
    ia[0] = NAE_INPUT_NONE;
    r[1].node = a;
    assert(nae_engine_commit(e, r, 2, 0) == NAE_ERROR_INVALID); /* Same instance twice. */
    r[1].node = b;
    assert(nae_engine_commit(e, r, 2, 1) == NAE_OK);
    nae_node_unrefp(&a);
    nae_node_unrefp(&b);
    nae_engine_freep(&e); /* Also frees an unadopted plan. */
}

static int ones(void *user, const struct nae_process *p)
{
    (void)user;
    for (uint32_t i = 0; i < p->frames * p->channels; i++)
        p->output[i] = 1;
    return NAE_OK;
}

static void test_fader(void)
{
    struct nae_engine *e = NULL;
    const struct nae_config c = {48000, 2, 16};
    assert(nae_engine_create(&c, &e) == NAE_OK);
    struct nae_node *one = NULL, *fader = NULL;
    const struct nae_processor p = {.process = ones};
    assert(nae_node_create(e, &p, &one) == NAE_OK);
    assert(nae_fader_create(e, &fader) == NAE_OK);
    uint32_t in[] = {0};
    const struct nae_route r[] = {{one, NULL}, {fader, in}};
    assert(nae_engine_commit(e, r, 2, 1) == NAE_OK);
    float out[2 * 40];
    assert(nae_engine_render(e, 0, 40, out) == NAE_OK);
    for (int i = 0; i < 80; i++)
        close_to(out[i], 1);
    const struct nae_keyframe k[] = {{10, 0, 0.5f}, {20, 10, 1.5f}, {35, 2, 0}};
    assert(nae_fader_set_keyframes(fader, k, 3) == NAE_OK);
    /* Snapshotted by the next commit */
    assert(nae_engine_render(e, 0, 40, out) == NAE_OK);
    close_to(out[30], 1);
    assert(nae_engine_commit(e, r, 2, 1) == NAE_OK);
    assert(nae_engine_render(e, 0, 40, out) == NAE_OK);
    for (int i = 0; i < 40; i++) {
        float v = i < 10 ? 1 : i < 20 ? 0.5f : i < 30 ? 0.5f + (float)(i - 20) / 10 : i < 35 ? 1.5f : i < 37 ? 1.5f * (1 - (float)(i - 35) / 2) : 0;
        close_to(out[2 * i], v);
        close_to(out[2 * i + 1], v);
    }
    /* From any frame, as after a seek */
    assert(nae_engine_render(e, 25, 1, out) == NAE_OK);
    close_to(out[0], 1);
    const struct nae_keyframe unsorted[] = {{10, 0, 1}, {5, 0, 1}};
    assert(nae_fader_set_keyframes(fader, unsorted, 2) == NAE_ERROR_INVALID);
    const struct nae_keyframe negative[] = {{10, 0, -1}};
    assert(nae_fader_set_keyframes(fader, negative, 1) == NAE_ERROR_INVALID);
    assert(nae_fader_set_keyframes(one, k, 1) == NAE_ERROR_INVALID);
    assert(nae_fader_set_keyframes(fader, NULL, 0) == NAE_OK);
    assert(nae_engine_commit(e, r, 2, 1) == NAE_OK);
    assert(nae_engine_render(e, 30, 1, out) == NAE_OK);
    close_to(out[0], 1);
    nae_node_unrefp(&one);
    nae_node_unrefp(&fader);
    nae_engine_freep(&e);
}

int main(void)
{
    test_mix_edit();
    test_schedule();
    test_validation();
    test_fader();
    puts("nae core: OK");
    return 0;
}
