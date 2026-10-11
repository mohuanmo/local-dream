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
#include <string>
#include <system_error>

#if defined(__ANDROID__) || defined(__linux__)
#include <malloc.h>
#endif

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

// Short tag mixed into the OpenCL tuning-cache file name so the two builds
// never read each other's tuned kernels.
inline const char *mnnGpuMemoryTag() {
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
  // MNN OpenCL tuning level. NORMAL still enumerates (LWS + GEMM tuning), but
  // WIDE additionally grows the GEMM candidate list, and the GEMM tuner
  // enqueues *every* candidate at once (OpenCLGemmTune.cpp's
  // measureGemmTuneCandidates) -- unlike the LWS tuner, which flushes every
  // kTuneBatch=32 precisely because an unbounded batch "would sit on driver
  // resources". So WIDE's peak scales with the candidate count, which is what
  // made ~512x512 runs die at the end of a pass. Upstream ships FAST (which
  // skips GEMM tuning entirely); NORMAL keeps the tuning without the blow-up.
  int gpu_tuning = MNN_GPU_TUNING_NORMAL;
};

// Hands the native heap's free pages back to the OS. The enumeration pass peaks
// far above steady state, so anything released by the previous phase (CLIP, the
// UNet session, the cached preview) is worth returning before it starts.
inline void mnnPurgeHeap() {
#ifdef M_PURGE
  mallopt(M_PURGE, 0);
#endif
}

// Creates a session with the standard pipeline configuration. The interpreter
// keeps no reference to the local configs after createSession returns.
inline MNN::Session *createMnnSession(MNN::Interpreter *interpreter,
                                      const MnnSessionOptions &opts) {
  MNN::ScheduleConfig config;
  MNN::BackendConfig backendConfig;
  bool tuning = false;
  if (opts.use_opencl) {
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
    backendConfig.precision = MNN::BackendConfig::Precision_Low;
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
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, true);
  if (tuning) mnnPurgeHeap();
  MNN::Session *session = interpreter->createSession(config);
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, false);
  return session;
}

#endif  // MNNUTILS_HPP
