cmake_minimum_required(VERSION 3.25)

project(FileOp_New VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(CENTER_FILE_ENABLE_THREAD_CENTER "Enable ThreadCenter integration" ON)
option(CENTER_FILE_REQUIRE_THREAD_CENTER "Fail configure if ThreadCenter is enabled but missing" OFF)
option(CENTER_FILE_ENABLE_MEMORY_CENTER "Enable MemoryCenter include/define integration" OFF)
option(CENTER_FILE_REQUIRE_MEMORY_CENTER "Fail configure if MemoryCenter is enabled but missing" OFF)
option(CENTER_FILE_ENABLE_MODULES "Enable C++23 module interfaces for Center::File" ON)
option(CENTER_FILE_BUILD_TESTS "Build test executables" ${PROJECT_IS_TOP_LEVEL})
option(CENTER_FILE_BUILD_BENCHMARKS "Build benchmark executables" ${PROJECT_IS_TOP_LEVEL})
option(CENTER_FILE_INSTALL "Enable install/export package config" ${PROJECT_IS_TOP_LEVEL})
option(CENTER_FILE_AUTO_DETECT_DEPENDENCIES "Auto-detect dependency roots from workspace-level project tree" ON)

set(CENTER_FILE_WORKSPACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Workspace root used to auto-detect ThreadCenter/MemoryCenter")
set(CENTER_FILE_THREAD_CENTER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ThreadCenter" CACHE PATH "Path to ThreadCenter project root")
set(CENTER_FILE_MEMORY_CENTER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../MemoryCenterNew" CACHE PATH "Path to MemoryCenter project root")

# Subproject-friendly defaults:
# - when consumed via add_subdirectory/FetchContent, tests & benchmarks stay OFF by default.
if (NOT PROJECT_IS_TOP_LEVEL)
    if (NOT DEFINED CENTER_FILE_BUILD_TESTS)
        set(CENTER_FILE_BUILD_TESTS OFF CACHE BOOL "Build test executables")
    endif()
    if (NOT DEFINED CENTER_FILE_BUILD_BENCHMARKS)
        set(CENTER_FILE_BUILD_BENCHMARKS OFF CACHE BOOL "Build benchmark executables")
    endif()
endif()

function(center_file_resolve_dependency_root dependency_dir_name configured_root workspace_root marker_relative_path out_root out_found)
    set(candidate_roots "")

    if (configured_root)
        list(APPEND candidate_roots "${configured_root}")
    endif()

    if (workspace_root)
        list(APPEND candidate_roots "${workspace_root}/${dependency_dir_name}")
    endif()

    if (CMAKE_SOURCE_DIR)
        list(APPEND candidate_roots "${CMAKE_SOURCE_DIR}/${dependency_dir_name}")
    endif()

    if (CMAKE_CURRENT_SOURCE_DIR)
        list(APPEND candidate_roots "${CMAKE_CURRENT_SOURCE_DIR}/../${dependency_dir_name}")
    endif()

    list(REMOVE_DUPLICATES candidate_roots)

    set(resolved_root "")
    foreach(candidate_root IN LISTS candidate_roots)
        if (EXISTS "${candidate_root}/${marker_relative_path}")
            set(resolved_root "${candidate_root}")
            break()
        endif()
    endforeach()

    if (NOT resolved_root AND CENTER_FILE_AUTO_DETECT_DEPENDENCIES)
        if (workspace_root AND EXISTS "${workspace_root}")
            file(GLOB workspace_children LIST_DIRECTORIES true "${workspace_root}/*")
            foreach(workspace_child IN LISTS workspace_children)
                if (IS_DIRECTORY "${workspace_child}" AND EXISTS "${workspace_child}/${marker_relative_path}")
                    set(resolved_root "${workspace_child}")
                    break()
                endif()
            endforeach()
        endif()
    endif()

    if (resolved_root)
        set(${out_root} "${resolved_root}" PARENT_SCOPE)
        set(${out_found} ON PARENT_SCOPE)
    else()
        set(${out_root} "${configured_root}" PARENT_SCOPE)
        set(${out_found} OFF PARENT_SCOPE)
    endif()
endfunction()

set(center_file_thread_center_available OFF)
set(center_file_memory_center_available OFF)

center_file_resolve_dependency_root(
    "ThreadCenter"
    "${CENTER_FILE_THREAD_CENTER_ROOT}"
    "${CENTER_FILE_WORKSPACE_ROOT}"
    "include/thread_center/thread_center.hpp"
    center_file_thread_center_root_resolved
    center_file_thread_center_available
)
set(center_file_thread_center_include_dir "${center_file_thread_center_root_resolved}/include")
set(center_file_thread_center_taskflow_dir "${center_file_thread_center_root_resolved}/taskflow-4.0.0")

center_file_resolve_dependency_root(
    "MemoryCenterNew"
    "${CENTER_FILE_MEMORY_CENTER_ROOT}"
    "${CENTER_FILE_WORKSPACE_ROOT}"
    "include/Center/Memory/MemoryCenter.hpp"
    center_file_memory_center_root_resolved
    center_file_memory_center_available
)
set(center_file_memory_center_include_dir "${center_file_memory_center_root_resolved}/include")

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

    if (CENTER_FILE_ENABLE_THREAD_CENTER AND center_file_thread_center_available)
        target_include_directories(${target_name}
            PUBLIC
                $<BUILD_INTERFACE:${center_file_thread_center_include_dir}>
                $<BUILD_INTERFACE:${center_file_thread_center_taskflow_dir}>
        )
        target_compile_definitions(${target_name} PUBLIC CENTER_FILE_HAS_THREAD_CENTER=1)
    endif()

    if (CENTER_FILE_ENABLE_MEMORY_CENTER AND center_file_memory_center_available)
        target_include_directories(${target_name}
            PUBLIC
                $<BUILD_INTERFACE:${center_file_memory_center_include_dir}>
        )
        target_compile_definitions(${target_name} PUBLIC CENTER_FILE_HAS_MEMORY_CENTER=1)
    endif()
endfunction()

if (CENTER_FILE_ENABLE_THREAD_CENTER)
    if (center_file_thread_center_available)
        message(STATUS "CenterFile dependency: ThreadCenter FOUND at ${center_file_thread_center_root_resolved}")
    else()
        if (CENTER_FILE_REQUIRE_THREAD_CENTER)
            message(FATAL_ERROR "CenterFile dependency: ThreadCenter MISSING. configured='${CENTER_FILE_THREAD_CENTER_ROOT}', workspace='${CENTER_FILE_WORKSPACE_ROOT}' (CENTER_FILE_REQUIRE_THREAD_CENTER=ON)")
        else()
            message(WARNING "CenterFile dependency: ThreadCenter MISSING. configured='${CENTER_FILE_THREAD_CENTER_ROOT}', workspace='${CENTER_FILE_WORKSPACE_ROOT}'. integration disabled")
        endif()
    endif()
else()
    message(STATUS "CenterFile dependency: ThreadCenter integration OFF")
endif()

if (CENTER_FILE_ENABLE_MEMORY_CENTER)
    if (center_file_memory_center_available)
        message(STATUS "CenterFile dependency: MemoryCenter FOUND at ${center_file_memory_center_root_resolved}")
    else()
        if (CENTER_FILE_REQUIRE_MEMORY_CENTER)
            message(FATAL_ERROR "CenterFile dependency: MemoryCenter MISSING. configured='${CENTER_FILE_MEMORY_CENTER_ROOT}', workspace='${CENTER_FILE_WORKSPACE_ROOT}' (CENTER_FILE_REQUIRE_MEMORY_CENTER=ON)")
        else()
            message(WARNING "CenterFile dependency: MemoryCenter MISSING. configured='${CENTER_FILE_MEMORY_CENTER_ROOT}', workspace='${CENTER_FILE_WORKSPACE_ROOT}'. integration disabled")
        endif()
    endif()
else()
    message(STATUS "CenterFile dependency: MemoryCenter integration OFF (current code has no hard dependency)")
endif()

add_library(CenterFile STATIC
    src/PlatformFileWin32.cpp
)
set_target_properties(CenterFile PROPERTIES EXPORT_NAME File)
add_library(Center::File ALIAS CenterFile)
center_file_apply_common(CenterFile)

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
