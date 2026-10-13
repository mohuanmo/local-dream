#ifndef MNNUTILS_HPP
#define MNNUTILS_HPP

#include <MNN/MNNDefine.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <MNN/Interpreter.hpp>
#include <filesystem>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#endif

// Declared in MNN's private core/Backend.hpp and exported (MNN_PUBLIC). Only the
// declaration is repeated here so this header does not have to pull in MNN's
// private include tree; it is how we ask whether a backend was really linked in.
namespace MNN {
class RuntimeCreator;
MNN_PUBLIC const RuntimeCreator *MNNGetExtraRuntimeCreator(MNNForwardType type);
} // namespace MNN

// Returns "{model_dir}/cache", creating it if needed. Returns "" when
// model_dir is empty or directory creation fails; callers must treat that
// as "caching disabled for this run".
inline std::string ensureCacheDir(const std::string &model_dir) {
  if (model_dir.empty()) return "";
  std::filesystem::path p = std::filesystem::path(model_dir) / "cache";
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  if (ec) return "";
  return p.string();
}

// Load an MNN model via mmap + createFromBuffer instead of createFromFile.
// createFromFile reads the whole .mnn in 4 KB chunks and then merges them into
// one contiguous buffer, transiently holding ~2x the model size in anonymous
// (non-reclaimable) memory. Mapping the file read-only keeps that source as
// clean, file-backed pages the kernel can reclaim under pressure, so the peak
// anonymous footprint during load drops to the single owned buffer MNN copies
// into. createFromBuffer copies the bytes, so the mapping can be released right
// away. Falls back to createFromFile on any mmap-path failure.
inline MNN::Interpreter *createMnnInterpreterMmap(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return MNN::Interpreter::createFromFile(path);
  }
  struct stat st{};
  if (0 != fstat(fd, &st) || st.st_size <= 0) {
    close(fd);
    return MNN::Interpreter::createFromFile(path);
  }
  size_t size = static_cast<size_t>(st.st_size);
  void *mapped = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  // The mapping holds its own file reference, so the fd can be closed now.
  close(fd);
  if (MAP_FAILED == mapped) {
    return MNN::Interpreter::createFromFile(path);
  }
  // MNN copies the whole buffer once, sequentially; hint readahead to match.
  madvise(mapped, size, MADV_SEQUENTIAL);
  MNN::Interpreter *interpreter =
      MNN::Interpreter::createFromBuffer(mapped, size);
  munmap(mapped, size);
  if (interpreter) {
    // createFromFile sets a default external weight path; createFromBuffer does
    // not. Mirror it so models that store weights in a companion ".weight" file
    // still resolve them at session creation. Harmless when no such file
    // exists.
    interpreter->setExternalFile((std::string(path) + ".weight").c_str());
  }
  return interpreter;
}

// GPU memory mode is a BUILD-time choice, so the app can ship two otherwise
// identical libraries and let the user pick at runtime:
//   SDCORE_GPU_MEMORY_IMAGE=1 -> MNN_GPU_MEMORY_IMAGE  (Texture + L1 cache;
//                                Qualcomm's recommendation for Adreno)
//   SDCORE_GPU_MEMORY_IMAGE=0 -> MNN_GPU_MEMORY_BUFFER (the tamer fallback
//                                for drivers/models that dislike IMAGE)
#ifndef SDCORE_GPU_MEMORY_IMAGE
#define SDCORE_GPU_MEMORY_IMAGE 1
#endif

// Short tag mixed into the GPU tuning-cache file name so no two builds read
// each other's tuned kernels: Vulkan stores a different cache format (MNN's
// VKCache) than OpenCL, and the two OpenCL libraries differ in memory mode.
inline const char *mnnGpuCacheTag(bool use_vulkan) {
  if (use_vulkan) return "vk";
#if SDCORE_GPU_MEMORY_IMAGE
  return "img";
#else
  return "buf";
#endif
}

// Notified around an OpenCL tuning ("enumeration") pass. MNN exposes no
// per-kernel tuning progress, so a pass can only be bracketed: `started` is
// true right before a session whose on-disk tuning cache is missing is
// created, and false once createSession returns. A null callback is a no-op.
using MnnTuningNotify =
    std::function<void(const std::string &stage, int width, bool started)>;

