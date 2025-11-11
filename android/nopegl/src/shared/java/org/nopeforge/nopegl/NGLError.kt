package org.nopeforge.nopegl

class NGLError(message: String) : RuntimeException(message) {
    constructor(code: Int) : this(nativeGetMessage(code))

    companion object {
        @JvmStatic
        private external fun nativeGetMessage(code: Int): String
    }
}
