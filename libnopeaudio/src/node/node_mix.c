/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stddef.h>

#include "nopeaudio/nopeaudio.h"

/* Sum the inputs into the output, which is cleared before the call */
static int mix_process(void *user, const struct nau_block *p)
{
    for (uint32_t bus = 0; bus < p->input_count; bus++)
        for (size_t i = 0; i < (size_t)p->frames * p->channels; i++)
            p->output[i] += p->inputs[bus][i];
    return NAU_OK;
}

int nau_mix_init(struct nau_node *node, struct nau_engine *s, uint32_t input_count)
{
    const struct nau_processor processor = {
        .input_count = input_count,
        .process     = mix_process,
    };
    return nau_node_init(node, s, &processor);
}
