# DUST_LOG 四层重构规划

> 2026-10-25。目标：把 `log.cpp` / `log.hpp` 收成一个**纯组装文件**，把词法分发、加工、入队、发送四件事各自归位。
> 在原有三层（命令 / 队列 / 发送）之间**插入一层「加工层」**，承接上色、分频、帧包装。

---

## 1 现状与问题

### 1.1 文件规模

| 文件 | 行数 | 现状 |
|---|---|---|
| `log.cpp` | 423 | 混了命令处理、日志入口、加工、入队、发送五个职责 |
| `log.hpp` | 127 | 公共宏 + 全部函数声明 + 静态成员 + 内联转发，一锅炖 |
| `log_record.hpp` | 230 | 记录结构 + 参数快照 + 格式化 + **帧包装（WrapOutputFrame）** |
| `log_queue.hpp` | 324 | 记录队列 + 帧队列 |
| `log_transport.hpp` | 79 | 发送泵 |
| `log_policy.hpp` | 120 | 策略表 |
| `log_debug.hpp` | 160 | DBG 注册表 |

### 1.2 具体问题

| # | 问题 | 证据 |
|---|---|---|
| 1 | **`log.cpp` 里命令处理和日志处理混在一起** | `CmdList`/`CmdOn`/`CmdOff`/`CmdMode`/`CmdUsage`/`ProcessLogCommand`（约 100 行）与 `PrintInfo`/`PrintColoredLog`/`FormatOnePendingRecord` 同文件 |
| 2 | **「加工」散落在两处** | 同步段在 `PrintColoredLog` 里 `vsnprintf + WrapOutputFrame`；异步段在 `FormatOnePendingRecord` 里 `FormatRecordToText + WrapOutputFrame`。**同样的事写了两遍**，改前缀/颜色要改两处 |
| 3 | **分频藏在 DBG 入口** | `PrintSelectedDebug` 开头那 6 行配额判断，本质是「一条日志该不该输出」的过滤，属于加工职责，不该埋在入口函数体里 |
| 4 | **`log.hpp` 既是门面又是实现** | 公共宏、记录/队列/泵的访问器（`ActiveDebugEntry` / `SendNextFrameIfIdle` / `HandleTxDone`）、静态成员全挤在一起 |
| 5 | **帧包装函数位置错** | `WrapOutputFrame` 在 `log_record.hpp`（"记录"层）里，但它做的是**输出帧加工**，应在加工层 |

---

## 2 四层划分

```text
┌───────────────────────────────────────────────────────────┐
│ ① 命令层  log_cmd.hpp / .cpp                              │
│    词法解析 + 查表匹配 + 各子命令处理 + log list 输出      │
│    只依赖：加工层公开接口（SelectDebugEntry / SetDebugVofa）│
├───────────────────────────────────────────────────────────┤
│ ② 加工层  log_process.hpp / .cpp                          │
│    日志入口（Info/Error/Ok/Warning/Debug）、分频过滤、      │
│    格式化、上色、帧包装                                    │
│    产出：已格式化 + 已上色的文本                           │
│    只依赖：策略表、记录结构、DBG 注册表                     │
├───────────────────────────────────────────────────────────┤
│ ③ 队列层  log_queue.hpp（沿用）                           │
│    记录队列 recq_（原始请求）+ 帧队列 txq_（三档优先级）    │
│    只管存取、池管理、优先级插队、epoch 作废                │
├───────────────────────────────────────────────────────────┤
│ ④ 发送层  log_transport.hpp（沿用）                       │
│    Stream 绑定、DMA 空闲续发、TX_DONE 回调                  │
│    只管把 TxFrame 交给 Stream，不感知内容                   │
└───────────────────────────────────────────────────────────┘
```

**`log.hpp` / `log.cpp` → 配合类（facade）**：持有四层的静态实例，串起调用链，并保留全部对外宏与公开入口，使调用方零改动。

### 2.1 一次日志的完整流向

