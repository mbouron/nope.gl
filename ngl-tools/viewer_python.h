/*
 * Copyright 2025 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef VIEWER_PYTHON_H
#define VIEWER_PYTHON_H

#include <Python.h>
#include <stddef.h>
#include <nopegl/nopegl.h>

int viewer_python_init(void);
void viewer_python_uninit(void);

/*
 * Load a Python module by .py file path or importable module name. Caller
 * must hold the GIL. Returns a new reference (DECREF when done) or NULL on
 * error, leaving the Python error set.
 */
PyObject *viewer_python_load_module(const char *path);

int viewer_python_list_scenes(const char *path, char ***namesp, size_t *nb_namesp,
                              char *err_buf, size_t err_buf_size);
void viewer_python_free_scenes(char **names, size_t nb_names);

struct ngl_scene *viewer_python_build_scene(const char *path, const char *func_name,
                                            char *err_buf, size_t err_buf_size);

#endif /* VIEWER_PYTHON_H */
