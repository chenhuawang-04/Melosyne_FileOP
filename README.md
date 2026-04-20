# Melosyne FileOP（C++23 高性能文件 I/O 模块）

> UTF-8 文档。面向游戏引擎场景：高吞吐、可调度、可流式、可验证。

## 1. 项目目标

`FileOp_New` 提供一套偏底层、可组合的文件读写能力：

- 平台后端读写（当前以 Win32 为主）
- 面向引擎的读取调度（按紧急度 / 方法分流）
- 流式读取（同步 / 异步）
- 三缓冲基础设施（TripleReadBuffer）
- ThreadCenter 集成（可开关）
- POD 一次性读取 helper（读入连续 span）

适用场景：

- 资源包 / 二进制资产加载
- 大文件“读一点、消耗一点”的流式播放（音频 / 视频 / 流媒体）
- 启动阶段批量资源预取

---

## 2. 目录结构

```text
FileOp_New/
├─ include/Center/File/
│  ├─ FileOp.hpp                 # 聚合头
│  ├─ PlatformFile.hpp           # 平台文件 API
│  ├─ FileReadScheduler.hpp      # 读取调度器
│  ├─ StreamReadSession.hpp      # 同步流式读取
│  ├─ StreamReadSessionAsync.hpp # 异步流式读取
│  ├─ TripleReadBuffer.hpp       # 三缓冲核心结构
│  ├─ BinaryReadHelper.hpp       # POD span 一次性读取 helper
│  └─ ...
├─ src/
│  └─ PlatformFileWin32.cpp      # Win32 后端实现
├─ modules/Center/File/
│  └─ *.cppm                     # 对称模块实现（真实 cppm）
├─ tests/
├─ bench/
└─ CMakeLists.txt
```

---

## 3. 构建与运行

### 3.1 CMake 配置（推荐 Ninja）

```powershell
cmake -S . -B build -G Ninja
cmake --build build -j 8
```

### 3.2 运行测试

```powershell
cd build
./platform_file_safety.exe
./file_read_scheduler_tests.exe
./global_file_scheduler_tests.exe
./triple_read_buffer_tests.exe
./stream_read_session_tests.exe
./stream_read_session_async_tests.exe
./binary_read_helper_tests.exe
```

### 3.3 运行基准

```powershell
cd build
./platform_file_benchmark.exe
./file_read_scheduler_bench.exe
./stream_read_benchmark.exe
```

---

## 4. 快速开始

### 4.1 最底层读取：PlatformFile

```cpp
#include "Center/File/FileOp.hpp"
#include <array>

std::array<std::byte, 4096> buffer{};
auto file_result = Tool::File::PlatformFile::openRead("assets/data.bin");
if (!file_result) {
    return;
}

auto file = std::move(*file_result);
auto read_result = file.readAt(0, std::span<std::byte>{buffer.data(), buffer.size()});
if (!read_result) {
    return;
}
```

### 4.2 POD 一次性读取到 span（高性能、单线程快捷接口）

```cpp
#include "Center/File/FileOp.hpp"

struct Packet {
    std::uint32_t id;
    std::uint16_t channel;
    std::uint16_t flags;
    float gain;
};

std::vector<Packet> packets;
packets.resize(expected_count);

auto status = Tool::File::readFileToSpan(
    "assets/packets.bin",
    std::span<Packet>{packets.data(), packets.size()}
);
if (!status) {
    // status.error()
}
```

约束：

- `readFileToSpan` 仅支持 POD 语义（`trivial + standard_layout`）
- 文件大小必须与目标 span 字节数完全一致

### 4.3 同步流式读取：StreamReadSession

```cpp
#include "Center/File/FileOp.hpp"
#include <array>

Tool::File::StreamReadSession session{};
Tool::File::StreamReadConfig config{};
config.chunk_bytes = 256 * 1024;

auto start_status = session.start("assets/stream.bin", 0, 0, config);
if (!start_status) {
    return;
}

std::array<std::byte, 256 * 1024> temp{};
while (true) {
    auto read_result = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
    if (!read_result) {
        break;
    }
    if (*read_result == 0) {
        break; // EOS
    }
}

session.stop();
```

---

## 5. 核心设计（容器重构后的 API）