```text
外部：DUST_LOG_INF("x=%d", 1)          命令：log on imu 10
        ↓（宏）                              ↓
   Log::PrintInfo                      Log::ProcessLogCommand
        ↓                                    ↓
  ┌─ ② 加工层 ─────────────┐          ① 命令层查表 → CmdOn
  │ 取策略（color/prefix）  │                 ↓
  │ 分频过滤（DBG）         │          SelectDebugEntry(name, 10)
  │ 同步段：格式+上色+包装  │                 ↓（改注册表，不输出）
  │ 异步段：参数快照        │          ② 加工层 SendCommandLine
  └──────────┬──────────────┘                 ↓
             ↓                                │
  ┌─ ③ 队列层 ─────────────┐                  │
  │ recq_ 入队（异步）      │                  │
  │ txq_  入帧（帧包装后）  │←─────────────────┘
  └──────────┬──────────────┘
             ↓
  ┌─ ④ 发送层 ─────────────┐
  │ PumpTxQueue → Send      │
  └─────────────────────────┘
```

---

## 2.2 硬约束：只允许单向流水线，层间不许互相调用

> **规则（本规划的验收红线）**：四层之间**只允许「上层 → 紧邻下层」的单向调用**，禁止跳层、禁止反向、禁止同层互调。

### 允许

```text
① 命令层  →  ② 加工层  →  ③ 队列层  →  ④ 发送层
```

每一层**只能**调用它右边紧邻的那一层，且只能调**该层公开的方法**。

### 禁止

| 禁止形态 | 例子 |
|---|---|
| **跳层** | ① 命令层直接调 ③ 队列层或 ④ 发送层 |
| **反向** | ③ 队列层回调 ② 加工层；④ 发送层反向点 ② 的回调 |
| **同层互调** | `log_cmd.cpp` 里的 `CmdOn` 调 `CmdList` |
| **兄弟组件越权** | 加工层直接读改 `dbg_` 里别的条目状态（只能通过注册表公开方法） |

### 唯一的例外：④ → ② 的**回调**（不是调用）

```text
④ 发送层 HandleTxDone()  --give sem-->  shell 线程醒来
                                           ↓
                              ② 加工层 ExpandOneRecord()
```

这是**异步唤醒**，不是 ④ 调 ②：④ 只做 `k_sem_give`，具体谁醒来、做什么由 shell 线程决定。**④ 不 include ② 的任何头文件。**

### 允许的「横穿」：③ ④ 的共享数据结构

`TxFrame` / `TxFrameQueue` 定义在 `log_queue.hpp`，④ 需要它来弹帧。这不算跳层，因为：

- **③ 是 ④ 的数据定义者**，④ 依赖 ③ 的数据结构是**向下依赖**（上层定义、下层使用），符合流水线方向
- ④ **不调用 ③ 的方法**（`PopNextFrameLocked` 是队列自己提供的，由 ② 传递进来的引用访问）

> **实现约束**：`log_process.cpp` 持有 `recq_` / `txq_` 的引用并传给下层；下层**不持有**加工层的任何东西。

### 验收方式

每层 `.cpp` 的 `#include` 列表必须自证：

| 文件 | 允许 include | 禁止 include |
|---|---|---|
| `log_cmd.cpp` | `log_process.hpp` | `log_queue.hpp` / `log_transport.hpp` |
| `log_process.cpp` | `log_queue.hpp` / `log_transport.hpp` / `log_policy.hpp` / `log_record.hpp` / `log_debug.hpp` | `log_cmd.hpp` |
| `log_queue.hpp` | `log_record.hpp` / `log_policy.hpp` | `log_process.hpp` / `log_cmd.hpp` |
| `log_transport.hpp` | `log_queue.hpp` / `stream.hpp` | `log_process.hpp` / `log_cmd.hpp` |

**`log_cmd.cpp` 一旦出现 `#include "log_queue.hpp"` 就是违例。**

---

## 3 每层接口

### 3.1 ① 命令层 `log_cmd.hpp` / `.cpp`

从 `log.cpp` 原样搬出，**逻辑零改动**：

