# DUST_LOG 调试台架构与异步化总结

> 2026-08-10 汇总。本文档是 DUST_LOG 调试台（log / var / shell）+ 异步化改造 + 发布职责重构的**最终综合文档**，替代之前的分散规划文档（log异步化规划.md / log异步化阶段3参数快照规划.md / log发布职责重构规划.md / log命令响应直发规划.md / log命令响应回退规划.md / 调试台DUST_LOG融合介绍.md，已删除合并）。
>
> 一句话：**一套自研的 DMA 日志系统与自维护 UART shell 融合的统一调试台——四色一次性日志 + DBG 可选择流式 + 分时段同步异步发送 + 参数快照异步格式化 + 三档优先级帧队列仲裁。替代 Zephyr log，根治 uart3 回调槽冲突卡死。**

---

## 目录

1. [背景与目标](#1-背景与目标)
2. [模块总览与依赖链](#2-模块总览与依赖链)
3. [数据结构与常量](#3-数据结构与常量)
4. [核心机制：三档优先级 + 链表入队](#4-核心机制三档优先级--链表入队)
5. [核心机制：分时段同步异步发送](#5-核心机制分时段同步异步发送)
6. [核心机制：发布职责分离](#6-核心机制发布职责分离)
7. [核心机制：参数快照异步格式化](#7-核心机制参数快照异步格式化)
8. [发送链与并发保护](#8-发送链与并发保护)
9. [初始化与线程](#9-初始化与线程)
10. [命令系统](#10-命令系统)
11. [容量模型与实测数据](#11-容量模型与实测数据)
12. [修复记录](#12-修复记录)
13. [演进历史与决策轨迹](#13-演进历史与决策轨迹)
14. [验证记录](#14-验证记录)
15. [FAQ](#15-faq)

---

## 1. 背景与目标

### 1.1 直接原因：uart3 回调槽冲突卡死

- HPM UART 驱动异步 API 只有 **1 个回调槽** `idle_user_callback`（[uart_hpmicro.c:1091](E:/Zephyr_HPMicro/sdk_glue/drivers/serial/uart_hpmicro.c#L1091) 直接覆盖）
- Zephyr log 后端（`LOG_BACKEND_UART_ASYNC=y`）先注册到 uart3
- shell 的 `dbg_init` 用 `UartDma` 初始化同一 uart3 → `uart_callback_set` 覆盖 log 回调 → log 后端 `k_sem_take(K_FOREVER)` 永等 UART_TX_DONE → **main 永久阻塞 → CPU 进 wfi 空闲**

### 1.2 对 Zephyr log 的三点不满

1. **log 没用 DMA**：`LOG_BACKEND_UART_ASYNC` 实际轮询发送慢
2. **与 shell 抢资源**：共用 uart3，互相踩
3. **无颜色**：黑/红/绿/橘无法区分等级

### 1.3 异步化动机（阶段2/3）

普通日志在中断/实时场景调用时，原实现调用点做 `vsnprintf` + DMA 提交 + ISR 续发——ISR 被拉长（picolibc 无 %f 短日志也几十 µs）。目标：**调用点只做极轻量入队，格式化与 DMA 发送由后台线程收尾**。

---

## 2. 模块总览与依赖链

| 模块 | 文件 | 职责 | Kconfig 符号 |
|------|------|------|-------------|
| log | [log.hpp](../framework/cmd/shell/log.hpp) / [log.cpp](../framework/cmd/shell/log.cpp) | Log 类 + DUST_LOG 宏族 + 三档优先级帧队列仲裁 + 参数快照异步格式化 + 分时段发送 | `DUST_CMD_SHELL_LOG` |
| var | [var.hpp](../framework/cmd/shell/var.hpp) / [var.cpp](../framework/cmd/shell/var.cpp) | 调试变量注册（`.shell_var` 链接段收集）+ var list/get/set | `DUST_CMD_SHELL_VAR` |
| shell | [shell.hpp](../framework/cmd/shell/shell.hpp) / [shell.cpp](../framework/cmd/shell/shell.cpp) | UART 线程底座（接收循环 + 命令分发 + PumpSend 驱动） | `DUST_CMD_SHELL` |

**依赖链**：

```
USE_CMD_SHELL → select DUST_CMD_SHELL_LOG ─┐
             → select DUST_CMD_SHELL_VAR ─┴→ DUST_CMD_SHELL → DUST_COM_UART_DMA
```

- `DUST_CMD_SHELL` 公共底座（线程 + 分发），`DUST_CMD_SHELL_LOG`/`DUST_CMD_SHELL_VAR` 各自 select 它
- `log` 依赖 `shell`：`Log::Init()`/`Log::BindStream()` 在 shell `dbg_init` 里执行（`stream_ == nullptr` 时发送静默丢弃）；`log` 命令走 shell `ProcessLine`

**通道抽象（Stream）**：shell/log 都持 `Stream*` 而非 `UartDma*`——`UartDma`、未来的 `Usb`、`RS485` 都是 `Stream` 子类，调试通道可整体替换。发送路径唯一（log 与 shell 共用 `stream_` DMA 通道，`tx_cb = Log::OnTxDone`），不碰 Zephyr `uart_callback_set` 槽位。

---

## 3. 数据结构与常量

### 3.1 常量

| 常量 | 值 | 含义 |
|------|-----|------|
| `kTxFrameSize` | 128 | 帧数据区大小（含尾部 `\0` 保险） |
| `kTxMaxLen` | 127 | 发送最长字节长度，超长截断 |
| `kTxPoolCount` | **8** | 帧池帧数（8×128B = 1KB，2026-08-10 由 4 扩到 8：命令响应连发突发缓冲） |
| `kMaxLogEntries` | 64 | DBG 条目数上限 |
| `kMaxLogArgs` | 4 | 参数快照槽上限（`%f` 占 2 槽） |
| `kMaxLogRecords` | 8 | LogRecord 原始请求队列深度（8×26B=208B） |
| `kNullIndex` | 255 | 链表空值（数组索引，255=无下一帧） |

### 3.2 颜色与优先级

```cpp
enum class LogColor : uint32_t    // TrueColor 24 位 RGB，输出 \x1b[38;2;R;G;Bm
{
    Black = 0x000000, Red = 0xF50002, Green = 0x00F700, Orange = 0xF6A753, White = 0xFFFFFF,
};
enum class TxPriority : uint8_t
{
    Event = 0,   // INF/ERR/OK/WRN：最高，插队头，永不挤
    Cmd   = 1,   // 命令响应（var/log 输出）：中，插事件后、DBG 前
    Dbg   = 2,   // DBG 流式：最低，排队尾，先被挤
};
```

### 3.3 结构

```cpp
struct TxFrame        // 发送帧（三档共用，已格式化）
{
    char       data[kTxFrameSize];   // 格式化后内容（含 ANSI 颜色）
    uint16_t   len;                  // 实际有效长度（≤127）
    TxPriority prio;                 // 优先级档位
    bool       is_stale;             // DBG 切换时标记作废
    uint8_t    next;                 // 帧索引链表（255=nullptr）
};

struct LogEntry       // DBG 日志条目
{
    const char* name;   // DBG 名字（log on 用，运行时 FindOrCreate 创建）
};

struct LogRecord      // 日志原始请求（未格式化，异步段生产队列条目）
{
    const char* fmt;               // 格式串（静态字面量，禁止临时栈串）
    LogColor    color;             // 颜色
    uint32_t    args[kMaxLogArgs]; // 参数快照（%f 占 2 槽，%s/%p 指针 1 槽，整型 1 槽）
    uint8_t     nargs;             // 参数槽数（0~kMaxLogArgs）
    uint8_t     next;              // 记录链表（255=nullptr）
};
```

---

## 4. 核心机制：三档优先级 + 链表入队

### 4.1 三类链，零动态分配

整个日志系统队列全部是**静态数组 + 数组索引链表**（`uint8_t next` 存数组下标，255=空），运行期不 new/malloc。

| 链 | 成员 | 用途 |
|----|------|------|
| 帧空闲链 | `free_head_` | 可用空 TxFrame（`AllocFrame` 摘头、`RecycleFrame` 归还头插） |
| 帧发送队列 | `tx_head_/tx_tail_` | 已格式化待发送帧，**按三档优先级有序** |
| LogRecord 队列 | `rec_head_/rec_free_` | 未格式化请求，**严格 FIFO**（尾插） |

### 4.2 EnqueueByPrio：链表按档插队

```cpp
void Log::EnqueueByPrio(TxFrame* f)
{
    f->next = kNullIndex;
    if (tx_head_ == kNullIndex) { tx_head_ = tx_tail_ = static_cast<uint8_t>(f - tx_pool_); return; }

    uint8_t* pp = &tx_head_;              // 找插入点：第一个 prio > f->prio 的帧之前
    while (*pp != kNullIndex && tx_pool_[*pp].prio <= f->prio)
        pp = &tx_pool_[*pp].next;

    uint8_t idx = static_cast<uint8_t>(f - tx_pool_);
    f->next = *pp;
    *pp = idx;
    if (f->next == kNullIndex) tx_tail_ = idx;
}
```

- Event(0) 插队头、Cmd(1) 插事件后/DBG 前、Dbg(2) 排队尾
- **同档先来后到**：`while (prio <= f->prio)` 跳过同档，插到第一个更高 prio（即更低优先）之前
- 单链表"跳找插入点 + 指针重链"，`pp` 是指向前帧 next 字段的指针

### 4.3 EvictLowest：池满挤帧

- 找 prio 值最大（最低优先）的非 Event 帧摘除，优先挤 DBG、其次 Cmd
- **事件永不挤**；全是事件帧返回 nullptr（丢弃新帧）
- 注意：**同档（如全 Cmd）时挤队头最先入队的**——命令连发超帧池容量时先入队的被挤（见 §13 演进）

### 4.4 Dequeue：队列弹头

弹 `tx_head_`；`is_stale` 作废帧跳过回收（DBG 切换及时顶替：`log on B` 时残留 A 帧作废）。

---

## 5. 核心机制：分时段同步异步发送

shell 线程 `PreThread` 阶段启动，boot 早期 shell 没跑，若调用点只入队会无人消费（帧积压满吞日志）。用 `shell_own_` 标志分两个时段：

| 时段 | `shell_own_` | 调用点行为 | 实时要求 |
|------|-------------|-----------|---------|
| **同步段**（boot 早期） | `false` | `vsnprintf` + 入帧池 + 调用点直发 | 无（启动阶段） |
| **异步段**（shell 接管后） | `true` | 参数快照入 LogRecord 队列 + give，不格式化 | 有（ISR/实时可能） |

- `shell_own_` 在 `Shell::Task()` 首行 `Log::SetShellOwn()` 置 true，只执行一次
- **同步段日志即时出、不积压**（避免 boot 日志吞帧——阶段2 回归诊断的教训）
- **异步段 ISR 只做快照+入队+give**（~µs 级）

**Dbgl 特例**：boot 早期 DBG 直接 `return`（早期不选 DBG、也不会用），只有接管后且选中才走参数快照。

---

## 6. 核心机制：发布职责分离

`TrySend` 曾一个函数混了"入帧池 + 时段 + 命令标志"三个职责，重构拆成三个明确原语：

| 原语 | 语义 | 尾部动作 | 调用者 |
|------|------|---------|--------|
| `EnqueueFrame(data,len,prio)` | 锁内公共：截断+取帧+入队 | 返回 bool（true=已入队） | 由 Publish 原语持锁调用 |
| `PublishDirect(data,len,prio)` | 直发：入帧池+入队+**立即 SendFrame** | DMA 空闲即直发（帧即取即还） | 命令响应、boot 早期日志 |
| `PublishQueued(data,len,prio)` | 异步：入帧池+入队+**give** | give 让 shell 泵 | shell 线程 FormatRecord |

```cpp
bool Log::EnqueueFrame(const char* data, int len, TxPriority prio)
{
    if (len > kTxMaxLen) len = kTxMaxLen;
    TxFrame* f = AllocFrame();
    if (f == nullptr)
    {
        if (k_is_in_isr()) return false;          // ISR：池满直接丢
        f = EvictLowest();
        if (f == nullptr) return false;           // 全是事件帧（极端）→ 丢
    }
    memcpy(f->data, data, static_cast<size_t>(len));
    f->len = static_cast<uint16_t>(len);
    f->prio = prio;
    f->is_stale = false;
    EnqueueByPrio(f);
    return true;
}

void Log::PublishDirect(const char* data, int len, TxPriority prio)
{
    unsigned key = irq_lock();
    if (EnqueueFrame(data, len, prio))
        if (!sending_) { TxFrame* next = Dequeue(); if (next != nullptr) SendFrame(next); }
    irq_unlock(key);
}

void Log::PublishQueued(const char* data, int len, TxPriority prio)
{
    unsigned key = irq_lock();
    if (EnqueueFrame(data, len, prio))
        if (!sending_ && stream_ != nullptr) k_sem_give(&stream_->sem_);
    irq_unlock(key);
}
```

**直发 vs 异步的本质**：帧都先入队排序，直发是"入队后当场从队头发走"（DMA 空闲即发，忙则留给线程收尾），异步是"入队后直接交给线程泵"。差别只在"谁把帧从队头取出来、什么时候取"。

**最终映射**（当前代码）：

| 调用点 | 原语 | 优先级 | 说明 |
|--------|------|--------|------|
| `SendLine`（命令响应） | `PublishDirect` | Cmd | 调用点直发（帧池 8 缓存） |
| `PrintColor` 同步段 | `PublishDirect` | Event | boot 早期直发 |
| `FormatRecord` | `PublishQueued` | Event | 线程展开后异步入队泵 |
| `PrintColor`/`Dbgl` 异步段 | rec 队列 + give | — | 快照入队，线程展开 |

---

## 7. 核心机制：参数快照异步格式化

**目标**：异步段调用点（可能 ISR）连 vsnprintf 都不做，只把 `fmt + 参数值` 快照入 LogRecord 队列，格式化在 shell 线程。

**为什么快照而非存 va_list**：`va_list` 是栈指针，调用点函数一返回栈即回收，跨线程延迟使用是野指针。快照存的是**值拷贝**，不依赖调用点栈存活。

### 7.1 SnapshotArgs：参数值快照（调用点，va_end 前）

```cpp
void Log::SnapshotArgs(const char* fmt, va_list ap, uint32_t* args, uint8_t* nargs)
{
    uint8_t n = 0;
    while (*fmt && n <= kMaxLogArgs)   // n==kMaxLogArgs 时仍扫（消费参数防失步），不写槽
    {
        if (*fmt != '%') { fmt++; continue; }
        fmt++; if (*fmt == '%') { fmt++; continue; }   // "%%" 转义
        // 跳过 flags/width/.prec/length
        switch (spec)
        {
            case 'd': case 'i': case 'u': case 'x': case 'X': case 'c':
                if (n < kMaxLogArgs) args[n++] = static_cast<uint32_t>(va_arg(ap, int));
                else                 (void)va_arg(ap, int);       // 超限：仍消费
                break;
            case 'f': case 'F':
            {
                double d = va_arg(ap, double);
                if (n + 1 < kMaxLogArgs) { /* memcpy 拆低/高 32 位存 2 槽 */ }
                break;
            }
            case 's': case 'p':
                if (n < kMaxLogArgs) args[n++] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(va_arg(ap, void*)));
                else (void)va_arg(ap, void*);
                break;
        }
        if (*fmt) fmt++;
    }
    *nargs = (n <= kMaxLogArgs) ? n : kMaxLogArgs;
}
```

- **类型严格匹配 va_arg 防失步**；超 `kMaxLogArgs` 停止写槽但继续消费参数
- `%f` 占 2 槽（double 位模式，little-endian 低/高 32 位）
- 支持转换符：`%d %i %u %x %X %c %f %F %lf %s %p`；禁用 `%lld/%e/%g/%n`
- **`%s` 约束**：快照存指针，延迟格式化要求指针仍有效——**只许字符串字面量**（临时栈数组会读野指针，最高风险）

### 7.2 TryEnqueueRecord / DequeueRecord

```cpp
void Log::TryEnqueueRecord(const char* fmt, LogColor color, const uint32_t* args, uint8_t nargs)
{
    unsigned key = irq_lock();
    if (rec_free_ == kNullIndex) { irq_unlock(key); return; }   // 池满直接丢（O(1)）
    // 取 rec_free_ 头 → 写字段 → 尾插（FIFO 保序）
    irq_unlock(key);
}

LogRecord* Log::DequeueRecord()   // shell 线程弹 rec_head_；归还空闲链进锁（防与 ISR Enqueue 竞争）
```

### 7.3 FormatRecord：消费端展开（shell 线程，不依赖 libc va_list）

- 扫 fmt：普通字符直接拷；`%` 段收集到 seg，按转换符从 args 取槽
  - `%d/%u/...` → `snprintf(seg, int)`；`%f` → memcpy 2 槽拼回 double → `snprintf(seg, double)`
  - `%s` → 指针还原，null 转 `"(null)"`；`%p` → 指针还原
- ANSI 上色 + `\r\n` → `PublishQueued(Event)`
- 展开在 shell 线程，用 picolibc snprintf 逐段（%f 精度由 libc 保证）

---

## 8. 发送链与并发保护

### 8.1 发送链

```
SendFrame(f)                  // stream_->Send 提交 DMA，Send 后 RecycleFrame（帧即取即还）
OnTxDone()                    // TX_DONE ISR：清 sending_ + give stream_->sem_（不再 ISR 续发）
PumpSend()                    // shell 线程：DMA 空闲则 Dequeue + SendFrame
```

**帧即取即还**：`Stream::Send`（UartDma::Send）内部把帧内容拷进自身缓冲，DMA 搬运的是 Stream 缓冲而非 TxFrame——Send 提交成功即帧使命结束，立即回空闲池，**帧池永不枯竭**。

### 8.2 并发保护

| 入口 | 锁 | 说明 |
|------|----|------|
| `EnqueueFrame`（被 Publish 调用） | 调用者持 `irq_lock` | 截断+取帧+入队 |
| `PublishDirect`/`PublishQueued` | `irq_lock` | 入队 + 直发/give |
| `OnTxDone` | `irq_lock`（清标志） | give 在锁外 |
| `PumpSend` | `irq_lock` | Dequeue+SendFrame |
| `TryEnqueueRecord` | `irq_lock` | rec 入队 |
| `DequeueRecord` | `irq_lock`（归还需） | 防与 ISR Enqueue 竞争 |
| `MarkStaleDbg` | `irq_lock` | DBG 切换标记 |

加锁点集中在入口，内部 helper（AllocFrame/RecycleFrame/EnqueueByPrio/EvictLowest/Dequeue/SendFrame）只在锁内调用。

---

## 9. 初始化与线程

```
REGISTER_INIT  (thread_init,  PreInit, High, HaltOnFail, "dbg_init")    ← PreInit 初始化 uart3 + Log
REGISTER_THREAD(thread_start, PreThread, "dbg_start")                    ← PreThread 启动 shell 线程

thread_init:
  UartDma::Config cfg; cfg.base_cfg.tx_cb = Log::OnTxDone;
  rx.Init(DEVICE_DT_GET(DT_ALIAS(shell_uart)), cfg)   ← shell-uart = uart3
  shell_.Init(rx);  DUST_LOG_INF("shell init\n");
  Log::Init()          ← 清 active_/count_/sending_，重建空闲帧链 + rec 空闲链
  Log::BindStream(&rx)

Shell::Task():
  Log::SetShellOwn();  DUST_LOG_OK("shell send owner taken\n");
  for (;;)
  {
      k_sem_take(&stream_->sem_, K_FOREVER);   // 通道事件：接收 OR 日志发送需要驱动
      Log::PumpSend();                          // 先驱动日志发送
      while (auto* r = Log::DequeueRecord())    // 出队参数快照 → 展开 → 泵
      { Log::FormatRecord(r); Log::PumpSend(); }
      uint16_t n = stream_->Read(buf, sizeof(buf));
      if (n == 0) continue;
      ...逐行解析...
  }
```

**sem_ 语义**：从"接收通知信号量"升级为"通道事件信号量"（接收数据 OR 日志发送需要驱动）。limit=1 计数，接收/发送一起 give 合并成一次唤醒——但安全：一次唤醒顺序执行 PumpSend+FormatRecord+Read，数据不依赖 sem_ 计数（在 Stream 缓冲/队列里）。**前提**：shell 线程是 `stream_` 的唯一消费者。

---

## 10. 命令系统

```
h / ?                 帮助（7 行）
var list              列出所有调试变量（类型 + 当前值 + 尾部计数 --- N variables ---）
var get <name>        查看单变量
var set <name> <val>  修改变量（11 种类型：u8~u64/i8~i64/float/double/bool）
log list              列出所有 DBG 条目（名字 + [ON]/[off]）
log on <name>         选中某条日志流式打印（同一时间只打一条）
log off               停止打印
```

- 变量注册 `REGISTER_SHELL_VAR("name", var)`：编译期 `.shell_var` 链接段收集，**支持类成员变量**（取地址 + TypeMap 推导）
- 命令响应走 `SendLine`（Cmd 档，`PublishDirect` 直发）

**命令响应直发决策**（演进见 §13）：命令响应只在 shell 线程命令处理时调用（Var::Process/Log::Process/CmdHelp），非 ISR/实时路径，**直发保证即时出**；帧池 8 缓冲突发。

---

## 11. 容量模型与实测数据

### 11.1 工程公式（帧池 8）

```
B_max = K + 1               同一时间允许的最大日志调用数（1 DMA 中 + K 排队）
K_min = max(1, B - 1)       给定突发需求 B 所需最少帧数
不丢帧 ⇔ R_p < baud/10 且 B ≤ K + 1    （稳态吞吐 + 瞬时突发）
```

**帧池 8 代入**：`K=8 → B_max = 9`。瞬时突发 ≤9 条不丢，**第 10 条起必丢（公式预言，非偶发）**。

### 11.2 实测（2026-08-06，921600 波特；帧池 4 时）

| 项 | 实测值 |
|----|--------|
| 单帧周期（30/64/127B） | 371 / 743 / 1430us（理论 330/694/1378），回归 T = 38.2 + 10.96×len |
| 固定开销/帧 | ~40us |
| 瞬时容量（4 帧池） | 最多 5 条同时发送，第 6 条起丢（事件永不挤） |
| 无限闭环 | 稳态 <92160 B/s 零丢帧；满速（>92KB/s）丢帧 60% = 容量边界 |

**帧池扩到 8 + 命令直发后**（2026-08-10）：命令响应走 `PublishDirect`（帧即取即还，不排队占池）。按公式 `B_max = 8+1 = 9`：
- `h`（7 条 ≤ 9）→ **不丢**
- `var list`（17 条 > 9）→ **必然丢帧**（实测丢 1 帧，公式预言一致）

**工程建议**：日志内容 <98B（127B 上限含 ~29B TrueColor ANSI 开销）；瞬时突发 ≤9 条（8 帧池）不丢，超出按公式丢帧；`var list`（17 条极端突发）会丢——正常业务不会出现这种 17 条极端，接受该容量边界；持续输出限速 <92KB/s。

---

## 12. 修复记录

| 问题 | 根因 | 修复 |
|------|------|------|
| 连续发送丢尾（`pwm ready`→`pwm r`） | HPM 驱动 TX_DONE 由 DMA 完成触发（≠FIFO 排空）+ `uart_hpm_tx` 无条件 `uart_reset_tx_fifo` | [uart_hpmicro.c:1267-1276](E:/Zephyr_HPMicro/sdk_glue/drivers/serial/uart_hpmicro.c#L1267)：TX 完成回调里 `async_evt_tx_done` 前忙等 `LSR & TEMT` |
| 连发丢帧（第 3/5 条起） | **帧泄漏**：SendFrame 只在失败时回收，成功路径永不归还 → 池耗尽 | SendFrame 成功/失败都 RecycleFrame（帧即取即还） |
| `log on nope` 创建幽灵条目 | Select 用 FindOrCreate 凭空创建 | Select 只选中已存在条目，不存在回 not found |
| boot 日志慢 + 吞 | 阶段2 发送移入 shell 线程但 shell LateThread 才启动，帧积压满后 Early/Mid/Late 阶段被吞 | 分时段方案 B：`shell_own_`，boot 早期调用点直发 |
| 命令响应被 Event 挤丢（help 丢 var get/set） | 命令帧 Cmd 档入队等泵，Event 风暴池满 Evict 挤 Cmd | 发布职责分离 + 命令改直发 + 帧池 8 |

---

## 13. 演进历史与决策轨迹

| 阶段 | 内容 | 决策 |
|------|------|------|
| 阶段1 | shell stream 化：`UartDma*`→`Stream*`、`BindStream`、`Init(Stream&)` | 可换 USB/RS485 通道 |
| 阶段2 | 发送驱动移入 shell 线程（PumpSend），复用线程不新建 | ISR 只入队+give，省 1KB 栈 |
| 回归诊断 | boot 日志慢+吞（shell LateThread 启动前无消费者） | 方案 B：`shell_own_` 分时段（boot 直发/接管后异步） |
| 阶段3 | 参数快照异步：调用点 SnapshotArgs→LogRecord，线程 FormatRecord | ISR 连 vsnprintf 都不做 |
| 发布重构 | TrySend 拆 EnqueueFrame/PublishDirect/PublishQueued | 职责分离：命令直发 vs 日志异步 |
| 直发方案 | SendLine 改 PublishDirect（force_direct） | 修复命令响应被 Event 挤 |
| 回退 | 帧池 4→8 + 命令改回 PublishQueued | 实测线程发送丢更多，改回直发；帧池 8 保留 |
| **最终** | 帧池 8 + SendLine 直发 + 分时段 + 参数快照 + 职责分离 | 压力测试 PASS 36/0 |

**关键教训**：
- 命令响应（shell 线程调用，非实时）**该直发**；普通日志（可能 ISR）**该异步**——诉求相反，必须分开。
- 命令连发走直发（帧即取即还）：`h`（7 条）全量、`var list`（17 条）极端突发丢 1 帧——正常业务不会出现，接受。

---

## 14. 验证记录

| 项 | 结果 |
|----|------|
| 帧池极限性能测试 | ✅ 全过（§11.2） |
| shell 命令暴力测试（log_var_stress.py，120s 风暴 2382 条命令） | ✅ **PASS 36 / FAIL 0**（握手、var 矩阵、log 命令、DBG 流式优先级、风暴存活、风暴后恢复） |
| 命令响应不被 Event 挤 | ✅ 握手 `h` 收到 var list，命令响应直发生效 |
| 类成员变量注册 | ✅ REGISTER_SHELL_VAR 对类成员取地址 + TypeMap 推导 |
| 异步段 ISR 不阻塞 | ✅ 参数快照 + 线程格式化 |

---

## 15. FAQ

**Q：DUST_LOG 打印依赖线程吗？**
A：分时段。boot 早期调用点直发（不依赖）；shell 接管后普通日志走参数快照 + shell 线程格式化/泵（依赖）。命令响应永远直发。

**Q：为什么线程里不用 vsnprintf，要 FormatRecord 逐段拼？**
A：vsnprintf 需要 `va_list`，而 va_list 是栈指针、调用点返回即失效，传不到线程。快照存的是参数值，FormatRecord 只能按槽位约定用 snprintf 逐段展开。

**Q：为什么命令响应用直发，普通日志用异步？**
A：命令响应只在 shell 线程命令处理时调用，非 ISR/实时——直发即时出、不被 Event 挤；普通日志可能 ISR 调用——异步让 ISR 只入队不阻塞。

**Q：`%s` 有坑吗？**
A：有。参数快照存指针，延迟格式化要求指针仍有效——只许字符串字面量。临时栈数组会读野指针（最高风险）。

**Q：`sem_` 是 limit=1，接收和发送一起 give 会不会丢事件？**
A：不会。合并成一次唤醒但顺序执行 PumpSend+FormatRecord+Read，数据在队列/缓冲里不依赖 sem_ 计数。

**Q：帧池为什么是 8？**
A：2026-08-10 由 4 扩到 8（8×128B=1KB）：普通日志异步段突发缓冲。命令响应走直发不占池（帧即取即还）。

**Q：var list（17 条）会丢吗？**
A：8 帧池下极端突发（17 条连发）**实测丢 1 帧**（8 帧突发上限 ~9 条，`var list` 17 条超容）。`h`（7 条）全量。正常业务不会出现 17 条极端突发，接受该容量边界。

**Q：CONFIG_DUST_CMD_SHELL_LOG 关闭时调用 DUST_LOG_* 会怎样？**
A：宏为空（#else 空宏），调用点零开销、编译不报错——与 LOG=n 静默语义一致。

---

## 相关

- 代码：framework/cmd/shell/（log.hpp/log.cpp、shell.hpp/shell.cpp、var.hpp/var.cpp）
- 通道抽象：framework/drivers/communication/stream/stream.hpp
- 压力测试：scripts/log_var_stress.py
- 环境：COM21 @ 921600，MobaXterm
