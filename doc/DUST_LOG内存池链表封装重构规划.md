# DUST_LOG 内存池链表封装重构规划

> 目标文件：`framework/cmd/shell/log.hpp` / `framework/cmd/shell/log.cpp` / `framework/cmd/shell/shell.cpp`
> 目标：把 `Log` 里散落的静态池、空闲链、发送队列、原始请求队列收束到私有管理结构里，让 `Log` 只保留日志语义和对外接口。
> 日期：2026-08-24

## 1. 结论

建议做一次**结构收束型重构**：不改成动态堆，不引入 STL 容器，不换二叉堆；保留“静态数组 + `uint8_t next` 索引链表”的嵌入式模型，但把链表细节从 `Log` 中抽出来。

推荐拆成两个 `Log` 私有结构：

- `TxFrameQueue`：管理 `TxFrame` 帧池、空闲链、发送队列、优先级插入、池满挤出、DBG stale 标记。
- `LogRecordQueue`：管理 `LogRecord` 原始请求池、空闲链、FIFO 队列、出队所有权、记录 stale 标记。

`Log` 最终只“包含”两个私有成员：

```cpp
static inline TxFrameQueue   txq_ {};
static inline LogRecordQueue recq_ {};
```

这样 `Log` 的职责变成：

- 对外提供 `DUST_LOG_*` 的入口；
- 决定同步段/异步段；
- 负责 `irq_lock()` 并发边界；
- 负责格式化和调用 `Stream::Send()`；
- 不再直接维护 `tx_pool_ / free_head_ / tx_head_ / tx_tail_ / rec_pool_ / rec_head_ / rec_free_` 这些链表字段。

## 2. 为什么要封装

当前 `Log` 同时承担了四类职责：

1. 日志 API：`Inf/Err/Ok/Wrn/Dbgl/SendLine/Process`。
2. 格式化：`PrintColor/SnapshotArgs/FormatRecord`。
3. 发送仲裁：`EnqueueByPrio/EvictLowest/Dequeue/MarkStaleDbg/EnqueueFrame`。
4. 内存池生命周期：`AllocFrame/RecycleFrame`、`rec_pool_` 空闲链重建与回收。

这让 `log.hpp` 私有区很难读：`Log` 看起来像日志类，实际里面夹着两个不同队列的固定块分配器。封装后，读代码时可以一眼分清：

- `TxFrameQueue` 管“已经格式化、准备发送的帧”；
- `LogRecordQueue` 管“还没格式化、等待 shell 线程展开的请求”；
- `Log` 管“什么时候入哪个队列、什么时候格式化、什么时候唤醒 shell”。

## 3. 当前必须顺手修正的队列边界

这次封装不是纯美化。当前代码里有两个边界问题适合在封装时一起处理。

### 3.1 `LogRecord` 出队后立即回收到空闲链

当前 `DequeueRecord()` 逻辑是：

1. 从 `rec_head_` 摘下一个记录；
2. 立刻把该记录挂回 `rec_free_`；
3. 返回 `&rec_pool_[i]` 给 shell 线程 `FormatRecord(r)`。

问题是：返回后记录已经重新进入空闲链，ISR/任务如果马上再调用 `TryEnqueueRecord()`，可能拿到同一块 `LogRecord` 并覆盖它。shell 线程随后继续格式化这个指针，就可能读到被覆盖的数据。

封装后要改成明确所有权：

```cpp
LogRecord* r = recq_.PopLocked();     // 从队列摘下，但不回收到 free
// 解锁后 FormatRecord(r)
recq_.ReleaseLocked(r);               // 格式化完成后才回收到 free
```

这比“把代码挪进结构体”更重要，是 `LogRecordQueue` 应该提供的核心语义。

### 3.2 `LogRecord` 没有保存 `TxPriority`

当前异步段 `Dbgl()` 进入 `LogRecord` 后，只保存 `fmt/color/args/nargs`；最后 `FormatRecord()` 统一：

```cpp
PublishQueued(out, n, TxPriority::Event);
```

这会让 DBG 流式日志在异步格式化后也按 `Event` 档进入发送队列，破坏“事件 > 命令响应 > DBG”的设计。

封装时建议给 `LogRecord` 增加：

```cpp
TxPriority prio;
bool       is_stale;
```

然后：

- `Dbgl()` 入队时传 `TxPriority::Dbg`；
- `Inf/Err/Ok/Wrn` 入队时传 `TxPriority::Event`；
- `FormatRecord()` 使用 `r->prio` 调 `PublishQueued()`；
- `Deselect()/Select()` 调用 `recq_.MarkStaleDbgLocked()`，让还没格式化的 DBG 记录也能作废。

