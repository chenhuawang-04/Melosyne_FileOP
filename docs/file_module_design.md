# 文件模块设计与实现规划

## 1. 文档目的

本文档用于固定 `FileOp_New` 项目的整体文件模块设计，明确：

- 模块目标与非目标
- 分层架构
- 对外 API 规划
- 与 `MemoryCenterNew` 的集成策略
- 高性能设计原则
- 平台后端职责边界
- 后续实现阶段与测试计划

当前阶段已经开始实现 **最底层平台后端层**，本文档同时作为后续上层实现的约束说明。

---

## 2. 模块目标

本模块用于游戏引擎中的高性能文件读取与写入，目标如下：

1. **高性能**
   - 优先保证资源读取、配置加载、二进制资产读写的吞吐与低额外开销。
   - 避免不必要的中间拷贝。
   - 避免使用 `std::fstream` / iostream 作为核心后端。
   - 采用平台原生文件 API。

2. **统一 API**
   - 给定文件名以及目标容器或目标内存区间，可以统一完成读写。
   - 支持“读取整个文件到容器”。
   - 支持“读取到给定内存区间/子区间”。
   - 支持“写出整个容器/内存区间”。

3. **自动识别目标类型**
   - 自动识别标准连续容器、自定义连续容器、原始内存区域、`std::span`、`std::array` 等。
   - 在编译期通过 traits / concepts 完成分派，不依赖运行时反射。

4. **与项目内存系统兼容**
   - 上层整文件读入容器时，能够自然接入 `MemoryCenterNew`。
   - 支持 `CustomerAllocator<T, Tags::Container>` 等容器分配器。
   - 必要时支持 `Tags::Temporary` 作为 staging / scratch buffer 来源。

5. **适配游戏引擎场景**
   - 支持大文件。
   - 支持面向 offset 的 positional I/O。
   - 方便后续扩展异步 I/O、内存映射、资源包读取等能力。

---

## 3. 非目标

当前设计明确不做以下事情，避免职责污染：

1. **不在底层后端做文本语义处理**
   - 不做编码转换。
   - 不做 BOM 识别。
   - 不做换行归一化。
   - 不做格式化输入输出。

2. **不在底层后端做序列化协议**
   - 不在平台层理解对象版本、字段演进、endianness 适配。
   - `readObject` / `writeObject` 仅表示“按原始字节视图读写 trivially copyable 对象”。

3. **不追求支持所有 STL 容器**
   - 高性能快路径仅支持“连续内存容器”或“可表示为连续内存区间的目标”。
   - `std::list` / `std::set` / `std::deque` 这类非连续结构不作为核心支持目标。

4. **不在当前阶段直接实现完整异步文件系统**
   - 第一阶段先完成同步 positional 后端。
   - 异步和 mmap 作为后续扩展层。

---

## 4. 总体架构

模块采用 4 层设计。

### 4.1 第 1 层：平台后端层 `PlatformFile`

职责：

- 打开文件
- 关闭文件
- 获取文件大小
- 基于 offset 读取字节
- 基于 offset 写入字节
- resize / truncate
- flush

特点：

- 不依赖容器
- 不做堆分配
- 只接受字节区间 `std::span<std::byte>` / `std::span<const std::byte>`
- 提供稳定、低层、可复用的原语

当前已开始实现此层，Windows 版本文件为：

- `include/Center/File/PlatformFile.hpp`
- `src/PlatformFileWin32.cpp`

### 4.2 第 2 层：字节视图 / traits 适配层

职责：

- 将任意可支持的容器或内存区域统一转换为字节区间
- 识别是否可写、是否可 resize、是否连续、元素是否可按字节搬运
- 为自定义引擎容器提供扩展点

核心形式：

- `bufferTraits<T>`
- `contiguousReadableRange`
- `contiguousWritableRange`
- `resizableByteTarget`

### 4.3 第 3 层：高层算法层

职责：

- `readFile(path_, container_)`
- `readInto(path_, range_)`
- `writeFile(path_, range_)`
- `readObject(path_, object_)`
- `writeObject(path_, object_)`

此层组合：

- 文件大小查询
- 容器扩容
- 直接读入目标内存
- exact / short I/O 策略处理

### 4.4 第 4 层：扩展功能层

计划后续支持：

- `MappedFile` / 内存映射
- 异步文件 I/O
- 资源包 / archive reader
- 压缩 / 加密流
- scatter / gather I/O