### 5.1 彻底去除 `std::vector` 绑定

调度核心 API 已改为“容器无关”模式：

- 输入：`std::span<const ReadRequest>` 或任意 `ReadRequestContainer`
- 输出：任意 `ReadViewContainer`

不再提供仅限 `std::vector<ReadRequest>` / `std::vector<ReadView>` 的专用入口。

### 5.2 容器概念要求

`ReadRequestContainer` 需要：

- `data()` 可转为 `const ReadRequest*`
- `size()` 可转为 `std::size_t`

`ReadViewContainer` 需要：

- `clear()`
- `push_back(const ReadView&)`
- `size()`
- `reserve()` 是可选（有则自动利用，无则降级）

### 5.3 内部动态数组类型抽象

内部动态容器统一走：

```cpp
template<typename element_type>
using DynamicArray = CENTER_FILE_VECTOR_TEMPLATE<element_type>;
```

默认是 `std::vector`，但可由项目级宏替换（见第 6 节）。

---

## 6. 自定义 Vector 接入（重点）

如果你的容器模板第一个参数是元素类型，后续模板参数均有默认值，可直接替换。

### 6.1 CMake 侧定义宏

```cmake
target_compile_definitions(MyGame PRIVATE
    CENTER_FILE_CUSTOM_VECTOR_HEADER="MyContainer/MyVector.hpp"
    CENTER_FILE_VECTOR_TEMPLATE=MyNamespace::Vector
)
```

说明：

- `CENTER_FILE_CUSTOM_VECTOR_HEADER`：可选，自定义容器头文件
- `CENTER_FILE_VECTOR_TEMPLATE`：容器模板名（默认 `std::vector`）

### 6.2 最小容器能力建议

用于 `DynamicArray<T>` 的容器建议至少支持：

- 默认构造、移动
- `size()/empty()/data()`
- `begin()/end()`
- `reserve()/push_back()/front()/back()/operator[]`

当前代码路径已经覆盖这些常见能力。

---

## 7. 与 ThreadCenter 集成

CMake 选项：

- `CENTER_FILE_ENABLE_THREAD_CENTER=ON/OFF`
- `CENTER_FILE_THREAD_CENTER_ROOT=<path>`
- `CENTER_FILE_REQUIRE_THREAD_CENTER=ON/OFF`

依赖检测顺序：

1. 先看父工程已存在的 ThreadCenter target
2. 再看配置路径 / workspace 自动探测路径

---

## 8. C++23 Modules 版本（对称实现）

本仓库已提供与 `include/Center/File` 对称的模块接口单元：

- 路径：`modules/Center/File/*.cppm`
- 聚合入口：`Tool.File.FileOp`

示例：

```cpp
import Tool.File.FileOp;
using namespace Tool::File;
```

说明：

- 这些 `.cppm` 是真实模块实现，不是头文件包装。
- 当 CMake >= 3.28 且 `CENTER_FILE_ENABLE_MODULES=ON` 时，启用 `Tool::FileModules`。

---

## 9. 便捷接入任意项目

### 9.1 add_subdirectory / FetchContent

```cmake
set(CENTER_FILE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CENTER_FILE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CENTER_FILE_ENABLE_MODULES ON CACHE BOOL "" FORCE)

add_subdirectory(external/FileOp_New)

target_link_libraries(MyGame PRIVATE Tool::File)
# target_link_libraries(MyGame PRIVATE Tool::FileModules)
```

### 9.2 安装后 find_package

```powershell
cmake -S . -B build -G Ninja -DCENTER_FILE_INSTALL=ON -DCENTER_FILE_BUILD_TESTS=OFF -DCENTER_FILE_BUILD_BENCHMARKS=OFF
cmake --build build
cmake --install build --prefix <install-prefix>
```

```cmake
find_package(CenterFile CONFIG REQUIRED)
target_link_libraries(MyGame PRIVATE Tool::File)
```

---

## 10. 命名规范（项目约定）

- 函数名：首字母小写驼峰（`readNext`）
- 变量名：小写蛇形（`ready_count`）
- 函数参数：小写蛇形 + 末尾下划线（`path_`）

---

## 11. 许可证

见 `LICENSE`。