## 4. 目标结构

### 4.1 `TxFrameQueue`

放在 `Log` 的 `private:` 内，管理发送帧池。

```cpp
struct TxFrameQueue
{
    TxFrame frames[kTxPoolCount] {};
    uint8_t free_head = kNullIndex;
    uint8_t head      = kNullIndex;
    uint8_t tail      = kNullIndex;

    void Reset();
    bool PushLocked(const char* data, int len, TxPriority prio, bool allow_evict);
    TxFrame* PopLocked();
    void ReleaseLocked(TxFrame* f);
    void MarkStaleDbgLocked();

private:
    TxFrame* AllocLocked();
    TxFrame* EvictLowestLocked();
    void PushByPrioLocked(TxFrame* f);
    uint8_t IndexOf(const TxFrame* f) const;
};
```

命名里的 `Locked` 表示：这些方法只管链表，不自己 `irq_lock()`；调用方 `Log` 负责锁边界。这样不会把并发策略藏在数据结构里，后续查 ISR 路径也清楚。

原函数映射：

| 现在 | 重构后 |
|---|---|
| `tx_pool_` | `txq_.frames` |
| `free_head_` | `txq_.free_head` |
| `tx_head_` / `tx_tail_` | `txq_.head` / `txq_.tail` |
| `AllocFrame()` | `txq_.AllocLocked()` |
| `RecycleFrame()` | `txq_.ReleaseLocked()` |
| `EnqueueByPrio()` | `txq_.PushByPrioLocked()` |
| `EvictLowest()` | `txq_.EvictLowestLocked()` |
| `Dequeue()` | `txq_.PopLocked()` |
| `MarkStaleDbg()` | `txq_.MarkStaleDbgLocked()` |
| `EnqueueFrame()` | `txq_.PushLocked()` |

### 4.2 `LogRecordQueue`

放在 `Log` 的 `private:` 内，管理未格式化日志请求。

```cpp
struct LogRecordQueue
{
    LogRecord records[kMaxLogRecords] {};
    uint8_t free_head = kNullIndex;
    uint8_t head      = kNullIndex;
    uint8_t tail      = kNullIndex;

    void Reset();
    bool PushLocked(const char* fmt, LogColor color, TxPriority prio,
                    const uint32_t* args, uint8_t nargs);
    LogRecord* PopLocked();
    void ReleaseLocked(LogRecord* r);
    void MarkStaleDbgLocked();

private:
    uint8_t IndexOf(const LogRecord* r) const;
};
```

这里新增 `tail`，让原始请求 FIFO 入队从“锁内扫描到尾部”变成 O(1) 尾插。深度只有 8，不是性能瓶颈，但封装后顺手补上尾指针能让语义更干净。

原函数映射：

| 现在 | 重构后 |
|---|---|
| `rec_pool_` | `recq_.records` |
| `rec_head_` | `recq_.head` |
| `rec_free_` | `recq_.free_head` |
| 无 `rec_tail_` | `recq_.tail` |
| `TryEnqueueRecord()` 的链表部分 | `recq_.PushLocked()` |
| `DequeueRecord()` 的链表部分 | `recq_.PopLocked()` + `recq_.ReleaseLocked()` |

## 5. `Log` 对外接口调整

当前 `shell.cpp` 直接知道 `LogRecord`：

```cpp
while (auto* r = Log::DequeueRecord())
{
    Log::FormatRecord(r);
    Log::PumpSend();
}
```

封装后建议不要再把 `LogRecord*` 暴露给 `Shell`。改成：

```cpp
while (Log::ProcessOneRecord())
{
    Log::PumpSend();
}
```

新增 `Log::ProcessOneRecord()`：

```cpp
static bool ProcessOneRecord();
```

职责：

1. 加锁，从 `recq_` 弹出一个记录；
2. 解锁，调用私有 `FormatRecord(const LogRecord& r)`；
3. 再加锁，把记录释放回 `recq_` 空闲链；
4. 返回是否处理了一条记录。

这样 `Shell` 不再关心记录池内部结构，`LogRecord` 可以完全变成 `Log` 私有实现细节。

建议调整：

| 现在 public | 重构后 |
|---|---|
| `static LogRecord* DequeueRecord();` | 删除或改为 private |
| `static void FormatRecord(LogRecord* r);` | 改为 private `FormatRecord(const LogRecord& r)` |
| 无 | 新增 public `static bool ProcessOneRecord();` |

## 6. 分阶段实施规划

### 阶段 1：抽 `TxFrameQueue`，保持发送行为不变

目标：先把发送帧池和发送链表从 `Log` 私有字段中搬进 `TxFrameQueue`。

