# 底层平台文件后端性能与安全性测试报告

## 1. 目的

本报告用于验证当前 `PlatformFile` 底层后端在以下方面的表现：

1. **性能**
   - 对比多种底层读取方案
   - 观察包装层相对原生 Win32 API 的额外开销
   - 加入 `mmap` 方案进行对照
   - 设置一个理论上限基线：**把等量数据从一块预分配内存直接 `memcpy` 到另一块预分配内存**

2. **安全性 / 正确性**
   - 验证基本文件读写
   - 验证错误路径
   - 验证 move 语义
   - 验证 EOF 处理
   - 验证 resize / flush / close 行为

---

## 2. 本次新增文件

### 测试与基准源码

- `bench/platform_file_benchmark.cpp`
- `tests/platform_file_safety.cpp`
- `tools/run_platform_file_validation.ps1`

### 相关被测实现

- `include/Center/File/PlatformFile.hpp`
- `src/PlatformFileWin32.cpp`

---

## 3. 运行方式

### 3.1 手动编译

```powershell
clang++ -std=c++23 -O2 -DNDEBUG -Iinclude bench\platform_file_benchmark.cpp src\PlatformFileWin32.cpp -o bench\platform_file_benchmark.exe
clang++ -std=c++23 -O2 -DNDEBUG -Iinclude tests\platform_file_safety.cpp src\PlatformFileWin32.cpp -o tests\platform_file_safety.exe
```

### 3.2 直接运行脚本

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run_platform_file_validation.ps1
```

---

## 4. 性能基准设计

## 4.1 基准前提

- 平台：Windows
- 编译器：`clang++ 22.1.0`
- 优化级别：`-O2 -DNDEBUG`
- 测试文件大小：`256 MiB`
- 每组迭代次数：`6`
- 测试类型：**warm cache**
  - 先进行一次预热读取
  - 主要用于隔离 API 层开销与内存路径差异
  - 不代表机械硬盘/冷缓存/首帧真实加载的绝对值

## 4.2 对照组定义

### A. `idealMemcpyUpperBound`
理论上限，不是文件 I/O。

语义：
- 源数据已经在内存中
- 目标缓冲区已预分配
- 仅做一次完整 `memcpy`

用途：
- 作为“如果完全没有文件系统与页缓存开销，只剩纯内存搬运”时的理想参考值

### B. `rawWin32ReadPrealloc`
原生 Win32 基线。

语义：
- `CreateFileW + SetFilePointerEx + ReadFile`
- 直接读入一块预分配内存

用途：
- 衡量平台 API 直接调用的实际速度
- 作为 `PlatformFile` 包装层的对照基准

### C. `platformFileReadPrealloc`
当前被测底层实现。

语义：
- `PlatformFile::readExactAt(0, destination_)`
- 直接读入一块预分配内存

用途：
- 衡量当前底层封装的性能与额外开销

### D. `ifstreamReadPrealloc`
标准流实现对照。

语义：
- `std::ifstream::read`
- 读入同样大小的预分配内存

用途：
- 作为常规 C++ 文件流方案的对照组

### E. `mappedCopyReuseView`
`mmap` + 拷贝方案。

语义：
- 通过 `CreateFileMappingW + MapViewOfFile` 建立映射视图
- 然后把映射区 `memcpy` 到目标预分配内存

用途：
- 对比“映射后再复制”与“直接 ReadFile”谁更优

### F. `mappedZeroCopyFullScan`
零拷贝映射访问方案。

语义：
- 直接访问映射视图
- 不复制到目标 buffer
- 但完整触碰全部字节并做 checksum

用途：
- 代表“若上层消费者直接使用映射视图而不要求复制”的一种零拷贝思路
- **注意它与‘读入调用方给定内存’不是同一语义，因此只能做参考，不可与 copy-to-buffer 路径简单等价比较**

---

## 5. 性能结果

### 5.1 原始结果

测试输出如下：

```text
=== 底层文件后端性能基准（warm cache）===
payload=artifacts\platform_file_bench_payload.bin
file_size=256 MiB, iterations=6, note=idealMemcpyUpperBound 为理论内存带宽上限；mappedZeroCopyFullScan 为直接访问映射视图并完整触碰全部字节的零拷贝方案。

