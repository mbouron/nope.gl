/*
 * Copyright 2022 GoPro Inc.
 * Copyright 2022 Clément Bœsch <u pkh.me>
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

void main()
{
    /*
     * Dilate the quad outward by margin_px canvas pixels to ensure fragments
     * exist beyond the shape boundary for:
     *   - outside/center outline (which extends past the geometry edge)
     *   - correct fwidth() derivatives (need one extra pixel of coverage)
     *
     * sign(uvcoord - 0.5) gives the outward direction at each corner:
     *   (0,0) → (-1,-1),  (1,0) → (1,-1),  (0,1) → (-1,1),  (1,1) → (1,1)
     *
     * uv is shifted by margin_uv = margin_px / rect_size so that
     * pos = (uv - 0.5) * rect_size in the fragment shader remains the correct
     * canvas-pixel coordinate for SDF evaluation.
     * tex_coord uses the original uvcoord (undilated) so texture sampling is
     * unaffected.
     */
    vec2 dir = sign(uvcoord - 0.5);
    ngl_out_pos = projection_matrix * modelview_matrix * vec4(position.xy + dir * margin_px, 0.0, 1.0);
    uv = uvcoord + dir * margin_uv;
    vec2 adj_uvcoord = (uvcoord - 0.5) * uv_scale + 0.5;
    tex_coord = (tex_coord_matrix * vec4(adj_uvcoord, 0.0, 1.0)).xy;
}