改动：

1. 在 `Log::private` 内新增 `TxFrameQueue`。
2. 把 `tx_pool_ / free_head_ / tx_head_ / tx_tail_` 搬进 `TxFrameQueue`。
3. 把 `AllocFrame / RecycleFrame / EnqueueByPrio / EvictLowest / Dequeue / EnqueueFrame` 的链表逻辑搬进 `TxFrameQueue`。
4. `Log::Init()` 改为 `txq_.Reset()`。
5. `PumpSend()` / `PublishDirect()` / `PublishQueued()` / `SendFrame()` 改为调用 `txq_`。
6. 保持 `irq_lock()` 位置不变：仍由 `Log` 外层方法负责加锁。

验收：

- `TxFrameQueue` 外没有 `tx_pool_ / free_head_ / tx_head_ / tx_tail_`。
- `Log` 外层发送流程仍是 `Push -> Pop -> SendFrame -> Release`。
- `TxPriority` 语义不变：Event 在前，Cmd 居中，Dbg 最后，同档 FIFO。

### 阶段 2：抽 `LogRecordQueue`，修正记录所有权

目标：把原始请求队列从 `Log` 中搬出来，并修掉“出队后立刻回收到空闲链”的所有权问题。

改动：

1. 在 `Log::private` 内新增 `LogRecordQueue`。
2. 把 `rec_pool_ / rec_head_ / rec_free_` 搬进 `LogRecordQueue`，新增 `tail`。
3. `TryEnqueueRecord()` 改为外层加锁后调用 `recq_.PushLocked(...)`。
4. 新增 `Log::ProcessOneRecord()`。
5. `ProcessOneRecord()` 用 `PopLocked()` 拿到记录，但不立即释放；`FormatRecord()` 完成后再 `ReleaseLocked()`。
6. `shell.cpp` 的记录循环从 `DequeueRecord()+FormatRecord()` 改成 `ProcessOneRecord()`。

验收：

- `Shell` 不再直接使用 `LogRecord*`。
- `LogRecord` 不会在格式化前回到空闲链。
- 原始请求 FIFO 仍保持先来先格式化。

### 阶段 3：补齐异步记录优先级和 stale 语义

目标：让异步 DBG 仍然是 DBG 档，而不是被统一转成 Event 档。

改动：

1. `LogRecord` 增加 `TxPriority prio`。
2. `LogRecord` 增加 `bool is_stale`。
3. `TryEnqueueRecord()` 签名改为：

```cpp
static void TryEnqueueRecord(const char* fmt, LogColor color, TxPriority prio,
                             const uint32_t* args, uint8_t nargs);
```

4. `Dbgl()` 调用：

```cpp
TryEnqueueRecord(fmt, LogColor::White, TxPriority::Dbg, args, nargs);
```

5. `PrintColor()` 异步段调用：

```cpp
TryEnqueueRecord(fmt, c, TxPriority::Event, args, nargs);
```

6. `FormatRecord()` 最后：

```cpp
PublishQueued(out, n, r.prio);
```

7. `MarkStaleDbg()` 改成同时处理：

```cpp
txq_.MarkStaleDbgLocked();
recq_.MarkStaleDbgLocked();
```

验收：

- `DUST_LOG_DBG` 通过异步段后仍以 `TxPriority::Dbg` 入发送队列。
- `log off` / `log on 新名字` 后，已排队但未格式化的 DBG 记录不会继续变成事件日志输出。
- `DUST_LOG_INF/ERR/OK/WRN` 仍保持 `Event` 档。

### 阶段 4：清理头文件私有区和注释

目标：让 `log.hpp` 的 `private:` 区域读起来像“两个队列 + 少量 Log 状态”，不再是一大坨散链表函数。

改动：

1. 删除 `Log` 直接暴露的 `AllocFrame/ReycleFrame/EnqueueByPrio/EvictLowest/Dequeue` 声明。
2. `private` 区只保留：

```cpp
static inline LogEntry*      active_ = nullptr;
static inline uint8_t        count_ = 0;
static inline Stream*        stream_ = nullptr;
static inline bool           sending_ = false;
static inline bool           shell_own_ = false;
static inline TxFrameQueue   txq_ {};
static inline LogRecordQueue recq_ {};
```

3. 更新注释：`Log` 负责日志生命周期，`TxFrameQueue` / `LogRecordQueue` 负责链表生命周期。
4. 同步 `framework/cmd/shell/README.md` 里队列结构说明，尤其 `kTxPoolCount = 8` 的容量表。

验收：

- 头文件私有字段数量明显下降。
- 链表操作集中在两个私有结构里。
- README 不再写旧的 `kTxPoolCount = 4`。

