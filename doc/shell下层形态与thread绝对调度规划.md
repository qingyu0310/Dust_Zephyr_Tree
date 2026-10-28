# shell 下层形态与 thread 绝对时间调度规划

> 2026-10-26。**本文只写规划，没有改任何代码。**
>
> 三个部分：
> ① shell 下层（`var` / `thread` / `boot`）为什么不照 `log` 分文件——记录为定案，并给出「什么时候才该拆」的判据；
> ② thread 绝对时间调度——用户提出的方向，本文给出可执行设计与边界；
> ③ 其余候选改进——自查补充，**逐条由用户决定采纳与否**。
>
> 标注约定：**源码事实** = 已在本次阅读中核对到的代码；**架构判断** = 基于代码的评价；**待验证** = 有依据但没有实测。

---

## 0 结论速览

| # | 议题 | 结论 | 用户动作 |
|---|---|---|---|
| 1 | `var`/`thread`/`boot` 要不要像 `log` 分四层 | **不拆**（体量与状态机都不够） | 确认 |
| 2 | 不拆的前提下要不要统一分派写法 | 建议改 `boot_cmd.cpp` 一处（8 行） | 决定 |
| 3 | thread 绝对时间调度 | **建议做**，默认 `Delay` 保持现状，`Absolute` 按需开启 | 决定 + 拍板追赶策略 |
| 4 | 64 位统计撕裂（旧 P2，仍未做） | **建议做**（高危，未被后续改动覆盖） | 决定 |
| 5 | `DefaultHandle` 的 `Isolate` 分支缺 `break` | **建议做**（1 行） | 决定 |
| 6 | `Start()` 幂等保护 | 建议做（低危但代价极小） | 决定 |
| 7 | `Log::ProcessLogCommand` 死代码 | 建议删（零风险） | 决定 |
| 8 | `var set` 的数字解析与项目既定规则不一致 | 待验证后再定 | 决定是否验证 |

---

## 1 shell 下层：为什么只有 log 分了四层

### 1.1 log 的四层是被什么逼出来的（源码事实）

`framework/shell/log/` 现在 11 个文件 1820 行：

| 层 | 文件 | 行数 |
|---|---|---|
| ① 命令 | `log/cmd/log_cmd.hpp` / `.cpp` | 28 + 141 |
| ② 加工 | `log/process/log_process.hpp` / `.cpp`、`log_debug.hpp` | 194 + 381 + 160 |
| ③ 队列 | `log/queue/log_record.hpp`、`log_queue.hpp`、`log_policy.hpp` | 208 + 324 + 120 |
| ④ 发送 | `log/transport/log_transport.hpp` | 79 |
| 门面 | `log.hpp` / `log.cpp` | 144 + 41 |

它之所以能拆，不是因为「日志重要」，而是因为它同时具备四个条件，**少一个都拆不成**：

1. **有真实的状态机**：三档优先级队列、帧池 8×128B、`source/epoch` 出生证明、挤帧仲裁、DMA 空闲续发、TX_DONE 回调。这些状态跨文件共享，塞在一起就没法读。
2. **有单向流水线**：①→②→③→④，只允许下行。层间接口是稳定的，所以分文件后不需要互相 include。
3. **有所有权收口**：四层实例全部由加工层持有（`dbg_`/`pump_`/`txq_`/`recq_`），命令层根本拿不到实例，"越权"在结构上就不可能。这是拆分的收益，不是代价。
4. **有 ABI 约束**：参数快照用 `uint32_t` 槽（`log_record.hpp`）、`%f` 从 2 槽拼回 double、64 位 Linux 移植时布局会变。这类约束必须写在独立文件里才守得住。

### 1.2 其余三族的体量（源码事实）

| 族 | 文件 | 行数 | 有状态机？ | 有跨文件状态？ |
|---|---|---|---|---|
| `boot` | `boot/boot_cmd.hpp` / `.cpp` | 34 + 102 | 否 | 否（只读 `BootTimingSlot`） |
| `thread` | `thread/thread_cmd.hpp` / `.cpp` | 50 + 306 | 否 | 否（只调 `thread_registry` / `ThreadBase`） |
| `var` | `var/var.hpp` / `.cpp` | 138 + 294 | 否（一个 union + 一个 TypeMap） | 否（只遍历 `.shell_var` 段） |

三族加起来 924 行，**各自都是「一个 Process 入口 + 若干子命令 + 一次查表」的平铺结构**，没有任何跨文件的隐藏状态。`var` 甚至连一个运行时实例都没有（全 `static`）。

**架构判断**：把它们拆成 `cmd/process/queue/transport` 只会产出 4 个 30~70 行的空壳文件，文件名暗示"这里有一台流水线"，实际打开发现每个文件只有两三个函数。**拆分的成本（4 个 CMake 路径、4 个 include 目录、读者多跳 4 次）大于收益。**

### 1.3 定案

| 项 | 决定 |
|---|---|
| `var` / `thread` / `boot` | **保持单文件（hpp + cpp），不分层** |
| `log` | 保持现有四层，不动 |
| 判据 | 只在 §1.4 的触发条件满足时重新讨论 |

这一条写进文档的目的，是避免以后每次看到 `log` 四层就觉得"其余三族欠了债"。

### 1.4 什么时候才该拆（触发条件）

任一族满足下面**任意两条**时再拆：

1. 该族 **cpp 超过 600 行**，或出现第 3 个"有状态的服务对象"。
2. 出现 **ISR / 回调上下文**参与的路径（如某命令要等一个异步完成）。
3. 出现 **固定容量队列**或**资源池**（那才有"队列层"这个真东西）。
4. 出现 **跨编译单元的共享状态**（当前三族全部 `static` 在单文件内，这一条是硬分界）。
5. 出现 **需要按目标 ABI 重写的部分**（如 64 位指针槽、格式参数宽度）。

