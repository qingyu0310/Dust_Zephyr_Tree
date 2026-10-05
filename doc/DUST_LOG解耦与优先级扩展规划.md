# DUST_LOG 解耦与优先级扩展规划

> 目标文件：`framework/cmd/shell/log.hpp` / `framework/cmd/shell/log.cpp` / `framework/cmd/shell/shell.cpp` / `framework/cmd/CMakeLists.txt`
>
> 目标：把当前臃肿的 `Log` 单体拆成“日志入口、调试源注册、记录快照、格式化、优先级仲裁、发送泵、命令适配”几个边界清楚的层次。后续新增不同优先级、不同输出形态、不同调试信息类型时，只改策略表或小模块，不继续膨胀 `Log`。
>
> 日期：2026-10-04

## 1. 结论

当前 `framework/cmd/shell/log` 的问题不是功能错，而是职责压得太密：

- `DUST_LOG_*` 宏入口、一次性日志、DBG 流式日志在同一个类里。
- DBG 名字注册、`log on/off/list` 命令、当前选中状态在同一个类里。
- `LogRecord` 参数快照、格式化展开、ANSI 上色、VOFA+ 纯文本兼容在同一个类里。
- `TxFrame` 帧池、三档优先级、池满挤出、stale 作废、DMA 发送泵在同一个类里。
- boot 早期直发与 shell 接管后的异步路径也散落在同一个类里。

推荐做“分层解耦”，而不是只把函数挪到更多文件里。最终 `Log` 应该退化成一个很薄的门面：

```cpp
class Log
{
public:
    static bool Init();
    static void BindOutputStream(Stream* s);
    static void MarkShellThreadReady();

    static void Print(LogChannel channel, const char* fmt, ...);
    static LogEntry* RegisterDebugSource(const char* name);
    static void PrintDebug(LogEntry* e, const char* fmt, ...);

    static void SendCommandLine(const char* text);
    static void ProcessLogCommand(uint8_t* line);
    static bool DrainOneRecord();
    static void SendNextFrameIfIdle();
    static void HandleTxDone();
};
```

`Log` 保持旧宏兼容，但它不再直接管理所有内部细节。

## 2. 当前耦合点

### 2.1 API 与语义耦合

现在 `DUST_LOG_INF/ERR/OK/WRN/DBG` 直接绑定到 `PrintInfo/PrintError/PrintOk/PrintWarning/PrintSelectedDebug`。如果以后要加：

- `TRACE`：最低优先级、可大量丢弃；
- `WARN`：事件级，但颜色和前缀不同；
- `FAULT`：最高优先级、不可被挤出；
- `SCOPE`：带名字、可独立开关；
- `METRIC`：给 VOFA+/上位机的纯数据流；

就会继续往 `Log` 里加函数、颜色、优先级、格式规则和命令逻辑。

根因是“日志等级/通道”没有被抽象成数据，当前是散落在函数名和分支里的硬编码。

### 2.2 DBG 注册与命令耦合

`FindOrCreateDebugEntry()`、`SelectDebugEntry()`、`DeselectDebugEntry()`、`PrintLogList()`、`ProcessLogCommand()` 都在 `Log` 内部。这样 DBG 源管理和日志发送路径互相知道太多：

- `log on` 必须知道 `active_`。
- `Deselect` 必须直接操作 `txq_` 和 `recq_` 的 stale 标记。
- `PrintLogList` 直接遍历 `entries_`。

更合理的边界是 `DebugSourceRegistry` 管名字和选中状态，发送层只接收“这个记录是否应该作废”的判定结果。

### 2.3 记录快照与格式化耦合

`CopyFormatArgs()` 与 `FormatRecordToFrame()` 是一对协议：前者按格式串取 `va_arg`，后者按相同格式串从 `uint32_t args[]` 还原。它们必须同步演进。

现在这对协议埋在 `Log` 里，导致两个风险：

- 新增格式支持时，很容易只改快照或只改展开。
- 新增日志类型时，很容易把“格式化规则”和“发送优先级”写在同一个分支里。

