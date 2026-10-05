# DUST_LOG DBG 输出模式标志规划

> 目标文件：`framework/cmd/shell/log_debug.hpp` / `log_record.hpp` / `log_policy.hpp` / `log_queue.hpp` / `log.hpp` / `log.cpp` / `shell.cpp`
>
> 目标：给每条 `DUST_LOG_DBG("name", ...)` 流加一个**输出模式标志位**——标志关闭（默认）走**上色打印**（ANSI TrueColor，终端里当普通调试日志看），标志打开走 **VOFA+ 打印**（纯文本，`v0,v1\r\n`，FireWater 协议可解析）。标志按 DBG 名字独立，shell 运行时切换，掉电不保留。
>
> 日期：2026-10-05
>
> 前置：本文承接 `doc/DUST_LOG解耦与优先级扩展规划.md` 的重构结果（阶段 0–5 已完成，`framework/cmd/shell/` 已拆成 门面 + 策略表 + 源注册 + 记录/格式化 + 队列/仲裁 + 发送泵 六件）。

---

## 1. 结论

能做，改动小、边界清楚。

- 现状 DBG 只有一种输出形态（纯文本，`LogChannelPolicy` 里 `Debug.ansi_color = false` 写死），要么全上色要么全纯文本，**没有按名字分档的能力**。
- 本次给 `LogEntry` 加一个 `bool vofa` 标志位（默认 `false`），把「这条 DBG 流要不要上色」从**通道策略**下放到**条目**，DBG 记录在入队时把该标志快照进 `LogRecord`，出队格式化时按快照决定走 ANSI 还是纯文本。
- 上层接口（`DUST_LOG_DBG` 宏、`log on/off/list`）全部不变，只新增一条 `log mode <name> on|off` 子命令。

改动集中在 7 个文件、约 8 处，**不动**队列结构、优先级仲裁、epoch 作废、发送泵。

---

## 2. 现状：DBG 的输出形态是怎么定死的

### 2.1 链路走读

DBG 一条日志从调用点到串口，形态由谁决定，逐段列清：

