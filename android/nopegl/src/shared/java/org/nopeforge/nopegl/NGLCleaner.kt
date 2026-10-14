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

package org.nopeforge.nopegl

import java.lang.ref.PhantomReference
import java.lang.ref.ReferenceQueue
import java.util.Objects
import java.util.concurrent.ConcurrentHashMap

class NGLCleaner {
    interface Cleanable {
        fun clean()
    }

    internal class CleanerReference(referent: Any, private val cleaningAction: Runnable) :
        PhantomReference<Any>(referent, queue),
        Cleanable {
            override fun clean() {
                if (references.remove(this)) {
                    super.clear()
                    cleaningAction.run()
                }
            }
        }

    internal class NGLCleanerThread : Thread() {
        init {
            name = "NGLCleanerThread"
            isDaemon = true
            priority = MIN_PRIORITY
        }

        override fun run() {
            while (true) {
                val ref = queue.remove() as CleanerReference
                ref.clean()
            }
        }
    }

    companion object {
        private val references : MutableSet<CleanerReference> = ConcurrentHashMap.newKeySet()
        private val queue = ReferenceQueue<Any>()
        private val thread = NGLCleanerThread()

        init {
            thread.start()
        }

        fun register(obj: Any, cleaningAction: Runnable): Cleanable {
            val cleanable = CleanerReference(
                Objects.requireNonNull(obj),
                Objects.requireNonNull(cleaningAction)
            )
            references.add(cleanable)
            return cleanable
        }
    }

}
