package org.nopeforge.nopegl.kotlin

fun String.toCamelCase(pascalCase: Boolean = false): String {
    var uppercase = pascalCase
    val str = this
    return buildString {
        str.forEach { c ->
            when {
                c.isLetter() -> {
                    if (uppercase) {
                        append(c.titlecase())
                        uppercase = false
                    } else {
                        append(c)
                    }
                }

                c.isDigit() -> {
                    append(c)
                }

                else -> {
                    uppercase = true
                }
            }
        }
    }.let { if (it in KEYWORDS) "_$it" else it }
}