idealMemcpyUpperBound        avg=0.026268  s throughput=9745.69      MiB/s ideal_ratio=1.000    checksum=0xc3cd81298fb4d541
rawWin32ReadPrealloc         avg=0.076046  s throughput=3366.40      MiB/s ideal_ratio=0.345    checksum=0xf03075f13232293b
platformFileReadPrealloc     avg=0.075684  s throughput=3382.48      MiB/s ideal_ratio=0.347    checksum=0xf03075f13232293b
ifstreamReadPrealloc         avg=0.182854  s throughput=1400.02      MiB/s ideal_ratio=0.144    checksum=0xf03075f13232293b
mappedCopyReuseView          avg=0.048651  s throughput=5261.92      MiB/s ideal_ratio=0.540    checksum=0xf03075f13232293b
mappedZeroCopyFullScan       avg=0.594735  s throughput=430.44       MiB/s ideal_ratio=0.044    checksum=0xe0c73ad706d55168
```

### 5.2 结果表

| 组别 | 平均耗时 | 吞吐量 MiB/s | 相对理想上限 |
|---|---:|---:|---:|
| idealMemcpyUpperBound | 0.026268 s | 9745.69 | 1.000 |
| rawWin32ReadPrealloc | 0.076046 s | 3366.40 | 0.345 |
| platformFileReadPrealloc | 0.075684 s | 3382.48 | 0.347 |
| ifstreamReadPrealloc | 0.182854 s | 1400.02 | 0.144 |
| mappedCopyReuseView | 0.048651 s | 5261.92 | 0.540 |
| mappedZeroCopyFullScan | 0.594735 s | 430.44 | 0.044 |

---

## 6. 性能结论

### 6.1 `PlatformFile` 对比原生 Win32 API

结论：**当前 `PlatformFile` 包装层几乎没有显著额外开销。**

本次结果中：

- `rawWin32ReadPrealloc`：`3366.40 MiB/s`
- `platformFileReadPrealloc`：`3382.48 MiB/s`

两者差异很小，已经落在正常测试波动范围内，可以认为：

- 当前 `PlatformFile` 的封装并没有引入肉眼可见的性能损失
- 说明“expected 错误模型 + RAII 句柄包装 + 小型 helper”这一层的抽象成本是合理的

### 6.2 对比 `ifstream`

结论：**`ifstream` 在本组基准下明显慢于底层 Win32 API 与当前 `PlatformFile`。**

- `ifstreamReadPrealloc` 约为 `1400.02 MiB/s`
- 仅约等于 `PlatformFile` 的 `41%`

这验证了当前设计路线：

- 底层模块不应以 `std::fstream` / iostream 作为核心后端

### 6.3 对比 `mmap + memcpy`

结论：**在 warm cache 条件下，复用映射视图后再复制到目标 buffer，表现优于直接 `ReadFile`。**

- `mappedCopyReuseView`：`5261.92 MiB/s`
- `platformFileReadPrealloc`：`3382.48 MiB/s`

说明：

- 当文件已经进入页缓存，且映射已建立时，`memcpy` from mapped view 可能比重复 `ReadFile` 更快
- 但这并不自动意味着 mmap 应该取代普通文件读取接口，因为：
  1. 生命周期管理更复杂
  2. 页错误行为更隐蔽
  3. 零拷贝访问与“读入调用方给定 buffer”是两种不同语义

### 6.4 对比理论理想上限

- `idealMemcpyUpperBound`：`9745.69 MiB/s`
- `PlatformFile`：约达理想值的 `34.7%`

解释：

- 这很正常，因为理想上限完全没有文件系统路径、系统调用、页缓存协调等成本
- 该组数据不应当被当作“必须达到”的目标，而是用来估计“当前实现距离纯内存带宽还有多少差距”

### 6.5 关于 `mappedZeroCopyFullScan`

该组结果：`430.44 MiB/s`

这个结果**不能简单地说 mmap 很慢**，因为这里测的是：

- 不复制
- 但完整逐字节触碰映射视图并计算 checksum

它更接近“解析器直接在线扫描映射文件”的 CPU + 内存访问混合场景，而不是“拷贝到目标 buffer”的纯加载场景。

因此：

- `mappedZeroCopyFullScan` 的意义是**给零拷贝解析路径提供一个参考下界**
- 它不是 copy-based file loading 的直接竞品结论

---

## 7. 安全性测试设计

本次安全性测试使用 `tests/platform_file_safety.cpp`，覆盖以下内容：

1. 打开不存在文件
2. round-trip 写入再读取
3. `readExactAt` 超过 EOF
4. `resize` 扩大与缩小文件
5. move construct / move assign 语义
6. 空读 / 空写
7. 已关闭句柄上的错误返回

---

## 8. 安全性测试结果

### 8.1 运行输出

```text
[PASS] openRead 缺失文件应失败
[PASS] 缺失文件错误操作类型应为 open
[PASS] openWrite 应成功创建 roundtrip 文件
[PASS] writeExactAt 应完整写入 roundtrip 文件
[PASS] flush 应成功
[PASS] close 应成功
[PASS] openRead 应成功打开 roundtrip 文件
[PASS] size 应成功返回文件大小
[PASS] size 应与写入大小一致
[PASS] readExactAt 应完整读取 roundtrip 文件
[PASS] 读取内容应与写入内容完全一致
[PASS] 生成 short_read 测试文件应成功
[PASS] openRead short_read 文件应成功
[PASS] readExactAt 超过 EOF 时应失败
[PASS] 超过 EOF 时应标记 end_of_file
[PASS] 超过 EOF 时 processed 应等于实际可读字节数
[PASS] openReadWrite createAlways 应成功
[PASS] resize 扩大文件应成功
[PASS] 扩大后 size 应成功
[PASS] 扩大后文件大小应为 8192
[PASS] resize 缩小文件应成功
[PASS] 缩小后 size 应成功
[PASS] 缩小后文件大小应为 512
[PASS] 生成 move_semantics 文件应成功
[PASS] openRead move_semantics 文件应成功
[PASS] move 前 file_a 应保持打开
[PASS] move construct 后 file_a 应为空
[PASS] move construct 后 file_b 应打开
[PASS] move construct 后 file_b 应可读
[PASS] move construct 后读取结果应正确
[PASS] move assign 后 file_b 应为空
[PASS] move assign 后 file_c 应打开
[PASS] openWrite empty 文件应成功
[PASS] 空写入应成功
[PASS] 空写入返回值应为 0
[PASS] empty 文件 close 应成功
[PASS] openRead empty 文件应成功
[PASS] 空读取应成功
[PASS] 空读取返回值应为 0
[PASS] 生成 closed_handle 文件应成功
[PASS] openRead closed_handle 文件应成功
[PASS] 显式 close 应成功
[PASS] 已关闭句柄的 readExactAt 应失败
[PASS] 已关闭句柄 read 错误类型应为 read

