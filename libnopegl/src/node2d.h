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

#ifndef NODE2D_H
#define NODE2D_H

#include "aabb.h"
#include "blend_mode.h"
#include "utils/utils.h"

struct ngl_ctx;
struct ngl_node;

/*
 * Common options for 2D nodes that participate in the transform hierarchy.
 * Embedded in each 2D node's opts struct and accessed via
 * node_class.node2d_opts_offset.
 */
struct ngli_node2d_opts {
    struct ngl_node *translate_node;
    float translate[2];
    struct ngl_node *rotation_node;
    float rotation;
    struct ngl_node *scale_node;
    float scale[2];
    struct ngl_node *anchor_node;
    float anchor[2];
    struct ngl_node *opacity_node;
    float opacity;
    int visible;
    enum ngli_blend_mode blend_mode;
};

/*
 * Layout information for 2D nodes. Must be the first member of the private
 * node context for all nodes flagged with NGLI_NODE_FLAG_2D.
 */
struct ngli_node2d_info {
    NGLI_ATTR_ALIGNED struct aabb aabb;
    struct ngli_mat4 transform_matrix;
    NGLI_ATTR_ALIGNED struct aabb screen_aabb;
    float effect_margin;
};

/*
 * Compute the TRS matrix from the node's ngli_node2d_opts, resolving a NAN
 * anchor to the center of the node's local aabb (from ngli_node2d_info).
 */
void ngli_node2d_compute_trs(const struct ngl_node *node, float *trs_matrix);

/*
 * Compute TRS and apply the node's transform + opacity to the current 2D
 * state.
 */
void ngli_node2d_apply_transform(struct ngl_node *node);

/*
 * Set the default transform and opacity to the current 2D state.
 */
void ngli_node2d_apply_default_transform(struct ngl_ctx *ctx);

/*
 * Scale the uniform margin added on all four sides of a node's local AABB
 * into a single screen-space margin.
 *
 * The modelview can expand that margin by different amounts along the screen x
 * and y axes, so use the larger amount to conservatively cover both axes. See
 * ngli_node2d_info.effect_margin.
 */
float ngli_node2d_scale_effect_margin(const struct ngli_mat4 *modelview, float effect_margin);

/*
 * Rounded-rectangle clip entry.
 */
struct ngli_clip2d {
    struct ngli_vec4 inv; // canvas->local 2x2 matrix (inverse of local trs)
    struct ngli_vec4 rect; // clip rectangle (center_x, center_y, half_w, half_h)
    struct ngli_vec4 radius; // corner radii (radius_x, radius_y, 0, 0)
};

/*
 * Compute a cascading rounded-rectangle clip from clip rectangle local
 * parameters.
 */
bool ngli_node2d_compute_clip(const struct ngli_mat4 *modelview,
                              const float clip_rect[4], const float corner_radius[2],
                              struct ngli_clip2d *out);

#endif
