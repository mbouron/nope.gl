/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NAE_INTERNAL_H
#define NAE_INTERNAL_H

#include <stdint.h>

#include "nae/nae.h"

/* nae_engine_render(), where a jump to first_frame is a loop going round when
 * `wrap` is set, a commit pending adopted only when `adopt` is set */
int nae_engine_render_jump(struct nae_engine *engine, int64_t first_frame, uint32_t frames,
                           float *output, int wrap, int adopt);

/* Render thread: positions the nodes at `frame` as a jump there would,
 * without rendering, for the sources to get their data ahead. Forced,
 * as when nothing rendered meanwhile, the nodes reset even if already
 * there. */
void nae_engine_prepare(struct nae_engine *engine, int64_t frame, int wrap, int force);

#endif
