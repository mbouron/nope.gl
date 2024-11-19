package org.nopeforge.nopegl

import android.util.Log

enum class LogLevel(val priority: Int) {
    Verbose(Log.VERBOSE),
    Debug(Log.DEBUG),
    Info(Log.INFO),
    Warning(Log.WARN),
    Error(Log.ERROR);

    companion object {
        internal fun of(value: Int): LogLevel? {
            return values().firstOrNull { it.priority == value }
        }
    }
}

internal class NGLLogger(
    level: LogLevel,
    private val onLog: (level: LogLevel, tag: String, message: String) -> Unit,
) {
    private val nativePtr: Long

    init {
        nativePtr = nativeInit(level.priority)
        if (nativePtr == 0L) throw OutOfMemoryError()
    }

    fun release() {
        nativeRelease(nativePtr)
    }

    fun log(level: Int, message: String) {
        onLog(LogLevel.of(level) ?: return, "ngl", message)
    }

    private external fun nativeInit(level: Int): Long
    private external fun nativeRelease(nativePtr: Long)

}
