/*
 * Copyright 2018-2022 GoPro Inc.
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

#ifndef EGL_H
#define EGL_H

#include "config.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "ngpu/opengl/glcontext.h"

EGLImageKHR ngpu_eglCreateImageKHR(struct glcontext *gl,
                                   EGLContext handle,
                                   EGLenum target,
                                   EGLClientBuffer buffer,
                                   const EGLint *attrib_list);

EGLBoolean ngpu_eglDestroyImageKHR(struct glcontext *gl,
                                   EGLImageKHR image);

#if defined(TARGET_ANDROID)
EGLClientBuffer ngpu_eglGetNativeClientBufferANDROID(struct glcontext *gl,
                                                     const struct AHardwareBuffer *buffer);
#endif

const char *ngpu_eglGetDisplayDriverName(struct glcontext *gl);

#endif