```cpp
namespace debug::logcmd {

void Process(uint8_t* line);                 // log 命令入口（DispatchSubCmd）
void CmdList(uint8_t* args);
void CmdOn(uint8_t* args);
void CmdOff(uint8_t* args);
void CmdMode(uint8_t* args);
void CmdUsage(uint8_t* args);

} // namespace debug::logcmd
```

**依赖方向**：只允许 `#include "log_process.hpp"`。要输出命令响应 → 调 `logproc::CommandLine()`；要改选中/模式 → 调 `logproc` 暴露的注册表操作，**不直接碰 `dbg_`**。

### 3.2 ② 加工层 `log_process.hpp` / `.cpp`

**这是本次新增的一层**，收拢"把一条日志变成可发送文本"的全部工作。

```cpp
namespace debug::logproc {

// —— 对外日志入口（宏的目标，从 Log 挪过来或由 Log 转发）——
void Info(const char* fmt, ...);
void Error(const char* fmt, ...);
void Ok(const char* fmt, ...);
void Warning(const char* fmt, ...);
void Debug(LogEntry* e, const char* fmt, ...);      // DBG：含分频过滤

// —— 命令响应直发 ——
void CommandLine(const char* text);

// —— 加工原语 ——
bool PassDivisor(LogEntry* e);                      // 分频过滤：true=放行
int  Wrap(const LogChannelPolicy& p, const char* text, char* out, size_t cap);   // 前缀+上色+\r\n
bool ExpandOneRecord();                             // shell 线程：出队→格式化→包装→入帧

// —— 向命令层暴露的注册表操作（命令层不许直接碰 dbg_）——
bool SelectDebugEntry(const char* name, uint16_t divisor = 1);
void DeselectDebugEntry();
bool SetDebugVofa(const char* name, bool on);
void PrintLogList();                                // 遍历输出（内部走 CommandLine）

} // namespace debug::logproc
```

**从别处搬进来的：**
- `WrapOutputFrame`（现在在 `log_record.hpp`）→ 改名 `Wrap`，进加工层
- `PrintColoredLog` 的同步/异步两条路径 → 合并成一个 `Emit(channel, fmt, ap)`
- `FormatOnePendingRecord` → `ExpandOneRecord`
- `PrintSelectedDebug` 的配额判断 → 抽成 `PassDivisor`

**加工层向下依赖**：`log_queue.hpp`（recq_/txq_）、`log_transport.hpp`（pump_）、`log_policy.hpp`、`log_record.hpp`、`log_debug.hpp`。
**四个静态实例（recq_ / txq_ / pump_ / dbg_）由加工层持有**，配合类只做转发，避免层间为了拿实例而横穿。

### 3.3 ③ 队列层 / ④ 发送层

**不动**。`log_queue.hpp`、`log_transport.hpp` 原样保留。

**只被紧邻的上层（加工层）调用**：
- ③ `recq_.Push / Pop / Release`、`txq_.Push / Pop / Release` —— 只有 `log_process.cpp` 调
- ④ `pump_.Bind / PumpTxQueue / HandleTxDone / Sending / Channel` —— 只有 `log_process.cpp` 和 `log.cpp`（`SendNextFrameIfIdle` 一行）调

**④ 的 `HandleTxDone` 是 shell 线程被唤起的入口**，由 `shell.cpp` 注册成 `UartDma` 的 `tx_cb` —— 这是**外部装配**，不是层间调用。

### 3.3.1 跨层数据流（唯一的传参方式）

```text
① → ②：只传「字符串」（命令名、name、N）
② → ③：只传「已加工好的文本 / 参数快照」
③ → ④：只传「TxFrame&」（队列自己弹出来的引用）
④ → Stream：只传「裸字节 + 长度」
```

**任何一层都不传上层/下层的对象指针或引用**（`dbg_` / `recq_` / `txq_` / `pump_` 的引用只在加工层内部串接，不外泄）。

### 3.4 配合类 `log.hpp` / `.cpp`

只剩「组合 + 对外门面」：

