# ThreadCenter 接入（Phase 2）报告

## 1. 本阶段目标

在 Phase 1（规划器 + 适配层骨架）基础上，补齐三项能力：

1. read-only 句柄复用（降低每任务 open/close 成本）
2. `mappedCopy` / `mappedView` 执行分支（不止计划标记）
3. 并发压测与正确性回归

---

## 2. 代码变更

### 新增文件

- `include/Center/File/ReadHandlePool.hpp`
- `bench/file_read_scheduler_bench.cpp`
- `tests/file_read_scheduler_tests.cpp`

### 主要更新

- `include/Center/File/FileReadScheduler.hpp`
  - 接入 `ReadHandlePool`
  - `executeTask` 分发三种读取方式：
    - `directRead`
    - `mappedCopy`
    - `mappedView`（当前实现为 mapped 路径）
  - ThreadCenter 不可用时自动回退到顺序执行（不再直接失败）
- `include/Center/File/FileOp.hpp`
  - 聚合导出新增头
- `CMakeLists.txt`
  - 新增 `file_read_scheduler_bench`
  - 新增 `file_read_scheduler_tests`

---

## 3. 执行语义说明

## 3.1 directRead

- 使用 `ReadHandlePool`（当前线程本地）获取只读句柄
- 复用同路径句柄，避免重复 `openRead`
- 调用 `PlatformFile::readExactAt` 执行

## 3.2 mappedCopy

- Windows 路径使用：
  - `CreateFileW`
  - `CreateFileMappingW`
  - `MapViewOfFile`
  - `memcpy`
- 支持任意 offset：按 allocation granularity 对齐后映射

## 3.3 mappedView

- 当前阶段与 mapped 分支同源执行（兼容目标 buffer 语义）
- 后续会在“调用方接受 view 生命周期管理”的前提下升级为真正零拷贝输出接口

---

## 4. 回退机制

当 `ThreadCenterAdapter` 返回 `function_not_supported`（例如未启用 ThreadCenter）时：

- `FileReadScheduler::runPlan` 自动转为顺序执行
- 不影响底层模块可用性

---

## 5. 测试结果

## 5.1 新增 scheduler 测试

可执行：`tests/file_read_scheduler_tests.exe`

结果：

- `passed=7`
- `failed=0`

覆盖点：

1. 规划器 split + lane 行为
2. scheduler direct 读取正确性
3. scheduler mapped_copy 读取正确性

## 5.2 既有底层安全回归

可执行：`tests/platform_file_safety.exe`

结果：

- `passed=44`
- `failed=0`

说明：Phase 2 改造未破坏底层读写行为与错误语义。

---

## 6. 并发基准结果

可执行：`bench/file_read_scheduler_bench.exe`

场景参数：

- 文件数：24
- 单文件：4 MiB
- 总量：96 MiB
- 迭代：5

结果：

```text
sequentialDirectRead         avg=0.028972s throughput=3313.58 MiB/s
schedulerThreadCenterDirect  avg=0.008855s throughput=10841.43 MiB/s
schedulerThreadCenterMapped  avg=0.019634s throughput=4889.43 MiB/s
```

解读：

- 当前场景下 ThreadCenter 并发 direct 读取显著快于顺序读取。
- mapped 分支目前用于“映射+复制”路径，性能取决于场景与缓存状态；在该组数据里低于 direct 并发路径。

---

## 7. 已知限制

1. 句柄池当前为线程本地 map，未做全局上限与淘汰策略。
2. mappedView 还未提供“持久化 view 对象”给调用方。
3. 未实现跨请求区间合并（planner 目前仅按单请求 split）。
4. 还未加入 deadline/timeout/抢占调度。

---

## 8. 下一步（Phase 3 建议）

1. 增加 `ReadView` 类型与生命周期管理，完成真正 mappedView 零拷贝路径。
2. 增加跨请求区间合并（同路径邻近区间 merge）。
3. 句柄池增加上限和 LRU 淘汰。
4. 加入 deadline 驱动的 lane 抢占策略。
5. 增加冷缓存与混合负载基准（大文件 + 小文件 + 紧急请求）。
