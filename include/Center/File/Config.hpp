#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32) || defined(_WIN64)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #define CENTER_FILE_WINDOWS 1
#elif defined(__linux__)
    #define CENTER_FILE_LINUX 1
#elif defined(__APPLE__)
    #define CENTER_FILE_MACOS 1
#endif

#if defined(__clang__)
    #define CENTER_FILE_CLANG 1
#elif defined(__GNUC__)
    #define CENTER_FILE_GCC 1
#elif defined(_MSC_VER)
    #define CENTER_FILE_MSVC 1
#endif

#if defined(CENTER_FILE_CLANG) || defined(CENTER_FILE_GCC)
    #define CENTER_FILE_LIKELY(x) __builtin_expect(!!(x), 1)
    #define CENTER_FILE_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define CENTER_FILE_ALWAYS_INLINE __attribute__((always_inline)) inline
#elif defined(CENTER_FILE_MSVC)
    #define CENTER_FILE_LIKELY(x) (x)
    #define CENTER_FILE_UNLIKELY(x) (x)
    #define CENTER_FILE_ALWAYS_INLINE __forceinline
#else
    #define CENTER_FILE_LIKELY(x) (x)
    #define CENTER_FILE_UNLIKELY(x) (x)
    #define CENTER_FILE_ALWAYS_INLINE inline
#endif

#ifndef CENTER_FILE_HAS_THREAD_CENTER
    #define CENTER_FILE_HAS_THREAD_CENTER 0
#endif

namespace Tool::File {

inline constexpr std::size_t max_io_chunk_bytes = 1u << 30;

} // namespace Tool::File