// One process-wide hook (generations are serialized, so a single slot is
// enough); set by the /generate handler while a client is streaming.
inline MnnTuningNotify &mnnTuningNotify() {
  static MnnTuningNotify notify;
  return notify;
}

// Maps the Settings choice to MNN's OpenCL tuning level:
//   fast   -> MNN_GPU_TUNING_FAST   try the per-kernel heuristic first and skip
//                                   the enumeration when it matches; the GEMM
//                                   tuning is skipped outright. Fastest first
//                                   run, and what upstream shipped.
//   deep   -> MNN_GPU_TUNING_HEAVY  search the hardest (slowest first run).
//   default-> MNN_GPU_TUNING_WIDE   MNN's own default.
// Only the OpenCL backend enumerates, so this has no effect on Vulkan or CPU.
inline int mnnTuningFromLevel(const std::string &level) {
  if (level == "fast") return MNN_GPU_TUNING_FAST;
  if (level == "deep") return MNN_GPU_TUNING_HEAVY;
  return MNN_GPU_TUNING_WIDE;
}

// Compute precision for the GPU backends, from the Settings choice:
//   fp16 -> MNN::BackendConfig::Precision_Low     half arithmetic (default,
//            what we have always shipped; faster and lighter on memory)
//   fp32 -> MNN::BackendConfig::Precision_Normal  full precision (MNN's own
//            default, slower)
// MNN also has Precision_High and Precision_Low_BF16; they are not exposed here.
// The CPU branch deliberately ignores this: Precision_Low there enables arm82
// fp16, which turned the CLIP conditioning (and therefore the whole image) into
// solid white/black on at least one device.
inline int mnnPrecisionFromLevel(const std::string &level) {
  if (level == "fp32") return MNN::BackendConfig::Precision_Normal;
  return MNN::BackendConfig::Precision_Low;
}

// Backend label for the memory log below.
inline const char *mnnBackendName(bool use_opencl, bool use_vulkan) {
  if (!use_opencl) return "cpu";
  return use_vulkan ? "vulkan" : "opencl";
}

// System-wide free memory from /proc/meminfo (MemAvailable, in kB); 0 when it
// cannot be read. Shared by the memory log and the Vulkan pre-flight check.
inline long long mnnMemAvailableKb() {
  long long availKb = 0;
  if (FILE *f = std::fopen("/proc/meminfo", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "MemAvailable:", 13) == 0) {
        availKb = std::atoll(line + 13);
        break;
      }
    }
    std::fclose(f);
  }
  return availKb;
}

// One line of process footprint per checkpoint (current RSS and the high-water
// mark) so a log shows how far memory had climbed before a crash. Worth having
// per backend: MNN's Vulkan path copies every weight into device memory, while
// OpenCL can zero-copy them out of the mmapped file, so the two peaks differ a
// lot -- measuring beats guessing. Reads /proc, which needs no permission.
inline void mnnLogMemory(const char *stage, const char *backend) {
  long rssKb = 0;
  long hwmKb = 0;
  if (FILE *f = std::fopen("/proc/self/status", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "VmRSS:", 6) == 0)
        rssKb = std::atol(line + 6);
      else if (std::strncmp(line, "VmHWM:", 6) == 0)
        hwmKb = std::atol(line + 6);
    }
    std::fclose(f);
  }
  const long long availKb = mnnMemAvailableKb();
  std::printf("[MEM] %-22s %-6s rss=%4ldMB peak=%4ldMB avail=%4lldMB\n", stage,
              backend, rssKb / 1024, hwmKb / 1024, availKb / 1024);
}