```cpp
class Log
{
public:
    // 四层实例（唯一实体，各层自身无静态状态）
    static inline DebugSourceRegistry dbg_  {};
    static inline LogTransport        pump_ {};
    static inline LogRecordQueue      recq_ {};
    static inline TxFrameQueue        txq_  {};
    static inline bool                shell_own_ = false;

    // 对外入口（转发到加工层）
    static void PrintInfo(const char* fmt, ...)    { logproc::Info(fmt, ...); }
    static void PrintError(const char* fmt, ...)   { logproc::Error(fmt, ...); }
    static void PrintOk(const char* fmt, ...)      { logproc::Ok(fmt, ...); }
    static void PrintWarning(const char* fmt, ...) { logproc::Warning(fmt, ...); }
    static void PrintSelectedDebug(LogEntry* e, const char* fmt, ...) { logproc::Debug(e, fmt, ...); }
    static void SendCommandLine(const char* text)  { logproc::CommandLine(text); }

    // 命令入口（转发到命令层）
    static void ProcessLogCommand(uint8_t* line)   { logcmd::Process(line); }

    // 发送层钩子（shell.cpp 直接调）
    static void BindOutputStream(Stream* s)        { pump_.Bind(s); }
    static void MarkShellThreadReady()             { shell_own_ = true; }
    static void SendNextFrameIfIdle();
    static void HandleTxDone();
    static bool FormatOnePendingRecord()           { return logproc::ExpandOneRecord(); }

    static bool Init();
};
```

**`log.cpp` 只剩两个函数**：`Init()`（重置四层）和 `SendNextFrameIfIdle()`（锁 + pump，2 行）。

---

## 4 重构后文件结构

> `log/` 目录下**只留门面**（`log.hpp` / `log.cpp`），四层各进自己的子目录。

```text
framework/shell/log/
├── log.hpp                     门面：宏 + Log 类（组装）     ~90 行（从 127 减）
├── log.cpp                     只留 Init / SendNextFrameIfIdle ~40 行（从 423 减）
│
├── cmd/                        ① 命令层
│   ├── log_cmd.hpp                                           ~25 行
│   └── log_cmd.cpp                                           ~110 行（搬迁）
│
├── process/                    ② 加工层
│   ├── log_process.hpp                                       ~50 行
│   └── log_process.cpp                                       ~180 行（搬迁+合并）
│
├── queue/                      ③ 队列层（挪目录，代码不动）
│   ├── log_queue.hpp                                        324 行
│   ├── log_record.hpp                                        ~200 行（移出 Wrap）
│   └── log_policy.hpp                                       120 行
│
└── transport/                  ④ 发送层（挪目录，代码不动）
    └── log_transport.hpp                                     79 行

（log_debug.hpp 归 ② 加工层，随 process/ 一起走 —— 它是加工层独占的注册表）
```

**净效果**：`log.cpp` 423 → ~40 行；新增两个子目录共 4 个文件；逻辑无新增，全是搬迁 + 去重。

### 4.1 目录与 include 路径

| 层 | 目录 | include 写法 |
|---|---|---|
| ① 命令 | `log/cmd/` | `#include "log_process.hpp"`（由 CMake 加 `process/` 到 include 路径） |
| ② 加工 | `log/process/` | `#include "log_queue.hpp"`、`"log_transport.hpp"` 等 |
| ③ 队列 | `log/queue/` | `#include "log_record.hpp"`、`"log_policy.hpp"`（同目录，直接相对） |
| ④ 发送 | `log/transport/` | `#include "log_queue.hpp"`、`"stream.hpp"` |

`framework/shell/CMakeLists.txt` 的 `target_include_directories` 需补：

```cmake
${CMAKE_CURRENT_SOURCE_DIR}/log/cmd
${CMAKE_CURRENT_SOURCE_DIR}/log/process
${CMAKE_CURRENT_SOURCE_DIR}/log/queue
${CMAKE_CURRENT_SOURCE_DIR}/log/transport
```

### 4.2 依赖关系图（自证单向）

