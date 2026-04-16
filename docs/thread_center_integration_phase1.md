# ThreadCenter 接入（第一阶段）

## 已完成

1. 新增读取规划类型：`include/Center/File/SchedulingTypes.hpp`
2. 新增启发式规划器：`include/Center/File/FileReadPlanner.hpp`
3. 新增 ThreadCenter 适配层：`include/Center/File/ThreadCenterAdapter.hpp`
4. 新增调度器封装：`include/Center/File/FileReadScheduler.hpp`
5. 聚合入口更新：`include/Center/File/FileOp.hpp`
6. CMake 增加可选接入开关并自动探测 `../ThreadCenter`

## 设计边界

- `PlatformFile` 仍只负责底层原语，不内置调度策略。
- `FileReadPlanner` 负责按紧急度/体量生成任务计划。
- `ThreadCenterAdapter` 负责把计划下发到 `ThreadCenter::Center` 执行。
- ThreadCenter 不可用时，适配器返回 `function_not_supported`，不影响底层模块编译。

## 当前限制

- 当前执行器默认每个任务单独 `openRead`，后续会补 `read-only handle pool`。
- 目前先支持 `directRead` 执行路径，`mappedCopy/mappedView` 仅在计划阶段标记。
- 还未实现抢占、超时和跨请求区间合并。

## 下一步

1. 增加句柄池与路径级复用。
2. 实现 `mappedCopy/mappedView` 实际执行分支。
3. 增加并发正确性与吞吐压测（多文件/同文件分块）。
