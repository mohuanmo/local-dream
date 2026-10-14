//
//  BackendAudit.hpp
//  Local Dream
//
//  Reports which operators did not get the backend they were asked for.
//
//  MNN schedules every op on its own and falls back to the CPU silently for ops
//  the requested backend cannot execute -- the backup backend in its scheduler.
//  So a run that says "GPU" can be mostly CPU work, and a "the GPU is barely
//  faster" observation may just be measuring the CPU. Interpreter::getBackend
//  is the documented way to ask, per tensor, which backend that tensor was
//  created on:
//
//    "获取指定 tensor 创建时使用的后端 Backend；可以在代码中使用该函数来判断
//     当前 Session 的推理实际使用什么后端。"   -- MNN/docs/cpp/Interpreter.md
//
//  Only op outputs are audited, not every tensor: weights are constants that
//  live on the CPU by design, and counting them would drown the answer.
//
#pragma once

#ifdef SDCORE_OP_AUDIT

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <MNN/Interpreter.hpp>
#include "MNN_generated.h"
#include "core/Backend.hpp"

namespace backend_audit {

inline const char *forwardName(MNNForwardType type) {
  switch (type) {
    case MNN_FORWARD_CPU:
      return "cpu";
    case MNN_FORWARD_OPENCL:
      return "opencl";
    case MNN_FORWARD_VULKAN:
      return "vulkan";
    case MNN_FORWARD_USER_0:
      return "npu";
    default:
      return "other";
  }
}

// Caps the listed names so one run cannot flood the log; the per-backend counts
// above the list stay complete.
constexpr size_t kMaxListed = 24;

inline void report(MNN::Interpreter *interpreter, const MNN::Session *session,
                   const std::string &modelPath, const char *stage,
                   MNNForwardType requested) {
  if (modelPath.empty() || session == nullptr) return;

  std::ifstream file(modelPath, std::ios::binary);
  if (!file) {
    std::printf("[OPS] %s: audit skipped, cannot read %s\n", stage, modelPath.c_str());
    return;
  }
  std::string buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (buffer.size() < 16) {
    std::printf("[OPS] %s: audit skipped, empty model\n", stage);
    return;
  }
  const auto *net = MNN::GetNet(buffer.data());
  if (net == nullptr || net->oplists() == nullptr) {
    std::printf("[OPS] %s: audit skipped, no net\n", stage);
    return;
  }

  std::map<MNNForwardType, int> counts;
  std::vector<std::string> strays;
  int total = 0;
  const auto *names = net->tensorName();

  for (const auto *op : *net->oplists()) {
    if (op == nullptr || op->outputIndexes() == nullptr || op->outputIndexes()->size() == 0) {
      continue;
    }
    const int index = op->outputIndexes()->Get(0);
    auto *tensor = session->getTensor(index);
    if (tensor == nullptr) continue;

    const auto *backend = interpreter->getBackend(session, tensor);
    const auto type = backend != nullptr ? backend->type() : MNN_FORWARD_CPU;
    counts[type]++;
    total++;

    if (type != requested && strays.size() < kMaxListed) {
      const flatbuffers::String *name = op->name();
      if (name == nullptr && names != nullptr && index < static_cast<int>(names->size())) {
        name = names->Get(index);
      }
      strays.push_back(std::string(name != nullptr ? name->str() : "?") + " [" +
                       forwardName(type) + "]");
    }
  }

  std::printf("[OPS] %s: %d ops, requested %s\n", stage, total, forwardName(requested));
  for (const auto &entry : counts) {
    std::printf("[OPS] %s:   %-8s %d\n", stage, forwardName(entry.first), entry.second);
  }
  if (!strays.empty()) {
    std::printf("[OPS] %s:   not on %s:\n", stage, forwardName(requested));
    for (const auto &entry : strays) {
      std::printf("[OPS] %s:     %s\n", stage, entry.c_str());
    }
  }
  std::fflush(stdout);
}

} // namespace backend_audit

#else

namespace backend_audit {

// Compiled out when MNN's schema headers were not available at configure time.
// Taking anything keeps the call sites unchanged.
template <typename... Args> inline void report(Args &&...) {}

} // namespace backend_audit

#endif