```text
log.hpp（门面，无实现）
  ├─→ cmd/log_cmd.hpp        ──→ process/log_process.hpp
  └─→ process/log_process.hpp
        ├─→ queue/log_queue.hpp        ──→ queue/log_record.hpp, queue/log_policy.hpp
        ├─→ transport/log_transport.hpp ──→ queue/log_queue.hpp, stream.hpp
        ├─→ queue/log_policy.hpp
        ├─→ queue/log_record.hpp
        └─→ process/log_debug.hpp

cmd/log_cmd.cpp        只 include process/log_process.hpp              ✅
process/log_process.cpp include queue/* + transport/*                  ✅
queue/log_queue.hpp    只 include log_record.hpp / log_policy.hpp       ✅
transport/log_transport.hpp 只 include log_queue.hpp / stream.hpp       ✅
谁 include log_cmd.hpp？只有 log.hpp 和 log_cmd.cpp                     ✅
谁 include log_process.hpp？只有 log_cmd.cpp、log.hpp、log_process.cpp  ✅
```

**没有任何一条反向边。** 新增功能时照此判断：新代码要 include 的层，必须是紧邻下层。

**目录即约束**：`cmd/` 里出现 `#include "log_queue.hpp"`、`queue/` 里出现 `#include "log_process.hpp"`，一眼就是违例（跨了两个目录）。

---

## 5 顺带值得重构的点

| # | 问题 | 处理 |
|---|------|------|
| 1 | **同步段和异步段的"包装"重复** | 加工层合并为 `Emit()` 一个函数，内部分 sync/async 两条路，但 `Wrap` 只调一次 |
| 2 | `WrapOutputFrame` 在 `log_record.hpp` 里位置错 | 移到 `log_process.cpp`，`log_record.hpp` 只管记录结构与格式化 |
| 3 | `log.hpp` 里 `ActiveDebugEntry` / `FirstDebugEntry` / `NextDebugEntry` 三个内联转发 | `CmdList` 直接用 `dbg_`，**三个都删** |
| 4 | `PrintLogList` 属于命令层输出 | 搬到 `log_cmd.cpp` |
| 5 | `Log::Init()` 里的池重置逻辑 | 各层加 `Reset()`，`Init()` 只负责依次调（当前已是，保留） |
| 6 | `log.hpp` 的 `#ifdef CONFIG_DUST_CMD_SHELL_LOG` 包裹整块 | 保留（编译期裁剪，符合项目惯例） |

---

## 6 执行步表

> **拆成 11 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。

**排序原则：先建目录 + 挪文件（一次性，不动内容）→ 再拆代码 → 最后改 CMake。挪目录不改内容，可以整批做；拆代码必须一步一文件。**

| 步 | 文件 / 动作 | 具体动作 | 依赖前置 | 连带 |
|----|------------|---------|----------|------|
| **1** | 建目录 `log/cmd`、`log/process`、`log/queue`、`log/transport` | 只建空目录，不动文件 | 无 | 无 |
| **2** | 挪文件（不改内容） | `log_queue.hpp`/`log_record.hpp`/`log_policy.hpp` → `queue/`；`log_transport.hpp` → `transport/`；`log_debug.hpp` → `process/` | 步 1 | include 路径全断（步 11 改 CMake 修） |
| **3** | `shell/CMakeLists.txt` | `target_include_directories` 补四个子目录 | 步 2 | 无（此时工程恢复可编译） |
| **4** | `log/process/log_process.hpp`（新建） | 声明加工层接口（§3.2） | 步 3 | 无（只是声明） |
| **5** | `log/process/log_process.cpp`（新建） | 从 `log.cpp` 搬 `PrintColoredLog`→`Emit`、`PrintSelectedDebug`→`Debug`、`SendCommandLine`→`CommandLine`、`FormatOnePendingRecord`→`ExpandOneRecord`；从 `log_record.hpp` 搬 `WrapOutputFrame`→`Wrap`；抽出 `PassDivisor`、`SelectDebugEntry`/`DeselectDebugEntry`/`SetDebugVofa`/`PrintLogList` | 步 4 | `log.cpp` 里旧定义与新定义重名（步 6 删旧的） |
| **6** | `log/log.cpp` | 删除已搬到加工层的函数；`Log::PrintXxx` / `PrintSelectedDebug` / `SendCommandLine` 改为转发 `logproc::Xxx` | 步 5 | 无 |
| **7** | `log/queue/log_record.hpp` | 删除 `WrapOutputFrame`（已搬走） | 步 5 | 无（步 6 已改引用） |
| **8** | `log/cmd/log_cmd.hpp`（新建） | 声明命令层接口（§3.1） | 步 4 | 无 |
| **9** | `log/cmd/log_cmd.cpp`（新建） | 从 `log.cpp` 搬 `ProcessLogCommand`/`CmdList`/`CmdOn`/`CmdOff`/`CmdMode`/`CmdUsage`/`PrintLogList`；**只 include `log_process.hpp`** | 步 8 | `log.cpp` 重名（步 10 删） |
| **10** | `log/log.cpp` + `log/log.hpp` | 删除已搬到命令层的函数；`Log::ProcessLogCommand` 转发 `logcmd::Process`；删 `ActiveDebugEntry`/`FirstDebugEntry`/`NextDebugEntry` 三个内联转发 | 步 9 | 无 |
| **11** | `shell/CMakeLists.txt` | `target_sources` 加 `log/process/log_process.cpp`、`log/cmd/log_cmd.cpp` | 步 5、9 | 不加则链接失败 |

