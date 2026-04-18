cmake_minimum_required(VERSION 3.25)

project(FileOp_New LANGUAGES CXX)

option(CENTER_FILE_ENABLE_THREAD_CENTER "Enable ThreadCenter integration" ON)
set(CENTER_FILE_THREAD_CENTER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ThreadCenter" CACHE PATH "Path to ThreadCenter project root")

add_library(CenterFile STATIC
    src/PlatformFileWin32.cpp
)
add_library(Center::File ALIAS CenterFile)

target_include_directories(CenterFile
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(CenterFile PUBLIC cxx_std_23)

if (WIN32)
    target_compile_definitions(CenterFile
        PUBLIC
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )
endif()

if (CENTER_FILE_ENABLE_THREAD_CENTER)
    set(thread_center_include_dir "${CENTER_FILE_THREAD_CENTER_ROOT}/include")
    set(thread_center_taskflow_dir "${CENTER_FILE_THREAD_CENTER_ROOT}/taskflow-4.0.0")
    if (EXISTS "${thread_center_include_dir}/thread_center/thread_center.hpp")
        target_include_directories(CenterFile PUBLIC
            ${thread_center_include_dir}
            ${thread_center_taskflow_dir}
        )
        target_compile_definitions(CenterFile PUBLIC CENTER_FILE_HAS_THREAD_CENTER=1)
        message(STATUS "CenterFile: ThreadCenter enabled from ${CENTER_FILE_THREAD_CENTER_ROOT}")
    else()
        message(WARNING "CenterFile: ThreadCenter not found at ${CENTER_FILE_THREAD_CENTER_ROOT}, integration disabled")
    endif()
endif()

add_executable(platform_file_benchmark
    bench/platform_file_benchmark.cpp
)
target_link_libraries(platform_file_benchmark PRIVATE Center::File)
target_compile_features(platform_file_benchmark PRIVATE cxx_std_23)

add_executable(platform_file_safety
    tests/platform_file_safety.cpp
)
target_link_libraries(platform_file_safety PRIVATE Center::File)
target_compile_features(platform_file_safety PRIVATE cxx_std_23)

add_executable(file_read_scheduler_bench
    bench/file_read_scheduler_bench.cpp
)
target_link_libraries(file_read_scheduler_bench PRIVATE Center::File)
target_compile_features(file_read_scheduler_bench PRIVATE cxx_std_23)

add_executable(file_read_scheduler_tests
    tests/file_read_scheduler_tests.cpp
)
target_link_libraries(file_read_scheduler_tests PRIVATE Center::File)
target_compile_features(file_read_scheduler_tests PRIVATE cxx_std_23)

add_executable(global_file_scheduler_tests
    tests/global_file_scheduler_tests.cpp
)
target_link_libraries(global_file_scheduler_tests PRIVATE Center::File)
target_compile_features(global_file_scheduler_tests PRIVATE cxx_std_23)


add_executable(triple_read_buffer_tests
    tests/triple_read_buffer_tests.cpp
)
target_link_libraries(triple_read_buffer_tests PRIVATE Center::File)
target_compile_features(triple_read_buffer_tests PRIVATE cxx_std_23)

add_executable(stream_read_session_tests
    tests/stream_read_session_tests.cpp
)
target_link_libraries(stream_read_session_tests PRIVATE Center::File)
target_compile_features(stream_read_session_tests PRIVATE cxx_std_23)





add_executable(stream_read_benchmark
    bench/stream_read_benchmark.cpp
)
target_link_libraries(stream_read_benchmark PRIVATE Center::File)
target_compile_features(stream_read_benchmark PRIVATE cxx_std_23)


add_executable(stream_read_session_async_tests
    tests/stream_read_session_async_tests.cpp
)
target_link_libraries(stream_read_session_async_tests PRIVATE Center::File)
target_compile_features(stream_read_session_async_tests PRIVATE cxx_std_23)

