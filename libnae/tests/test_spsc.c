/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/spsc.h"

static void test_wrap(size_t first)
{
    struct spsc *queue = calloc(1, sizeof(*queue));
    assert(queue);
    assert(spsc_init(queue, 3, sizeof(size_t)) == 0);
    assert(spsc_capacity(queue) == 4);
    atomic_store(&queue->head, first);
    atomic_store(&queue->tail, first);

    size_t value;
    assert(!spsc_pop(queue, &value));
    for (size_t round = 0; round < 100; round++) {
        for (size_t i = 0; i < 4; i++) {
            value = 4 * round + i;
            assert(spsc_push(queue, &value));
        }
        assert(!spsc_push(queue, &value));
        for (size_t i = 0; i < 4; i++) {
            assert(spsc_pop(queue, &value));
            assert(value == 4 * round + i);
        }
        assert(!spsc_pop(queue, &value));
    }
    spsc_uninit(queue);
    free(queue);
}

int main(void)
{
    static_assert(_Alignof(struct spsc) <= _Alignof(max_align_t));
    static_assert(offsetof(struct spsc, tail) - offsetof(struct spsc, head) >= 64);
    test_wrap(0);
    test_wrap(SIZE_MAX - 1);

    struct spsc queue = {0};
    assert(spsc_init(&queue, 0, 1) < 0);
    assert(spsc_init(&queue, 1, 0) < 0);
    assert(spsc_init(&queue, SIZE_MAX, 1) < 0);
    assert(spsc_init(&queue, 2, SIZE_MAX) < 0);
    spsc_uninit(&queue);
    return 0;
}
