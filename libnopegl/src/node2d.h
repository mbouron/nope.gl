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
#include "blending.h"
#include "utils/utils.h"

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
    enum ngli_blending blending;
};

/*
 * Layout information for 2D nodes. Must be the first member of the private
 * node context for all nodes flagged with NGLI_NODE_FLAG_2D.
 */
struct ngli_node2d_info {
    NGLI_ATTR_ALIGNED struct aabb aabb;
    NGLI_ALIGNED_MAT(transform_matrix);
    NGLI_ATTR_ALIGNED struct aabb screen_aabb;
};

/*
 * Compute the TRS matrix from the node's ngli_node2d_opts, resolving a NAN
 * anchor to the center of the node's local aabb (from ngli_node2d_info).
 */
void ngli_node2d_compute_trs(const struct ngl_node *node, float *trs_matrix);

/*
 * Compute TRS and push composed transform + opacity onto the 2D stacks.
 * Returns 0 on success, NGL_ERROR_* on failure.
 */
int ngli_node2d_push_transform(struct ngl_node *node);

/*
 * Pop transform + opacity from the 2D stacks. Must be paired with a
 * successful ngli_node2d_push_transform().
 */
void ngli_node2d_pop_transform(struct ngl_node *node);

#endif
