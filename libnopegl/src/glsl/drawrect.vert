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

void main()
{
    /*
     * Dilate the quad outward by ngli_margin_px canvas pixels to ensure fragments
     * exist beyond the shape boundary for:
     *   - outside/center outline (which extends past the geometry edge)
     *   - correct fwidth() derivatives (need one extra pixel of coverage)
     *
     * sign(uvcoord - 0.5) gives the outward direction at each corner:
     *   (0,0) → (-1,-1),  (1,0) → (1,-1),  (0,1) → (-1,1),  (1,1) → (1,1)
     *
     * ngli_uv is shifted by ngli_margin_uv = ngli_margin_px / ngli_rect_size so that
     * pos = (ngli_uv - 0.5) * ngli_rect_size in the fragment shader remains the correct
     * canvas-pixel coordinate for SDF evaluation.
     * ngli_tex_coord uses the original uvcoord (undilated) so texture sampling is
     * unaffected.
     */
    vec2 dir = sign(uvcoord - 0.5);
    vec4 canvas_pos = modelview_matrix * vec4(position.xy + dir * ngli_margin_px, 0.0, 1.0);
    ngl_out_pos = projection_matrix * canvas_pos;
    /* Canvas-pixel position, used to test cascaded clip planes from Group2D. */
    ngli_clip_pos = canvas_pos.xy;
    ngli_uv = uvcoord + dir * ngli_margin_uv;
    vec2 adj_uvcoord = (uvcoord - 0.5) * ngli_uv_scale + 0.5;
    ngli_tex_coord = (tex_coord_matrix * vec4(adj_uvcoord, 0.0, 1.0)).xy;
}