**两步 CMake 的原因**：步 3 只补 include 目录（挪文件后立即恢复可编译），步 11 才加新源文件。中间任何一步停下，工程都是可编译的。

**编译是用户的动作，AI 不代跑。**

---

## 7 验证清单

| # | 场景 | 预期 |
|---|------|------|
| 1 | `log list` | 输出不变（`name [ON] [color] x1`） |
| 2 | `log on imu 10` + 高频 DBG | 每 10 条 1 条，第一条立即可见 |
| 3 | `log mode imu vofa` | 切纯文本，无 ANSI |
| 4 | `var set xxx` 等命令响应 | 立刻输出（走 `CommandLine` 直发） |
| 5 | boot 早期日志（shell 未接管） | 正常直发 |
| 6 | 三档优先级混发 | Event 插队、Cmd 次之、Dbg 最后被挤 |
| 7 | `grep -c "" log.cpp` | ≤ 60 行 |
| 8 | 全量 `grep -rn "Log::" framework project` | 外部调用点零改动（宏与公开入口签名不变） |
| 9 | **单向依赖验收** | `grep -n "#include" log/cmd/*.cpp` 只有 `log_process.hpp` |
| 10 | **单向依赖验收** | `grep -n "#include" log/queue/*.hpp` 无 `log_process` / `log_cmd` / `log_transport` |
| 11 | **单向依赖验收** | `grep -n "#include" log/transport/*.hpp` 无 `log_process` / `log_cmd` |
| 12 | 目录结构 | `log/` 下只有 `log.hpp` / `log.cpp` + 四个子目录 |

---

## 8 已定设计（无需拍板）

| 项 | 结论 | 理由 |
|---|---|---|
| **命名空间** | `debug::logcmd`（命令层）、`debug::logproc`（加工层） | 层名即命名空间，跨层调用一眼看得出方向；塞进 `Log` 静态成员则层次模糊 |
| **`PrintLogList` 归属** | **加工层** `logproc::PrintLogList()`，命令层只调它 | 它要遍历 `dbg_` 注册表，而 `dbg_` 归加工层持有。放命令层就得直接碰 `dbg_` → 违反单向依赖 |
| **四个静态实例** | **加工层独占**（`dbg_`/`recq_`/`txq_`/`pump_`），`Log` 只转发 | 命令层拿不到实例，天然满足单向约束；上层越权无从下手 |
| **`shell_own_`** | **随四个实例一起进加工层** | 它决定"同步直发还是异步入队"，是加工层的分支条件，不是门面的状态 |
| **`log_debug.hpp`** | 归 `process/` 目录 | DBG 注册表只被加工层使用（分频、选中、遍历） |
