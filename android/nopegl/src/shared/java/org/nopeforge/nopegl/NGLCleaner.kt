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