按当前代码，**三族一条都不满足**。

### 1.5 不拆，但有一处「已成型却写法不统一」（源码事实）

`log` / `var` / `thread` 三族都走「词法解析 + 查表匹配」：

```cpp
// log_cmd.cpp:129-136、var.cpp:229-235、thread_cmd.cpp:227-236
static constexpr SubCmdEntry kSubCmds[] = { { "list", CmdList }, ... };
DispatchSubCmd(line, kSubCmds, sizeof(kSubCmds) / sizeof(kSubCmds[0]), &CmdUsage);
```

但 `boot_cmd.cpp:88-98` **退回成了 if/else**：

```cpp
const char* sub = nullptr;
line = NextToken(line, sub);

if (std::strcmp(sub, "timing") == 0) {
    CmdTiming();
} else {
    Log::SendCommandLine("boot timing");
}
```

**架构判断**：`boot` 只有一个子命令，所以 if/else 目前完全正确、也确实更短。问题是它让「两级命令分发」这件事出现了唯一一个例外——读者看到 `log`/`var`/`thread` 用表、`boot` 用 if，会以为这里有语义差异，其实没有。

**这条是纯一致性，不是 bug。** 两个选项：

| 选项 | 做法 | 代价 | 收益 |
|---|---|---|---|
| **A（建议）** | 改成 `DispatchSubCmd`，零行为变化 | 多 3 行 | 四族写法一致，新增第二个子命令时不必再改结构 |
| B | 保持 if/else | 0 | 省 3 行 |

改法（选项 A，`boot_cmd.cpp:88-98`）：

**old：**

```cpp
void BootCmd::Process(uint8_t* line)
{
    const char* sub = nullptr;
    line = NextToken(line, sub);

    if (std::strcmp(sub, "timing") == 0) {
        CmdTiming();
    } else {
        Log::SendCommandLine("boot timing");
    }
}
```

**new：**

```cpp
static void CmdTimingSub(uint8_t* args)
{
    (void)args;
    BootCmd::CmdTiming();
}

static void CmdUsage(uint8_t* args)
{
    (void)args;
    Log::SendCommandLine("boot timing");
}

void BootCmd::Process(uint8_t* line)
{
    static constexpr SubCmdEntry kSubCmds[] = {
        { "timing", CmdTimingSub },
    };

    DispatchSubCmd(line, kSubCmds, sizeof(kSubCmds) / sizeof(kSubCmds[0]), &CmdUsage);
}
```

**注意**：`CmdTiming()` 现在是 `private`，静态自由函数调不到。选项 A 要连带把 `CmdTiming` 改成 `public`，或写成 `BootCmd` 的 private static 分派壳（与 `ThreadCmd::CmdListSub` / `Var::CmdListSub` 同款）。**这一条连带的改动量比看着大**——如果只为一行一致性不值得，可以直接选 B。

> ⚠️ **这条我不推荐强行做**。理由：「有现成模板就照抄」在本项目是硬规矩，但这条改动要动可见性，属于"为一致性付结构代价"。用户拍板。

---

## 2 thread 绝对时间调度

### 2.1 现状（源码事实）

`Thread::Run()` 每拍结尾做的是「任务执行完成后再睡」：

```cpp
// thread.hpp:381-384
if (!K_TIMEOUT_EQ(task_.period, K_NO_WAIT))          // 非 K_NO_WAIT 才等待
{
    k_sleep(task_.period);               		   // 周期等待
}
```

所以实际周期 = `任务体耗时 + k_sleep(period) + 调度延迟`，**每一拍的耗时都会加成到下一拍起点上，误差累积**。

当前参数配置（`project/build/zephyr/.config`）：

```text
CONFIG_SYS_CLOCK_TICKS_PER_SEC=2000      → 1 tick = 0.5 ms
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=24000000
CONFIG_TICKLESS_KERNEL=y
CONFIG_TIMEOUT_64BIT=y
```

现有调用点（5 处，源码事实）：

| 调用点 | period | 语义 |
|---|---|---|
| `imu.hpp:46` | `K_MSEC(1)` | 采集节拍，任务体自己用 `k_cycle_get_32` 修正 `dt` |
| `remote.hpp:79` | `K_NO_WAIT` | 事件驱动，节拍由 `k_sem_take` 决定 |
| `shell.hpp:39` | `K_NO_WAIT` | 事件驱动 |
| `trd_gpio.cpp:57` | `K_MSEC(1)` | 心跳 |
| `trd_test.cpp:82` | `K_MSEC(10)` | 测试 |

**受影响的是 `imu` 与 `gpio` 这两处**。`gpio` 只是翻转心跳，漂移无所谓；**`imu` 的 1kHz 采样漂移会直接进 EKF**——现在靠 `kMeasureDT()` 用真实 cycle 差值补偿 `sample_.dt` 兜住了数值正确性，但**采样时刻本身仍然在漂**，这对后续要做固定频率控制环是个地基问题。

### 2.2 两种语义

| 语义 | 定义 | 误差特性 | 适用 |
|---|---|---|---|
| **Delay**（现状） | 任务结束后睡 `period` | 误差**累积**：第 n 拍起点 = n×(period + 平均任务耗时) | 轮询、低频状态检查、心跳 |
| **Absolute**（新增） | 维护一张绝对时刻表 `next += period`，睡到该时刻 | 误差**不累积**：只有 ±1 tick 的量化抖动 | 采样、控制环、任何要吃 `dt` 的环 |

