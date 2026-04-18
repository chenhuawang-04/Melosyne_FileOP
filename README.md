# Melosyne FileOP（C++23 高性能文件 I/O 模块）

> UTF-8 文档。面向游戏引擎场景：高吞吐、可调度、可流式、可验证。

## 1. 项目目标

`FileOp_New` 提供一套偏底层、可组合的文件读写能力：

- 平台后端读写（当前以 Win32 为主）
- 面向引擎的读取调度（按紧急度/方法分流）
- 流式读取（同步/异步）
- 三缓冲基础设施（TripleReadBuffer）
- ThreadCenter 集成（可开关）

适用场景：

- 资源包/二进制资产加载
- 大文件“读一点、消耗一点”的流式播放（音频/视频/流媒体）
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
│  └─ ...
├─ src/
│  └─ PlatformFileWin32.cpp      # Win32 后端实现
├─ tests/
│  ├─ stream_read_session_tests.cpp
│  ├─ stream_read_session_async_tests.cpp
│  ├─ triple_read_buffer_tests.cpp
│  └─ file_read_scheduler_tests.cpp
├─ bench/
│  ├─ platform_file_benchmark.cpp
│  ├─ file_read_scheduler_bench.cpp
│  └─ stream_read_benchmark.cpp
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
./stream_read_session_tests.exe
./stream_read_session_async_tests.exe
./triple_read_buffer_tests.exe
./file_read_scheduler_tests.exe
```

### 3.3 运行基准

```powershell
cd build
./stream_read_benchmark.exe
```

---

## 4. 快速开始

### 4.1 最底层读取：PlatformFile

```cpp
#include "Center/File/FileOp.hpp"
#include <array>

std::array<std::byte, 4096> buffer{};
auto file_result = Center::File::PlatformFile::openRead("assets/data.bin");
if (!file_result) {
    // 处理 file_result.error()
    return;
}

auto file = std::move(*file_result);
auto read_result = file.readAt(0, std::span<std::byte>{buffer.data(), buffer.size()});
if (!read_result) {
    // 处理 read_result.error()
    return;
}
// *read_result 为实际读取字节数
```

### 4.2 同步流式读取：StreamReadSession

```cpp
#include "Center/File/FileOp.hpp"
#include <array>

Center::File::StreamReadSession session{};
Center::File::StreamReadConfig config{};
config.chunk_bytes = 256 * 1024;

auto start_status = session.start("assets/stream.bin", 0, 0, config);
if (!start_status) {
    return;
}

std::array<std::byte, 256 * 1024> temp{};
while (true) {
    auto read_result = session.readNext(std::span<std::byte>{temp.data(), temp.size()});
    if (!read_result) {
        break; // 错误处理
    }
    if (*read_result == 0) {
        break; // EOS
    }
    // 消费 temp[0, *read_result)
}

session.stop();
```

### 4.3 异步流式读取：StreamReadSessionAsync

```cpp
#include "Center/File/FileOp.hpp"
#include <array>

Center::File::StreamReadSessionAsync session{};
Center::File::StreamReadConfig config{};
config.chunk_bytes = 256 * 1024;

auto start_status = session.start("assets/stream.bin", 0, 0, config);
if (!start_status) {
    return;
}

std::array<std::byte, 256 * 1024> temp{};
while (true) {
    auto read_result = session.tryReadNext(std::span<std::byte>{temp.data(), temp.size()});
    if (!read_result) {
        break; // 错误处理
    }
    if (*read_result == 0) {
        if (session.isEndOfStream()) {
            break;
        }
        // 当前无数据可取，可短暂让出时间片
        std::this_thread::yield();
        continue;
    }

    // 消费 temp[0, *read_result)
}

session.stop();
```

---

## 5. 关键设计说明

### 5.1 错误体系

统一使用：

- `FileResult<T> = std::expected<T, FileError>`
- `FileStatus = FileResult<void>`

`FileError` 包含：

- 操作类型（open/read/write/...）
- 平台错误码（`std::error_code`）
- offset / requested / processed / eof 标志

### 5.2 内存与对齐

- 流式缓冲支持 `std::pmr::memory_resource`
- 关键缓冲区按 64 字节对齐（cache line 友好）
- 三缓冲支持 stop 唤醒与错误传播

### 5.3 调度与方法

调度器可依据请求特征选择：

- `directRead`
- `mappedCopy`
- `mappedView`

并结合紧急度（urgent/normal/background）进行执行。

---

## 6. 与 ThreadCenter 集成

CMake 选项：

- `CENTER_FILE_ENABLE_THREAD_CENTER=ON/OFF`
- `CENTER_FILE_THREAD_CENTER_ROOT=<path>`

默认启用；未找到依赖时会自动降级为可用路径并给出提示。

---

## 7. 性能与验证建议

建议至少覆盖以下对照：

1. direct chunked read（理论近似最快基线）
2. stream pull（同步）
3. stream async（异步）

并同时关注：

- 吞吐（MiB/s）
- 尾延迟抖动（建议补充 P50/P90/P99）
- CPU 占用
- 冷热缓存差异

---

## 8. 文档索引

当前仓库中的设计/报告文档（均 UTF-8）：

- `file_module_design.md`
- `platform_file_validation_report.md`
- `stream_triple_buffer_plan_v1.md`
- `thread_center_integration_phase1.md`
- `thread_center_integration_phase2_report.md`
- `thread_center_integration_phase3_report.md`
- `thread_center_integration_phase4_report.md`
- `code_quality_review_2026-04-16.md`

---

## 9. 命名规范（项目约定）

- 函数名：首字母小写驼峰（`readNext`）
- 变量名：小写蛇形（`ready_count`）
- 函数参数：小写蛇形 + 末尾下划线（`path_`）

---

## 10. 许可证

见 `LICENSE`。
