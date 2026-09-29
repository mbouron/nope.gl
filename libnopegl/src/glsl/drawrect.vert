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

const vec2 uvcoords[] = vec2[](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

vec2 ngli_map_coord(vec4 lin, vec4 off, vec2 uv)
{
    return mat2(lin.xy, lin.zw) * uv + off.xy;
}

void ngli_vertex()
{
    vec2 uvcoord = uvcoords[ngl_vertex_index];
    vec2 dir = sign(uvcoord - 0.5);
    vec2 local_pos = ngli_rect.xy + uvcoord * ngli_rect.zw + dir * ngli_margin_px;
    vec4 canvas_pos = ngli_modelview_matrix * vec4(local_pos, 0.0, 1.0);
    ngl_out_pos = ngli_projection_matrix * canvas_pos;
    ngli_clip_pos = canvas_pos.xy;

    /* Match the geometry dilation for both coverage and public coordinates. */
    vec2 rect_uv = uvcoord + dir * ngli_margin_uv;
    ngli_rect_uv = rect_uv;
    ngli_v_content_uv = ngli_map_coord(ngli_vert_content_uv_lin, ngli_vert_content_uv_off, rect_uv);
    ngli_fill_uv = ngli_map_coord(ngli_vert_fill_uv_lin, ngli_vert_fill_uv_off, rect_uv);
    ngli_stroke_uv = ngli_map_coord(ngli_vert_stroke_uv_lin, ngli_vert_stroke_uv_off, rect_uv);
    ngli_fill_tex_uv = ngli_map_coord(ngli_vert_fill_tex_uv_lin, ngli_vert_fill_tex_uv_off, rect_uv);
    ngli_stroke_tex_uv = ngli_map_coord(ngli_vert_stroke_tex_uv_lin, ngli_vert_stroke_tex_uv_off, rect_uv);
}