关键区别不是"哪个更准"，而是**误差是累加还是收敛**。Absolute 模式下即使某一拍超时，下一拍仍然回到原来的时刻表（丢弃落后的节拍），长跑下来周期严格等于 `period` 的整数倍。

### 2.3 API 设计

三个选项：

| 选项 | 形状 | 优点 | 缺点 |
|---|---|---|---|
| **A（建议）** | `Start(...)` 追加第 7 参 `PeriodMode mode = PeriodMode::Delay` | 现有 5 个调用点**一行都不用改** | 参数已经 6 个，第 7 个开始难读 |
| B | 把 `period` 换成结构体 `ThreadPeriod{ k_timeout_t interval; PeriodMode mode; }` | 语义成组，永远只有 6 个位置参数 | 5 个调用点全改；`K_MSEC(1)` 要写成 `{K_MSEC(1), PeriodMode::Absolute}` |
| C | 单独 `SetPeriodMode()` | 调用点不变 | **已被否决过**：故障配置就是从这个模式改成 Start 参数的（`doc/线程故障配置注册表vs调用点规划.md`），不能倒退 |

**建议 A**，理由：本项目已经确立「配置在 `Start()` 调用点一次写死」的规矩（`thread_fault.hpp:49-53` 的注释原文就是这句话），A 是这个规矩的最小增量。等第 8 个参数出现时再整体收成结构体。

新增枚举（放 `thread.hpp`，紧邻 `ThreadPrio` / `TaskResult`）：

```cpp
/**
 * @brief 周期语义
 */
enum class PeriodMode : uint8_t
{
    Delay    = 0,   // 任务结束后睡 period：误差随拍数累积（现状，默认）
    Absolute = 1,   // 按绝对时刻表对齐：next += period，误差不累积
};
```

签名改为：

```cpp
void Start(TaskFunc entry, k_timeout_t period, ThreadPrio prio, void* context,
           const char* name, const ThreadFaultConfig& fault = {},
           PeriodMode mode = PeriodMode::Delay);
```

**约束**：`Absolute` 要求 `period` 是**有限的相对时长**。传 `K_NO_WAIT` / `K_FOREVER` 时降级为 `Delay` 并打一条 `DUST_LOG_WRN`（不能静默）。

### 2.4 实现

#### 2.4.1 时刻表

Zephyr 4.3 提供绝对超时的公开入口：`K_TIMEOUT_ABS_TICKS(t)`（`include/zephyr/kernel.h:1579`），`k_sleep` 接受它，内核在 `z_add_timeout()` 里分流（`kernel/timeout.c:118-127`）：

```c
if (Z_IS_TIMEOUT_RELATIVE(timeout)) {
    ...
} else {
    k_ticks_t dticks = Z_TICK_ABS(timeout.ticks) - curr_tick;
    to->dticks = max(1, dticks);      // 已过期 → 1 tick 后立即醒，不会卡死
    ticks = timeout.ticks;
}
```

所以**不需要碰 `k_timepoint_t`**（它是 opaque 类型，且 4.3 没有 `sys_timepoint_add()`，要推进只能自己改 `.tick`，属于"inspect 内部结构"，不做）。

用 `k_uptime_ticks()`（`int64_t`，`include/zephyr/kernel.h:1966`）拿当前 tick，自己维护 `next_tick_` 即可。

#### 2.4.2 `Run()` 改动（`thread.hpp:338-386`）

**old（结尾部分）：**

```cpp
            // 外壳开销 = 整圈 - 任务体；k_sleep 在下面，不计入
            const uint64_t overhead_elapsed = k_cycle_get_64() - overhead_begin - elapsed;
            stats_.last_overhead_cycles = overhead_elapsed;     		// 最近一次外壳开销

            if (overhead_elapsed > stats_.max_overhead_cycles) {
                stats_.max_overhead_cycles = overhead_elapsed;  		// 单次最长外壳开销
            }

            if (!K_TIMEOUT_EQ(task_.period, K_NO_WAIT))          // 非 K_NO_WAIT 才等待
			{
                k_sleep(task_.period);               		   // 周期等待
            }
        }
```

**new：**

```cpp
            // 外壳开销 = 整圈 - 任务体；k_sleep 在下面，不计入
            const uint64_t overhead_elapsed = k_cycle_get_64() - overhead_begin - elapsed;
            stats_.last_overhead_cycles = overhead_elapsed;     		// 最近一次外壳开销

            if (overhead_elapsed > stats_.max_overhead_cycles) {
                stats_.max_overhead_cycles = overhead_elapsed;  		// 单次最长外壳开销
            }

            WaitNextPeriod();                                   		// 周期等待（按模式分流）
        }
```

新增私有方法：

```cpp
    /**
     * @brief 周期等待：按 PeriodMode 分流
     *
     * Delay    —— 任务结束后睡 period，误差随拍数累积（历史行为）。
     * Absolute —— 睡到时刻表上的下一格；若本拍已越过该格，丢弃落后的节拍
     *             并把时刻表重新对齐到 now + period，避免连跑补课。
     */
    void WaitNextPeriod()
    {
        if (K_TIMEOUT_EQ(task_.period, K_NO_WAIT)) return;       // 事件驱动：不睡

        if (task_.mode == PeriodMode::Delay)
        {
            k_sleep(task_.period);
            return;
        }

        next_tick_ += period_ticks_;                             // 时刻表前进一格

        const int64_t now = k_uptime_ticks();
        if (next_tick_ <= now)                                   // 已落后：丢弃本格，重新对齐
        {
            atomic_inc(&stats_.overrun_count);
            next_tick_ = now + period_ticks_;
        }

        k_sleep(K_TIMEOUT_ABS_TICKS(next_tick_));
    }
```

