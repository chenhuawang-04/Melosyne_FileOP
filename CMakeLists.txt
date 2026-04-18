cmake_minimum_required(VERSION 3.25)

project(FileOp_New VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CENTER_FILE_ENABLE_THREAD_CENTER "Enable ThreadCenter integration" ON)
option(CENTER_FILE_ENABLE_MODULES "Enable C++23 module interfaces for Center::File" ON)
option(CENTER_FILE_BUILD_TESTS "Build test executables" ${PROJECT_IS_TOP_LEVEL})
option(CENTER_FILE_BUILD_BENCHMARKS "Build benchmark executables" ${PROJECT_IS_TOP_LEVEL})
option(CENTER_FILE_INSTALL "Enable install/export package config" ${PROJECT_IS_TOP_LEVEL})

set(CENTER_FILE_THREAD_CENTER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ThreadCenter" CACHE PATH "Path to ThreadCenter project root")

set(center_file_thread_center_available OFF)
set(center_file_thread_center_include_dir "${CENTER_FILE_THREAD_CENTER_ROOT}/include")
set(center_file_thread_center_taskflow_dir "${CENTER_FILE_THREAD_CENTER_ROOT}/taskflow-4.0.0")

function(center_file_apply_common target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_23)

    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    if (WIN32)
        target_compile_definitions(${target_name}
            PUBLIC
                UNICODE
                _UNICODE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
        )
    endif()

    if (CENTER_FILE_ENABLE_THREAD_CENTER)
        if (EXISTS "${center_file_thread_center_include_dir}/thread_center/thread_center.hpp")
            target_include_directories(${target_name}
                PUBLIC
                    $<BUILD_INTERFACE:${center_file_thread_center_include_dir}>
                    $<BUILD_INTERFACE:${center_file_thread_center_taskflow_dir}>
            )
            target_compile_definitions(${target_name} PUBLIC CENTER_FILE_HAS_THREAD_CENTER=1)
        endif()
    endif()
endfunction()

add_library(CenterFile STATIC
    src/PlatformFileWin32.cpp
)
set_target_properties(CenterFile PROPERTIES EXPORT_NAME File)
add_library(Center::File ALIAS CenterFile)
center_file_apply_common(CenterFile)

if (CENTER_FILE_ENABLE_THREAD_CENTER)
    if (EXISTS "${center_file_thread_center_include_dir}/thread_center/thread_center.hpp")
        set(center_file_thread_center_available ON)
        message(STATUS "CenterFile: ThreadCenter enabled from ${CENTER_FILE_THREAD_CENTER_ROOT}")
    else()
        message(WARNING "CenterFile: ThreadCenter not found at ${CENTER_FILE_THREAD_CENTER_ROOT}, integration disabled")
    endif()
endif()

set(center_file_modules_enabled OFF)
set(center_file_module_files
    modules/Center/File/Config.cppm
    modules/Center/File/Error.cppm
    modules/Center/File/Types.cppm
    modules/Center/File/PlatformFile.cppm
    modules/Center/File/SchedulingTypes.cppm
    modules/Center/File/FileReadPlanner.cppm
    modules/Center/File/ReadHandlePool.cppm
    modules/Center/File/ThreadCenterAdapter.cppm
    modules/Center/File/FileReadScheduler.cppm
    modules/Center/File/GlobalFileScheduler.cppm
    modules/Center/File/StreamTypes.cppm
    modules/Center/File/TripleReadBuffer.cppm
    modules/Center/File/StreamReadSession.cppm
    modules/Center/File/StreamReadSessionAsync.cppm
    modules/Center/File/FileOp.cppm
)

if (CENTER_FILE_ENABLE_MODULES)
    if (CMAKE_VERSION VERSION_LESS 3.28)
        message(STATUS "CenterFile modules: CMake < 3.28, module source files are provided but CXX_MODULES target is disabled")
    else()
        add_library(CenterFileModules STATIC
            modules/Center/File/module_anchor.cpp
        )
        set_target_properties(CenterFileModules PROPERTIES EXPORT_NAME FileModules)
        add_library(Center::FileModules ALIAS CenterFileModules)

        target_sources(CenterFileModules
            PUBLIC
                FILE_SET CXX_MODULES FILES
                    ${center_file_module_files}
        )

        center_file_apply_common(CenterFileModules)
        target_link_libraries(CenterFileModules PUBLIC Center::File)

        set(center_file_modules_enabled ON)
        message(STATUS "CenterFile modules: CXX_MODULES target enabled (Center::FileModules)")
    endif()
endif()

function(center_file_add_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE Center::File)
    target_compile_features(${target_name} PRIVATE cxx_std_23)
endfunction()

if (CENTER_FILE_BUILD_BENCHMARKS)
    center_file_add_executable(platform_file_benchmark bench/platform_file_benchmark.cpp)
    center_file_add_executable(file_read_scheduler_bench bench/file_read_scheduler_bench.cpp)
    center_file_add_executable(stream_read_benchmark bench/stream_read_benchmark.cpp)
endif()

if (CENTER_FILE_BUILD_TESTS)
    center_file_add_executable(platform_file_safety tests/platform_file_safety.cpp)
    center_file_add_executable(file_read_scheduler_tests tests/file_read_scheduler_tests.cpp)
    center_file_add_executable(global_file_scheduler_tests tests/global_file_scheduler_tests.cpp)
    center_file_add_executable(triple_read_buffer_tests tests/triple_read_buffer_tests.cpp)
    center_file_add_executable(stream_read_session_tests tests/stream_read_session_tests.cpp)
    center_file_add_executable(stream_read_session_async_tests tests/stream_read_session_async_tests.cpp)
endif()

if (CENTER_FILE_INSTALL)
    install(TARGETS CenterFile
        EXPORT CenterFileTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    if (center_file_modules_enabled)
        install(TARGETS CenterFileModules
            EXPORT CenterFileTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_LIBDIR}/cxx/modules
        )
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CenterFileConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/CenterFileConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CenterFile
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/CenterFileConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(EXPORT CenterFileTargets
        FILE CenterFileTargets.cmake
        NAMESPACE Center::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CenterFile
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CenterFileConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CenterFileConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CenterFile
    )
endif()