这里应拆成 `LogRecordBuilder` 和 `LogFormatter`，或者至少放进独立文件，形成明确契约。

### 2.4 优先级仲裁与发送耦合

`TxFrameQueue` 已经从裸数组封装成私有结构，这是好的第一步。但它仍然嵌在 `Log` 里，优先级规则也只有三档：

```cpp
enum class TxPriority : uint8_t
{
    Event = 0,
    Cmd   = 1,
    Dbg   = 2,
};
```

如果以后添加更多优先级，仅靠 enum 扩展还不够。不同档位至少需要这些策略：

- 队列满时是否允许挤出别人；
- 自己是否允许被挤出；
- 同档是否 FIFO；
- 是否允许 stale；
- 是否允许 ANSI；
- 是否需要被 `log on/off` 控制；
- 是否允许 ISR 路径入队。

这些不应该散落在 `if (prio == TxPriority::Dbg)` 这样的分支里。

## 3. 目标层次

推荐按 7 层拆。越上层越接近用户语义，越下层越接近发送机制。

```text
DUST_LOG_* 宏
    ↓
LogFacade / Log
    ↓
LogChannelPolicy       DebugSourceRegistry       CommandAdapter
    ↓
LogRecordBuilder       LogFormatter
    ↓
LogRecordQueue
    ↓
TxScheduler / TxFrameQueue
    ↓
LogTransport / StreamPump
    ↓
Stream(UartDma/Usb/RS485...)
```

### 3.1 LogFacade：对外兼容层

职责：

- 保持现有 `DUST_LOG_*` 宏不变。
- 保持 `Log::SendCommandLine()`、`Log::HandleTxDone()`、`Log::SendNextFrameIfIdle()` 对 shell 的调用方式不变。
- 把旧 API 转换成统一的 `LogChannel` 或 `LogClass`。

不做：

- 不直接遍历 DBG 条目。
- 不直接操作帧池链表。
- 不直接写 ANSI 格式。

建议文件：

- `framework/cmd/shell/log.hpp`
- `framework/cmd/shell/log.cpp`

### 3.2 LogChannelPolicy：优先级和输出策略层

职责：

- 定义所有日志通道的元信息。
- 把“颜色、前缀、发送优先级、是否上色、是否可丢、是否可被命令选择”做成表。

建议模型：

```cpp
enum class LogChannel : uint8_t
{
    Info,
    Error,
    Ok,
    Warning,
    Debug,
    Command,
};

enum class TxPriority : uint8_t
{
    Critical = 0,
    Event    = 1,
    Command  = 2,
    Data     = 3,
    Debug    = 4,
    Trace    = 5,
};

struct LogChannelPolicy
{
    LogChannel channel;
    TxPriority priority;
    LogColor color;
    const char* prefix;
    bool ansi_color;
    bool selectable;
    bool stale_on_switch;
    bool evictable;
};
```

现有行为可以映射为：

| 现有入口 | Channel | Priority | 前缀 | ANSI | 选择控制 |
| --- | --- | --- | --- | --- | --- |
| `DUST_LOG_INF` | `Info` | `Event` | `[inf] ` | 是 | 否 |
| `DUST_LOG_ERR` | `Error` | `Event` | `[err] ` | 是 | 否 |
| `DUST_LOG_OK` | `Ok` | `Event` | `[ok] ` | 是 | 否 |
| `DUST_LOG_WRN` | `Warning` | `Event` | `[wrn] ` | 是 | 否 |
| `DUST_LOG_DBG` | `Debug` | `Debug` | 无 | 否 | 是 |
| `SendCommandLine` | `Command` | `Command` | 无 | 否 | 否 |

后续新增优先级时，优先改这个表，而不是新增一组 `PrintXxx()` 分支。

### 3.3 DebugSourceRegistry：DBG 源注册与选择层

职责：

