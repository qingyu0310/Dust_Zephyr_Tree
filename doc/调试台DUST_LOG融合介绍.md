# DUST_LOG 调试台（自研 DMA 日志 + shell 统一调试台）

> 日期：2026-08-06
> 状态：**已实现 ✅**
> 一句话：**一套自研的 DMA 日志系统（DUST_LOG）与自维护 UART shell 融合的统一调试台——INF/ERR/OK/WRN 四色一次性日志调用即 DMA 打印，DBG 日志独立注册、默认静默、`log on <name>` 选中才流式打印（同一时间只打一条），`var`/`log` 子命令统一调试。替代 Zephyr log，根治 uart3 回调槽冲突卡死。**

---

## 0. 简介

DUST_LOG 调试台是运行在 Zephyr 子树上的一套自研调试系统，由三个模块组成（均在 `framework/cmd/shell/`）：

| 模块 | 文件 | 职责 |
|------|------|------|
| **log** | log.hpp / log.cpp | Log 类 + DUST_LOG 宏族 + 帧池三档优先级发送仲裁 |
| **var** | var.hpp / var.cpp | 调试变量注册（链接段收集）+ var list/get/set 命令 |
| **shell** | shell.hpp / shell.cpp | UART 线程底座（接收循环 + 命令分发）+ 启动初始化 |

它解决的核心问题：**uart3 回调槽冲突卡死**（Zephyr log 后端与 shell 抢同一个 UART 异步回调槽，上电卡死在 `dbg_init`）——彻底弃用 Zephyr log，log 与 shell 共用一条 DMA 发送路径，天然无冲突。

---

## 1. 背景（为什么做）

### 1.1 直接原因：uart3 回调槽冲突卡死