## 7. 建议的最终代码形态

`Log` 最终大致长这样：

```cpp
class Log
{
public:
    static bool Init();
    static void BindStream(Stream* s) { stream_ = s; }
    static LogEntry* FindOrCreate(const char* name);
    static bool Select(const char* name);
    static void Deselect();

    static void Dbgl(LogEntry* e, const char* fmt, ...);
    static void Inf(const char* fmt, ...);
    static void Err(const char* fmt, ...);
    static void Ok(const char* fmt, ...);
    static void Wrn(const char* fmt, ...);

    static void SendLine(const char* text);
    static void PumpSend();
    static void SetShellOwn() { shell_own_ = true; }
    static bool ProcessOneRecord();
    static void Process(uint8_t* line);
    static void CmdLogList();
    static void OnTxDone();

private:
    struct TxFrameQueue;
    struct LogRecordQueue;

    static void FormatRecord(const LogRecord& r);
    static void PrintColor(LogColor c, const char* fmt, va_list ap);
    static void SnapshotArgs(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs);
    static void TryEnqueueRecord(const char* fmt, LogColor color, TxPriority prio,
                                 const uint32_t* args, uint8_t nargs);
    static void SendFrame(TxFrame* f);
    static void MarkStaleDbg();

    static inline LogEntry*      active_ = nullptr;
    static inline uint8_t        count_ = 0;
    static inline Stream*        stream_ = nullptr;
    static inline bool           sending_ = false;
    static inline bool           shell_own_ = false;
    static inline TxFrameQueue   txq_ {};
    static inline LogRecordQueue recq_ {};
};
```

实际写代码时可以保留 `TxFrame` / `LogRecord` 在 `Log` 私有区，或者继续放在 namespace 内。推荐把 `LogRecord` 私有化，因为 `Shell` 不应该知道它；`TxFrame` 也可以私有化，因为发送帧本身同样是 `Log` 内部实现。

## 8. 风险与注意事项

1. **不要把锁藏进队列类里**：否则调用路径里看不出 ISR 临界区。队列方法用 `Locked` 命名，明确要求调用者已加锁。
2. **不要在格式化期间持锁**：`FormatRecord()` 里有 `snprintf()`，必须在锁外执行。
3. **不要提前释放 `LogRecord`**：记录必须在 `FormatRecord()` 完成后再回到空闲链。
4. **不要改变 `Stream::Send()` 假设**：`SendFrame()` 仍依赖 `Stream::Send()` 返回前完成拷贝。这个契约应在注释或 README 中补清楚。
5. **不要扩大成日志系统重写**：本规划只管内存池/链表封装和与封装直接相关的优先级、所有权问题；命令解析、颜色格式、宏接口不动。

## 9. 验证清单

静态检查：

- `rg "tx_pool_|free_head_|tx_head_|tx_tail_" framework/cmd/shell/log.hpp framework/cmd/shell/log.cpp` 只能命中 `TxFrameQueue` 内部。
- `rg "rec_pool_|rec_head_|rec_free_" framework/cmd/shell/log.hpp framework/cmd/shell/log.cpp` 只能命中 `LogRecordQueue` 内部。
- `rg "DequeueRecord|FormatRecord" framework/cmd/shell/shell.cpp` 不再命中。
- `rg "TxPriority::Event" framework/cmd/shell/log.cpp` 确认 `FormatRecord()` 不再硬编码事件档。

行为检查由用户编译/上板时执行：

- boot 早期 `DUST_LOG_INF("shell init")` 正常输出。
- shell 接管后 `DUST_LOG_INF/ERR/OK/WRN` 仍可输出。
- `DUST_LOG_DBG("name", ...)` 默认静默，`log on name` 后输出，`log off` 后停止。
- `log list/on/off` 命令响应仍正常。
- 连续多条命令响应仍由 8 帧池缓冲，不回退到 4 帧语义。

## 10. 最终判断

这次重构的重点不是换数据结构，而是**把固定池链表封装成有名字、有所有权边界的内部组件**。`Log` 现在太像“日志 API + 两个手写分配器 + 两个队列 + 格式化器”揉在一起；拆出 `TxFrameQueue` 和 `LogRecordQueue` 后，代码读起来会从“散链表”变成“三层结构”：

```text
DUST_LOG 宏 / Log public API
        ↓
Log：决定同步/异步、格式化、唤醒、发送
        ↓
TxFrameQueue / LogRecordQueue：固定内存池 + 索引链表
```

这样既保留嵌入式确定性，又把最乱的链表状态从 `Log` 主体里隔离出去。
