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

    /**
     * SharedPreferences key holding the GPU API used for "run on GPU": "opencl"
     * (default) or "vulkan". Sent per request as `gpu_backend`; the OpenCL
     * memory-mode and tuning options do not apply to the Vulkan backend, which
     * has its own tuning cache and no kernel-enumeration pass.
     */
    const val PREF_GPU_BACKEND = "gpu_backend"

    /** True when the experimental Vulkan GPU backend was selected. */
    fun prefersVulkan(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(PREF_GPU_BACKEND, GPU_BACKEND_OPENCL) == GPU_BACKEND_VULKAN

    const val GPU_BACKEND_OPENCL = "opencl"
    const val GPU_BACKEND_VULKAN = "vulkan"

    /**
     * SharedPreferences key holding the OpenCL kernel-enumeration effort:
     * "fast" | "default" | "deep", mapped to MNN's tuning levels by the backend
     * (mnnTuningFromLevel). Only the OpenCL backend enumerates, so this has no
     * effect on the Vulkan or CPU paths.
     */
    const val PREF_TUNING_LEVEL = "gpu_tuning_level"

    const val TUNING_FAST = "fast"
    const val TUNING_DEFAULT = "default"
    const val TUNING_DEEP = "deep"

    /** The configured enumeration effort, defaulting to MNN's own default. */
    fun tuningLevel(context: Context): String =
        when (
            val level = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(PREF_TUNING_LEVEL, TUNING_DEFAULT)
        ) {
            TUNING_FAST, TUNING_DEEP -> level
            else -> TUNING_DEFAULT
        }

    /**
     * SharedPreferences key: keep the backend process (and this app's foreground
     * status) alive for a while after the last use instead of tearing both down
     * 1.5s after the model screen closes. A live foreground service keeps the
     * app out of the cached state, which is what lets Android -- and vendor
     * battery savers -- drop the backend "while there is still memory"; Android
     * 12+ additionally trims child processes of background apps, which is
     * exactly what this backend is.
     */
    const val PREF_KEEP_BACKEND_ALIVE = "keep_backend_alive"

    /** Whether to hold the backend alive between generations. */
    fun keepBackendAlive(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(PREF_KEEP_BACKEND_ALIVE, true)

    /**
     * SharedPreferences key: run MNN models on Huawei's NPU (MNN's hiai backend,
     * MNN_FORWARD_USER_0). Off by default, and only offered on a Kirin device.
     *
     * The backend loads Huawei's runtime libraries on demand -- nothing is loaded
     * unless a request sets this -- but whether those libraries will serve an app
     * Huawei has not registered is not something we can determine from here, so
     * the call is the user's.
     */
    const val PREF_HIAI_BACKEND = "npu_backend_hiai"

    const val NPU_BACKEND_HIAI = "hiai"

    /** Whether to run on Huawei's NPU. */
    fun prefersHiai(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(PREF_HIAI_BACKEND, false)

    /**
     * SharedPreferences key holding the GPU compute precision: "fp16" (default,
     * half arithmetic) or "fp32" (full precision). The CPU path always runs
     * fp32: Precision_Low there enables arm82 fp16, which corrupted the CLIP
     * conditioning (solid white/black frames) on at least one device.
     */
    const val PREF_PRECISION = "gpu_precision"

    const val PRECISION_FP16 = "fp16"
    const val PRECISION_FP32 = "fp32"

    /** The configured GPU precision, defaulting to the fp16 we always shipped. */
    fun precision(context: Context): String =
        when (
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(PREF_PRECISION, PRECISION_FP16)
        ) {
            PRECISION_FP32 -> PRECISION_FP32
            else -> PRECISION_FP16
        }

    private const val PREFS = "app_prefs"
    private const val IMAGE_NAME = "libstable_diffusion_core_image.so"
    private const val BUFFER_NAME = "libstable_diffusion_core_buffer.so"
    private const val LEGACY_NAME = "libstable_diffusion_core.so"

    /**
     * True when the image-memory library should be used.
     *
     * Defaults to false (BUFFER). IMAGE is what Qualcomm recommends for Adreno
     * and it is usually faster, but some Adreno/驱动 combinations segfault
     * inside MNN's kernel enumeration with it -- Adreno 618 (Snapdragon 730G)
     * among them. Buffer memory is the safe default; users who want to try
     * image memory can opt in from Settings.
     */
    fun prefersImage(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(PREF_MEMORY_IMAGE, false)

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