=== 安全性测试汇总 ===
passed=44, failed=0
```

### 8.2 结论

当前已覆盖的安全性与正确性结论：

- 基础打开/关闭/读/写/flush 正常
- round-trip 数据完整性正常
- EOF 错误路径正常
- `processed` / `end_of_file` 错误上下文正常
- `resize` 行为正常
- move 语义正常
- 空区间读写正常
- 已关闭句柄上的错误返回正常

---

## 9. 当前结论总结

### 9.1 性能方面

当前底层 `PlatformFile`：

- 与原生 Win32 API 几乎同级
- 明显优于 `ifstream`
- 与 `mmap` 相比，各有语义与场景边界

可得出实际结论：

1. **继续沿用当前 `PlatformFile` 作为同步文件 I/O 基础层是合理的**
2. **上层“读入给定容器/内存区间”完全可以建立在这层之上，而不会引入明显额外开销**
3. **mmap 应作为后续扩展能力，而不是立即替换现有底层后端**

### 9.2 安全性方面

当前底层实现至少在本轮验证中满足：

- 正常路径正确
- 常见错误路径正确
- 生命周期与 move 语义正确

因此可以继续向上实现：

- `bufferTraits`
- `readFile`
- `readInto`
- `writeFile`
- `readObject`
- `writeObject`

---

## 10. 仍未覆盖的风险点

本次测试还没有覆盖以下内容，建议后续补齐：

1. **超大文件（> 4 GiB）**
   - 当前后端实现内部有 chunk loop，但还未做真实超大文件回归

2. **并发读取**
   - 多线程同时调用 `readAt` 的压力测试尚未完成

3. **无缓冲 I/O (`FILE_FLAG_NO_BUFFERING`)**
   - 当前仅预留选项，尚未建立对齐约束测试

4. **共享模式与文件竞争**
   - `FileShare` 的更多组合场景尚未系统测试

5. **冷缓存场景**
   - 本次性能数据主要反映 warm cache 条件
   - 若要模拟首次启动/首次加载，需额外设计冷缓存方案

6. **`mmap` 真正资源访问模型**
   - 若未来引擎要直接消费映射内存，而不是 copy-out，需要按资源解析实际访问模式重做基准

---

## 11. 建议的下一步

基于本次结果，建议开发顺序如下：

1. 继续实现 `BufferTraits.hpp`
2. 实现 `Algorithms.hpp`
3. 接入 `MemoryCenterNew`
4. 保留当前 `PlatformFile` 作为默认同步 I/O 后端
5. 之后再单独实现 `MappedFile`，并针对真实资源解析路径做第二轮专项基准

