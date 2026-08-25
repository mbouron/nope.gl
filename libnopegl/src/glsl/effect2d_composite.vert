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

const vec2 quad_coords[] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

void main()
{
    vec2 quad_coord = quad_coords[ngl_vertex_index];
    /* Keep rasterization consistent across backends by shifting Y half a pixel. */
    vec2 position = rect.xy + quad_coord * rect.zw + vec2(0.0, 0.5);
    ngl_out_pos = projection_matrix * modelview_matrix * vec4(position, 0.0, 1.0);

    /*
     * Offset by half a canvas pixel so bilinear samples hit RTT texel centers.
     * Y uses a full pixel to compensate for the position shift above.
     */
    vec2 uv_coord = quad_coord + vec2(0.5, 1.0) / rect.zw;
    uv = uv_coord;
    tex_coord = (tex_coord_matrix * vec4(uv_coord, 0.0, 1.0)).xy;
}