- 管理 `LogEntry entries_[kMaxLogEntries]`。
- 管理当前选中项。
- 提供 `Register(name)`、`Select(name)`、`Deselect()`、`First()/Next()`。
- 在选择变化时返回一个“选择代数”或“epoch”，用于作废旧 DBG 记录。

推荐从“直接 stale 队列里的 DBG”升级为“记录携带 source/epoch”：

```cpp
struct DebugSourceToken
{
    LogEntry* entry;
    uint16_t epoch;
};
```

`log on A` 或 `log off` 时只递增 `epoch`。后续出队发送时，如果记录里的 `epoch` 与当前不一致，就丢弃。这样 `DebugSourceRegistry` 不需要知道 `TxFrameQueue` 和 `LogRecordQueue` 的内部结构。

短期也可以继续保留 `MarkDebugFramesStaleLocked()`，但要把调用边界收敛到一个适配函数，避免命令层直接碰两个队列。

建议文件：

- `framework/cmd/shell/log_debug.hpp`
- `framework/cmd/shell/log_debug.cpp`

### 3.4 LogRecordBuilder：调用点快照层

职责：

- 在异步段从 `fmt + va_list` 提取固定大小参数快照。
- 生成 `LogRecord`。
- 只做短临界区前的准备，不做格式化。

建议 `LogRecord` 不只保存 `color/prio`，而保存 `channel/policy/source/epoch`：

```cpp
struct LogRecord
{
    const char* fmt;
    LogChannel channel;
    TxPriority priority;
    LogColor color;
    uint16_t source_epoch;
    LogEntry* source;
    uint32_t args[kMaxLogArgs];
    uint8_t nargs;
    uint8_t next;
};
```

这样新增日志类型时，记录模型不需要反复补字段。

建议文件：

- `framework/cmd/shell/log_record.hpp`
- `framework/cmd/shell/log_record.cpp`

### 3.5 LogFormatter：格式化与输出形态层

职责：

- 把 `LogRecord` 展开成文本。
- 根据 policy 决定是否加 ANSI、是否加前缀、是否加 `\r\n`。
- 保持 DBG/VOFA+ 纯文本行为。

关键原则：

- `LogFormatter` 不决定优先级。
- `LogFormatter` 不决定是否发送。
- `LogFormatter` 不操作 `Stream`。

建议接口：

```cpp
class LogFormatter
{
public:
    static int FormatRecord(const LogRecord& record, char* out, size_t out_size);
    static int FormatImmediate(LogChannel channel, const char* text, char* out, size_t out_size);
};
```

### 3.6 LogRecordQueue：未格式化记录队列

职责：

- 管理 `LogRecord` 静态池。
- 负责 free 链、FIFO 链、出队所有权。
- 不理解 ANSI，不理解 Stream。

当前 `LogRecordQueue` 已经基本成形，后续主要改两点：

- 从 `Log` 私有结构挪成独立小类。
- stale 策略从“遍历标记所有 DBG”逐步替换成“出队时检查 source/epoch”。

建议文件：

- `framework/cmd/shell/log_record_queue.hpp`
- `framework/cmd/shell/log_record_queue.cpp`

### 3.7 TxScheduler / TxFrameQueue：发送优先级仲裁层

职责：

- 管理 `TxFrame` 静态池。
- 按 `TxPriority` 排队。
- 根据 `PriorityPolicy` 决定池满时挤出谁。
- 出队时跳过过期帧。

建议把“优先级数值”和“挤出策略”分开：

```cpp
struct TxPriorityPolicy
{
    TxPriority priority;
    bool may_evict_lower;
    bool may_be_evicted;
    bool preserve_fifo;
};
```

例如：

| Priority | 用途 | 池满策略 |
| --- | --- | --- |
| `Critical` | fatal/硬错误 | 不被挤出，可挤出低档 |
| `Event` | INF/ERR/OK/WRN | 不主动丢，可挤出 Debug/Trace |
| `Command` | shell 响应 | 尽量保留，可挤出 Debug/Trace |
| `Data` | 上位机数据流 | 可配置，通常低于命令高于 Debug |
| `Debug` | `log on` 流式调试 | 可被高档挤出，可 stale |
| `Trace` | 高频细节 | 最先丢 |

