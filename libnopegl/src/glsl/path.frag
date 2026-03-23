/*
 * Copyright 2023 Nope Forge
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

#include path.glsl
#include slug.glsl

void main()
{
    bool needDist = outline != 0.0 || glow != 0.0 || blur != 0.0;
    vec2 result = SlugRender(texcoord, banding, glyph, needDist);
    float coverage = result.x;

    if (path_is_open) {
        float dist = -abs(result.y) * dist_scale;
        ngl_out_color = get_path_color(dist, vec4(color, opacity), vec4(outline_color, outline), vec4(glow_color, glow), blur, outline_pos);
    } else if (!needDist) {
        ngl_out_color = vec4(vec3(color) * opacity, opacity) * coverage;
    } else {
        ngl_out_color = get_path_color(result.y * dist_scale, vec4(color, opacity), vec4(outline_color, outline), vec4(glow_color, glow), blur, outline_pos);
    }
}
