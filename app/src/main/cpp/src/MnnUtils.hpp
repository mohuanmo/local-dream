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
// CPU: 4 threads, low-memory, high-power, fp16 (arm82) where available.
// OpenCL: wide tuning + image memory with low precision (fp16), plus an
// on-disk tuning cache when cache_file is set.
//
// GPU tuning knobs (relevant on Adreno, e.g. the 730G's Adreno 618):
//   - MNN_GPU_TUNING_WIDE is MNN's default and selects better kernels than
//     MNN_GPU_TUNING_FAST, at the cost of a slower FIRST run per shape; the
//     result is persisted via cache_file, so later runs pay it once.
//   - MNN_GPU_MEMORY_IMAGE routes tensors through the texture unit + L1
//     cache, which Qualcomm recommends over MNN_GPU_MEMORY_BUFFER. If a
//     specific driver/model misbehaves (black or NaN output) with IMAGE,
//     switch this single expression back to MNN_GPU_MEMORY_BUFFER.
struct MnnSessionOptions {
  bool use_opencl = false;
  std::string cache_file;  // OpenCL tuning cache path ("" = no cache file)
  int num_threads = 4;
  std::string stage;       // label for tuning notifications ("" = none)
  int width = 0;           // resolution the session is created for
};

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
    config.mode = MNN_GPU_MEMORY_IMAGE | MNN_GPU_TUNING_WIDE;
    backendConfig.precision = MNN::BackendConfig::Precision_Low;
  } else {
    config.type = MNN_FORWARD_CPU;
    config.numThread = opts.num_threads;
    backendConfig.memory = MNN::BackendConfig::Memory_Low;
    // Precision_Low enables arm82 fp16 on ARMv8.2+ cores (the 730G's A76),
    // cutting memory traffic; cores without fp16 just stay fp32.
    backendConfig.precision = MNN::BackendConfig::Precision_Low;
  }
  backendConfig.power = MNN::BackendConfig::Power_High;
  config.backendConfig = &backendConfig;
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, true);
  MNN::Session *session = interpreter->createSession(config);
  if (tuning && mnnTuningNotify())
    mnnTuningNotify()(opts.stage, opts.width, false);
  return session;
}

#endif  // MNNUTILS_HPP
