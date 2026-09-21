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

#ifndef NODE_GRAPH_EDIT_H
#define NODE_GRAPH_EDIT_H

#include <stddef.h>

struct ngl_node;
struct node_param;

int ngli_node_graph_edit_add_children(struct ngl_node *node, const struct node_param *par,
                                      size_t nb_nodes, struct ngl_node **nodes);
int ngli_node_graph_edit_insert_children(struct ngl_node *node, const struct node_param *par,
                                         size_t index, size_t nb_nodes, struct ngl_node **nodes);
int ngli_node_graph_edit_remove_children(struct ngl_node *node, const struct node_param *par,
                                         size_t nb_nodes, struct ngl_node **nodes);
int ngli_node_graph_edit_swap_children(struct ngl_node *node, const struct node_param *par,
                                       size_t from, size_t to);

#endif