// Pre-flight free-memory check for a Vulkan run. MNN's Vulkan backend copies
// every weight into device memory and offers no way around it: no zero-copy
// import (no VK_EXT_external_memory_host usage anywhere in it, unlike OpenCL's
// MmapPool), no int8 ops, and it ignores BackendConfig::Memory_Low entirely.
// So a Vulkan run costs roughly the model size *again*. The failure mode
// otherwise is an OOM SIGKILL with no log, so refuse up front with something
// readable instead. The estimate is deliberately crude; it only has to be
// conservative enough to catch the case that actually hurts.
inline void mnnRequireMemoryForVulkan(const std::string &model_path) {
  std::error_code ec;
  auto modelBytes = std::filesystem::file_size(model_path, ec);
  if (ec) return; // Unknown size: don't block the run.

  long long availKb = 0;
  if (FILE *f = std::fopen("/proc/meminfo", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "MemAvailable:", 13) == 0) {
        availKb = std::atoll(line + 13);
        break;
      }
    }
    std::fclose(f);
  }
  const long long modelKb = (long long)(modelBytes / 1024);
  // The extra device copy plus room for activations and the runtime.
  const long long needKb = (long long)(modelKb * 1.2) + 300 * 1024;
  std::printf("[MEM] vulkan-preflight model=%lldMB need=%lldMB available=%lldMB\n",
              modelKb / 1024, needKb / 1024, availKb / 1024);
  if (availKb <= 0 || availKb >= needKb) return;

  char message[512];
  std::snprintf(message, sizeof(message),
                "Vulkan needs about %lldMB of free memory for this model (its "
                "weights are copied into GPU memory, not shared) but only "
                "%lldMB is available. Switch the GPU backend to OpenCL, or use "
                "a smaller model.",
                needKb / 1024, availKb / 1024);
  throw std::runtime_error(message);
}

// Whether MNN's Vulkan backend is really available. Its registrar uses
// needCheck=true, so the lookup constructs (and immediately destroys) a Vulkan
// runtime -- far too expensive to repeat per session, hence the cached result.
// This is also the only honest way to ask: MNN itself falls back to the CPU
// without complaint when a requested forward type is missing.
inline bool mnnVulkanAvailable() {
  static const bool available =
      MNN::MNNGetExtraRuntimeCreator(MNN_FORWARD_VULKAN) != nullptr;
  return available;
}

// Session creation options shared by every MNN model in the pipeline.
// CPU: 4 threads, low-memory, high-power, default precision.
// OpenCL: wide tuning + low precision, plus an on-disk tuning cache when
// cache_file is set.
//
// GPU tuning knobs (relevant on Adreno, e.g. the 730G's Adreno 618):
//   - MNN_GPU_TUNING_WIDE selects better kernels than MNN_GPU_TUNING_FAST, at
//     the cost of a slower FIRST run per shape; the result is persisted via
//     cache_file, so later runs pay it once.
//   - The memory mode (IMAGE vs BUFFER) is fixed at build time by
//     SDCORE_GPU_MEMORY_IMAGE, so the two shipped libraries differ only there.
//     BUFFER is the default; IMAGE is an opt-in experiment.
struct MnnSessionOptions {
  bool use_opencl = false;
  std::string cache_file;  // OpenCL tuning cache path ("" = no cache file)
  int num_threads = 4;
  std::string stage;       // label for tuning notifications ("" = none)
  int width = 0;           // resolution the session is created for
  // MNN OpenCL tuning level, kept configurable so it can be A/B'd per stage.
  //
  // WIDE (MNN's default, and what we ship) measures wideXgemmPool(): 14 curated
  // GEMM combinations. NORMAL instead generates the cartesian product of the
  // tile parameters and prunes it to the 16 closest to the heuristic
  // preference. The two measure a comparable number of candidates, so there is
  // no memory argument for one over the other -- WIDE is simply the curated
  // pool. FAST, which upstream shipped, skips GEMM tuning entirely (LWS only).
  int gpu_tuning = MNN_GPU_TUNING_WIDE;
  // Run the GPU work on MNN's Vulkan backend instead of OpenCL. Vulkan has its
  // own tuning cache and no kernel-enumeration pass, so gpu_tuning and the
  // memory-mode bits below do not apply to it.
  bool use_vulkan = false;
  // GPU compute precision: MNN::BackendConfig::Precision_Low (fp16, default) or
  // Precision_Normal (fp32), from the Settings choice.
  int gpu_precision = MNN::BackendConfig::Precision_Low;
};

// bionic declares mallopt() only for API 26+, while M_PURGE is defined at every
// level -- and the CMake build targets android21 even though the app's minSdk is
// 28. Declare it weak so the call still compiles and is simply skipped if the
// symbol is missing at runtime.
#if defined(__ANDROID__) && (!defined(__ANDROID_API__) || __ANDROID_API__ < 26)
extern "C" int mallopt(int param, int value) __attribute__((weak));
#define MNN_MALLOPT_IS_WEAK 1
#endif

