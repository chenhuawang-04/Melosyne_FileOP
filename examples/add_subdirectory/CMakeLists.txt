cmake_minimum_required(VERSION 3.25)
project(CenterFileAddSubdirExample LANGUAGES CXX)

# 假设本示例位于 FileOp_New/examples/add_subdirectory
set(CENTER_FILE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CENTER_FILE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CENTER_FILE_ENABLE_MODULES ON CACHE BOOL "" FORCE)

add_subdirectory(../../ FileOp_New)

add_executable(add_subdir_example main.cpp)
target_compile_features(add_subdir_example PRIVATE cxx_std_23)
target_link_libraries(add_subdir_example PRIVATE Tool::File)

# 如果希望使用模块目标：
# target_link_libraries(add_subdir_example PRIVATE Tool::FileModules)

