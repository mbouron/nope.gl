package org.nopeforge.nopegl.providers

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import org.nopeforge.nopegl.utils.AssetUtils
import timber.log.Timber
import java.io.File

class AssetContentProvider : ContentProvider() {
    override fun onCreate(): Boolean {
        return true
    }

    override fun query(
        uri: Uri,
        projection: Array<out String>?,
        selection: String?,
        selectionArgs: Array<out String>?,
        sortOrder: String?
    ): Cursor? {
        return null
    }

    override fun getType(uri: Uri): String? {
        val path = uri.path ?: return null
        return when {
            path.endsWith(".mp4") -> "video/mp4"
            path.endsWith(".jpg") -> "image/jpeg"
            path.endsWith(".jpeg") -> "image/jpeg"
            path.endsWith(".png") -> "image/png"
            else -> "application/octet-stream"
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? {
        return null
    }

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?): Int {
        return 0
    }

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<out String>?
    ): Int {
        return 0
    }

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val name = uri.path?.removePrefix("/") ?: return null
        val context = context ?: return null
        val path = AssetUtils.resolveAssetPath(context, name)

        return try {
            return ParcelFileDescriptor.open(File(path), ParcelFileDescriptor.MODE_READ_ONLY).also {
                Timber.d("Serving file from cache $path (${it.statSize} bytes)")
            }
        } catch (e: Exception) {
            Timber.e(e, "Failed to open file from cache: $path")
            null
        }
    }

    companion object {
        const val AUTHORITY = "org.nopeforge.nopegl.viewer.asset"

        fun getUri(filename: String): String {
            return "content://${AUTHORITY}/$filename"
        }
    }
}
