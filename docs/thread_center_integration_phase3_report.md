# ThreadCenter 接入（Phase 3）报告

## 1. 本阶段目标

在 Phase 2 基础上推进三项能力：

1. 真正可返回视图对象的 `mappedView` 读取路径
2. 同路径区间合并（跨请求 merge）
3. lane 抢占语义强化（urgent > normal > background）

---

## 2. 主要代码改动

### 2.1 调度类型扩展

文件：`include/Center/File/SchedulingTypes.hpp`

新增与调整：

- `ReadRequest.deadline_ticks`
- `ReadSegment`
- `PlannedReadTask.segments`
- `StorageProfile.enable_range_merge`
- `StorageProfile.merge_gap_bytes`
- `StorageProfile.merge_max_bytes`

意义：

- planner 可按 segment 组织“合并读块 + 回填目标区间”
- 为 deadline 驱动排序预留字段

### 2.2 规划器增强

文件：`include/Center/File/FileReadPlanner.hpp`

新增能力：

- 请求合法性判定放宽：允许 `mapped_view + destination=nullptr`
- 任务排序增加 `deadline_ticks` 优先级
- 同路径/同 lane/同 method 的邻近区间合并
- 合并后输出单个 `PlannedReadTask`，内部携带多个 `segments`

### 2.3 ThreadCenter 执行优先级强化

文件：`include/Center/File/ThreadCenterAdapter.hpp`

新增能力：

- lane 分阶段依赖：
  - urgent 先执行
  - normal 依赖 urgent gate
  - background 依赖 normal gate（若无 normal 则依赖 urgent）
- 错误短路：一旦出现错误，后续任务体快速返回

### 2.4 调度器增强

文件：`include/Center/File/FileReadScheduler.hpp`

新增能力：

1. `ReadView` 结构
2. `runViewRequests(requests_, out_views_)`
3. `mappedView` 真正返回 view（带 owner 生命周期）
4. `directRead` 支持 merged task（先读 merged buffer，再按 segment 回填）
5. `mappedCopy` 支持 merged segment 回填
6. ThreadCenter 不可用时保持顺序 fallback

### 2.5 入口导出

文件：`include/Center/File/FileOp.hpp`

新增导出：

- `ReadHandlePool.hpp`
- 新的 scheduler/view 能力

---

## 3. 测试与基准

## 3.1 新增/更新测试

文件：`tests/file_read_scheduler_tests.cpp`

当前覆盖：

1. split 行为验证
2. range merge 行为验证
3. scheduler direct 正确性
4. scheduler mapped_copy 正确性
5. scheduler mapped_view 正确性（view 数据一致）

运行结果：

- `passed=13`
- `failed=0`

## 3.2 回归测试

文件：`tests/platform_file_safety.cpp`

运行结果：

- `passed=44`
- `failed=0`

说明：Phase 3 未破坏底层后端契约。

## 3.3 并发基准

文件：`bench/file_read_scheduler_bench.cpp`

样本结果（24×4MiB，总96MiB，5轮）：

```text
sequentialDirectRead         throughput=3502.82 MiB/s
schedulerThreadCenterDirect  throughput=4269.87 MiB/s
schedulerThreadCenterMapped  throughput=5427.59 MiB/s
```

说明：该场景下并发调度与 mapped 路径均有收益，具体比值会随缓存与文件布局波动。

---

## 4. 当前能力边界

已实现：

- 基于紧急度 + lane 的自动分配
- split + merge 组合规划
- direct/mappedCopy/mappedView 三路径执行
- view 生命周期可安全托管

未完成：

1. handle pool 上限与 LRU 淘汰
2. deadline 驱动的动态抢占（当前仅排序优先）
3. 跨请求 merge 的成本模型（目前为规则型启发式）
4. 冷缓存与混合负载长期基准矩阵

---

## 5. Phase 4 建议

1. 为 `ReadHandlePool` 增加线程内上限与淘汰策略
2. 将 `deadline_ticks` 与 runtime collector 联动，做动态抢占
3. 引入“merge 收益估计器”，按文件大小/请求密度自适应启停 merge
4. 给 `runViewRequests` 增加按 group/request 的回调投递接口
