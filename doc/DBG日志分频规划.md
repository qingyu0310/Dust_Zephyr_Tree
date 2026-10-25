# DBG 日志分频（`log on <name> <N>`）规划

> 2026-10-25。需求：`log on <name> 10` 表示该 DBG 流**每 10 条打印 1 条**，用于压制高频日志、减轻队列与串口压力。
>
> **拍板（用户 2026-10-25）**：采用**方案 A（调用点分频）**。

---

## 1 现状

### 1.1 DBG 链路

```text
DUST_LOG_DBG("name", fmt, ...)
    → Log::PrintSelectedDebug(FindOrCreateDebugEntry(name), fmt, ...)   log.hpp:101 宏
        → shell_own_ / e==nullptr / e!=Active()  → return            log.cpp:65-68
        → BuildRecordFromVaList(fmt, ap, args, &nargs)               参数格式化 ← 大头开销
        → QueueLogRecord(fmt, Debug, ActiveEpoch(), !e->vofa, ...)   入记录队列
        → k_sem_give(shell 线程)                                      唤醒消费者
```

`epoch_`（`log_debug.hpp:146`）是「出生证明」：入队时快照，出队时与 `ActiveEpoch()` 比对，不一致丢弃。

### 1.2 队列现状

`LogRecordQueue` 深度 8（`kTxPoolCount`），`PopNextRecordLocked` 按优先级出队。**DBG 是低优先级档**，高频时会挤掉或阻塞 Event/Cmd 档——这就是要分频的原因。

---

## 2 方案（已定：A）

### 2.1 机制

分频判断放在 **`PrintSelectedDebug` 的最开头，`va_start` 之前**：

```cpp
void Log::PrintSelectedDebug(LogEntry* e, const char* fmt, ...)
{
    if (!shell_own_) return;
    if (e == nullptr) return;
    if (e != dbg_.Active()) return;

    // 分频：每 divisor 次调用放行 1 次（divisor<=1 全放行，短路不碰 quota）
    const atomic_val_t divisor = atomic_get(&e->divisor);
    if (divisor > 1) {
        if (atomic_dec(&e->quota) > 0) return;      // 还有配额：丢弃
        atomic_set(&e->quota, divisor - 1);         // 配额用尽：放行并重填
    }

    va_list ap;
    ...
}
```

**被丢弃的调用在格式化之前就返回**，不构造参数槽、不入队、不碰 `irq_lock`。

### 2.2 为什么不是队列侧分频（评估后否决）

| 维度 | A 调用点 | B 队列侧 |
|---|---|---|
| 丢弃时省掉格式化 | ✅ 完全省 | ❌ 白算 |
| 计数粒度 | ✅ 每个名字独立 | ❌ `PushLogRecordLocked` 所有名字共用，`log on imu 10` 会串到 remote |
| 队列改动 | 零 | `LogRecord` 加字段 + 签名 + 全部调用点 |
| 临界区 | 不进锁 | 判断在 `irq_lock` 内 |
| 1kHz 下 CPU 收益 | 丢弃的 90% 全免 | 几乎为零 |

**核心：分频的目的是省资源，而资源开销的 80% 在参数格式化。B 在格式化之后才判，等于没省。**

---

## 3 设计

### 3.1 `LogEntry` 扩展

`framework/shell/log/log_debug.hpp`：

```cpp
struct LogEntry
{
    const char* name;                   // DBG 名字（log on 用，运行时 FindOrCreate 创建）
    bool        vofa;                   // 输出模式：false=上色（默认）/ true=VOFA+ 纯文本
    atomic_t    divisor;                // 分频周期：1=不分频，N=每 N 条打 1 条（任务写 / ISR 读，必须原子）
    atomic_t    quota;                  // 剩余可丢条数（0 = 本次放行；任务写 / ISR 改，必须原子）
};
```

RAM 增量：`LogEntry` 8 B → 16 B（有对齐填充），64 条约 **+256~384 B**。