- HPM UART 驱动异步 API 只有 **1 个回调槽** `idle_user_callback`（[uart_hpmicro.c:1091](E:/Zephyr_HPMicro/sdk_glue/drivers/serial/uart_hpmicro.c#L1091) 直接覆盖）
- Zephyr log 后端（`LOG_BACKEND_UART_ASYNC=y`）先注册到 uart3
- shell 的 `dbg_init` 用 `UartDma` 初始化同一 uart3 → `uart_callback_set` 覆盖 log 回调 → log 后端 `k_sem_take(K_FOREVER)` 永等 UART_TX_DONE → **main 永久阻塞 → CPU 进 wfi 空闲**

### 1.2 用户对 Zephyr log 的三点不满

1. **log 没用 DMA**：`LOG_BACKEND_UART_ASYNC` 实际轮询发送慢
2. **与 shell 抢资源**：Zephyr log 与自研 shell 共用 uart3，互相踩
3. **无颜色**：黑/红/绿/橘无法区分等级

### 1.3 设计决定

- 弃用 Zephyr log 后端，自研 DUST_LOG，与 shell 融合成统一调试台
- **INF/ERR/OK/WRN 是一次性事件日志**：调用即打，无名字、无选择机制
- **DBG 是唯一可选择的日志**：带名字注册、默认静默、`log on <name>` 选中 → 流式 DMA 打印，同一时间只打一条
- 发送帧队列三档共用（事件 > 命令响应 > DBG），按优先级插队

---

## 2. 系统架构

### 2.1 模块依赖链（Kconfig）

```
USE_CMD_VAR → DUST_CMD_SHELL_VAR ─┐
                                  ├→ DUST_CMD_SHELL → DUST_COM_UART_DMA
USE_CMD_LOG → DUST_CMD_SHELL_LOG ─┘
```

- `DUST_CMD_SHELL`：公共底座（线程 + 命令分发），select `DUST_COM_UART_DMA`
- `DUST_CMD_SHELL_VAR`：var 命令族，select `DUST_CMD_SHELL`
- `DUST_CMD_SHELL_LOG`：DUST_LOG 系统 + log 命令族，select `DUST_CMD_SHELL`（log 的初始化与命令入口都依赖 shell 底座）
- 业务层：`USE_CMD_VAR` / `USE_CMD_LOG` 默认 y，独立 select 对应架构符号
- CMake 按符号装配：`CONFIG_DUST_CMD_SHELL` → shell.cpp、`_VAR` → var.cpp、`_LOG` → log.cpp

### 2.2 初始化与线程

```
REGISTER_INIT  (dbg_init, PreInit, High)   ← shell.cpp：初始化 uart3 + Log::Init/BindUart + Shell
REGISTER_THREAD(dbg_start, LateThread)     ← shell 线程：k_sem_take 阻塞等接收 → 逐行解析 → 分发
```

- shell 线程循环：`k_sem_take(&uart.sem_, K_FOREVER)` 阻塞等数据 → `UartDma::Read` 批量读 → 逐字符拼行 → `\r/\n` 触发 `ProcessLine` → 按命令族分发 `Var::Process` / `Log::Process` / `CmdHelp`
- 接收优先：发送全走异步 DMA，任何时刻用户输入命令都能立即处理

---

## 3. 核心机制

### 3.1 日志等级与颜色

| 宏 | 等级 | 颜色 | 语义 |
|----|------|------|------|
| `DUST_LOG_INF` | INF | 黑 `\x1b[30m` | 一次性信息 |
| `DUST_LOG_ERR` | ERR | 亮红 `\x1b[91m` | 一次性错误 |
| `DUST_LOG_OK` | OK | 亮绿 `\x1b[92m` | 一次性状态正常 |
| `DUST_LOG_WRN` | WRN | 亮黄 `\x1b[93m` | 一次性警告 |
| `DUST_LOG_DBG` | DBG | 白 `\x1b[37m` | **可选择流式**（带名字） |

- 输出前带等级前缀：`[inf]` / `[err]` / `[ok]` / `[wrn]`（宏层字符串拼接），行尾 `\x1b[0m` 复位
- 宏在 `#ifdef CONFIG_DUST_CMD_SHELL_LOG` 内定义真实调用，`#else` 空宏——未开时调用点零开销、编译不报错（仿 EXEC_BUZZER 模式）
- 用法与 LOG_INF 一致：`DUST_LOG_INF("vx=%.2f", vx);`；DBG 多一个名字参数：`DUST_LOG_DBG("test_vx", "vx=%.2f", vx);`

### 3.2 DBG 条目机制（数组 + active_ 指针）

- **无 DEFINE、无链接段、无链表**：DBG 条目由首次 `DUST_LOG_DBG(name_, ...)` 调用时 `FindOrCreate` 运行时创建入静态数组 `entries_[64]`（名字即标识，同名复用同一条目）
- **选中状态**：单个 `active_` 指针——`log on <name>` 改指针指向选中条目（旧选中自动被顶替）、`log off` 置空，**同一时间只打一条 DBG**
- `Dbgl` 仅当 `e == active_` 才发（未选中静默，零开销）
- `Log::Select` **只选中已存在条目**（不存在回 `not found`，不创建幽灵条目——2026-08-06 修复）

### 3.3 发送帧队列与三档优先级仲裁

```
事件（INF/ERR/OK/WRN） > 命令响应（var/log 输出） > DBG 流式
```

| 档位 | 内容 | 理由 |
|------|------|------|
| **Event（最高）** | 四色一次性日志 | 打一遍就没了，丢一条永远补不回来——插队头、**池满永不挤** |
| **Cmd（中）** | var/log 命令响应（SendLine） | 用户主动请求正等着看——插事件后/DBG 前，池满次挤 |
| **Dbg（最低）** | DBG 流式帧 | 下轮代码执行又会打，可以等——排队尾，池满先挤 |

**帧池参数**（用户拍板）：

| 参数 | 值 | 依据 |
|------|-----|------|
| 帧数据区 | 128B | 7 float vofa 最坏 107B；现有最长日志实际 118B |
| 发送最长字节长度 | 127B（超长截断） | 128B 含尾部 `\0` 保险；127B 含 ANSI 开销，内容建议 <116B |
| 帧池 | **4 帧 = 512B** | 2→4 用户拍板（瞬时突发 + 余量） |

- 队列状态（`tx_head_/tx_tail_/free_head_/sending_`）被任务上下文与 ISR 同时访问——**所有队列写操作用 `irq_lock()/irq_unlock()` 包裹**（入口：TrySend / OnTxDone / MarkStaleDbg）
- DBG 切换及时顶替：`log on B` 时队列中残留的旧 Dbg 档帧标记 `is_stale`，`Dequeue` 跳过回收

### 3.4 发送管线（调用者入队 + ISR 续发）

```
任务/任意上下文                    ISR（TX_DONE 中断）
DUST_LOG_* → TrySend → 入队     DMA 完成 → on_tx_done → Log::OnTxDone
  → DMA 空闲时 Dequeue + Send       → 清 sending_ → Dequeue 下一帧 → Send
  → Send 提交即 RecycleFrame      （链式驱动续发）
```

- **打印不依赖线程**：调用者（任务/ISR/main 初始化）直接入队，TX_DONE 中断回调驱动续发
- `UartDma::Send` 内部 memcpy 到自身发送缓冲，DMA 搬运的是 UartDma 缓冲而非 TxFrame——**Send 提交成功即回收帧**（帧即取即还，帧池永不枯竭）
- 唯一前置：`Log::Init()` + `Log::BindUart()` 先执行（`dbg_init`，PreInit 阶段）

### 3.5 命令系统

```
h / ?                 帮助
var list              列出所有调试变量（类型 + 当前值 + 尾部计数）
var get <name>        查看单变量
var set <name> <val>  修改变量（11 种类型解析：u8~u64/i8~i64/float/double/bool）
log list              列出所有 DBG 条目（名字 + [ON]/[off]）
log on <name>         选中某条日志流式打印（同一时间只打一条）
log off               停止打印
```

- 变量注册：`REGISTER_SHELL_VAR("name", var)`——编译期 `.shell_var` 链接段收集，**支持类成员变量**（取成员地址 + TypeMap 推导）
- 命令响应走 `Log::SendLine`（Cmd 档，不经日志过滤）

### 3.6 启动横幅

- 每条目后：`[boot] <name> init` / `[boot] <name> start`（黑 INF，谁在初始化/谁的线程启动）
- 每阶段完成：`===== [boot] <阶段名> ok =====`（亮绿 OK，表示哪个阶段初始化完成）
- 条目失败：High → `[err] [boot] <name> fail`（亮红，halt 前打印）+ Mid/Low → `[wrn]`（亮黄）
- PreInit 阶段条目横幅打不出（log 在其中初始化，物理限制），PreInit 完成横幅可出

---

## 4. 容量模型（实测确认）

### 4.1 工程公式

```
B_max = K + 1               同一时间允许的最大日志调用数（1 DMA 中 + K 排队）
K_min = max(1, B - 1)       给定突发需求 B 所需最少帧数
不丢帧 ⇔ R_p < baud/10 且 B ≤ K + 1    （稳态吞吐 + 瞬时突发）
```

- 稳态吞吐：DMA 消费上限 = 波特率/10（921600 → 92160 B/s）；稳态平均生产速率 ≥ 上限 → 积压无限增长 → **任何有限帧数不可闭环**
- 瞬时突发：单次调用条数 ≤ K+1（第 K+2 条起丢，事件永不挤）

### 4.2 实测数据（2026-08-06，921600 波特）

| 测试 | 实测结论 |
|------|---------|
| 单帧周期（30/64/127B，MCU 端计时 1000 次） | **371/743/1430us**（理论 330/694/1378）；线性回归 T = 38.2 + 10.96×len：固定开销 ~40us/帧 + 字节速率 10.96us/B；抖动 <5us |
| 突发容量（4/5/6/8/16 条 × 50 轮） | **B_max = K+1 = 5 确认**（第 6 条起丢）；丢帧仅出现在"长帧紧跟首轮连发"窗口；**稳态突发 16 条也不丢**（OnTxDone 链式腾帧） |
| 无限闭环（满速 10s / 限速 5ms 60s / 边界 1ms 60s） | **限速零丢帧 = 无限闭环成立**；**满速丢帧 60% = 容量边界**（扩帧不可救，只能限速） |

### 4.3 工程使用建议

- 日志内容 <116B（127B 上限含 ~11B ANSI 开销）
- 瞬时突发 ≤5 条（4 帧池）；持续输出限速 <92KB/s（约 725 帧/s 满帧）
- 满速连发必丢是设计边界：DBG 档可丢弃、事件档在限速场景下保证不丢

---

## 5. 修复记录（测试中发现并修复）

| 问题 | 根因 | 修复 |
|------|------|------|
| 连续发送丢尾（`pwm ready`→`pwm r`） | HPM 驱动 TX_DONE 由 DMA 完成触发（≠FIFO 排空）+ `uart_hpm_tx` 无条件 `uart_reset_tx_fifo` | [uart_hpmicro.c:1267-1276](E:/Zephyr_HPMicro/sdk_glue/drivers/serial/uart_hpmicro.c#L1267)：TX 完成回调里 `async_evt_tx_done` 前忙等 `LSR & TEMT`（FIFO 排空），带超时 |
| 连发丢帧（第 3 条第 5 条起丢） | **帧泄漏**：`SendFrame` 只在 Send 失败时回收帧，成功路径帧永不归还空闲池 → 池耗尽 | `SendFrame` Send 成功/失败都 `RecycleFrame`（帧即取即还） |
| `log on nope` 创建幽灵条目 | `Select` 用 `FindOrCreate`，log on 不存在名字会凭空创建条目 | `Select` 改为只选中已存在条目（遍历查找），不存在回 `not found` |

---

## 6. 集成与迁移

### 6.1 Zephyr log 弃用（已执行）

- `project/Kconfig` 的 DEBUG_LOG 配置删除（无 `select LOG/LOG_BACKEND_UART/LOG_BACKEND_UART_ASYNC`）——Zephyr log 彻底不启用
- **批量迁移**：23 个文件 157 处 `LOG_*` → `DUST_LOG_*`（LOG_ERR 113、LOG_INF 44、LOG_MODULE_REGISTER 23 行删除、`#include <zephyr/logging/log.h>` → `"log.hpp"`）
- 无 LOG_DBG/WRN/HEXDUMP（纯机械替换，无需名字参数）
- `CONFIG_DUST_CMD_SHELL_LOG=n` 时 DUST_LOG_* 宏为空，与 LOG=n 静默语义一致

### 6.2 环境

- 串口：**COM21 @ 921600**（用户环境固定）
- 终端：MobaXterm（支持 ANSI 亮色）
- PC 脚本：`scripts/log_var_stress.py`（命令暴力测试，含握手）、`scripts/log_test_c_parse.py`（测试 C 解析）

---

## 7. 验证记录（2026-08-06 全部通过）

| 项 | 结果 |
|----|------|
| 帧池极限性能测试 A/B/C | ✅ 全过（§4.2） |
| shell 命令暴力测试 | ✅ **PASS 36 / FAIL 0**（手测 M1~M18 + 脚本：命令风暴 2385 条/120s 存活、坏值矩阵、DBG 流式优先级 Cmd>Dbg、log off 1s 内停、垃圾输入不崩溃、风暴后恢复） |
| 类成员变量注册 | ✅ REGISTER_SHELL_VAR 对类成员（TestMember::vx_/cnt_/armed_）取地址 + TypeMap 推导正常 |
| 启动横幅 | ✅ 全阶段完成提示 + 条目 init/start + 失败 ERR/WRN |

---

## 8. 遗留待办

- [x] vofa+/其他终端对亮色 ANSI 的兼容性 —— 2026-08-06 已验证正常（MobaXterm）
- [x] 未迁移文件的最终核查 —— 2026-08-06 编译烧录验证通过，无残留

> 日志源分组（订阅式）概念：**已决定不做**（2026-08-06 用户拍板）——DBG 单条选中已满足调试需求。