| 环节 | 代码位置 | 形态相关行为 |
| --- | --- | --- |
| 入口宏 | [log.hpp:100-101](../framework/cmd/shell/log.hpp#L100) `DUST_LOG_DBG(name_, ...)` | 无形态信息，只传名字 + fmt |
| 调用点 | [log.cpp:61-82](../framework/cmd/shell/log.cpp#L61) `Log::PrintSelectedDebug` | 快照参数 → `QueueLogRecord(fmt, LogChannel::Debug, epoch, args, nargs)` |
| 入队 | [log.cpp:192-198](../framework/cmd/shell/log.cpp#L192) `Log::QueueLogRecord` | 查通道策略，把 `policy.color` / `policy.priority` 填进记录 |
| 出队 | [log.cpp:206-226](../framework/cmd/shell/log.cpp#L206) `Log::FormatOnePendingRecord` | 展开文本 → 查通道策略 → `WrapOutputFrame(policy, r->color, text, ...)` |
| 包装 | [log_record.hpp:222-229](../framework/cmd/shell/log_record.hpp#L222) `WrapOutputFrame` | **`if (!policy.ansi_color)` → 纯文本；否则 → ANSI** ← 形态的唯一决定点 |
| 策略表 | [log_policy.hpp:76](../framework/cmd/shell/log_policy.hpp#L76) | `{ LogChannel::Debug, TxPriority::Dbg, LogColor::Mint, "", false, true, true, true }` ← `false` 写死 |

### 2.2 问题

- `ansi_color` 是**通道级**常量，Debug 通道一条策略管所有 DBG 名字——想「A 流上色看文本、B 流纯数字喂 VOFA+」做不到。
- 想改就得改策略表常量 + 重新编译，运行中切不了。
- 与既有模型不匹配：DBG 本来就是**按名字**管理（`log on <name>` / `log list` / 每个名字一个 `LogEntry`），唯独输出形态还挂在通道上。

### 2.3 为什么放在条目上是对的

`DebugSourceRegistry` 已经按名字管条目（[log_debug.hpp](../framework/cmd/shell/log_debug.hpp)），选中状态 `active_` + 选择代数 `epoch_` 都在条目维度。输出模式与「选中哪条」同维度——**同一批 DBG 名字，各自记各自的形态**，语义一致，命令也只需在 `log` 家族里加一支。

---

## 3. 目标设计

```text
DUST_LOG_DBG("name", ...)
    ↓
LogEntry.vofa  ← 标志位（按名字，默认 false=上色）
    ↓ 入队时快照
LogRecord.ansi  ← 已解析的布尔（true=上色 / false=纯文本）
    ↓ shell 线程出队
WrapOutputFrame(prefix, ansi, color, text, ...)
    ↓
TxFrame（内容已定型）→ 发送泵 → Stream
```

三个关键决策：

1. **标志位语义**：`vofa == false`（默认）→ 上色；`vofa == true` → VOFA+ 纯文本。与用户措辞一致（"默认关闭就是走上色打印，打开就是走vofa打印"）。
2. **快照而非引用**：`LogRecord` 存**已解析的布尔** `ansi`，不存 `LogEntry*`。理由：与 `LogRecord` 的"调用点快照"定位一致（[log_record.hpp](../framework/cmd/shell/log_record.hpp) 头部注释），不新增指针字段，且已有 epoch 作废机制保证切换语义干净。
3. **通道策略降级为默认值**：`Debug.ansi_color` 由 `false` 改 `true`（默认上色），非 DBG 通道仍读它；DBG 通道的实际形态改由 `LogRecord.ansi` 覆盖。

---

## 4. 分阶段执行方案

### 阶段 1：条目标志位（`log_debug.hpp`）

**① 目标**：`LogEntry` 带上 `vofa` 标志，注册表提供设置入口。

**② 具体干什么**

改动 1 —— `LogEntry` 加字段（[log_debug.hpp:27-30](../framework/cmd/shell/log_debug.hpp#L27)）：

```cpp
// old
struct LogEntry
{
    const char* name;   						// DBG 名字（log on 用，运行时 FindOrCreate 创建）
};

// new
struct LogEntry
{
    const char* name;   						// DBG 名字（log on 用，运行时 FindOrCreate 创建）
    bool        vofa;   						// 输出模式：false=上色（默认）/ true=VOFA+ 纯文本
};
```

改动 2 —— 新建条目时置默认值（[log_debug.hpp](../framework/cmd/shell/log_debug.hpp) `FindOrCreate` 尾部）：

```cpp
// old
        LogEntry* e = &entries_[count_++];
        e->name = name;
        return e;

// new
        LogEntry* e = &entries_[count_++];
        e->name = name;
        e->vofa = false;                        // 新条目默认上色
        return e;
```

改动 3 —— 新增 `SetVofa`（放在 `Select` 之后、`Deselect` 之前）：

```cpp
    /**
     * @brief 设置某 DBG 条目的输出模式
     *
     * 只设置已存在的条目（DUST_LOG_DBG 注册过的名字），不存在返回 false，
     * 不创建幽灵条目。
     *
     * @param name DBG 名字
     * @param vofa true=VOFA+ 纯文本；false=上色
     * @return true 设置成功；false 条目不存在
     */
    bool SetVofa(const char* name, bool vofa)
    {
        for (uint8_t i = 0; i < count_; ++i)
        {
            if (std::strcmp(entries_[i].name, name) == 0)
            {
                entries_[i].vofa = vofa;
                return true;
            }
        }
        return false;                          // 不存在：不创建幽灵条目
    }
```

文件头 `@version` 0.3 → 0.4。

**③ 产出**：`log_debug.hpp` 修改后文件。

**④ 验证**：编译通过（此阶段无调用点，纯增量）。

---

### 阶段 2：记录携带模式 + 包装函数解耦

**① 目标**：`LogRecord` 带上已解析的 `ansi`；`WrapOutputFrame` 不再从策略里读形态，改由调用方传。

**② 具体干什么**

改动 1 —— `LogRecord` 加字段（[log_record.hpp](../framework/cmd/shell/log_record.hpp)）：

```cpp
// old
    TxPriority  prio;              				// 展开后的发送优先级
    uint16_t    epoch;             				// DBG 选择代数（出队比对，不一致丢弃）

// new
    TxPriority  prio;              				// 展开后的发送优先级
    uint16_t    epoch;             				// DBG 选择代数（出队比对，不一致丢弃）
    bool        ansi;              				// 是否上 ANSI 颜色（DBG 按条目模式，其余取通道策略）
```

改动 2 —— `WrapOutputFrame` 签名改传 `prefix` + `ansi_color`（[log_record.hpp](../framework/cmd/shell/log_record.hpp)）：

```cpp
// old
inline int WrapOutputFrame(const LogChannelPolicy& policy, LogColor color, const char* text, char* out, size_t out_size)
{
    if (!policy.ansi_color) return snprintf(out, out_size, "%s%s\r\n", policy.prefix, text);

    const uint32_t rgb = static_cast<uint32_t>(color);
    return snprintf(out, out_size, "\x1b[38;2;%d;%d;%dm%s%s\x1b[0m\r\n",
                    (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, policy.prefix, text);
}

// new
inline int WrapOutputFrame(const char* prefix, bool ansi_color, LogColor color, const char* text, char* out, size_t out_size)
{
    if (!ansi_color) return snprintf(out, out_size, "%s%s\r\n", prefix, text);

    const uint32_t rgb = static_cast<uint32_t>(color);
    return snprintf(out, out_size, "\x1b[38;2;%d;%d;%dm%s%s\x1b[0m\r\n",
                    (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, prefix, text);
}
```

同步改函数头 Doxygen 的 `@param`（`policy` → `prefix`/`ansi_color`），文件头 `@version` 0.2 → 0.3。

改动 3 —— `LogChannelPolicy` 注释 + Debug 默认值（[log_policy.hpp](../framework/cmd/shell/log_policy.hpp)）：

```cpp
// old
    bool        ansi_color;       // 是否上 ANSI 颜色
...
    { LogChannel::Debug,   TxPriority::Dbg,   LogColor::Mint,   "",       false, true,  true,  true  },

// new
    bool        ansi_color;       // 通道默认是否上 ANSI 颜色（DBG 实际形态由条目标志覆盖）
...
    { LogChannel::Debug,   TxPriority::Dbg,   LogColor::Mint,   "",       true,  true,  true,  true  },
```

文件头 `@version` 0.2 → 0.3。

改动 4 —— `PushLogRecordLocked` 加形参（[log_queue.hpp](../framework/cmd/shell/log_queue.hpp)）：

```cpp
// old（声明 + 定义同处）
    bool PushLogRecordLocked(const char* fmt, LogChannel channel, LogColor color, TxPriority prio, uint16_t epoch, const uint32_t* args, uint8_t nargs)

// new
    bool PushLogRecordLocked(const char* fmt, LogChannel channel, LogColor color, TxPriority prio, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs)
```

函数体内赋值段：

```cpp
// old
        records[i].epoch 	= epoch;
        records[i].nargs 	= nargs;

// new
        records[i].epoch 	= epoch;
        records[i].ansi 	= ansi;
        records[i].nargs 	= nargs;
```

函数头 Doxygen 的 `@param` 补一行：

```cpp
     * @param epoch   DBG 选择代数（非 DBG 传 0）
     * @param ansi    是否上 ANSI 颜色
```

文件头 `@version` 0.2 → 0.3。

**③ 产出**：`log_record.hpp` / `log_policy.hpp` / `log_queue.hpp` 修改后文件。

**④ 验证**：此阶段改签名会破坏 `log.hpp`/`log.cpp` 既有调用，**必须与阶段 3 一起编译**；阶段 2 单独不验证。

---

### 阶段 3：门面接线

**① 目标**：`Log` 各调用点把 `ansi` 传到底，DBG 走条目标志、其余走通道策略。

**② 具体干什么**

改动 1 —— `QueueLogRecord` 声明加形参（[log.hpp:89](../framework/cmd/shell/log.hpp#L89)）：

```cpp
// old
    static void QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, const uint32_t* args, uint8_t nargs);		// 入队原始请求（异步段调用点）

// new
    static void QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs);	// 入队原始请求（异步段调用点）
```

文件头 `@version` 0.9 → 1.0。

改动 2 —— `QueueLogRecord` 定义（[log.cpp:192-198](../framework/cmd/shell/log.cpp#L192)）：

```cpp
// old
void Log::QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, const uint32_t* args, uint8_t nargs)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(channel);
    unsigned key = irq_lock();                // 并发保护（任务或 ISR 上下文）
    (void)recq_.PushLogRecordLocked(fmt, channel, policy.color, policy.priority, epoch, args, nargs);
    irq_unlock(key);
}

// new
void Log::QueueLogRecord(const char* fmt, LogChannel channel, uint16_t epoch, bool ansi, const uint32_t* args, uint8_t nargs)
{
    const LogChannelPolicy& policy = GetLogChannelPolicy(channel);
    unsigned key = irq_lock();                // 并发保护（任务或 ISR 上下文）
    (void)recq_.PushLogRecordLocked(fmt, channel, policy.color, policy.priority, epoch, ansi, args, nargs);
    irq_unlock(key);
}
```

函数头 Doxygen 的 `@param` 补一行 `@param ansi 是否上 ANSI 颜色`。

改动 3 —— DBG 入队带条目模式（[log.cpp:77](../framework/cmd/shell/log.cpp#L77)）：

```cpp
// old
    QueueLogRecord(fmt, LogChannel::Debug, dbg_.ActiveEpoch(), args, nargs);

// new
    QueueLogRecord(fmt, LogChannel::Debug, dbg_.ActiveEpoch(), !e->vofa, args, nargs);   // 模式取条目标志
```

改动 4 —— 一次性日志入队带通道策略（[log.cpp:162](../framework/cmd/shell/log.cpp#L162)）：

```cpp
// old
    QueueLogRecord(fmt, channel, 0, args, nargs);    	// 入原始请求队列

// new
    QueueLogRecord(fmt, channel, 0, policy.ansi_color, args, nargs);    	// 入原始请求队列
```

改动 5 —— 三处 `WrapOutputFrame` 调用点改签名：

```cpp
// old（log.cpp:152，boot 早期直发）
        int n = WrapOutputFrame(policy, policy.color, buf, out, sizeof(out));

// new
        int n = WrapOutputFrame(policy.prefix, policy.ansi_color, policy.color, buf, out, sizeof(out));
```

```cpp
// old（log.cpp:177，命令响应）
    int n = WrapOutputFrame(policy, policy.color, text, out, sizeof(out));

// new
    int n = WrapOutputFrame(policy.prefix, policy.ansi_color, policy.color, text, out, sizeof(out));
```

```cpp
// old（log.cpp:219，shell 线程出队展开：形态取记录快照，不再查策略）
    int n = WrapOutputFrame(policy, r->color, text, out, sizeof(out));

// new
    int n = WrapOutputFrame(policy.prefix, r->ansi, r->color, text, out, sizeof(out));
```

改动 6 —— `PrintSelectedDebug` 函数头 Doxygen 更新（[log.cpp:54](../framework/cmd/shell/log.cpp#L54)）：

```cpp
// old
 * @brief DBG 流式打印（仅当 e 是当前选中条目才发，纯文本无 ANSI，兼容 VOFA+ FireWater，低优先级）

// new
 * @brief DBG 流式打印（仅当 e 是当前选中条目才发；形态按条目标志：默认上色，vofa 模式纯文本，低优先级）
```

文件头 `@version` 0.9 → 1.0。

**③ 产出**：`log.hpp` / `log.cpp` 修改后文件。

**④ 验证**：
- 编译通过。
- 上电 `DUST_LOG_INF("shell init")` 仍带 `[inf] ` 前缀 + 绿色 + `\r\n`。
- `DUST_LOG_DBG("vofa", "%f,%f", x, y)` 未选中时静默；`log on vofa` 后默认**带 ANSI 颜色**（与旧行为相反，见 §6 影响）。

---

### 阶段 4：shell 命令与帮助

**① 目标**：新增 `log mode <name> on|off` 子命令；`log list` 显示模式；帮助文本补一行。

**② 具体干什么**

改动 1 —— `ProcessLogCommand` 加 `mode` 分支（[log.cpp:278](../framework/cmd/shell/log.cpp#L278)）。在 `"off"` 分支之后、`else SendCommandLine("?: log list|on <name>|off")` 之前插入：

```cpp
    else if (std::strcmp(reinterpret_cast<const char*>(sub), "mode") == 0)
    {
        // line 形如 "<name> on|off"（on=VOFA+ 纯文本，off=上色）
        uint8_t* name = line;
        while (*line && *line != ' ') line++;
        if (*line == ' ') { *line = '\0'; line++; }
        while (*line == ' ') line++;

        if (std::strcmp(reinterpret_cast<const char*>(line), "on") == 0 ||
            std::strcmp(reinterpret_cast<const char*>(line), "vofa") == 0)
        {
            if (SetDebugVofa(reinterpret_cast<const char*>(name), true)) SendCommandLine("log mode: vofa");
            else SendCommandLine("log mode: not found");
        }
        else if (std::strcmp(reinterpret_cast<const char*>(line), "off") == 0 ||
                 std::strcmp(reinterpret_cast<const char*>(line), "color") == 0)
        {
            if (SetDebugVofa(reinterpret_cast<const char*>(name), false)) SendCommandLine("log mode: color");
            else SendCommandLine("log mode: not found");
        }
        else SendCommandLine("?: log mode <name> on|off");
    }
```

末行 else 的帮助串同步：

```cpp
// old
    else SendCommandLine("?: log list|on <name>|off");

// new
    else SendCommandLine("?: log list|on <name>|off|mode <name> on|off");
```

改动 2 —— `Log` 增加薄转发（[log.hpp:45](../framework/cmd/shell/log.hpp#L45) `DeselectDebugEntry()` 之后）：

```cpp
    static bool      SetDebugVofa(const char* name, bool on) { return dbg_.SetVofa(name, on); }	// 设置 DBG 输出模式（on=VOFA+ 纯文本）
```

（与 `FindOrCreateDebugEntry` / `ActiveDebugEntry` 一样，门面只做转发。）

改动 3 —— `PrintLogList` 输出带模式（[log.cpp:314-324](../framework/cmd/shell/log.cpp#L314)）：

```cpp
// old
        char line[160];
        snprintf(line, sizeof(line), "%s %s", e->name,
                 (e == active) ? "[ON]" : "[off]");

// new
        char line[160];
        snprintf(line, sizeof(line), "%s %s %s", e->name,
                 (e == active) ? "[ON]" : "[off]",
                 e->vofa ? "[vofa]" : "[color]");
```

改动 4 —— `Shell::CmdHelp` 补一行（[shell.cpp:45](../framework/cmd/shell/shell.cpp#L45) `log off` 行之后）：

```cpp
// old
    Log::SendCommandLine("log off                 停止打印");

// new
    Log::SendCommandLine("log off                 停止打印");
    Log::SendCommandLine("log mode <name> on|off  切换输出模式（on=VOFA+纯文本，off=上色）");
```

**③ 产出**：`log.cpp` / `log.hpp` / `shell.cpp` 修改后文件。

**④ 验证**：
- `h` 输出里能看到 `log mode <name> on|off` 一行。
- `log list` 每行形如 `vofa [ON] [color]`。
- `log mode vofa on` → 回 `log mode: vofa`；`log mode 不存在 on` → 回 `log mode: not found`。
- `log mode`（缺参）/`log mode x abc` → 回 `?: log mode <name> on|off`。

---

### 阶段 5：全链路验收

**① 目标**：上板跑通两种形态 + 切换。

**② 具体干什么**：无代码改动，按 §7 清单逐条测。

**③ 产出**：验证结论（记录到本文档或会话）。

**④ 验证**：见 §7。

> 编译与烧录是用户动作，AI 不代跑。

---

## 5. 参数与命名总表

| 项 | 值 | 位置 |
| --- | --- | --- |
| 标志字段 | `bool LogEntry::vofa` | `log_debug.hpp` |
| 记录字段 | `bool LogRecord::ansi` | `log_record.hpp` |
| 默认值 | `vofa = false` → `ansi = true`（上色） | `FindOrCreate` |
| 设置入口 | `DebugSourceRegistry::SetVofa(const char* name, bool vofa)` | `log_debug.hpp` |
| 门面转发 | `Log::SetDebugVofa(const char* name, bool on)` | `log.hpp` |
| shell 命令 | `log mode <name> on\|off`（别名 `vofa`/`color` 同义） | `log.cpp` |
| 命令回显 | `log mode: vofa` / `log mode: color` / `log mode: not found` / `?: log mode <name> on\|off` | `log.cpp` |
| 列表标记 | `[vofa]` / `[color]` | `log.cpp` `PrintLogList` |
| 调试色 | `LogColor::Mint`（0x7BF7CD）不变 | `log_policy.hpp` |
| 前缀 | `""`（DBG 无前缀）不变 | `log_policy.hpp` |

命令等价写法：

| 输入 | 效果 |
| --- | --- |
| `log on vofa` | 选中名为 `vofa` 的 DBG 流（与本次无关，形态不变） |
| `log mode vofa on` | 把名为 `vofa` 的流切到 VOFA+ 纯文本 |
| `log mode vofa off` | 把名为 `vofa` 的流切回上色（默认） |
| `log mode vofa vofa` | `vofa` 同义 `on`，等价上一行 |

---

## 6. 内存与性能

| 项 | 变化 | 说明 |
| --- | --- | --- |
| `LogEntry` 大小 | 4B → 8B（指针 + bool + 3B 对齐填充） | 64 条 → 池 256B → **512B**，净增 256B |
| `LogRecord` 大小 | +1B（bool，含对齐可能 +4B） | 8 条池，净增 ≤32B |
| `TxFrame` | 不变 | 形态在格式化时已定型进 `data`，帧不携带模式 |
| 调用点开销 | 宏不变，每次 DBG 调用多一次 `bool` 拷贝 | 可忽略 |
| shell 命令 | 新增一支字符串比较 | 可忽略 |

⚠️ **净增约 300B 静态 RAM**（主要是 `LogEntry` 的对齐）。若在意，可把 `LogEntry` 拆成 `name[]` + `uint8_t flags` 紧凑数组，或把 `vofa` 挪进按位标志——本规划不做，先按直白写法。

---

## 7. 验证标准（阶段 5 逐条勾）

- [ ] 编译通过（`hpm5361icb` 主干 test-only + 用户区任一工程）。
- [ ] 上电 `DUST_LOG_INF("shell init")` / `DUST_LOG_INF("shell send owner taken")` 正常输出（带色带前缀）。
- [ ] `h` 里出现 `log mode <name> on|off` 行。
- [ ] `log list` 列出所有 DBG 名字，每行带 `[ON]/[off]` 与 `[vofa]/[color]`。
- [ ] `log mode 不存在的名字 on` → `log mode: not found`（不创建幽灵条目）。
- [ ] `log mode`（缺参）→ `?: log mode <name> on|off`。
- [ ] **默认（不设置）**：`log on vofa` 后 `DUST_LOG_DBG("vofa", "%f,%f", x, y)` 输出为**带 ANSI 颜色**的 `\x1b[38;2;123;247;205m...\x1b[0m\r\n`。
- [ ] **vofa 模式**：`log mode vofa on` 后同一流输出变为**纯文本** `x.xx,y.yy\r\n`，无任何 ANSI 转义。
- [ ] **切回**：`log mode vofa off` 后恢复上色。
- [ ] **运行中切换**：上色 ↔ 纯文本切换后，下一条 DBG 立即生效（无需重选、无需重启）。
- [ ] **互不影响**：若有两条 DBG 流（如 `vofa` / `dbg2`），`log mode vofa on` 不影响 `dbg2` 的形态。
- [ ] **普通日志不受影响**：INF/ERR/OK/WRN 仍带色带前缀；命令响应仍纯文本。
- [ ] **VOFA+ 实测**：`log mode <name> on` 后 FireWater 能正确解析通道（数字不被 ANSI 转义串污染）。

---

## 8. 风险点

### 8.1 默认值翻转影响既有验证项

`doc/DUST_LOG解耦与优先级扩展规划.md` §10 原有验收项：

> `log on vofa` 后，`project/thread/test/trd_test.cpp` 里的 `DUST_LOG_DBG("vofa", "%f,%f", ...)` 输出仍是纯 `v0,v1\r\n`，没有 ANSI。

本次改动后 DBG **默认上色**，该条验收项**作废**，需改为：

> `log on vofa` 默认上色；要纯 `v0,v1\r\n` 需先 `log mode vofa on`。

受影响的既有文档（本次规划不含改动，按需另开）：

- `framework/cmd/shell/README.md`：§「DBG 纯文本无 ANSI」相关描述。
- `doc/DUST_LOG解耦与优先级扩展规划.md`：§10 验收项。
- 记忆 `dust-log-usage`：只说"默认静默"，不涉及形态，**无需改**。

### 8.2 DBG 名字与命令关键字撞名

既有示例里 DBG 名字就叫 `vofa`，于是出现 `log mode vofa on` 这种"名字恰好等于关键字"的写法。不冲突（命令位置不同：`log mode` 后的第一个 token 是名字），但读起来易混。**缓解**：命令别名同时接受 `on/off` 和 `vofa/color`，且 `log list` 直接显示每条的模式，不用猜。

### 8.3 快照 vs 实时读取

本规划把模式**快照**进 `LogRecord`（入队时定），不走实时读 `LogEntry`。后果：切换命令与生效之间最多差「已在队列里的那几条」（`kMaxLogRecords = 8`，且 DBG 队列池满即丢）。实测感受等同即时。**若**将来要求"切换瞬间清空在途"，用已有的 `epoch` 机制即可（切换时递增 epoch），不必引 `LogEntry*` 引用。

### 8.4 与队列/仲裁层无关

本次不碰 `TxFrameQueue`（优先级、挤出、epoch 作废）与 `LogTransport`（发送泵）。`TxFrame` 内容在 `WrapOutputFrame` 时已定型，形态不再回溯，因此**没有**跨层影响。

---

## 9. 执行清单（按序勾）

- [ ] 阶段 1：`log_debug.hpp` — `LogEntry.vofa` + `FindOrCreate` 默认值 + `SetVofa`；version → 0.4
- [ ] 阶段 2：`log_record.hpp` — `LogRecord.ansi` + `WrapOutputFrame` 改签名；version → 0.3
- [ ] 阶段 2：`log_policy.hpp` — Debug `ansi_color` → `true` + 注释；version → 0.3
- [ ] 阶段 2：`log_queue.hpp` — `PushLogRecordLocked` 加 `ansi`；version → 0.3
- [ ] 阶段 3：`log.hpp` — `QueueLogRecord` 加 `ansi` + `SetDebugVofa` 转发；version → 1.0
- [ ] 阶段 3：`log.cpp` — 4 处 `QueueLogRecord`/3 处 `WrapOutputFrame` 调用点改签名；version → 1.0
- [ ] 阶段 4：`log.cpp` — `ProcessLogCommand` 加 `mode` 分支 + 帮助串 + `PrintLogList` 加模式列
- [ ] 阶段 4：`shell.cpp` — `CmdHelp` 加 `log mode` 一行
- [ ] 阶段 5：按 §7 清单上板验收（用户动作）
- [ ] 收尾：同步 `framework/cmd/shell/README.md` 与 `DUST_LOG解耦与优先级扩展规划.md` §10（见 §8.1）