### 3.2 计数语义：递减配额，不用取余

```cpp
const atomic_val_t divisor = atomic_get(&e->divisor);   // 快照：避免重填时读到被 Select 改过的新值
if (divisor > 1) {
    if (atomic_dec(&e->quota) > 0) return;              // 还有配额：丢弃
    atomic_set(&e->quota, divisor - 1);                 // 配额用尽：放行并重填
}
```

**为什么不用 `%`**：RV32 的 `rem` 在 Andes 核上要几十周期，1kHz 下每个 DBG 调用都算一次不划算。递减配额只需 `atomic_dec`，重填 `atomic_set` **只在放行的 1/N 次执行**。

推演 `divisor = 10`（`Select` 时 `quota = 0`）：

| 调用 | dec 前 | `dec > 0` | 结果 | quota 变为 |
|---|---|---|---|---|
| 1 | 0 | 假 | **放行** | 9 |
| 2 | 9 | 真 | 丢弃 | 8 |
| … | … | 真 | 丢弃 | … |
| 10 | 1 | 真 | 丢弃 | 0 |
| 11 | 0 | 假 | **放行** | 9 |

第 1 次与第 11 次之间隔 10 次调用，正好每 10 条打 1 条，且**第一条立即可见**。

| 情况 | 行为 |
|---|---|
| `divisor = 1` | **短路**，不碰 `quota`，零开销 |
| 定期输出 | 每 `divisor` 次调用放行 1 次 |
| 溢出 | `quota` 值域锁在 `0 ~ divisor-1`，**永不溢出**，无需处理回绕 |
| 并发 | `atomic_dec` 返回自减前的值，两个上下文同时进来只有一个拿到 0（放行），不会重复放行 |
| `divisor` 并发 | 任务里 `Select` 改写、ISR 里读 → 也必须 `atomic_t`；判断前先 `atomic_get` 快照一次，避免重填时读到新值 |

### 3.3 `Select` 增加分频参数

`log_debug.hpp`：

```cpp
/**
 * @brief 选中 DBG 条目
 * @param name    DBG 名字
 * @param divisor 分频周期（1=不分频；0 按 1 处理）
 * @return true 选中成功；false 条目不存在
 */
bool Select(const char* name, uint16_t divisor = 1)
{
    for (uint8_t i = 0; i < count_; ++i)
    {
        if (std::strcmp(entries_[i].name, name) == 0)
        {
            atomic_set(&entries_[i].divisor, (divisor == 0) ? 1 : divisor);
            atomic_clear(&entries_[i].quota);      // 配额清零：下一条立即放行
            active_ = &entries_[i];
            ++epoch_;
            return true;
        }
    }
    return false;
}
```

**必须清零 `quota`**：换名字或重设周期都要从头数，否则接着上次的余数，用户切完看不到第一条。

`FindOrCreate` 里补初始化：`atomic_set(&e->divisor, 1); atomic_clear(&e->quota);`

### 3.4 命令解析

`framework/shell/log/log.cpp` 的 `on` 分支：

```cpp
else if (std::strcmp(sub, "on") == 0)
{
    const char* name = nullptr;
    line = NextToken(line, name);

    if (*name == '\0') {
        SendCommandLine("usage: log on <name> [divisor]");
        return;
    }

    uint16_t divisor = 1;
    if (*line != '\0') {
        uint32_t v = 0;
        for (const uint8_t* p = line; *p >= '0' && *p <= '9'; ++p) {
            v = v * 10 + static_cast<uint32_t>(*p - '0');
            if (v > 0xFFFF) { v = 0xFFFF; break; }
        }
        divisor = (v == 0) ? 1 : static_cast<uint16_t>(v);   // 0 或非数字 → 不分频
    }

    if (SelectDebugEntry(name, divisor)) {
        char ack[32];
        snprintf(ack, sizeof(ack), "log on: ok x%u", (unsigned)divisor);
        SendCommandLine(ack);
    } else {
        SendCommandLine("log on: not found");
    }
}
```

