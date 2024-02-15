/*
 * Copyright 2023-2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023-2024 Nope Forge
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

#include helper_srgb.glsl

void main()
{
    vec2 size = vec2(textureSize(tex, 0));
    vec2 direction = (tex_coord - center) * size;
    vec2 step = normalize(direction) / size;

    int nb_samples = 32;
    vec2 up = normalize(vec2(0.0, 1.0));
    vec4 color = vec4(0.0);
    for (int i = 0; i < nb_samples; i++) {
        vec2 offset = up * float(i) / size;
        vec4 value = texture(tex, tex_coord + offset);
        color += vec4(ngli_srgb2linear(value.rgb), value.a);
    }
    color /= float(nb_samples);
    ngl_out_color[0] = vec4(ngli_linear2srgb(color.rgb), color.a);

    vec2 down = normalize(vec2(-1.0, -0.57777));
    color = vec4(0.0);
    for (int i = 0; i < nb_samples; i++) {
        vec2 offset = down * float(i) / size;
        vec4 value = texture(tex, tex_coord + offset);
        color += vec4(ngli_srgb2linear(value.rgb), value.a);
    }
    color /= float(nb_samples);
    ngl_out_color[1] = vec4(ngli_linear2srgb(color.rgb), color.a);
}