这样以后新增“不同优先级信息”时，不需要重写 `EvictLowestPriorityFrameLocked()`。

建议文件：

- `framework/cmd/shell/log_tx.hpp`
- `framework/cmd/shell/log_tx.cpp`

### 3.8 LogTransport / StreamPump：发送泵层

职责：

- 保存 `Stream*`。
- 保存 `sending_`。
- 处理 `StartFrameSend()`、`HandleTxDone()`、`SendNextFrameIfIdle()`。
- 只消费已经格式化好的 `TxFrame`。

当前 `Log` 和 `shell` 的关键关系要保留：

- shell 线程仍然是发送泵的主要驱动者。
- `UartDma tx_cb` 仍然只清状态并 `k_sem_give()`。
- boot 早期同步直发路径仍然存在，避免 shell 线程未启动时积压。

建议文件：

- 初期仍放在 `log.cpp`，等上面几层稳定后再拆。
- 稳定后可拆成 `log_transport.hpp/cpp`。

## 4. 文件拆分建议

第一阶段不要一次性拆太碎。推荐最终形态如下：

```text
framework/cmd/shell/
├── log.hpp                  # 对外宏和 Log 门面
├── log.cpp                  # 门面实现、boot/shell 时段选择
├── log_policy.hpp           # LogChannel / TxPriority / policy 表
├── log_debug.hpp            # DBG 源注册、选择、遍历
├── log_debug.cpp
├── log_record.hpp           # LogRecord / 参数快照声明
├── log_record.cpp           # CopyFormatArgs / FormatRecord
├── log_queue.hpp            # LogRecordQueue / TxFrameQueue 类声明
├── log_queue.cpp            # 静态池、链表、优先级队列实现
└── shell.cpp                # 只负责调用 Log 门面
```

如果担心文件数量太多，可以先合并为四组：

```text
log.hpp / log.cpp            # 门面
log_policy.hpp               # 策略表，纯头文件
log_debug.hpp / log_debug.cpp
log_core.hpp / log_core.cpp  # record + queue + formatter + tx
```

先四组，后面再按复杂度拆细。

## 5. 迁移步骤

### 阶段 0：冻结现有行为

不急着改代码，先确认这些行为不能破：

- `DUST_LOG_INF/ERR/OK/WRN` 输出带颜色和 `\r\n`。
- `DUST_LOG_DBG(name, ...)` 默认静默。
- `log on <name>` 只选择已注册名字，不创建幽灵条目。
- `log off` 立即停止 DBG 输出。
- DBG 输出保持纯文本，兼容 VOFA+ FireWater。
- shell 接管前普通日志可直发。
- shell 接管后日志调用点只做快照，不做格式化。
- 发送优先级仍满足事件 > 命令响应 > DBG。

### 阶段 1：引入 policy，但不改变发送路径

新增 `log_policy.hpp`：

- 定义 `LogChannel`。
- 保留现有 `TxPriority` 数值语义。
- 建立 `GetLogChannelPolicy(channel)`。

然后让 `PrintInfo/PrintError/PrintOk/PrintWarning/PrintSelectedDebug/SendCommandLine` 都通过 policy 取颜色、前缀、ANSI 和优先级。

这一阶段只消灭硬编码，不拆队列。

### 阶段 2：拆 DBG 源管理

新增 `log_debug.hpp/cpp`：

- 把 `entries_`、`active_`、`count_` 移入 `DebugSourceRegistry`。
- `Log::FindOrCreateDebugEntry()` 变成薄转发。
- `Log::SelectDebugEntry()` / `DeselectDebugEntry()` 先保持旧行为，但由 registry 返回选择结果。
- `PrintLogList()` 通过 registry 遍历。

