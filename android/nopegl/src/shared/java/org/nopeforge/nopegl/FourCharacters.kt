package org.nopeforge.nopegl

internal fun fourCharacters(a: Char, b: Char, c: Char, d: Char): Int {
    return (a.code shl 24) or (b.code shl 16) or (c.code shl 8) or d.code
}
