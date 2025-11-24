/*
 * Copyright 2023-2025 Nope Forge
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

#ifndef NOPEGL_ANDROID_H
#define NOPEGL_ANDROID_H

#include <jni.h>
#include <nopegl/nopegl.h>

/**
 * Set a Java virtual machine that will be used to retrieve the JNI
 * environment.
 *
 * @param vm    pointer to the Java virtual machine
 *
 * @return 0 on success, NGL_ERROR_* (< 0) on error
 */
NGL_API int ngl_jni_set_java_vm(JavaVM *vm);

/**
 * Get the Java virtual machine pointer that has been set with
 * ngl_jni_set_java_vm().
 *
 * @return a pointer to the Java virtual machine or NULL if none has been set
 */
NGL_API JavaVM *ngl_jni_get_java_vm(void);

/**
 * Set the Android application context.
 *
 * @param application_context   JNI global reference of the Android application
 *                              context
 */
NGL_API int ngl_android_set_application_context(jobject *application_context);

/**
 * Get the Android application context that has been set with
 * ngl_android_set_application_context().
 *
 * @return a pointer to the JNI global reference of the Android application
 *         context or NULL if none has been set
 */
NGL_API jobject *ngl_android_get_application_context(void);

#endif
