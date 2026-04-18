# FileOp_New 流式读取 + Triple Buffer 规划方案（V1.2）

> 日期：2026-04-17  
> 目标：开始 S1 实装前的最终规划冻结版本。  
> 重点修正：semaphore 上限 UB、I/O 错误传播、PMR 64B 对齐。

---

## 1. 核心结论

1. 并发模型继续使用 `std::counting_semaphore<3>`，不采用 `atomic::wait/notify`（本阶段暂不接纳）。
2. **禁止盲目 release(3)**：避免 `counting_semaphore` 计数超过 `max()` 导致 UB。
3. 消费者接口必须可区分“无数据 / EOF / 错误”，采用 `std::expected` 语义。
4. Triple Buffer 内存通过 `std::pmr::memory_resource::allocate(size, alignment)`，强制 64 字节对齐。

---

## 2. 并发与停止流程（最终约束）

## 2.1 semaphore 上限约束（必须）

- `std::counting_semaphore<3>` 的内部计数上限是 3。
- 任意 `release()` 导致计数 > 3，属于 UB。

因此 stop 唤醒策略改为：

1. `stop_requested_ = true`
2. 对 `empty` 与 `filled` **各尝试 release(1)**（仅在内部计数 < 3 时）
3. 被唤醒线程第一时间检查 `stop_requested_` 并退出

> 绝不执行“保守 release 多次”策略。

## 2.2 双向阻塞唤醒

- 生产者可能阻塞在 empty
- 消费者可能阻塞在 filled

`requestStopWakeup()` 同时尝试唤醒两端，确保停机路径可达。

---

## 3. API 与错误传播（最终约束）

消费者读取接口采用 expected：

```cpp
auto getCurrent() -> std::expected<const BufferView*, FileError>;
auto switchToNext() -> std::expected<const BufferView*, FileError>;
```

语义：

1. `expected.has_value() == false`：发生错误（含 I/O 失败、取消）
2. `value() == nullptr`：当前无可读块（非阻塞接口）
3. `value()->end_of_stream == true`：到达 EOF 块

生产者出现不可恢复 I/O 错误时：

1. `publishError(file_error)`
2. `requestStopWakeup()` 唤醒阻塞消费者
3. 消费者在下一次 `getCurrent/switchToNext` 收到 error

---

## 4. EOF 与短块完整性

`BufferView` 必须携带真实长度（actual size）：

```cpp
struct BufferView {
    std::span<const std::byte> bytes;  // bytes.size() == actual_size
    std::uint32_t index;
    std::uint64_t file_offset;
    bool end_of_stream;
};
```

消费者严禁按固定 `chunk_bytes` 读取，必须按 `bytes.size()` 消费最后短块。

---

## 5. PMR 与缓存友好布局

使用 PMR 时必须显式对齐：

```cpp
void* ptr = memory_resource_->allocate(total_bytes, 64);
```

并保持：

1. 三个 chunk 连续布局
2. 状态原子分 cache line 对齐（`alignas(64)`）
3. 初始化后运行期零分配

---

## 6. Pause/Resume 语义

1. `pause()` 仅作用于生产者读取推进
2. 消费者暂停不需要额外状态：不 `switchToNext()` 即产生自然背压
3. `resume()` 唤醒生产者条件变量

---

## 7. S1 实装范围（冻结）

1. `StreamTypes.hpp`
2. `TripleReadBuffer.hpp`
3. `StreamReadSession.hpp`
4. 单测：
   - `triple_read_buffer_tests.cpp`
   - `stream_read_session_tests.cpp`

---

## 8. 验收标准

### 8.1 正确性

1. EOF 短块正确
2. I/O 错误可传递到消费者
3. stop 可中断阻塞路径
4. 无 semaphore 上限溢出风险

### 8.2 性能

1. 无 busy-wait
2. 初始化后无额外分配
3. 单流吞吐接近顺序直读

---

## 9. 最终说明

V1.2 方案已对关键工程风险做闭环，满足进入编码阶段条件。  
后续若要评估 `atomic::wait/notify`，应在 S2/S3 通过独立 benchmark 对比后再决定切换。