成员补两个（`task_` 结构体内加 `mode`，`run_` 同级加 `next_tick_`）：

```cpp
    struct
    {
        TaskFunc    entry   = nullptr;           					// 受管任务入口
        k_timeout_t period  = K_NO_WAIT;         					// 拍间睡眠时长
        PeriodMode  mode    = PeriodMode::Delay; 					// 周期语义
        void       *context = nullptr;           					// 任务上下文
        const char *name    = nullptr;           					// 线程名（日志与命令用）
    } task_;                                     					// 任务配置

    k_ticks_t period_ticks_ = 0;                 					// period 的 tick 数（Absolute 用）
    int64_t   next_tick_    = 0;                 					// 下一拍的绝对 tick（仅本线程读写）
```

`Start()` 里初始化：

```cpp
        task_.mode = mode;

        // Absolute 只支持有限相对时长；K_NO_WAIT / K_FOREVER 降级为 Delay
        if (mode == PeriodMode::Absolute &&
            (K_TIMEOUT_EQ(period, K_NO_WAIT) || K_TIMEOUT_EQ(period, K_FOREVER)))
        {
            task_.mode = PeriodMode::Delay;
            DUST_LOG_WRN("thread %s: absolute period requires finite timeout, falling back to delay", name);
        }

        period_ticks_ = (task_.mode == PeriodMode::Absolute) ? task_.period.ticks : 0;
        next_tick_    = 0;                              // 首拍结束后的 WaitNextPeriod 里对齐
```

**首拍处理**：`next_tick_` 初值 0，第一次进 `WaitNextPeriod()` 时 `next_tick_ = 0 + period_ticks_` 会远小于 now → 立刻走"落后重新对齐"分支并 `overrun_count++`。这是错的（首拍一定算超时）。

所以 `next_tick_` 必须在**进循环前**初始化成 `now + period_ticks_`，或者用 0 当哨兵：

```cpp
    void WaitNextPeriod()
    {
        if (K_TIMEOUT_EQ(task_.period, K_NO_WAIT)) return;

        if (task_.mode == PeriodMode::Delay)
        {
            k_sleep(task_.period);
            return;
        }

        const int64_t now = k_uptime_ticks();

        if (next_tick_ == 0) {                                   // 首拍：以本拍结束时刻为基准建立时刻表
            next_tick_ = now;
        }

        next_tick_ += period_ticks_;

        if (next_tick_ <= now)                                   // 已落后：丢弃本格，重新对齐
        {
            atomic_inc(&stats_.overrun_count);
            next_tick_ = now + period_ticks_;
        }

        k_sleep(K_TIMEOUT_ABS_TICKS(next_tick_));
    }
```

用 `next_tick_ == 0` 当哨兵是安全的：`k_uptime_ticks()` 在启动初期就远大于 0（2000Hz 下 1ms 就是 2），实际不可能撞上。

> ⚠️ **`task_.period.ticks` 是直接读 opaque 结构体字段。** Zephyr 文档说 `k_timeout_t` 不该被 inspect，但 4.3 没有任何公开的「timeout → ticks」取值函数（内核自己也直接读 `timeout.ticks`，见 `kernel/timeout.c:119-125`，`K_TIMEOUT_EQ` 同样如此）。这是**唯一可行路径**，必须在注释里写清理由，否则下一个读者会以为是笔误。

#### 2.4.3 `overrun_count` 统计

`Absolute` 模式的价值需要被观测，否则没人知道它是不是真在工作。`ThreadStats` 加一个字段（§3.1 顺带做）：

```cpp
    uint32_t    overrun_count;      // 本拍超出时刻表、被丢弃的节拍数（仅 Absolute）
```

`thread show <name>` 输出加一行。**不做额外 RAM 预算**：4B/线程。

### 2.5 追赶策略（需拍板）

任务体耗时超过 `period` 时，后面几格怎么办：

| 策略 | 行为 | 后果 | 适用 |
|---|---|---|---|
| **① 丢弃对齐（建议）** | `next = now + period`，`overrun_count++` | 周期恒等于 `period`，丢掉落后节拍 | 采样 / 控制环：宁可少一拍，不要连跑两拍 |
| ② 连跑补课 | 保持 `next += period`，`k_sleep` 立刻返回直到追上 | 时间戳严格对齐，但会连续执行多拍，输出挤成脉冲 | 有严格时间戳语义、且允许突发处理的后处理 |
| ③ 不处理 | `k_sleep` 自然返回 | 等于 Absolute 退化成"零漂移但会连跑"，且没有计数 | 不推荐 |

**建议 ①**。理由：`imu` 在 1kHz 下读失败/总线抖动时会偶发超时，连跑补课会让 `dt` 忽大忽小、EKF 更难受；丢弃一拍反而让下一拍回到干净的时刻表。

如果用户要 ②，把 `next_tick_ = now + period_ticks_;` 改成 `next_tick_ += period_ticks_;` 并去掉重新对齐即可，但 `overrun_count` 仍要计。

### 2.6 精度上限（诚实边界）

**Absolute 消除的是漂移，不是抖动。** 时间分辨率仍受 tick 限制：

- `SYS_CLOCK_TICKS_PER_SEC=2000` → 1 tick = **0.5 ms**。
- `imu` 的 `K_MSEC(1)` = **2 tick**，所以 `next_tick_` 每次 +2，长跑严格 1ms 一格，**零漂移**。
- 但唤醒时刻只能落在 tick 边界上，**相对理想时刻的抖动上限仍是 ±0.5ms（±50%）**。
- `CONFIG_TICKLESS_KERNEL=y` 只是让内核不为空转设 tick 中断，**超时时长依旧按整数 tick 折算**，分辨率不会变好。