**手写数字解析**，不用 `strtoul`——`picolibc` 在这一平台有多位数解析 bug（项目已有教训）。

`log.hpp` 的 `SelectDebugEntry` 声明同步加默认参数：

```cpp
static bool SelectDebugEntry(const char* name, uint16_t divisor = 1);
```

`log.cpp` 的实现同步改：

```cpp
bool Log::SelectDebugEntry(const char* name, uint16_t divisor)
{
    return dbg_.Select(name, divisor);
}
```

### 3.5 `log list` 输出

有分频时在行尾追加，并用固定宽度保持对齐：

```text
imu     [ON]  [color]  x10    ← divisor=10
remote  [off] [vofa]   x1     ← divisor=1 也显示，宽度统一
shell   [off] [color]  x1
```

每行固定三段 + 末尾 `xN`，`%-5s` 对齐，**不省略 x1**：省略会导致列宽跳动，反而不好读。

`PrintLogList` 改：

```cpp
snprintf(line, sizeof(line), "%-15s %-5s %-8s x%u",
         e->name,
         (e == active) ? "[ON]" : "[off]",
         e->vofa ? "[vofa]" : "[color]",
         (unsigned)atomic_get(&e->divisor));
```

---

## 4 执行步表

> **拆成 4 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|----|------|------|----------|------|
| **1** | `framework/shell/log/log_debug.hpp` | `LogEntry` 加 `divisor`/`quota`；`Select(name, divisor=1)` 设置分频并清零计数；`FindOrCreate` 初始化新字段 | 无 | `log.cpp` 的 `SelectDebugEntry` 定义签名不匹配会编不过（步 2 修） |
| **2** | `framework/shell/log/log.cpp` | `SelectDebugEntry` 加第 2 参数；`PrintSelectedDebug` 开头加分频判断；`log on` 分支解析可选数字；`PrintLogList` 输出 `xN` | 步 1 | `log.hpp` 声明不匹配（步 3 修） |
| **3** | `framework/shell/log/log.hpp` | `SelectDebugEntry` 声明加 `uint16_t divisor = 1` | 步 2 | 无 |
| **4** | 全量检查 | `grep -rn "SelectDebugEntry"` 确认只有 2 处；`log on imu` / `log on imu 10` 两种写法都能编过 | 步 1~3 | 无 |

**编译状态**：第 1、2 步之间编不过（签名不一致），第 3 步后恢复。

**编译是用户的动作，AI 不代跑。**

---

## 5 验证清单

| # | 场景 | 预期 |
|---|------|------|
| 1 | `log on imu` | 全量打印（同现在），回 `log on: ok x1` |
| 2 | `log on imu 10` | 每 10 条打印 1 条，**第一条立即可见**，回 `log on: ok x10` |
| 3 | `log on imu 10` 后 `log list` | `imu [ON] [color] x10` |
| 4 | 从 `x10` 改回 `log on imu` | 恢复全量（divisor=1，quota 清零） |
| 5 | `log off` 再 `log on imu 10` | 从 1 重新计数 |
| 6 | 两个 DBG 名随机交替调用 | 各自计数互不干扰，各自按自己的 N 输出 |
| 7 | `log on imu 0` | 当作不分频（`x1`） |
| 8 | `log on imu abc` | 当作不分频（`x1`），不报错 |

---

## 6 拍板记录

| 项 | 结论 |
|---|---|
| 分频落点 | **A：调用点分频**（`PrintSelectedDebug` 开头） |
| 计数方式 | **递减配额**，不用 `%` 取余 |
| `divisor` 类型 | **`atomic_t`**（任务写 / ISR 读） |
| `quota` 类型 | **`atomic_t`**（任务写 / ISR 改） |
| `log on imu 0` | **静默按 1 处理**，不报错、不提示 |
| `log list` 格式 | 末尾追加 `xN`，**不省略 x1**，`%-` 对齐 |
