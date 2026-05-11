/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#ifndef VIEWER_CONFIG_H
#define VIEWER_CONFIG_H

/*
 * Persisted viewer configuration. Mirrors the on-disk schema 1:1; adding a
 * setting means adding a field here plus matching save/load lines.
 */
struct config_data {
    char  script_path[2048];
    char  scene_name[2048];
    float panel_ratio; /* 0 = not set / use default. */
};

/* Write the config to the app's pref directory. */
void config_save(const struct config_data *cfg);

/* Read the config from the app's pref directory. cfg is zeroed first; fields
 * for which no value is found stay empty. */
void config_load(struct config_data *cfg);

#endif