---

## 5. 核心数据抽象

整个模块的统一核心抽象是：

- `MutableBytes = std::span<std::byte>`
- `ConstBytes = std::span<const std::byte>`

原因：

1. 平台文件 API 本质上只处理原始字节。
2. 统一字节视图后，所有容器适配都能退化到同一种低层调用。
3. 可以让容器层和平台层完全解耦。
4. 更容易保证“无中间拷贝”的快路径。

---

## 6. 与 MemoryCenterNew 的集成策略

### 6.1 当前阶段的原则

当前已经参考了 `MemoryCenterNew` 的设计，得到以下原则：

1. **平台后端层不直接依赖内存中心做分配**
   - `PlatformFile` 只处理文件句柄与原始 I/O。
   - 不应该在底层偷偷 new / delete / vector 临时缓冲。

2. **内存分配由上层容器或适配层决定**
   - 若用户传入容器，则容器自己的 allocator 决定分配来源。
   - 若用户传入的是 `std::vector<T, CustomerAllocator<T, Tags::Container>>`，则自动走 MemoryCenter 的容器分配路径。

3. **只在必要时引入 Temporary scratch**
   - 某些未来功能可能需要 staging buffer，例如压缩/解压、非连续目标聚合写入等。
   - 这类临时内存建议接 `Tags::Temporary`。
   - 但当前纯平台 I/O 层不应该主动引入 scratch 分配。

### 6.2 计划支持的 MemoryCenter 使用方式

后续高层 API 需要天然支持：

```cpp
std::vector<std::byte, Center::Memory::ContainerAllocator<std::byte>> data;
auto result = Center::File::readFile(path, data);
```

以及：

```cpp
using StringAllocator = Center::Memory::CustomerAllocator<char, Center::Memory::Tags::Container>;
std::basic_string<char, std::char_traits<char>, StringAllocator> text;
auto result = Center::File::readFile(path, text);
```

### 6.3 对 traits 层的要求

`bufferTraits<T>` 应尽量不关心 allocator 类型本身，只要容器满足：

- `data()`
- `size()`
- `resize()`
- 连续内存语义

即可接入。

这意味着：

- 支持 `std::vector<T>`
- 支持 `std::vector<T, CustomerAllocator<T, Tag>>`
- 支持 `std::pmr::vector<T>`
- 支持自定义引擎连续缓冲区类型

---

## 7. 平台后端层设计

### 7.1 设计目标

`PlatformFile` 是整个系统的 I/O 原语层，目标如下：

- RAII 句柄管理
- 原生平台 API 封装
- 不分配内存
- 不持有额外缓冲
- 接口简洁稳定
- 支持大文件
- 支持读写共享、创建策略、访问模式 hint

### 7.2 当前类设计

当前对外接口规划为：

```cpp
class PlatformFile {
public:
    static FileResult<PlatformFile> open(...);
    static FileResult<PlatformFile> openRead(...);
    static FileResult<PlatformFile> openWrite(...);
    static FileResult<PlatformFile> openReadWrite(...);

    bool isOpen() const noexcept;
    FileStatus close() noexcept;

    FileResult<std::uint64_t> size() const noexcept;

    FileResult<std::size_t> readAt(std::uint64_t offset_, MutableBytes destination_) const noexcept;
    FileStatus readExactAt(std::uint64_t offset_, MutableBytes destination_) const noexcept;

    FileResult<std::size_t> writeAt(std::uint64_t offset_, ConstBytes source_) const noexcept;
    FileStatus writeExactAt(std::uint64_t offset_, ConstBytes source_) const noexcept;

    FileStatus resize(std::uint64_t size_bytes_) noexcept;
    FileStatus flush() noexcept;
};
```

### 7.3 当前 Windows 实现

当前 Windows 后端采用：

- `CreateFileW`
- `GetFileSizeEx`
- `ReadFile`
- `WriteFile`
- `SetFilePointerEx`
- `SetEndOfFile`
- `FlushFileBuffers`

并支持：

- `FileAccess`
- `FileCreation`
- `FileShare`
- `FileHint`

其中 `FileHint` 映射到：

- `normal`
- `FILE_FLAG_SEQUENTIAL_SCAN`
- `FILE_FLAG_RANDOM_ACCESS`

### 7.4 positional I/O 语义

`readAt` / `writeAt` 使用 offset 参数，不依赖共享文件指针语义。