所以：

| 想要 | 需要 |
|---|---|
| 零漂移，±0.5ms 抖动 | 本规划的 Absolute 模式（够用） |
| ±0.1ms 抖动 | 把 tick 提到 10000Hz（`CONFIG_SYS_CLOCK_TICKS_PER_SEC=10000`），代价是内核定时器开销上升 |
| ±10µs 抖动 | 硬件定时器 + `k_busy_wait` 或专用 PWM 触发，**不属于 Thread 外壳的职责** |

**这一条必须写进文档**，否则下一个人会以为改了 Absolute 就得到了"硬实时 1kHz"。

### 2.7 与统计的关系

- **`overhead` 的定义不受影响**：`overhead_elapsed` 在 `WaitNextPeriod()` **之前**算完，仍然只统计「任务体 + 外壳处理」这一段，不含睡眠。
- **`imu` 的 `dt` 补偿要保留**：`imu.cpp:93-101` 的 `kMeasureDT()` 用真实 cycle 差值修正 `sample_.dt`，这是**数值正确性**的兜底，Absolute 只改善**时刻表**，两者不冲突、也不能互相替代。
- **建议顺带加「实际周期」观测**（旧 P7 §7.3 的选项 ②，当时默认不做）：`ThreadStats` 加 `last_period_ns`，在 `WaitNextPeriod()` 开头用 `k_cycle_get_64()` 与上一拍起点做差。**代价**：+8B/线程、每拍多两次读 cycle。**收益**：Absolute 模式没有它就无法验证——`overrun_count` 只告诉你"丢了几拍"，不告诉你"实际每拍多久"。**建议做**，理由：这正是本次改动的验收依据。

### 2.8 迁移范围（建议）

| 线程 | 是否迁 `Absolute` | 理由 |
|---|---|---|
| `imu`（`imu.hpp:46`） | **迁** | 唯一真正吃周期语义的线程，1kHz 采样 |
| `gpio`（`trd_gpio.cpp:57`） | 不迁 | 心跳翻转，漂移无影响；且它已是 `Thread<128>` 极小栈，不引入新代码路径 |
| `remote` / `shell` | 不适用 | `K_NO_WAIT`，事件驱动 |
| `test`（`trd_test.cpp:82`） | **迁**（作为验证载体） | 测试线程的职责就是验证外壳能力；迁过去才能用 `thread show test` 看到 `overrun_count` / `last_period_ns` |

**建议**：`imu` 迁，`test` 同时迁（用于上板验证），其余不动。

### 2.9 验证清单

| # | 操作 | 期望 |
|---|---|---|
| 1 | 编译通过 | 无新警告，5 个既有 `Start` 调用点零改动 |
| 2 | `thread show imu` 连续敲 10 次 | `last period` 稳定在 `1000.000 us` 附近，抖动 ≤ 500 us |
| 3 | `var set test_trigger 1` 触发 Retry | `retry` 递增，`overrun_count` **不增**（Retry 不等于超时） |
| 4 | 把 `imu` 的 `period` 临时改成 `K_MSEC(1)` 并在 `Task` 里加 2ms 忙等 | `overrun_count` 每拍 +1，`run` 计数不再等于墙钟/period（说明丢拍生效） |
| 5 | `log` 打开某条 DBG 观察 `imu` 的 `dt` | `dt` 收敛在 0.001 s 附近，不再出现 >1.05ms 的尖峰 |
| 6 | `thread list` | 表格新增列不破坏对齐 |

> 第 4 条是**破坏性验证**，要在验证后还原；第 3 条是回归验证（确认 Absolute 没有污染 Retry 路径）。

---

## 3 其余候选改进（逐条决定）

> 以下都是本次阅读源码时核对到的，**全部未改**。用户逐条拍板。

### 3.1 【建议做】64 位统计撕裂（旧规划 P2，至今未做）

**源码事实**：`thread.hpp:309-318` 有四个 `uint64_t`：

```cpp
uint64_t max_run_cycles  = 0;    // 仅工作线程写
uint64_t last_run_cycles = 0;    // 仅工作线程写
uint64_t max_overhead_cycles  = 0;
uint64_t last_overhead_cycles = 0;
```

写入在 `Run()`（工作线程），读取在 `GetStats()`（shell 线程，`thread.hpp:280-283`）。RV32 上 64 位读写**不是原子操作**，shell 恰好读到中间态会得到"半新半旧"的荒谬值（低 32 位是新值、高 32 位是旧值）。

**注意**：旧规划 `doc/thread语义缺口修补规划.md` §2 的代码与现在**已经不一致**——它引用的 `run_cycles_` 成员现在叫 `max_run_cycles`/`last_run_cycles`，而且没有 `run_cycles_` 累计字段了。**判定完全相同，但 old/new 片段必须按当前代码重写，不能照抄旧规划。**

**建议做法**：读写两侧套 `irq_lock`/`irq_unlock`（旧规划方案 A），`k_cyc_to_ns_floor64()` 放在临界区外。代价：每拍约 6 条额外指令 + 关中断极短窗口。`ResetStats()`（`thread.hpp:257-267`）同样要保护。

**判定**：**建议做**。这是唯一一条"能直接产生错误数据"的缺陷，且修复不影响任何调用点。

### 3.2 【建议做】`DefaultHandle` 的 `Isolate` 分支缺 `break`（1 行）

**源码事实**：`thread_fault.cpp:72-88`

