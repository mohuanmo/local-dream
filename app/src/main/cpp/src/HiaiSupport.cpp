//
//  HiaiSupport.cpp
//  Local Dream
//
//  On-demand loading of Huawei's NPU runtime.
//
//  Why this file exists. MNN's hiai backend reaches every HiAI entry point
//  through its own dynamic-loader table, with exactly two exceptions:
//  hiai::AiContext's constructor and destructor, which a plain function-pointer
//  table cannot cover. Referring to them normally made libhiai* hard DT_NEEDEDs
//  of the core library, so every device loaded Huawei's runtime at process
//  start -- and its initialisers abort the process rather than failing
//  gracefully, which showed up as "libc++abi: terminating" / SIGABRT before a
//  single line of model output, on any backend.
//
//  Defining those two symbols here, as ordinary functions that jump to the real
//  ones, keeps the dependency a *runtime* one: nothing is loaded until a request
//  actually asks for the Kirin backend, and a refusal from Huawei's side can
//  only take down that one run.
//
//  Only compiled when the DDK is staged and MNN's hiai backend is enabled.
//
#include <cstdio>
#include <dlfcn.h>

namespace {

// Itanium C++ ABI: constructors and destructors are ordinary functions taking
// the object pointer, so forwarding is a tail call through a resolved address.
using Ctor = void (*)(void *);
using Dtor = void (*)(void *);
// D0 is the "deleting" destructor: it also frees the storage.
using DeletingDtor = void (*)(void *, int);

void *gHandle = nullptr;

void *resolve(const char *name) {
  if (gHandle == nullptr) {
    // libhiai.so lives in the backend's runtime directory, which is on
    // LD_LIBRARY_PATH. RTLD_GLOBAL so MNN's own lookups see it as well.
    gHandle = ::dlopen("libhiai.so", RTLD_NOW | RTLD_GLOBAL);
    if (gHandle == nullptr) {
      const char *error = ::dlerror();
      std::fprintf(stderr, "[HIAI] dlopen(libhiai.so) failed: %s\n",
                   error == nullptr ? "unknown" : error);
      std::fflush(stderr);
      return nullptr;
    }
  }
  // dlsym on the handle searches only that library and its dependencies, so this
  // can never resolve back to one of our own stubs.
  return ::dlsym(gHandle, name);
}

// A missing symbol used to mean the constructor never ran, and the unconstructed
// hiai::AiContext then faulted inside MNN with nothing in the log to explain it
// (observed as "signal 11 at 0x1c" with every frame in our own library). Say what
// is missing instead: that is the difference between a diagnosis and a mystery.
[[noreturn]] void failMissing(const char *name) {
  std::fprintf(stderr,
               "[HIAI] libhiai.so does not export %s: cannot construct "
               "hiai::AiContext on this device\n",
               name);
  std::fflush(stderr);
  ::abort();
}

} // namespace

// The mangled names are spelled out so the linker never needs the real library:
// C1/C2 are the complete-object / base-object constructors, D0/D1/D2 the
// corresponding destructors. Defining more variants than the linker asks for is
// harmless; each stub resolves its own name.
extern "C" {
void hiai_ai_context_ctor_c1(void *) asm("_ZN4hiai9AiContextC1Ev");
void hiai_ai_context_ctor_c2(void *) asm("_ZN4hiai9AiContextC2Ev");
void hiai_ai_context_dtor_d0(void *, int) asm("_ZN4hiai9AiContextD0Ev");
void hiai_ai_context_dtor_d1(void *) asm("_ZN4hiai9AiContextD1Ev");
void hiai_ai_context_dtor_d2(void *) asm("_ZN4hiai9AiContextD2Ev");
}

void hiai_ai_context_ctor_c1(void *self) {
  auto fn = reinterpret_cast<Ctor>(resolve("_ZN4hiai9AiContextC1Ev"));
  if (fn == nullptr) failMissing("_ZN4hiai9AiContextC1Ev");
  fn(self);
}

void hiai_ai_context_ctor_c2(void *self) {
  auto fn = reinterpret_cast<Ctor>(resolve("_ZN4hiai9AiContextC2Ev"));
  if (fn == nullptr) failMissing("_ZN4hiai9AiContextC2Ev");
  fn(self);
}

void hiai_ai_context_dtor_d0(void *self, int flag) {
  auto fn = reinterpret_cast<DeletingDtor>(resolve("_ZN4hiai9AiContextD0Ev"));
  if (fn == nullptr) failMissing("_ZN4hiai9AiContextD0Ev");
  fn(self, flag);
}

void hiai_ai_context_dtor_d1(void *self) {
  auto fn = reinterpret_cast<Dtor>(resolve("_ZN4hiai9AiContextD1Ev"));
  if (fn == nullptr) failMissing("_ZN4hiai9AiContextD1Ev");
  fn(self);
}

void hiai_ai_context_dtor_d2(void *self) {
  auto fn = reinterpret_cast<Dtor>(resolve("_ZN4hiai9AiContextD2Ev"));
  if (fn == nullptr) failMissing("_ZN4hiai9AiContextD2Ev");
  fn(self);
}