这一阶段 `Log` 仍可调用 `txq_.MarkDebugFramesStaleLocked()` 和 `recq_.MarkDebugRecordsStaleLocked()`，先不引入 epoch，降低风险。

### 阶段 3：拆参数快照和格式化

新增 `log_record.hpp/cpp`：

- 把 `LogRecord`、`CopyFormatArgs()`、`FormatRecordToFrame()` 拆出去。
- 命名上区分：
  - `BuildRecordFromVaList()`：调用点快照；
  - `FormatRecordToText()`：shell 线程展开；
  - `WrapOutputFrame()`：加 ANSI、前缀、行尾。

这一阶段重点是把“格式协议”集中到一个模块，避免以后新增 `%lld`、`%zu`、二进制 dump 等格式时散改。

### 阶段 4：拆队列和仲裁

新增 `log_queue.hpp/cpp`：

- 把 `LogRecordQueue` 和 `TxFrameQueue` 从 `Log` 私有结构挪出去。
- 把 `EvictLowestPriorityFrameLocked()` 改成基于 policy 判断，而不是只看 enum 数值。
- 保留静态数组和 `uint8_t next` 链表，不引入堆和 STL。

这一阶段完成后，新增优先级只需要：

1. 在 `TxPriority` 加枚举；
2. 在 `TxPriorityPolicy` 表加策略；
3. 在 `LogChannelPolicy` 表把某个通道映射过去。

### 阶段 5：引入 source/epoch，去掉队列遍历作废

这是解耦最关键的一步，但建议放到最后：

- `DebugSourceRegistry` 内部维护 `uint16_t epoch_`。
- 每次 `Select/Deselect` 都递增 `epoch_`。
- DBG `LogRecord/TxFrame` 携带创建时的 `epoch`。
- 出队时发现 epoch 不一致就丢弃。

这样选择变化不需要调用：

- `txq_.MarkDebugFramesStaleLocked()`
- `recq_.MarkDebugRecordsStaleLocked()`

DBG 控制层就彻底不需要知道队列内部结构。

## 6. 新增优先级的推荐方式

以后不要直接新增 `PrintXxx()` 并手写一套发送分支。推荐流程：

1. 定义语义通道：

```cpp
enum class LogChannel : uint8_t
{
    Info,
    Error,
    Ok,
    Warning,
    Debug,
    Command,
    Metric,
    Trace,
    Fault,
};
```

2. 定义传输优先级：

```cpp
enum class TxPriority : uint8_t
{
    Critical = 0,
    Event    = 1,
    Command  = 2,
    Metric   = 3,
    Debug    = 4,
    Trace    = 5,
};
```

3. 在 policy 表声明行为：

```cpp
{ LogChannel::Metric, TxPriority::Metric, LogColor::Mint, "", false, true, true, true }
```

4. 宏只做薄包装：

```cpp
#define DUST_LOG_METRIC(name_, ...) \
    ::debug::Log::PrintNamed(::debug::LogChannel::Metric, name_, ##__VA_ARGS__)
```

这样“语义”和“发送仲裁”分离：`Metric` 是日志含义，`TxPriority::Metric` 是线路拥塞时的排序策略。

## 7. shell 边界

`shell.cpp` 以后只应该知道这些事：

- 初始化时 `Log::Init()`。
- 把 `Stream` 绑定给 `Log`。
- 线程开始时 `Log::MarkShellThreadReady()`。
- 主循环里调用 `Log::DrainOneRecord()` 和 `Log::SendNextFrameIfIdle()`。
- 命令分发时把 `log` 子命令交给 `Log::ProcessLogCommand()`。
- DMA 完成回调走 `Log::HandleTxDone()`。

`shell.cpp` 不应该知道：

- 有几档优先级。
- DBG 如何 stale。
- 参数如何快照。
- ANSI 如何包装。
- 池满时挤出谁。

这样以后即使 `Log` 支持 USB、RS485 或多路输出，shell 底座也不用被迫理解日志内部。

## 8. 保留约束

这些约束建议继续保留：

