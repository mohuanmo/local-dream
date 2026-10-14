//
//  CrashHandler.hpp
//  Local Dream
//
//  Prints a backtrace when the process dies on a signal, so the log says which
//  library the fault was in.
//
//  Huawei's NPU runtime is the case that motivated this: MNN's loader happily
//  reports "HiAI dynamic ABI loaded: scope=full functions=78" and then the next
//  call faults inside a library built against a different DDK revision than the
//  one on the device, and the log simply stops. Module names and frame offsets
//  are enough to tell "faulted inside libhiai*" from "faulted in our own code",
//  which is the question that decides whether the path is worth pursuing.
//
//  dladdr() reads the dynamic symbol table, so it still works on the stripped
//  binaries we ship.
//
#pragma once

#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>

namespace crash_handler {

// Alternates off the alt stack, so a fault caused by a blown stack can still be
// reported.
inline char *altStack() {
  static char buffer[SIGSTKSZ * 4];
  return buffer;
}

inline void writeFrame(int index, void *address) {
  Dl_info info{};
  char line[512];
  if (::dladdr(address, &info) != 0 && info.dli_fname != nullptr) {
    if (info.dli_sname != nullptr) {
      ::snprintf(line, sizeof(line), "  #%d  %p  %s (%s)\n", index, address,
                 info.dli_fname, info.dli_sname);
    } else {
      // Stripped: the module and the offset from its base are still enough to
      // tell which library faulted.
      const auto offset =
          reinterpret_cast<unsigned long>(address) -
          reinterpret_cast<unsigned long>(info.dli_fbase);
      ::snprintf(line, sizeof(line), "  #%d  %p  %s (+%#lx)\n", index, address,
                 info.dli_fname, offset);
    }
  } else {
    ::snprintf(line, sizeof(line), "  #%d  %p  <unknown>\n", index, address);
  }
  ::write(STDERR_FILENO, line, ::strlen(line));
}

inline void handler(int signal, siginfo_t *info, void *) {
  char header[192];
  ::snprintf(header, sizeof(header), "\n[CRASH] signal %d (%s) at %p\n", signal,
             ::strsignal(signal), info != nullptr ? info->si_addr : nullptr);
  ::write(STDERR_FILENO, header, ::strlen(header));

  void *frames[64];
  const int count = ::backtrace(frames, 64);
  for (int i = 0; i < count; ++i) {
    writeFrame(i, frames[i]);
  }
  static const char footer[] = "[CRASH] end of backtrace\n";
  ::write(STDERR_FILENO, footer, sizeof(footer) - 1);

  // Die the way the app expects to see it: same signal, default disposition.
  ::signal(signal, SIG_DFL);
  ::raise(signal);
}

inline void install() {
  stack_t stack{};
  stack.ss_sp = altStack();
  stack.ss_size = SIGSTKSZ * 4;
  ::sigaltstack(&stack, nullptr);

  struct sigaction action {};
  action.sa_sigaction = handler;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  ::sigemptyset(&action.sa_mask);
  for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE}) {
    ::sigaction(signal, &action, nullptr);
  }
}

} // namespace crash_handler