// Hands the native heap's free pages back to the OS. The enumeration pass peaks
// far above steady state, so anything released by the previous phase (CLIP, the
// UNet session, the cached preview) is worth returning before it starts.
inline void mnnPurgeHeap() {
#ifdef M_PURGE
#ifdef MNN_MALLOPT_IS_WEAK
  if (mallopt) mallopt(M_PURGE, 0);
#else
  mallopt(M_PURGE, 0);
#endif
#endif
}

// Creates a session with the standard pipeline configuration. The interpreter
// keeps no reference to the local configs after createSession returns.
inline MNN::Session *createMnnSession(MNN::Interpreter *interpreter,
                                      const MnnSessionOptions &opts) {
  MNN::ScheduleConfig config;
  MNN::BackendConfig backendConfig;
  bool tuning = false;
  if (opts.use_opencl && opts.use_vulkan) {
    // Refuse to continue when the Vulkan backend is not actually there. MNN
    // falls back to the CPU silently when a requested type is missing, which is
    // how a Vulkan run once ended up executing the whole UNet on the CPU with
    // fp16 enabled and got the process OOM-killed -- indistinguishable from a
    // GPU bug. (Only Vulkan is checked: the CPU fallback for a missing OpenCL is
    // upstream's long-standing behaviour and still produces an image.)
    if (!mnnVulkanAvailable()) {
      throw std::runtime_error(
          "Vulkan backend is unavailable in this build (see the GPU backend "
          "setting); no session was created");
    }
    // Vulkan: it keeps its own tuning cache (MNN's VKCache, verified by MNN on
    // load), but unlike OpenCL it has no per-kernel enumeration pass -- so
    // `tuning` stays false and the UI shows no enumeration indicator, which is
    // accurate. The OpenCL memory-mode/tuning bits do not apply either.
    if (!opts.cache_file.empty())
      interpreter->setCacheFile(opts.cache_file.c_str());
    config.type = MNN_FORWARD_VULKAN;
    backendConfig.precision =
        static_cast<MNN::BackendConfig::PrecisionMode>(opts.gpu_precision);
  } else if (opts.use_opencl) {
    if (!opts.cache_file.empty()) {
      interpreter->setCacheFile(opts.cache_file.c_str());
      // A missing or empty cache means this session will run a tuning pass,
      // which is exactly the slow "first run at each resolution" case.
      std::error_code ec;
      tuning = !std::filesystem::exists(opts.cache_file, ec) ||
               std::filesystem::file_size(opts.cache_file, ec) == 0;
    }
    config.type = MNN_FORWARD_OPENCL;
    // Tuning level comes from the caller; see MnnSessionOptions::gpu_tuning.
#if SDCORE_GPU_MEMORY_IMAGE
    config.mode = MNN_GPU_MEMORY_IMAGE | opts.gpu_tuning;
#else
    config.mode = MNN_GPU_MEMORY_BUFFER | opts.gpu_tuning;
#endif
    backendConfig.precision =
        static_cast<MNN::BackendConfig::PrecisionMode>(opts.gpu_precision);
  } else {
    config.type = MNN_FORWARD_CPU;
    config.numThread = opts.num_threads;
    backendConfig.memory = MNN::BackendConfig::Memory_Low;
    // No Precision_Low here. It would enable arm82 fp16 on ARMv8.2+ cores,
    // but the CPU path also runs the CLIP text encoder even when the UNet is
    // on OpenCL -- and fp16 there turned the conditioning (and therefore the
    // whole image) into solid white/black on at least one device. Upstream
    // leaves CPU precision at the default; keep it that way.
  }
  backendConfig.power = MNN::BackendConfig::Power_High;
  config.backendConfig = &backendConfig;
  const char *backend = mnnBackendName(opts.use_opencl, opts.use_vulkan);
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, true);
  if (tuning) mnnPurgeHeap();
  mnnLogMemory("session-create:begin", backend);
  MNN::Session *session = interpreter->createSession(config);
  mnnLogMemory("session-create:end", backend);
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, false);
  return session;
}

#endif  // MNNUTILS_HPP