```cpp
switch (thread->FaultPolicy())
{
    case ThreadFaultPolicy::Isolate:
    {
        DUST_LOG_WRN("thread %s fault, isolating", thread->Name());
        thread->Exit();
    }                                       // ← 没有 break

    case ThreadFaultPolicy::Recover:
    default:
    {
        DUST_LOG_ERR("thread %s fatal, halting", thread->Name());
        Halt();
    }
}
```

`Thread::Exit()` 在**自杀**时不返回（`k_thread_abort`），所以当前的唯一调用路径（`Run()` 里对 self 调 `DefaultHandle`）碰巧不出问题。

但只要出现下面任一情况，就会 **fallthrough 到 `Halt()` → 全杀**：

1. `Exit()` 因 `KeepAlive()` 提前 `return`（`thread.hpp:171-175`）；
2. 将来 `DefaultHandle` 被非故障线程调用（如监控线程代判某线程故障）。

**建议做法**：`Isolate` 分支末尾加 `break;`，并把 `default` 拆出来（`Recover` 单独一个 `case`，`default` 也调 `Halt()` 但注释写明"未知策略按最坏处理"）。

**判定**：**建议做**。1 行，消除一类未来会踩的坑。

### 3.3 【建议做】`Start()` 幂等保护

**源码事实**：`thread.hpp:131-154` 的 `Start()` 无条件 `k_thread_create()`。对同一对象重复调用会在**已运行**的 `k_thread` 上再创建一次，`k_thread` 结构被覆写、栈上旧上下文丢失 —— 后果不确定（可能只是丢线程，也可能栈越界）。

`run_.started` 已有标志，但 `Start()` 没有检查它。

**建议做法**：

```cpp
if (atomic_get(&run_.started) != 0)
{
    DUST_LOG_WRN("thread %s already started", (name != nullptr) ? name : "?");
    return;
}
```

放在 `Start()` 开头。代价 3 行，无 RAM。

**判定**：**建议做**。低概率但零成本。

### 3.4 【已作废】旧 P3「KeepAlive 假保护」与旧 P4「IsAlive 死条件」

这两条**已经被后续改动顺手修掉了**，不必再做，但要在文档里标注，避免以后有人照着旧规划去删 `KeepAlive`：

| 旧缺口 | 现状（源码事实） |
|---|---|
| P3：`KeepAlive` 全仓库零读取者 | **已复活**：`thread_fault.cpp:35`（`Halt()` 跳过保留线程）、`thread.hpp:171`（`Exit()` 拒绝保留线程）、`thread_cmd.cpp:191`（命令层拒绝）三处读取，语义闭合。**绝不能按旧规划删** |
| P4：`IsAlive()` 里 `"exited"` 永不匹配 | **已修正**：`thread.hpp:401` 现在查的是 `"dead"`，与内核 `Z_STATE_STR_DEAD "dead"`（`kernel/include/kthread.h:19`）一致 |

> ⚠️ 旧规划 `doc/thread语义缺口修补规划.md` 的 **P3 会删掉现在就绪的机制**。如果要继续执行那份规划，**必须先划掉 P3 与 P4**。

### 3.5 【建议做】`Log::ProcessLogCommand` 是死代码

**源码事实**：`log.hpp:115` 声明、`log.cpp:36-39` 定义：

```cpp
void Log::ProcessLogCommand(uint8_t* line)
{
    logcmd::Process(line);
}
```

但命令注册在 `log_cmd.cpp:141` 直接指向 `logcmd::Process`：

```cpp
REGISTER_SHELL_CMD("log", "log list|on <name> [N]|off|mode", ::shell::logcmd::Process);
```

全仓库 `grep ProcessLogCommand` 只有声明 + 定义两处，**零调用者**。

**建议做法**：删掉 `.hpp` 的声明和 `.cpp` 的实现。零风险。

**判定**：**建议做**。这是 shell 从 cmd 拆出来时留下的过渡残渣。

### 3.6 【待验证】`var set` 的数字解析与项目既定规则冲突

**源码事实**：

- `framework/shell/log/cmd/log_cmd.cpp:47-58` 手写十进制解析，注释写明理由：**"手写十进制解析（picolibc strtoul 多位数有 bug）"**。
- `doc/DBG日志分频规划.md:188` 同样记录："手写数字解析，不用 `strtoul`——`picolibc` 在这一平台有多位数解析 bug（项目已有教训）"。
- 但 `framework/shell/var/var.cpp:161/179/194/202` 用的是 `std::strtoull` / `std::strtoll` / `std::strtof` / `std::strtod`。

**架构判断**：这是同一模块内**明确冲突的两条规则**。要么 `var` 侧有隐患（多位数解析错），要么 `log` 侧的手写是过度防御。

**建议做法**：**先上板验证**，不要直接改：

| # | 操作 | 期望 |
|---|---|---|
| 1 | `var set <u32变量> 12345` 然后 `var get <同变量>` | 回读 12345 |
| 2 | 同上换 `1234567`（超过 16 位） | 回读 1234567 |
| 3 | `var set <float变量> 3.14159` | 回读 3.14159 |
| 4 | `var set <u32变量> 0x1F` | 回读 31 |

若 1~4 全部正确 → `var` 侧无问题，把 `log_cmd.cpp` 的手写解析换成 `strtoull` 以消除重复（**但要单独评估**：`log on` 的解析在 shell 线程里，换成 libc 会带入额外的 code size）。

若任一条错 → `var` 侧改成与 `log_cmd.cpp` 同款手写解析。

**判定**：**建议先验证再决定**。不验证就改，等于在两条规则里盲选一条。

### 3.7 【可选】`thread_registry` 的并发

**源码事实**：`thread_registry.cpp` 的 `Add/Remove/At/Count/Find` 全部**无保护**。