优点：

- 更适合多线程资产读取
- 更适合资源包随机访问
- 更利于后续迁移到异步/overlapped/IOCP 风格接口
- 调用方语义更清晰

### 7.5 当前后端层的约束

1. 不负责目录创建。
2. 不负责路径规范化策略。
3. 不负责锁策略抽象。
4. 不负责缓存管理。
5. 不负责压缩、解密、序列化。

---

## 8. 错误模型

模块统一使用：

- `std::expected<T, FileError>`

避免异常作为主路径错误传播机制。

### 8.1 `FileError` 内容

当前字段：

- `operation`
- `code`
- `offset`
- `requested`
- `processed`
- `end_of_file`

### 8.2 设计原因

1. 游戏引擎更适合显式错误返回。
2. 易于日志系统汇总上下文。
3. 易于性能敏感路径避免异常展开。
4. 平台错误码能够保留底层信息。

---

## 9. 性能设计原则

### 9.1 不使用 iostream 作为核心实现

原因：

- 状态机较重
- 行为不够透明
- 对大文件 / 精确控制不够理想
- 容易引入不必要层级

### 9.2 整文件读取必须走“先 size，后 resize，一次直读”

正确快路径：

1. 查询文件大小
2. 目标容器按字节扩容
3. 直接将文件内容读入 `data()`

避免：

- 先读到临时 `vector<char>` 再拷贝
- `stringstream`
- `istreambuf_iterator`

### 9.3 仅连续内存容器走零拷贝快路径

- `vector`
- `string`
- `array`
- `span`
- 自定义连续缓冲

非连续容器如 `list` / `deque` 不作为高性能目标。

### 9.4 支持大文件分块读写

单次 I/O 请求不应盲目假设无限大，因此后端内部采用 chunk loop。

### 9.5 flush 不默认执行

`flush` / `fsync` 成本高，应该由调用方显式决定。

### 9.6 对齐与无缓冲 I/O

`OpenOptions` 中预留了 `unbuffered` 选项，但后续若真正启用：

- 必须严格约束 buffer 对齐
- 偏移与长度必须满足平台对齐要求
- 上层需要额外的能力检查或专门 API

当前阶段先保留选项，不在高层默认使用。

---

## 10. 命名规范约束

根据当前项目约定，本模块后续代码统一遵守：

1. **函数名**：首字母小写驼峰
   - `readFile`
   - `writeAt`
   - `openRead`

2. **局部变量 / 成员变量**：小写蛇形
   - `native_handle_`
   - `total_size`
   - `chunk_size`

3. **函数参数**：小写蛇形并以 `_` 结尾
   - `path_`
   - `offset_`
   - `destination_`

4. **类型名**：当前保持大驼峰
   - `PlatformFile`
   - `FileError`
   - `OpenOptions`

---

## 11. 上层 API 规划

### 11.1 面向已有区间的 API

```cpp
readInto(path_, span_)
writeFile(path_, span_)
```

语义：

- 不扩容
- 直接读入 / 写出给定区间
- 面向 `std::span<T>`、`std::array<T, N>`、裸内存区域封装

### 11.2 面向可扩容容器的 API

```cpp
readFile(path_, container_)
```

语义：

- 自动获取文件大小
- 自动 resize
- 直接读入目标容器内存

### 11.3 面向单对象的 API

```cpp
readObject(path_, object_)
writeObject(path_, object_)
```

约束：

- `T` 必须是 trivially copyable
- 仅表示读写原始对象字节表示

### 11.4 面向文件句柄的复用 API

```cpp
readAt(file_, offset_, span_)
writeAt(file_, offset_, span_)
```

用于资源系统 / 资源包系统内部重复调用。

---

## 12. traits / concepts 规划

### 12.1 目标

让模块自动识别以下类型：

- `std::span<T>`
- `std::array<T, N>`
- `std::vector<T, Alloc>`
- `std::basic_string<Char, Traits, Alloc>`
- `std::pmr::vector<T>`
- 自定义连续容器
- 原始内存区域包装类型

### 12.2 基本要求

元素类型必须满足：

- `std::is_trivially_copyable_v<T>`

目标范围必须满足：

- 连续内存
- 可取 `data()`
- 可取 `size()`

可扩容目标额外要求：

- 可 `resize()`
- 能按字节数正确转换到元素个数

### 12.3 `bufferTraits<T>` 规划

