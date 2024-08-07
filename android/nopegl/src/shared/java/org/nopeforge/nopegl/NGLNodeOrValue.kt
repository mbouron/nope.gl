package org.nopeforge.nopegl

sealed interface NGLNodeOrValue<T : Any> {
    data class NGLNode<T : Any>(val value: org.nopeforge.nopegl.NGLNode) : NGLNodeOrValue<T>
    data class Value<T : Any>(val value: T) : NGLNodeOrValue<T>

    companion object {
        fun <T : Any> node(value: org.nopeforge.nopegl.NGLNode): NGLNodeOrValue<T> =
            NGLNode(value)

        fun <T : Any> value(value: T): NGLNodeOrValue<T> = Value(value)
    }
}