- `Add`：只在 `Start()` 里调用，即启动期（main 线程 / 各 `thread_start`）。
- `Remove`：在 `Thread::Exit()` 里调用，可能来自**任意线程**（`Halt()` 里批量处决）。
- `At/Count/Find`：在 shell 线程（`thread list` / `thread show`）。

**风险**：shell 正在 `for (i < Count())` 遍历时，另一次 `Remove` 把尾部元素填补到中间 → 该线程被跳过或输出两次。

**架构判断**：`Remove` 只在**线程终止**时发生（`Isolate` 故障或 `thread exit` 命令），是低频事件，`thread list` 也是人手动敲的。**碰撞概率极低，但不是零。**

**建议做法**（三选一，建议 ①）：

1. **不处理**，在 `thread_registry.hpp` 注释里写明"遍历期间可能有线程退出，结果允许跳项"。代价 0。
2. 遍历期间 `irq_lock`（会关中断跨整段 `snprintf`，**不可接受**）。
3. 快照遍历：`thread list` 先 `memcpy` 一份指针数组再输出。代价：+64B 栈（16 项）。

**判定**：**建议选 ①**（只加注释）。理由：③ 的 64B 栈开销换一个「人眼看不出的跳项」，不划算。

### 3.8 【文档】`doc/线程编写指南.md` §6.7 会因本改动过期

**源码事实**：`doc/线程编写指南.md:599-615` 现在写的是：

> - 周期是**两拍之间的间隔**，不是「固定频率节拍器」——Task 自身耗时会计入总周期
> - 当前外壳不做补偿，`imu` / `remote` 都是「间隔」语义。

加了 `Absolute` 之后，**这两句都变成"默认模式下的行为"**，必须补上另一种模式。同时 `framework/init/README.md:442` 也要补。

**判定**：**必须做**（若采纳 §2）。文档和代码不一致是本项目明确记过的坑。

---

## 4 阶段与步表

> 下面每个阶段独立可采纳，互不依赖（除 P2 内部的先后）。
> **用户说"开始第 N 步" = 只改表格里那一个文件，改完停下报告，等"继续"。**
> **不许一次做多步，不许跨步连带改**（必要的连带写在「连带」栏）。

### P1 —— thread 绝对时间调度（建议采纳）

> **P1 拆成 4 步，每次只执行一步。**
> 用户说"开始第 N 步" = 只改表格里那一个文件，改完停下报告，等"继续"。
> 不许一次做多步，不许跨步连带改（必要时见每步的「连带」栏）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **1-1** | `framework/init/thread/thread.hpp` | ① 加 `PeriodMode` 枚举（§2.3）；② `Start()` 加第 7 参 + 降级保护；③ `task_` 加 `mode`；④ 加 `period_ticks_` / `next_tick_` 成员；⑤ 抽出 `WaitNextPeriod()` 并替换 `Run()` 结尾的 `k_sleep` | 无 | 无（默认参数，5 个既有调用点零改动） |
| **1-2** | `framework/init/thread/thread.hpp` | `ThreadStats` 加 `overrun_count` + `last_period_ns`；`GetStats()` 填这两个字段；`WaitNextPeriod()` 里采集实际周期 | 1-1 | 无（结构性新增字段，调用方用 `ThreadStats s{}` 初始化，不受影响） |
| **1-3** | `framework/shell/thread/thread_cmd.cpp` | `CmdShow()` 输出 `overrun` 与 `period` 两行；`CmdList()` 表格加一列 | 1-2 | **1-2 之前做这一步编不过**（字段不存在） |
| **1-4** | `framework/modules/imu/drivers/imu.hpp`、`project/thread/imu/trd_imu.cpp` | `imu.hpp:46` 的 `Start` 追传 `PeriodMode::Absolute`；`trd_imu.cpp` 无需改（它调 `imu_.Start(prio)`） | 1-1 | 无 |

> **编译状态**：1-1 做完工程可编（默认参数）；1-2 做完可编；**1-3 依赖 1-2**，顺序不能倒；1-4 做完可编。

### P2 —— 线程统计与安全收尾（建议采纳）

> **P2 拆成 3 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **2-1** | `framework/init/thread/thread.hpp` | 四个 `uint64_t` 统计的读写套 `irq_lock`（`Run()` 写入侧 / `GetStats()` 读取侧 / `ResetStats()` 清零侧），`k_cyc_to_ns_floor64` 留在临界区外 | 建议在 P1 之后做（P1 会往 `Run()` 里加采集点，先做 2-1 再改 P1 会重复动同一段） | 无 |
| **2-2** | `framework/init/thread/thread.hpp` | `Start()` 开头加 `run_.started` 幂等检查（§3.3） | 无 | 无 |
| **2-3** | `framework/init/thread/thread_fault.cpp` | `DefaultHandle` 的 `Isolate` 分支加 `break;`，`Recover` 与 `default` 分开写（§3.2） | 无 | 无 |

> **编译状态**：三步都独立可编，任一步做完工程都能编。

### P3 —— 清理与统一（可选，逐条采纳）

> **P3 拆成 2 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **3-1** | `framework/shell/log/log.hpp` + `framework/shell/log/log.cpp` | 删 `ProcessLogCommand` 声明与定义（§3.5） | 无 | 无（零调用者）。**两个文件必须同一步改**，否则留未定义符号 |
| **3-2** | `framework/shell/boot/boot_cmd.cpp` + `boot_cmd.hpp` | 改 `DispatchSubCmd`（§1.5 选项 A）；要把 `CmdTiming` 提成 `public` 或加 private static 分派壳 | 无 | 同上，两文件同步 |

> **编译状态**：3-1、3-2 各自独立可编。