建议提供：

- `asBytes(const T&)`
- `asWritableBytes(T&)`
- `resizeForBytes(T&, std::size_t)`

允许用户为引擎自定义容器特化。

---

## 13. 目录规划

当前建议目录：

```text
FileOp_New/
├─ CMakeLists.txt
├─ include/
│  └─ Center/
│     └─ File/
│        ├─ Config.hpp
│        ├─ Error.hpp
│        ├─ Types.hpp
│        ├─ PlatformFile.hpp
│        ├─ BufferTraits.hpp          // 计划中
│        ├─ Algorithms.hpp            // 计划中
│        ├─ MappedFile.hpp            // 后续
│        └─ FileOp.hpp
├─ src/
│  ├─ PlatformFileWin32.cpp
│  ├─ PlatformFilePosix.cpp          // 后续
│  └─ MappedFileWin32.cpp            // 后续
└─ docs/
   └─ file_module_design.md
```

---

## 14. 实现阶段规划

### 阶段 1：平台后端层

目标：

- 完成 `PlatformFile`
- Windows 平台可用
- 通过基本语法/编译检查

状态：

- **进行中，已落盘第一版 Windows 实现**

### 阶段 2：traits 与基础算法层

目标：

- 完成 `bufferTraits`
- 支持 span / array / vector / string
- 实现 `readFile` / `readInto` / `writeFile`
- 初步接入 `MemoryCenterNew`

状态：

- **下一阶段优先事项**

### 阶段 3：测试与错误语义完善

目标：

- 单元测试
- 大小边界测试
- 短读/短写行为测试
- 文件创建 / 覆盖 / truncate 测试
- 自定义 allocator 容器测试

状态：

- **待开始**

### 阶段 4：扩展功能

目标：

- 内存映射
- 异步 I/O
- 资源包支持
- 压缩/解压流

状态：

- **远期规划**

---

## 15. 测试规划

至少覆盖以下测试：

1. **基础读写**
   - 写入字节数组，再读回比对

2. **整文件读入容器**
   - `vector<std::byte>`
   - `string`
   - `vector<int>`（大小整除）

3. **区间读写**
   - `span` 子区间
   - `array` 固定区间

4. **错误路径**
   - 文件不存在
   - 权限不足
   - 读取超出 EOF
   - `resize` 失败

5. **大文件 / 分块路径**
   - 验证 chunk loop 正确性

6. **MemoryCenter 集成**
   - `CustomerAllocator<T, Tags::Container>` 容器读写

7. **移动语义 / 句柄生命周期**
   - move construct
   - move assign
   - double close 安全性

---

## 16. 当前实现状态记录

当前已经落地的文件：

- `CMakeLists.txt`
- `include/Center/File/Config.hpp`
- `include/Center/File/Error.hpp`
- `include/Center/File/Types.hpp`
- `include/Center/File/PlatformFile.hpp`
- `include/Center/File/FileOp.hpp`
- `src/PlatformFileWin32.cpp`

当前完成内容：

- `PlatformFile` Windows 原生后端第一版
- 错误类型与 expected 返回模型
- 基础 open/read/write/resize/flush 接口
- 命名规范已对齐当前项目约束

当前验证情况：

- 已通过 `clang++ -std=c++23 -fsyntax-only -Iinclude src/PlatformFileWin32.cpp` 语法检查
- 当前环境未完成完整 CMake 构建，因为缺少默认生成器所需的 `nmake` / MSVC 命令行构建环境

---

## 17. 后续开发约束

后续实现必须遵守：

1. 底层平台后端不偷偷引入堆分配。
2. 上层容器读写必须优先采用零中间缓冲快路径。
3. MemoryCenter 集成要尽量通过容器 allocator 自然接入，而不是手写额外分配分支。
4. traits 设计必须允许自定义容器扩展。
5. 先保证正确性和接口稳定，再扩展 mmap / async。
6. 任何引入额外 staging buffer 的实现，都必须说明触发条件与成本。

---

## 18. 建议的下一步

推荐按照以下顺序继续：

1. 实现 `BufferTraits.hpp`
2. 实现 `Algorithms.hpp`
3. 接入 `MemoryCenterNew`
4. 写基础读写测试
5. 补 POSIX 预留实现

优先级最高的是第 1~3 步，因为这部分才能真正完成“给定任意容器或内存区域，自动识别类型并读入/写出”的目标。
