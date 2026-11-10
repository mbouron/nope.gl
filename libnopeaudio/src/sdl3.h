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

#ifndef NAU_SDL3_H
#define NAU_SDL3_H

#include <SDL3/SDL_log.h>

/*
 * SDL's audio subsystem, for the code opening SDL devices. sink_sdl3.c holds
 * its state, see there.
 */

/* Shown with SDL_LOGGING=audio=info */
#define LOG_INFO(...) SDL_LogInfo(SDL_LOG_CATEGORY_AUDIO, __VA_ARGS__)
#define LOG_ERROR(...) SDL_LogError(SDL_LOG_CATEGORY_AUDIO, __VA_ARGS__)

/*
 * Take a reference to the subsystem for a device, unless it is not
 * initialized. nau_sink_backend_uninit() fails while a device holds one.
 */
int nau_sdl3_acquire(void);
void nau_sdl3_release(void);

/*
 * Lock protecting the devices and their streams, between the main thread
 * polling and the threads controlling them
 */
void nau_sdl3_lock(void);
void nau_sdl3_unlock(void);

#endif
