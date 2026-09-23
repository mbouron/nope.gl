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

#ifndef NODE_GRAPH_H
#define NODE_GRAPH_H

#include <stddef.h>
#include <stdint.h>

struct ngl_node;
struct node_param;

uint64_t ngli_node_graph_new_traversal_id(void);

typedef int (*ngli_node_graph_edge_func)(void *user_arg, struct ngl_node *parent,
                                         struct ngl_node *child);

int ngli_node_graph_foreach_child(ngli_node_graph_edge_func func, void *user_arg,
                                  struct ngl_node *node);

struct ngli_node_graph_range {
    size_t index;
    size_t count;
};

struct ngli_node_graph_range ngli_node_graph_get_param_range(const struct ngl_node *node,
                                                             const struct node_param *par);

#endif