### P4 —— 文档同步（若采纳 P1 必做）

> **P4 拆成 2 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **4-1** | `doc/线程编写指南.md` | §6.7 补 `PeriodMode` 两种语义、精度上限、`overrun` 说明 | P1-1 完成 | 无 |
| **4-2** | `framework/init/README.md` | 第 442 行的 `Start` 参数说明补 `mode` 参数；周期表格补绝对模式 | P1-1 完成 | 无 |

### 不在本次范围

| 项 | 原因 |
|---|---|
| 旧规划 `thread语义缺口修补规划.md` 的 P3（删 `KeepAlive`） | **已作废**，机制现在是活的（§3.4） |
| 旧规划的 P4（`IsAlive` 死条件） | **已作废**，代码已查 `"dead"` |
| 旧规划的 P5（更新线程编写指南） | 内容已被本 P4 覆盖，且原条目描述的是更老的代码 |
| 旧规划的 P9（停止原语 / Start 幂等） | 停止原语不做（`Pause/Resume` 已够）；Start 幂等已拆到本 P2-2 |
| `var` / `thread` / `boot` 分文件 | 定案不做（§1.3） |

---

## 5 待拍板清单

| # | 问题 | 选项 | 建议 |
|---|---|---|---|
| 1 | `var`/`thread`/`boot` 分不分层 | 分 / **不分** | 不分 |
| 2 | `boot_cmd.cpp` 要不要改成 `DispatchSubCmd` | 改 / **不改** | **不改**（要动可见性，性价比低） |
| 3 | thread 绝对时间调度做不做 | 做 / 不做 | 做 |
| 4 | API 形状 | **A 第 7 参** / B 结构体 / C setter | A |
| 5 | 追赶策略 | **① 丢弃对齐** / ② 连跑补课 / ③ 不处理 | ① |
| 6 | 要不要 `last_period_ns` 实际周期观测 | **要** / 不要 | 要（否则无法验收） |
| 7 | `imu` 迁不迁 `Absolute` | **迁** / 不迁 | 迁 |
| 8 | `test` 线程是否同步迁（作验证载体） | **迁** / 不迁 | 迁 |
| 9 | 64 位撕裂修复（P2-1） | **做** / 不做 | 做 |
| 10 | `Start()` 幂等（P2-2） | **做** / 不做 | 做 |
| 11 | `Isolate` 补 `break`（P2-3） | **做** / 不做 | 做 |
| 12 | 删 `ProcessLogCommand`（P3-1） | **删** / 留 | 删 |
| 13 | `var` 数字解析要不要先上板验证（§3.6） | 验证 / 跳过 | 验证 |
| 14 | `thread_registry` 并发 | **只加注释** / 快照遍历 / 不处理 | 只加注释 |
| 15 | 精度不够要不要提 `SYS_CLOCK_TICKS_PER_SEC` | 提到 10000 / **保持 2000** | 保持（先看 `last_period_ns` 实测） |

---

## 6 附：本次核对到的源码位置索引

| 主题 | 位置 |
|---|---|
| `Start` 签名与参数 | `framework/init/thread/thread.hpp:131` |
| `Run()` 外壳 | `framework/init/thread/thread.hpp:338-386` |
| 周期等待 | `framework/init/thread/thread.hpp:381-384` |
| `task_` 配置结构 | `framework/init/thread/thread.hpp:301-307` |
| 64 位统计成员 | `framework/init/thread/thread.hpp:309-318` |
| `GetStats` / `ResetStats` | `framework/init/thread/thread.hpp:274-289` / `257-267` |
| `Exit()` | `framework/init/thread/thread.hpp:169-190` |
| `ThreadBase::Exit` 声明 | `framework/init/thread/thread.hpp:98-103` |
| `IsAlive` 的 `"dead"` 判断 | `framework/init/thread/thread.hpp:401` |
| `DefaultHandle` 策略分支 | `framework/init/thread/thread_fault.cpp:65-89` |
| `Halt()` 跳过保留线程 | `framework/init/thread/thread_fault.cpp:32-40` |
| 注册表实现 | `framework/init/thread/thread_registry.cpp` |
| shell 命令族分派 | `framework/shell/shell.cpp:60-77` |
| 命令注册宏 | `framework/shell/shell_cmd.hpp:72-76` |
| 子命令查表 | `framework/shell/token.hpp:77-95` |
| `boot` 的 if/else 例外 | `framework/shell/boot/boot_cmd.cpp:88-98` |
| `var` 用 `strtoull` | `framework/shell/var/var.cpp:161/179/194/202` |
| `log` 手写解析与理由 | `framework/shell/log/cmd/log_cmd.cpp:47-58` |
| `ProcessLogCommand` 死代码 | `framework/shell/log/log.cpp:36-39`、`log.hpp:115` |
| 命令族注册点（5 处） | `shell.cpp:89`、`boot_cmd.cpp:100`、`log_cmd.cpp:141`、`thread_cmd.cpp:304`、`var.cpp:292` |
| 绝对超时宏 | `zephyr/include/zephyr/kernel.h:1579`（`K_TIMEOUT_ABS_TICKS`） |
| 内核绝对超时分流 | `zephyr/kernel/timeout.c:118-127` |
| tick 数读取 | `zephyr/include/zephyr/kernel.h:1966`（`k_uptime_ticks`） |
| 线程状态字符串 | `zephyr/kernel/include/kthread.h:16-23` |
| 当前 tick 配置 | `project/build/zephyr/.config`（`SYS_CLOCK_TICKS_PER_SEC=2000`、`TICKLESS_KERNEL=y`、`TIMEOUT_64BIT=y`） |
