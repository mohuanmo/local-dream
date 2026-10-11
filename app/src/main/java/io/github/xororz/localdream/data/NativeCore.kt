package io.github.xororz.localdream.data

import android.content.Context
import java.io.File

/**
 * Locates the native core executable the backend should run.
 *
 * The build produces two otherwise identical libraries that differ only in the
 * MNN OpenCL memory mode, so the user can switch at runtime without rebuilding:
 *
 *   libstable_diffusion_core_image.so   OpenCL image (texture) memory
 *   libstable_diffusion_core_buffer.so  OpenCL buffer memory
 *
 * The choice is a single global preference (Settings), not per model.
 */
object NativeCore {
    /** SharedPreferences key: true = image memory, false = buffer memory. */
    const val PREF_MEMORY_IMAGE = "mnn_gpu_memory_image"

    private const val PREFS = "app_prefs"
    private const val IMAGE_NAME = "libstable_diffusion_core_image.so"
    private const val BUFFER_NAME = "libstable_diffusion_core_buffer.so"
    private const val LEGACY_NAME = "libstable_diffusion_core.so"

    /** True when the image-memory library should be used. Defaults to true. */
    fun prefersImage(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(PREF_MEMORY_IMAGE, true)

    /**
     * The executable for the current setting. Falls back to the other variant,
     * then to the pre-split single-library name, so an APK assembled from the
     * prebuilt native bundle (which has neither variant) still starts.
     */
    fun executable(context: Context): File {
        val dir = File(context.applicationInfo.nativeLibraryDir)
        val image = File(dir, IMAGE_NAME)
        val buffer = File(dir, BUFFER_NAME)
        val wantImage = prefersImage(context)
        val preferred = if (wantImage) image else buffer
        if (preferred.exists()) return preferred
        val other = if (wantImage) buffer else image
        if (other.exists()) return other
        return File(dir, LEGACY_NAME)
    }
}