- 不引入动态内存。
- 不引入 STL 容器。
- ISR 路径不做 `vsnprintf`。
- ISR 路径不遍历长链表做复杂挤出。
- DBG 纯文本输出优先级低于命令响应。
- 命令响应不能被 DBG 长流长期饿死。
- boot 早期普通日志仍能直发。
- `CONFIG_DUST_CMD_SHELL_LOG=n` 时宏为空，调用点零成本。

## 9. 风险点

### 9.1 参数快照协议风险

`CopyFormatArgs()` 和 `FormatRecordToFrame()` 必须成对改。拆文件时不要把它们拆到两个互相不了解的模块里。建议把格式串解析逻辑集中复用，至少保证支持列表在同一个头文件里可见。

### 9.2 DBG 切换及时性风险

当前通过遍历队列打 stale 标记，`log on/off` 后旧 DBG 能较快停止。改成 epoch 后，必须确保：

- 未格式化的 `LogRecord` 会在出队时丢弃。
- 已格式化未发送的 `TxFrame` 也能在出队时丢弃。
- 正在 DMA 中的一帧无法取消，可以接受最多残留一帧。

### 9.3 boot 早期路径风险

boot 早期没有 shell 线程消费队列，不能把所有输出统一改成异步入队。`shell_own_ == false` 的普通日志直发路径要保留，或者必须提供等价的同步泵。

### 9.4 命令响应优先级风险

`SendCommandLine()` 是 shell 交互体验的底线。即使加入 `Metric/Trace` 高频流，也不能让命令响应被淹没。

## 10. 验证标准

每个阶段完成后至少检查：

- 编译通过。
- `h` 能看到 `log list/on/off` 帮助。
- `log list` 能列出 `DUST_LOG_DBG` 注册过的名字。
- `log on vofa` 后，`project/thread/test/trd_test.cpp` 里的 `DUST_LOG_DBG("vofa", "%f,%f", ...)` 默认输出**带 ANSI 颜色**。
  （2026-10-05 更新：DBG 输出形态已改为按名字的 `LogEntry::vofa` 标志控制，默认上色；要纯 `v0,v1\r\n` 需先 `log mode vofa on`。见 `doc/DUST_LOG_DBG输出模式标志规划.md`。）
- `log off` 后 DBG 停止。
- 连续普通日志不会被 DBG 长流挤掉。
- 连续命令响应不会被 DBG 长流挤掉。
- shell 线程接管前后，`DUST_LOG_INF("shell init")` 与 `DUST_LOG_INF("shell send owner taken")` 仍能正常输出。

## 11. 推荐落地顺序

最稳妥的顺序是：

1. 先加 `log_policy.hpp`，把颜色、前缀、优先级从函数分支变成表。
2. 再拆 `DebugSourceRegistry`，让 DBG 名字和选择状态离开 `Log`。
3. 再拆 `LogRecordBuilder/LogFormatter`，集中快照与展开协议。
4. 再拆 `LogRecordQueue/TxFrameQueue` 到独立文件。
5. 最后引入 source/epoch，去掉队列遍历 stale。

每一步都保持现有宏和 shell 命令不变。这样用户代码不用跟着改，风险集中在 `framework/cmd/shell` 内部。

## 12. 最终目标形态

重构完成后，新增一种日志信息不再需要理解整个 `log.cpp`。例如要新增 `DUST_LOG_TRACE`：

- 在 `LogChannel` 加 `Trace`。
- 在 `TxPriority` 加或复用 `Trace`。
- 在 policy 表设置“最低优先级、可丢、不上色或灰色”。
- 加一个宏包装到 `Log::Print(LogChannel::Trace, ...)`。

不需要改：

- DBG 注册表。
- shell 命令解析。
- `Stream` 发送逻辑。
- 帧池 free 链。
- DMA 完成回调。
- boot/shell 分时段策略。

这才是解耦完成的标志：新增优先级和新增信息类型时，只扩展策略和入口，不继续扩胖 `Log` 单体。
