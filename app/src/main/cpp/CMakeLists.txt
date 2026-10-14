cmake_minimum_required(VERSION 3.18)
project(libstable_diffusion_core.so C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build and package against the same QNN SDK revision.
set(QNN_SDK_ROOT "$ENV{QNN_SDK_ROOT}" CACHE PATH "Qualcomm AI Runtime SDK 2.50.0.260828")
set(SAMPLE_APP_DIR "${CMAKE_CURRENT_BINARY_DIR}/3rdparty/SampleApp")
file(COPY "${QNN_SDK_ROOT}/examples/QNN/SampleApp/SampleApp/"
    DESTINATION "${SAMPLE_APP_DIR}")
execute_process(
    COMMAND git apply --unsafe-paths --directory=${CMAKE_CURRENT_BINARY_DIR}
        "${CMAKE_CURRENT_SOURCE_DIR}/SampleApp.patch"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../../../.."
    RESULT_VARIABLE patch_result
    ERROR_VARIABLE patch_error
)
if(NOT patch_result EQUAL 0)
    message(FATAL_ERROR "${patch_error}")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qnnlibs")
file(COPY "${QNN_SDK_ROOT}/LICENSE.pdf" "${QNN_SDK_ROOT}/NOTICE.txt"
    "${QNN_SDK_ROOT}/QNN_NOTICE.txt"
    DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/qnn-notices")
foreach(lib QnnHtp QnnSystem)
    file(COPY "${QNN_SDK_ROOT}/lib/aarch64-android/lib${lib}.so"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/qnnlibs")
endforeach()
foreach(arch 68 69 73 75 79 81)
    file(COPY "${QNN_SDK_ROOT}/lib/aarch64-android/libQnnHtpV${arch}Stub.so"
        "${QNN_SDK_ROOT}/lib/hexagon-v${arch}/unsigned/libQnnHtpV${arch}.so"
        "${QNN_SDK_ROOT}/lib/hexagon-v${arch}/unsigned/libQnnHtpV${arch}Skel.so"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/qnnlibs")
endforeach()

if(WIN32)
    set(PLATFORM_NAME "windows")
    set(HOST_TAG "windows-x86_64")
else()
    set(PLATFORM_NAME "linux")
    set(HOST_TAG "linux-x86_64")
endif()

set(OUTPUT_SUFFIX ${CMAKE_ANDROID_ARCH_ABI})
set(PLATFORM_LIBS log android dl m c GLESv2 EGL)
set(ANDROID_TOOLCHAIN_ROOT ${CMAKE_ANDROID_NDK}/toolchains/llvm/prebuilt/${HOST_TAG} CACHE PATH "Android NDK root path")

add_compile_options(
    -O3
    -Wall
    -fno-rtti
    -fPIC
    # -Werror
    -fvisibility=hidden
)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error=for-loop-analysis")

# ZSTD
set(ZSTD_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/zstd)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Build zstd programs" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build static zstd library" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "Enable multithreading support" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Build zstd tests" FORCE)

add_subdirectory(${ZSTD_ROOT_DIR}/build/cmake zstd EXCLUDE_FROM_ALL)

set(ZSTD_INCLUDE_DIR ${ZSTD_ROOT_DIR}/lib)

# MNN
set(MNN_ROOT_DIR 3rdparty/MNN)
set(MNN_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MNN_LOW_MEMORY ON CACHE BOOL "" FORCE)
set(MNN_BUILD_FOR_ANDROID ON CACHE BOOL "" FORCE)
set(MNN_BUILD_FOR_ANDROID_COMMAND ON CACHE BOOL "" FORCE)
set(MNN_USE_LOGCAT OFF CACHE BOOL "" FORCE)
set(MNN_OPENCL ON CACHE BOOL "" FORCE)
set(MNN_SUPPORT_TRANSFORMER_FUSE ON CACHE BOOL "" FORCE)
# GPU backends: OpenCL is the default, Vulkan is the opt-in experiment exposed
# in Settings. MNN dlopens the device's libvulkan.so through its own wrapper
# (MNN_USE_SYSTEM_LIB stays off), so no Vulkan SDK is needed to build. The
# Vulkan sources are chosen at compile time by MNN_VULKAN_IMAGE -- MNN's own
# default, and unlike the OpenCL memory mode it cannot differ per executable
# because both of ours link the same MNN.
#
# The pinned MNN revision (see the 3rdparty/MNN gitlink) already carries the
# Vulkan fixes a usable backend needs: #4940 (multi-image softmax through the
# buffer path) and #4946 (descriptor pools shared per layout, fixing large-graph
# crashes -- an SD UNet is exactly that). Older pins crash or produce garbage
# there.
set(MNN_VULKAN ON CACHE BOOL "" FORCE)
set(MNN_VULKAN_IMAGE ON CACHE BOOL "" FORCE)
# Huawei's NPU is parked, not wired up. MNN's hiai backend compiles fine against
# CANN Kit 6.0.1.0, but making it work needed libhiai* linked into the core, and
# that turned into a hard DT_NEEDED that every device loads at startup: Huawei's
# runtime then aborted the process (libc++abi: terminating, SIGABRT) instead of
# reporting that it would not serve us. Revisit only with a design where a
# missing or refusing library cannot take the whole app down with it.
#
# The switch below is conditional so that staging the DDK (headers plus
# libraries under source/backend/hiai/3rdParty) is all it takes to try again.
set(HIAI_DDK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/MNN/source/backend/hiai/3rdParty"
    CACHE PATH "HiAI / CANN Kit DDK staged for MNN's NPU backend")
set(HIAI_LIBS "")
if(EXISTS "${HIAI_DDK_ROOT}/arm64-v8a/libhiai.so" AND
   EXISTS "${HIAI_DDK_ROOT}/include/hiai_ir_build.h")
    set(MNN_NPU ON CACHE BOOL "" FORCE)
    # The two symbols MNN's hiai backend still references directly are resolved
    # by src/HiaiSupport.cpp instead of by linking Huawei's libraries, so the
    # libraries are only ever loaded when a request selects the Kirin backend.
    # Linking them made them hard DT_NEEDEDs, and their initialisers abort the
    # process on load -- which took every device down with it.
    set(HIAI_SUPPORT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/HiaiSupport.cpp")
    set(SDCORE_HIAI_INCLUDES "${HIAI_DDK_ROOT}/include")
    message(STATUS "HiAI backend enabled (libraries loaded on demand)")
else()
    set(MNN_NPU OFF CACHE BOOL "" FORCE)
    set(HIAI_SUPPORT_SRC "")
    message(STATUS "HiAI DDK not staged: building without the Kirin path")
endif()
# Per-op backend audit: needs MNN's generated flatbuffer schema and the
# flatbuffers runtime it was compiled against, both inside the MNN tree. Guarded
# so a missing one only means no audit, never a broken build.
set(MNN_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/${MNN_ROOT_DIR}")
if(EXISTS "${MNN_SOURCE_ROOT}/schema/current/MNN_generated.h" AND
   EXISTS "${MNN_SOURCE_ROOT}/3rdparty/flatbuffers/include/flatbuffers/flatbuffers.h" AND
   EXISTS "${MNN_SOURCE_ROOT}/source/core/Backend.hpp")
    set(SDCORE_OP_AUDIT ON)
    set(SDCORE_OP_AUDIT_INCLUDES
        "${MNN_SOURCE_ROOT}/schema/current"
        "${MNN_SOURCE_ROOT}/source"
        "${MNN_SOURCE_ROOT}/3rdparty/flatbuffers/include")
    message(STATUS "Per-op backend audit enabled")
else()
    set(SDCORE_OP_AUDIT OFF)
    message(STATUS "Per-op backend audit disabled: MNN schema headers unavailable")
endif()

add_subdirectory(3rdparty/MNN)

set(XTENSOR_USE_XSIMD ON)

set(PACKAGE_INCLUDES
    ${MNN_ROOT_DIR}/include
    # Empty unless the matching block above enabled them: PACKAGE_INCLUDES is
    # assigned here, after those checks, so anything appended before this point
    # would be discarded.
    ${SDCORE_HIAI_INCLUDES}
    ${SDCORE_OP_AUDIT_INCLUDES}
    ${QNN_SDK_ROOT}/include/QNN
    ${CMAKE_CURRENT_SOURCE_DIR}/include/
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/cpp-httplib
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include/nlohmann
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/json/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xtensor/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xtl/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xsimd/include
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xtensor-blas/include
    ${ZSTD_INCLUDE_DIR}

    ${SAMPLE_APP_DIR}/src/
    ${SAMPLE_APP_DIR}/src/CachingUtil
    ${SAMPLE_APP_DIR}/src/Log
    ${SAMPLE_APP_DIR}/src/PAL/include
    ${SAMPLE_APP_DIR}/src/Utils
    ${SAMPLE_APP_DIR}/src/WrapperUtils
)

# tokenizers-cpp
set(TOKENZIER_CPP_PATH ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/tokenizers-cpp)
set(MSGPACK_BUILD_DOCS OFF CACHE BOOL "Build msgpack documentation" FORCE)

if(NOT DEFINED CMAKE_ANDROID_ARCH_ABI)
    set(CMAKE_ANDROID_ARCH_ABI "arm64-v8a" CACHE STRING "Android ABI" FORCE)
endif()
if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI ${CMAKE_ANDROID_ARCH_ABI} CACHE STRING "Android ABI" FORCE)
endif()

add_subdirectory(${TOKENZIER_CPP_PATH} tokenizers EXCLUDE_FROM_ALL)

# set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SRC_DIR ${SAMPLE_APP_DIR}/src)
message(STATUS "SRC_DIR: ${SRC_DIR}")
set(SRC_DIR_LOG ${SRC_DIR}/Log)
set(SRC_DIR_PAL_LINUX ${SRC_DIR}/PAL/src/linux)
set(SRC_DIR_PAL_COMMON ${SRC_DIR}/PAL/src/common)
set(SRC_DIR_UTILS ${SRC_DIR}/Utils)
set(SRC_DIR_WRAPPER_UTILS ${SRC_DIR}/WrapperUtils)

# collect source files
file(GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    # "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${SRC_DIR}/QnnSampleApp.cpp"
    "${SRC_DIR_LOG}/*.cpp"
    "${SRC_DIR_PAL_LINUX}/*.cpp"
    "${SRC_DIR_PAL_COMMON}/*.cpp"
    "${SRC_DIR_UTILS}/*.cpp"
    "${SRC_DIR_WRAPPER_UTILS}/*.cpp"
)
# Appended after the glob, which replaces the variable: the file providing the
# two HiAI symbols MNN references directly, so Huawei's libraries stay a runtime
# dependency rather than a DT_NEEDED of every install.
if(NOT HIAI_SUPPORT_SRC STREQUAL "")
    list(APPEND SOURCES ${HIAI_SUPPORT_SRC})
endif()

if(QNN_DEBUG_ENABLE)
    add_compile_options(-O0 -g)
    add_definitions(-DQNN_API=)
else()
    add_definitions(-DQNN_API=__attribute__\(\(visibility\(\"default\"\)\)\))
endif()

# Two otherwise identical executables that differ only in the OpenCL memory
# mode, so the app can ship both and switch at runtime via a setting:
#   libstable_diffusion_core_image.so  -> MNN_GPU_MEMORY_IMAGE
#   libstable_diffusion_core_buffer.so -> MNN_GPU_MEMORY_BUFFER
foreach(mem_mode IN ITEMS image buffer)
    set(TARGET_NAME "libstable_diffusion_core_${mem_mode}.so")
    add_executable(${TARGET_NAME} ${SOURCES})
    if(NOT QNN_DEBUG_ENABLE)
        target_link_options(${TARGET_NAME} PRIVATE "-Wl,-s")
    endif()
    target_include_directories(${TARGET_NAME} PRIVATE ${PACKAGE_INCLUDES})

    set_target_properties(${TARGET_NAME}
        PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${OUTPUT_SUFFIX}"
    )

    target_compile_definitions(${TARGET_NAME}
        PRIVATE
        XTENSOR_USE_XSIMD
    )
    if(NOT HIAI_SUPPORT_SRC STREQUAL "")
        # Enables the on-demand registration path in MnnUtils.hpp.
        target_compile_definitions(${TARGET_NAME} PRIVATE SDCORE_HIAI_ENABLED=1)
    endif()
    if(SDCORE_OP_AUDIT)
        # Enables BackendAudit.hpp: which ops fell back off the requested backend.
        target_compile_definitions(${TARGET_NAME} PRIVATE SDCORE_OP_AUDIT=1)
    endif()
    if(mem_mode STREQUAL "image")
        target_compile_definitions(${TARGET_NAME} PRIVATE SDCORE_GPU_MEMORY_IMAGE=1)
    else()
        target_compile_definitions(${TARGET_NAME} PRIVATE SDCORE_GPU_MEMORY_IMAGE=0)
    endif()

    # link
    #
    # MNN is whole-archived on purpose. Its Vulkan backend registers itself from
    # a file-static initializer in source/backend/vulkan/runtime/VulkanRuntime.cpp
    # and nothing references that translation unit, so a plain static-archive
    # link drops it: MNN then prints "Can't Find type=7 backend, use 0 instead"
    # and silently runs the whole pipeline on the CPU instead. OpenCL needs no
    # such trick because registerBackend() calls it explicitly.
    target_link_libraries(${TARGET_NAME}
        PRIVATE
        ${PLATFORM_LIBS}
        "-Wl,--whole-archive"
        MNN
        "-Wl,--no-whole-archive"
        tokenizers_cpp
        libzstd
    )
endforeach()
