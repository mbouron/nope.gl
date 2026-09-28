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
    /*
     * Pixel centers sit at half-integer canvas coordinates, so the quad maps
     * the RTT texel-exactly without any offset.
     */
    vec2 position = rect.xy + quad_coord * rect.zw;
    vec4 canvas_pos = modelview_matrix * vec4(position, 0.0, 1.0);
    ngl_out_pos = projection_matrix * canvas_pos;

    /*
     * The quad may be cropped to the visible canvas: the rect coordinates
     * span the whole effect, while the input covers the quad only.
     */
    ngli_rect_px = position - effect_rect.xy;
    ngli_rect_uv = ngli_rect_px / effect_rect.zw;
    ngli_tex_coord = quad_coord;
    ngli_canvas_px = canvas_pos.xy;
}
