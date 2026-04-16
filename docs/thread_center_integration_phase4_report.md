# ThreadCenter 接入（Phase 4）报告

## 1. 本阶段目标

围绕 Phase 3 之后的可用性与调度细化，落实以下项：

1. 句柄池容量约束 + LRU 淘汰
2. deadline 预抢占（deadline 近时提升到 urgent）
3. view 回调分发接口
4. 保持回归通过

---

## 2. 主要改动

### 2.1 ReadHandlePool：容量与 LRU

文件：`include/Center/File/ReadHandlePool.hpp`

新增：

- 构造参数 `capacity_`
- 线程本地状态 `entries + lru + capacity`
- 访问命中时 `splice` 到 LRU 头部
- 插入前按容量淘汰尾部路径
- `sizeCurrentThread()` 观测接口

效果：

- 防止线程本地句柄无限增长
- 高频路径保持热句柄在前

### 2.2 PlannerConfig 扩展

文件：`include/Center/File/SchedulingTypes.hpp`

新增：

- `PlannerConfig.handle_pool_capacity`
- `PlannerConfig.deadline_preempt_window_ticks`

用途：

- 控制句柄池上限
- 控制 deadline 提升窗口

### 2.3 FileReadPlanner：deadline 预抢占

文件：`include/Center/File/FileReadPlanner.hpp`

新增：

- `makePlan(requests_, now_ticks_)` 重载
- `isDeadlineUrgent(...)`
- `selectLane(...)` 支持 deadline 触发 urgent lane

策略：

- 当 `deadline_preempt_window_ticks > 0` 且
  - `deadline_ticks <= now_ticks_` 或
  - `(deadline_ticks - now_ticks_) <= window`
  时，直接归入 urgent。

### 2.4 FileReadScheduler：view 回调

文件：`include/Center/File/FileReadScheduler.hpp`

新增：

- `using ReadViewCallback = std::function<void(const ReadView&)>`
- `runViewRequests(requests_, out_views_, view_callback_)`

行为：

- 每个生成的 `ReadView` 会在入 `out_views_` 后触发 callback
- callback 与 out_views 保持同一条数据语义来源

同时：

- `makePlan/runRequests/runViewRequests` 使用当前 steady ticks 调 planner
- `ReadHandlePool` 按 `planner_config.handle_pool_capacity` 初始化

---

## 3. 测试覆盖

文件：`tests/file_read_scheduler_tests.cpp`

新增验证：

1. deadline 提升 urgent
2. mapped view callback 触发次数

当前结果：

- `passed=15`
- `failed=0`

---

## 4. 回归与基准

## 4.1 底层回归

`tests/platform_file_safety.exe`

结果：

- `passed=44`
- `failed=0`

## 4.2 并发基准（样本）

`bench/file_read_scheduler_bench.exe`

场景：24 × 4MiB，5 轮。

结果：

```text
sequentialDirectRead         throughput=3691.98 MiB/s
schedulerThreadCenterDirect  throughput=3912.60 MiB/s
schedulerThreadCenterMapped  throughput=5236.71 MiB/s
```

---

## 5. 当前状态总结

Phase 4 后，调度层具备：

- 规则化规划（split/merge/lane）
- ThreadCenter 执行与 lane 顺序约束
- mappedCopy / mappedView 执行
- view 生命周期 + 回调分发
- 线程本地句柄池容量约束与 LRU
- deadline 预抢占（静态窗口）

---

## 6. 后续建议（Phase 5）

1. 动态 deadline 抢占：结合 runtime 进度与队列长度做实时重排。
2. 句柄池 metrics：导出命中率/淘汰计数用于调参。
3. merge 成本模型：按文件系统特性和最近吞吐动态调节 `merge_gap/merge_max`。
4. view 路径对接资源系统：增加 `group_id/request_id` 回调派发器与错误聚合策略。
