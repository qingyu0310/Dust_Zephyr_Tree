# fatal 处理重构规划：架构解耦 + 线程冻结 + 日志保底

> 日期：2026-10-20
> 对象：`framework/cmd/fatal/fatal.cpp`（当前 0.1，64 行）
> 前置文档：`doc/fatal架构化规划.md`（2026-08-09，只讲了 per-arch 拆分，未执行）
> **本篇取代旧规划**，把两个问题一起解决：
> 1. `fatal.cpp` 里直接 `#if CONFIG_CPU_CORTEX_M / #elif CONFIG_RISCV`
> 2. 致命错误时线程还在跑、日志可能发不出去

---

## 1. 现状与问题

### 1.1 当前实现（`fatal.cpp` 全文 64 行）

```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

#if defined(CONFIG_CPU_CORTEX_M)
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s pc=%lx xpsr=%lx", ...);
#elif defined(CONFIG_RISCV)
    unsigned long mcause = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    DUST_LOG_ERR("[fatal] %s ... mcause=%lx mepc=%lx mstatus=%lx", ...);
#else
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s", ...);
#endif

    while (1) {}
}
```

### 1.2 五个问题

| # | 问题 | 后果 |
|---|---|---|
| **P1** | **架构细节暴露在主干** | 每加一个架构（RISC-V → ARM → Xtensa → ARC）就在 `fatal.cpp` 加一个 `#elif`，主干被芯片细节污染 |
| **P2** | **其他线程继续跑** | fatal 只停了当前线程（`while(1)`），其它线程照常运行——电机可能还在转、加热器还在通电 |
| **P3** | **日志可能发不出去** | `DUST_LOG_ERR` 走异步队列，依赖 shell 线程消费。fatal 发生在高优先级线程/ISR 时，shell（优先级 7）可能永远得不到调度 |
| **P4** | **`while(1){}` 空转** | 占满 CPU、耗电、发热；看门狗若开启会被喂死（当前未开） |
| **P5** | **现场信息不完整** | 只打 `pc / xpsr` 或 `mcause / mepc / mstatus`，没有栈指针、没有线程栈使用率、没有其它线程状态 |

### 1.3 为什么要"线程暂停 + 清理"

fatal 之后系统处于**不确定状态**：

- 某个线程踩坏了内存，其它线程可能正在用被踩坏的数据
- 电机、加热器、PWM 等外设可能停在危险状态
- 但**必须保留现场**——复位后什么都看不到

**目标**：fatal 时把系统冻结成一个"可观察的静止状态"，让 shell 有机会把现场发出去，然后停机等复位。

---

## 2. 目标

### 2.1 要做

1. **架构解耦**：`fatal.cpp` 不再出现任何 `CONFIG_CPU_*` 分支
2. **线程冻结**：fatal 时暂停所有其它线程，只留当前线程和 shell
3. **日志保底**：先尝试异步路径，再进一个同步轮询兜底，确保现场一定能发出去
4. **现场完整**：原因 / 线程名 / 架构寄存器 / 栈使用率 / 其它线程状态
5. **安全清理**：按注册顺序逆序调用 cleanup，把外设放回安全态

### 2.2 不做

- 不做自动复位（保留现场）
- 不做 coredump 到 flash（Zephyr `CONFIG_COREDUMP` 未开，且 DUST_LOG 通道已够用）
- 不引入新的 Kconfig 开关（fatal 是无条件编译的防呆代码）

---

## 3. 设计

### 3.1 架构解耦：接口在架构层，实现在子树 drivers

**分层原则照抄 USB HAL**：

```text
framework/drivers/communication/stream/usb/interface/usb_hal.hpp   ← 纯虚接口，不依赖平台
drivers/usb/hal/hpm/usb_hal_hpm.cpp                                ← 芯片实现
drivers/usb/hal/stm32/usb_hal_stm32.cpp                            ← 芯片实现
```

对应到 fatal：

```text
framework/cmd/fatal/esf_dump.hpp      ← 接口 + 通用流程（架构层）
drivers/fatal/hpm/esf_dump_riscv.cpp  ← RISC-V 实现（子树）
drivers/fatal/st/esf_dump_cortex_m.cpp← Cortex-M 实现（子树）
```

#### 3.1.1 接口层（`framework/cmd/fatal/esf_dump.hpp`）

```cpp
#pragma once

#include <zephyr/kernel.h>

/**
 * @brief 转储异常栈帧的架构相关字段
 *
 * 架构层只声明接口；具体实现放在子树 drivers/fatal/<vendor>/。
 * 默认 __weak 空实现（fatal.cpp 内），新架构未提供时链接不报错。
 *
 * @param esf 异常栈帧（可能为 nullptr）
 */
void DumpEsf(const struct arch_esf* esf);
```

#### 3.1.2 实现层（`drivers/fatal/`）

```text
drivers/
└── fatal/
    ├── hpm/esf_dump_riscv.cpp      RISC-V：mcause / mepc / mstatus / ra / sp
    └── st/esf_dump_cortex_m.cpp    Cortex-M：pc / xpsr / lr / sp
```

**为什么 RISC-V 归 `hpm/` 而不是 `riscv/`**：与 USB HAL 一致——子树 drivers 按**厂商**分目录（`hpm` / `stm32`），而不是按架构。虽然当前 RISC-V 只有 HPM 一家，但目录语义保持一致。

#### 3.1.3 装配（`drivers/CMakeLists.txt`）

```cmake
# fatal 异常栈帧转储（芯片相关，2026-10-20）
if(CONFIG_DUST_FATAL_ESF_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm/esf_dump_riscv.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm
        ${FW_ROOT}/cmd/fatal)
endif()

if(CONFIG_DUST_FATAL_ESF_STM32)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st/esf_dump_cortex_m.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st
        ${FW_ROOT}/cmd/fatal)
endif()
```

#### 3.1.4 Kconfig（`drivers/Kconfig`）

```kconfig
# fatal 异常栈帧转储（芯片相关）
config DUST_FATAL_ESF_HPM
    bool "HPM RISC-V esf dump"
    default n
    depends on RISCV
    help
      RISC-V 异常栈帧转储：mcause / mepc / mstatus / ra / sp。

config DUST_FATAL_ESF_STM32
    bool "STM32 Cortex-M esf dump"
    default n
    depends on CPU_CORTEX_M
    help
      Cortex-M 异常栈帧转储：pc / xpsr / lr / sp。
```

**谁打开**：板卡 `.conf`

```conf
# project/boards/hpm/hpm5361icb/hpm5361icb.conf
CONFIG_DUST_FATAL_ESF_HPM=y

# project/boards/st/puzhong/stm32f4_disco.conf
CONFIG_DUST_FATAL_ESF_STM32=y
```

#### 3.1.5 默认兜底

`fatal.cpp` 内提供 `__weak` 空实现：

```cpp
/**
 * @brief 默认空实现：板卡没选 esf dump 实现时链接不报错
 */
__weak void DumpEsf(const struct arch_esf* esf)
{
    ARG_UNUSED(esf);
    DUST_LOG_WRN("[fatal] esf dump not configured for this board");
}
```

**为什么保留 `__weak`**：

- `fatal.cpp` 是**无条件编译**的防呆代码，不能因为某个板卡忘了开 Kconfig 就链接失败
- 这与 Zephyr 自己 `k_sys_fatal_error_handler` 用 `__weak` 的思路一致
- 新架构接进来时，即使忘写实现，系统照常编译运行，只是少打几个寄存器

#### 3.1.6 与 USB HAL 的对应关系

| USB | fatal |
|---|---|
| `framework/.../usb/interface/usb_hal.hpp` | `framework/cmd/fatal/esf_dump.hpp` |
| `drivers/usb/hal/hpm/usb_hal_hpm.cpp` | `drivers/fatal/hpm/esf_dump_riscv.cpp` |
| `drivers/usb/hal/stm32/usb_hal_stm32.cpp` | `drivers/fatal/st/esf_dump_cortex_m.cpp` |
| `CONFIG_DUST_USB_DEVICE_HAL_HPM` | `CONFIG_DUST_FATAL_ESF_HPM` |
| 板卡 conf 打开 | 板卡 conf 打开 |
| `UsbHal` 纯虚基类 | `DumpEsf` 自由函数 + `__weak` |

**差别**：USB HAL 是虚函数（运行时多态，因为 Usb 只持指针）；fatal 是编译期绑定（只有一个实现，用 `__weak` 更轻）。

### 3.2 线程冻结：留 shell 一条命

#### 3.2.1 冻结策略

**只冻结业务线程，shell 线程继续跑。**

```cpp
// 冻结除「当前线程」和「shell 线程」外的所有已登记线程
for (uint8_t i = 0; i < thread_registry::Count(); ++i)
{
    ThreadBase* t = thread_registry::At(i);
    if (t == nullptr)            continue;
    if (t->Tid() == self)        continue;   // 不冻结自己
    if (t->KeepAlive())          continue;   // 不冻结 shell（调试通道）
    t->Pause();
}
```

**为什么保留 shell**：

| 理由 | 说明 |
|---|---|
| **它是日志的唯一消费者** | `DUST_LOG_*` 只是入队，真正发出去靠 shell 线程 `FormatOnePendingRecord` + `SendNextFrameIfIdle` |
| **它是唯一的观察窗口** | fatal 之后还能在串口里敲 `thread list` / `thread show` 看现场 |
| **它是低优先级** | `ThreadPrio::Lowest`(7)，只有在所有业务线程冻结后才轮得上——天然不会干扰故障现场 |

**这与"冻结系统"并不矛盾**：冻结的目的是**停止业务逻辑继续破坏状态**，而不是停止观察。shell 只读、只发送、不改业务状态。

#### 3.2.2 标记方式：`SetKeepAlive`

不要用名字 `strcmp("shell")` 判断——名字是日志标识，随时可能改。

在 `ThreadBase` 加一个标记：

```cpp
class ThreadBase
{
public:
    virtual void SetKeepAlive(bool on) = 0;
    virtual bool KeepAlive() const = 0;
    ...
};
```

`Thread<StackSize>` 用 `atomic_t keep_alive_` 实现。

**由 shell 自己在启动时声明**：

```cpp
static bool thread_start()
{
    shell_.SetKeepAlive(true);      // ← fatal 时不冻结我
    shell_.Start();
    ...
}
```

**默认 `false`**——没声明的线程一律冻结。

#### 3.2.3 `k_thread_suspend` 在 fatal 上下文安全吗

**结论：安全，但要看 fatal 是从哪进来的。**

| 场景 | `k_is_in_isr()` | `k_thread_suspend` 行为 |
|---|---|---|
| 线程上下文发生异常 | `false` | 正常挂起 |
| ISR 中发生异常 | `true` | Zephyr 允许在 ISR 里 suspend 其它线程 |

Zephyr 文档明确：`k_thread_suspend` 在中断上下文可以调用，如果目标线程正在另一个 CPU 上跑会自旋等待。**单核 MCU 无 SMP，不涉及**。

**但有一个限制**：`k_thread_suspend` 不能挂起当前正在运行的线程（自己挂自己会死锁）。

所以顺序必须是：

```text
① 先暂停其它线程（不包括自己和 shell）
② 打印现场
③ 当前线程自己进入「让出 CPU 的休眠循环」
```

#### 3.2.4 为什么不 `k_thread_abort`

`abort` 会**终止线程并释放资源**，但：

- 线程持有的锁不会自动释放（Zephyr 文档明确说 kernel 不回收）
- 栈内容被标记为可复用，现场丢失
- 我们想要的是"冻结观察"，不是"清理退出"

**用 `suspend` 保留完整现场**。

### 3.3 日志：让 shell 正常发，不接管它的活

#### 3.3.1 主路径：shell 照常消费

fatal 线程打印后**主动让出 CPU**，shell（优先级 7）就能跑起来：

```cpp
DUST_LOG_ERR(...)              // ① 入队
DumpEsf(esf)                   // ② 架构寄存器
...
for (;;) { k_msleep(1000); }   // ③ 让出 CPU → shell 被调度 → 泵日志帧
```

**为什么这样就够**：

- shell 线程没被冻结（`KeepAlive`）
- 当前线程 `k_msleep` 后，调度器会选下一个就绪线程——shell 优先级最低，但只要业务线程都被冻结，它就是唯一可运行的
- shell 的 `Task` 循环本身就在做 `SendNextFrameIfIdle()`，不需要额外机制

**验证方式**：fatal 后串口应该看到完整的 `[fatal] ...` 和 `[err] ...`，并且还能继续敲命令。

#### 3.3.2 为什么之前会发不出去

原实现 `while (1) {}` **空转**，且没有让出 CPU：

- 如果 fatal 发生在高优先级线程（如 `imu` 是 `High`(3)），它空转时 shell(7) 永远得不到调度
- 队列里的帧全部堵死，串口一片安静

**修法就是让出 CPU**，而不是接管 shell 的工作。

#### 3.3.3 例外：fatal 在 ISR 里

ISR 上下文不能 `k_msleep`，也永远回不到线程调度——shell 跑不了。

这种情况**才**需要同步兜底：

```cpp
if (k_is_in_isr()) {
    Log::FlushBlocking();          // 轮询发送，不依赖线程
    for (;;) { __asm__ volatile("wfi"); }
}
```

`FlushBlocking()` 的实现：

```cpp
/**
 * @brief 阻塞式排空日志队列（仅 ISR 中 fatal 时使用）
 *
 * 绕过 DMA 和 shell 线程，直接逐字节轮询。会长时间占用 CPU，
 * 只能在系统已不可恢复、且无法回到线程调度时调用。
 */
void Log::FlushBlocking();
```

**实现要点**：

- 先把 `recq_` 的未格式化记录全部展开（`FormatOnePendingRecord`）
- 再遍历 `txq_` 所有帧，`uart_poll_out` 逐字节发送
- **不加锁**：其它线程已冻结，ISR 中加锁反而可能死锁
- 开头等 DMA 完成（最多 100ms），避免与进行中的 DMA 冲突

**代价**：921600 波特下，一帧 128B ≈ 1.4ms，10 帧 ≈ 14ms。

#### 3.3.4 为什么不用 `printk`

`printk` 在 `CONFIG_PRINTK=n` 时是空实现，而且即使开了也走 Zephyr console，与自研 shell 抢占同一个 UART——**正是本项目当初自研 DUST_LOG 要绕开的坑**。

用 `uart_poll_out` 直接操作设备，绕过所有中间层。

### 3.4 安全清理

fatal 时按注册的**逆序**调用 `cleanup`：

```cpp
for (int i = static_cast<int>(inited_count) - 1; i >= 0; --i)
{
    if (inited_entries[i]->cleanup != nullptr) {
        inited_entries[i]->cleanup();
    }
}
```

**复用启动期的 `RollbackInitedEntries()`**——它已经实现了逆序 + 空指针保护。

**为什么不是所有线程都要清理**：

- 清理针对的是**初始化项**（PWM / GPIO / Flash / 外设）
- 线程本身没有 `cleanup`，冻结即可

**清理时机**：

```text
冻结业务线程 → 打印现场 → 等 shell 发完 → 清理外设 → 休眠停机
```

清理放在日志之后，因为清理过程可能产生日志（例如"Flash 已掉电"），而这些日志还要靠 shell 发出去。

**"等 shell 发完"怎么写**：

```cpp
// 给 shell 一段时间把队列泵空（同步段：每帧 ~1.4ms，8 帧 ≈ 11ms）
for (uint8_t i = 0; i < 50; ++i) {
    k_msleep(10);           // 让 shell 跑
}
```

或者更精确——等队列空：

```cpp
for (uint8_t i = 0; i < 100 && Log::Pending(); ++i) {
    k_msleep(10);
}
```

需要 `Log` 新增一个只读查询：

```cpp
static bool Pending();      // 队列里还有未发送的帧
```

### 3.5 停机：休眠而不是空转，且保留可交互

```cpp
// 线程上下文
for (;;) {
    k_msleep(1000);
}
```

**为什么是 `k_msleep` 而不是 `while(1){}`**：

| | `while(1){}` | `k_msleep(1000)` |
|---|---|---|
| CPU | 100% 占用 | 让出调度 |
| shell | **永远跑不了** | 每 1 秒被调度一次，可以敲命令 |
| 功耗 | 高 | 低 |
| 温度 | 高 | 低 |

**fatal 之后系统不是"死掉"，而是"冻结 + 可观察"**：

```text
thread list      ← 能看到哪些线程被冻结（state=paused）
thread show imu  ← 能看到故障前最后一次运行的耗时
boot timing      ← 能看到初始化花了多久
```

这是保留 shell 的直接收益——**故障现场可以由人主动查询，而不是只能看开机那几行日志**。

**ISR 上下文**：无法回到调度，用 `wfi` 停机。

| `k_is_in_isr()` | 停机方式 |
|---|---|
| `false`（线程上下文） | `for(;;) k_msleep(1000)` — shell 保持可交互 |
| `true`（ISR 上下文） | `for(;;) { __asm__ volatile("wfi"); }` |

---

## 4. 目标代码结构

```text
framework/cmd/fatal/          架构层：接口 + 通用流程
├── fatal.cpp                主干：冻结 → 打印 → 排空 → 清理 → 停机（+ __weak 兜底）
└── esf_dump.hpp             void DumpEsf(const struct arch_esf*) 声明

zephyr_user/drivers/fatal/    子树层：芯片相关实现
├── hpm/esf_dump_riscv.cpp   RISC-V：mcause / mepc / mstatus / ra / sp
└── st/esf_dump_cortex_m.cpp Cortex-M：pc / xpsr / lr / sp
```

**CMake（`framework/cmd/CMakeLists.txt`）**：

```cmake
target_sources(app PRIVATE fatal/fatal.cpp)      # 无条件编译
```

**CMake（`drivers/CMakeLists.txt`）**：

```cmake
if(CONFIG_DUST_FATAL_ESF_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm/esf_dump_riscv.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm
        ${FW_ROOT}/cmd/fatal)
endif()

if(CONFIG_DUST_FATAL_ESF_STM32)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st/esf_dump_cortex_m.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st
        ${FW_ROOT}/cmd/fatal)
endif()
```

**`fatal.cpp` 主干（目标代码）**：

```cpp
/**
 * @file fatal.cpp
 * @brief 致命错误处理 — 冻结业务线程、保留 shell、安全清理
 * @version 0.2
 * @date 2026-10-20
 */

#include <zephyr/kernel.h>
#include <zephyr/fatal_types.h>
#include "log.hpp"
#include "esf_dump.hpp"
#include "thread_registry.hpp"
#include "Init_entry.hpp"

/**
 * @brief 致命错误原因字符串
 */
static const char* FatalReasonStr(unsigned int reason)
{
    switch (reason)
    {
        case K_ERR_CPU_EXCEPTION:  return "CPU_EXCEPTION";
        case K_ERR_SPURIOUS_IRQ:   return "SPURIOUS_IRQ";
        case K_ERR_STACK_CHK_FAIL: return "STACK_CHK_FAIL";
        case K_ERR_KERNEL_OOPS:    return "KERNEL_OOPS";
        case K_ERR_KERNEL_PANIC:   return "KERNEL_PANIC";
        default:                   return "UNKNOWN";
    }
}

/**
 * @brief 冻结业务线程，保留当前线程与 KeepAlive 线程（shell）
 * @param self 当前线程
 */
static void FreezeBusinessThreads(k_tid_t self)
{
    for (uint8_t i = 0; i < thread_registry::Count(); ++i)
    {
        ThreadBase* t = thread_registry::At(i);
        if (t == nullptr)     continue;
        if (t->Tid() == self) continue;      // 不冻结自己
        if (t->KeepAlive())   continue;      // 不冻结 shell（日志消费者）
        t->Pause();
    }
}

/**
 * @brief 等 shell 把日志队列泵空
 * @param timeout_ms 最长等待
 */
static void WaitLogDrained(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms && Log::Pending(); waited += 10) {
        k_msleep(10);                        // 让出 CPU → shell 得到调度
    }
}

/**
 * @brief 致命错误入口
 *
 * 线程上下文：冻结业务线程 → 打印 → 让 shell 发完 → 清理 → 休眠
 * ISR 上下文：无法回到调度 → 轮询发送 → wfi
 */
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const bool  in_isr = k_is_in_isr();
    const k_tid_t self = k_current_get();
    const char* thread = k_thread_name_get(self);

    // ① 冻结业务线程（ISR 中同样可调；shell 保留）
    FreezeBusinessThreads(self);

    // ② 打印现场
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s%s",
                 FatalReasonStr(reason), reason,
                 (thread != nullptr) ? thread : "?", in_isr ? " [ISR]" : "");

    DumpEsf(esf);

    size_t unused = 0;
    if (k_thread_stack_space_get(self, &unused) == 0) {
        DUST_LOG_ERR("[fatal] stack unused=%zu", unused);
    }

    // ③ 日志：线程上下文交给 shell，ISR 上下文轮询兜底
    if (in_isr) {
        Log::FlushBlocking();
    } else {
        WaitLogDrained(500);
    }

    // ④ 安全清理（清理产生的日志同样由 shell 发出）
    RollbackAllInited();

    if (in_isr) {
        Log::FlushBlocking();
        for (;;) { __asm__ volatile("wfi"); }
    }

    WaitLogDrained(500);
    for (;;) { k_msleep(1000); }             // 休眠停机：shell 继续可交互
}
```

---

## 5. 需要新增的接口

### 5.1 `ThreadBase::Tid()` 与 `KeepAlive()`

```cpp
class ThreadBase
{
public:
    virtual k_tid_t Tid() const = 0;
    virtual void    SetKeepAlive(bool on) = 0;
    virtual bool    KeepAlive() const = 0;
    ...
};
```

```cpp
// Thread<> 里
k_tid_t Tid() const override { return const_cast<k_thread*>(&thread_); }
void    SetKeepAlive(bool on) override { atomic_set(&keep_alive_, on ? 1 : 0); }
bool    KeepAlive() const override { return atomic_get(&keep_alive_) != 0; }

private:
    atomic_t keep_alive_ = ATOMIC_INIT(0);   // fatal 时是否保留（默认冻结）
```

**为什么需要 `Tid()`**：`FreezeBusinessThreads` 要区分"当前线程"和"其它线程"，不能暂停自己。

**为什么需要 `KeepAlive()`**：冻结时要跳过 shell 线程，否则日志发不出去、命令也敲不了。

**为什么不用名字判断**：`"shell"` 是日志标识，随时可能改；用显式标记更稳。

**谁设置**：

```cpp
// shell.cpp 的 thread_start
static bool thread_start()
{
    shell_.SetKeepAlive(true);      // ← fatal 时不冻结 shell
    shell_.Start();
    ...
}
```

### 5.2 `Log::FlushBlocking()`

见 §3.3.2。需要访问 `txq_` 和 `recq_`（`Log` 私有成员），因此在 `log.cpp` 里实现。

**签名**：

```cpp
static void FlushBlocking();
```

**内部**：

```cpp
void Log::FlushBlocking()
{
    Stream* s = pump_.Channel();
    if (s == nullptr) return;

    // 先把未格式化的记录展开
    while (FormatOnePendingRecord()) {}

    // 逐帧轮询发送（不加锁：其它线程已暂停）
    for (;;)
    {
        unsigned key = irq_lock();
        TxFrame* f = txq_.PopNextFrameLocked(dbg_.ActiveEpoch());
        irq_unlock(key);

        if (f == nullptr) break;

        for (uint16_t i = 0; i < f->len; ++i) {
            uart_poll_out(s->Dev(), static_cast<uint8_t>(f->data[i]));
        }

        key = irq_lock();
        txq_.ReleaseFrameLocked(f);
        irq_unlock(key);
    }
}
```

**需要 `Stream::Dev()`**：暴露底层 `const struct device*`。

```cpp
// Stream 基类
virtual const struct device* Dev() const = 0;
```

### 5.3 启动项表的访问接口

`Init_entry.cpp` 里的 `inited_entries[]` 是 `static`，fatal 拿不到。需要暴露两个只读接口：

```cpp
// Init_entry.hpp
uint8_t      BootInitCount();
CleanupFunc  BootInitCleanup(uint8_t index);
```

或更简洁：

```cpp
/**
 * @brief 逆序清理所有已成功初始化项（fatal / 停机安全路径）
 */
void RollbackAllInited();
```

**建议后者**——fatal 只需要"全部清理"，不需要遍历细节。

---

## 6. 分阶段执行

| 阶段 | 内容 | 优先级 | 风险 |
|---|---|---|---|
| **A** | per-arch `DumpEsf` 拆分 | 高 | 低 |
| **B** | `ThreadBase::Tid()` + `FreezeOtherThreads` | 高 | 低 |
| **C** | `Stream::Dev()` + `Log::FlushBlocking()` | 高 | 中（轮询发送与 DMA 并发） |
| **D** | `RollbackAllInited()` + fatal 调用清理 | 中 | 低 |
| **E** | 栈使用率打印 | 低 | 低 |
| **F** | ISR 分支用 `wfi` 停机 | 低 | 低 |

### 阶段 A：esf dump 下沉子树

**目标**：`fatal.cpp` 里不再出现任何 `CONFIG_CPU_*`，芯片相关实现移到子树 `drivers/fatal/`。

**改动 1**：新增 `framework/cmd/fatal/esf_dump.hpp`

```cpp
/**
 * @file esf_dump.hpp
 * @author qingyu
 * @brief 异常栈帧转储接口 — 架构层只声明，实现放在子树 drivers
 * @version 0.1
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <zephyr/kernel.h>

/**
 * @brief 转储异常栈帧的架构相关字段
 *
 * 架构层只声明接口；具体实现放在 zephyr_user/drivers/fatal/<vendor>/。
 * fatal.cpp 内提供 __weak 空实现，板卡未选实现时链接不报错。
 *
 * @param esf 异常栈帧（可能为 nullptr）
 */
void DumpEsf(const struct arch_esf* esf);
```

**改动 2**：`fatal.cpp` 删掉 `#if/#elif/#else`，改为调用 + `__weak` 兜底

```cpp
#include "esf_dump.hpp"

extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?");

    DumpEsf(esf);

    for (;;) {}
}

/**
 * @brief 默认空实现：板卡没选 esf dump 实现时链接不报错
 */
__weak void DumpEsf(const struct arch_esf* esf)
{
    ARG_UNUSED(esf);
    DUST_LOG_WRN("[fatal] esf dump not configured for this board");
}
```

**改动 3**：新增 `drivers/fatal/hpm/esf_dump_riscv.cpp`

```cpp
/**
 * @file esf_dump_riscv.cpp
 * @author qingyu
 * @brief HPM RISC-V 异常栈帧转储
 * @version 0.1
 * @date 2026-10-20
 */

#include "esf_dump.hpp"
#include "log.hpp"

void DumpEsf(const struct arch_esf* esf)
{
    unsigned long mcause  = 0;
    unsigned long mtval   = 0;
    __asm__ volatile("csrr %0, mcause" : "=r" (mcause));
    __asm__ volatile("csrr %0, mtval"  : "=r" (mtval));

    if (esf == nullptr) {
        DUST_LOG_ERR("[fatal] mcause=%08lx mtval=%08lx esf=null",
                     mcause, mtval);
        return;
    }

    DUST_LOG_ERR("[fatal] mcause=%08lx mtval=%08lx mepc=%08lx mstatus=%08lx ra=%08lx",
                 mcause, mtval,
                 (unsigned long)esf->mepc,
                 (unsigned long)esf->mstatus,
                 (unsigned long)esf->ra);
    DUST_LOG_ERR("[fatal] sp=%08lx a0=%08lx a1=%08lx",
                 (unsigned long)((uintptr_t)esf + sizeof(struct arch_esf)),
                 (unsigned long)esf->a0,
                 (unsigned long)esf->a1);
}
```

> **字段名核实**：实施前对照 `zephyr/arch/riscv/...` 的 `struct arch_esf`（`mepc` / `mstatus` / `ra` / `a0` / `a1`）。
> `sp` 在 esf 里通常不单独存，需要自己算（`esf + sizeof`），或者从 `esf->sp` 读（版本差异）。

**改动 4**：新增 `drivers/fatal/st/esf_dump_cortex_m.cpp`

```cpp
/**
 * @file esf_dump_cortex_m.cpp
 * @author qingyu
 * @brief STM32 Cortex-M 异常栈帧转储
 * @version 0.1
 * @date 2026-10-20
 */

#include "esf_dump.hpp"
#include "log.hpp"

void DumpEsf(const struct arch_esf* esf)
{
    if (esf == nullptr) {
        DUST_LOG_ERR("[fatal] esf=null");
        return;
    }

    DUST_LOG_ERR("[fatal] pc=%08lx xpsr=%08lx lr=%08lx",
                 (unsigned long)esf->basic.pc,
                 (unsigned long)esf->basic.xpsr,
                 (unsigned long)esf->basic.lr);
}
```

> **字段名核实**：对照 `zephyr/arch/arm/cortex_m/exc.h` 的 `struct arch_esf`（`basic.pc` / `basic.xpsr` / `basic.lr` 或 `callee_saved.lr`）。

**改动 5**：`drivers/Kconfig` 增加两个符号

```kconfig
# fatal 异常栈帧转储（芯片相关）
config DUST_FATAL_ESF_HPM
    bool "HPM RISC-V esf dump"
    default n
    depends on RISCV
    help
      RISC-V 异常栈帧转储：mcause / mtval / mepc / mstatus / ra。

config DUST_FATAL_ESF_STM32
    bool "STM32 Cortex-M esf dump"
    default n
    depends on CPU_CORTEX_M
    help
      Cortex-M 异常栈帧转储：pc / xpsr / lr。
```

**改动 6**：`drivers/CMakeLists.txt` 增加装配

见 §4。

**改动 7**：板卡 conf 打开

```conf
# hpm5361icb.conf
CONFIG_DUST_FATAL_ESF_HPM=y

# stm32f4_disco.conf
CONFIG_DUST_FATAL_ESF_STM32=y
```

**改动 8**：`framework/cmd/CMakeLists.txt` 保持不变

```cmake
target_sources(app PRIVATE fatal/fatal.cpp)   # 无条件编译（防呆）
```

**验证**：

1. 两块板分别编译通过；
2. 故意触发异常（空指针解引用），观察对应架构的寄存器被打印；
3. **把板卡 conf 里的 `CONFIG_DUST_FATAL_ESF_*` 关掉再编**，应看到 `[fatal] esf dump not configured for this board` 而不是链接错误。

**产出**：
- `framework/cmd/fatal/esf_dump.hpp`（新增）
- `framework/cmd/fatal/fatal.cpp`（改）
- `drivers/fatal/hpm/esf_dump_riscv.cpp`（新增）
- `drivers/fatal/st/esf_dump_cortex_m.cpp`（新增）
- `drivers/Kconfig` / `drivers/CMakeLists.txt`（改）
- 两块板卡 conf（改）

### 为什么不下沉到 framework/drivers

`framework/drivers` 是**通用外设封装**（Stream / Can / Spi），面对的是"所有板卡都有的外设 API"。

而 `esf` 结构体是 **Zephyr arch 层的芯片相关结构**（ARM 的 `basic.pc`、RISC-V 的 `mepc`），属于板卡/SoC 语义——和 USB HAL 一样，应该放子树 `drivers/`。

判断标准：

| 问题 | 放哪 |
|---|---|
| 所有板卡都能用同一份代码？ | `framework/` |
| 需要按芯片/厂商不同实现？ | `drivers/<厂商>/` |

### 阶段 B：线程冻结（保留 shell）

**改动 1**：`ThreadBase` 增加 `Tid()` 与 `KeepAlive()`

```cpp
virtual k_tid_t Tid() const = 0;
virtual void    SetKeepAlive(bool on) = 0;
virtual bool    KeepAlive() const = 0;
```

`Thread<StackSize>` 实现：

```cpp
k_tid_t Tid() const override { return const_cast<k_thread*>(&thread_); }
void    SetKeepAlive(bool on) override { atomic_set(&keep_alive_, on ? 1 : 0); }
bool    KeepAlive() const override { return atomic_get(&keep_alive_) != 0; }

private:
    atomic_t keep_alive_ = ATOMIC_INIT(0);   // 默认冻结
```

**改动 2**：`shell.hpp` 增加转发并在 `thread_start` 里置位

```cpp
// Shell
void SetKeepAlive(bool on) { thread_.SetKeepAlive(on); }

// shell.cpp 的 thread_start
static bool thread_start()
{
    shell_.SetKeepAlive(true);      // ← fatal 时不冻结我
    shell_.Start();
    Log::MarkShellThreadReady();
    ...
}
```

**改动 3**：`fatal.cpp` 增加 `FreezeBusinessThreads()`

见 §4。

**注意**：`thread_registry` 的 `At()` / `Count()` 是静态的，fatal 可以直接调。

**验证**：fatal 后 `thread list` 里业务线程应为 `paused`，shell 自己仍是 `running`。

### 阶段 C：日志可见性

**改动 0**：`Log` 增加 `Pending()`

```cpp
/**
 * @brief 队列里是否还有未发送的帧
 */
static bool Pending()
{
    unsigned key = irq_lock();
    bool pending = txq_.NotEmptyLocked() || recq_.NotEmptyLocked();
    irq_unlock(key);
    return pending;
}
```

需要 `TxFrameQueue` / `LogRecordQueue` 各加一个 `NotEmptyLocked()`（读 `head != kNullIndex`）。

**改动 1**：`Stream` 增加 `Dev()`

```cpp
virtual const struct device* Dev() const = 0;
```

`UartDma` / `Usb` / `Rs485` 各实现。**没有底层设备的 Stream 子类返回 `nullptr`**（`FlushBlocking` 会跳过）。

**改动 2**：`Log::FlushBlocking()`

见 §5.2。

**改动 3**：`fatal.cpp` 按上下文选择路径

```cpp
if (in_isr) {
    Log::FlushBlocking();       // ISR：轮询兜底
} else {
    WaitLogDrained(500);        // 线程：交给 shell
}
```

**风险**：如果 fatal 时 DMA 正在进行中，轮询发送会和 DMA 冲突（两个写同一个 UART）。

**处理**：`FlushBlocking` 开头先等一分钟或者检查 `pump_.Sending()`：

```cpp
// 等待当前 DMA 完成（最多 100ms）
const uint32_t start = k_uptime_get_32();
while (pump_.Sending() && k_uptime_get_32() - start < 100) {
    k_busy_wait(100);
}
```

### 阶段 D：安全清理

**改动 1**：`Init_entry.hpp` 增加声明

```cpp
/**
 * @brief 逆序清理所有已成功初始化项（停机安全路径）
 *
 * 用于 fatal 处理：把 PWM / GPIO / Flash 等外设放回安全态。
 * 与启动失败的回滚是同一套实现。
 */
void RollbackAllInited();
```

**改动 2**：`Init_entry.cpp` 实现

```cpp
void RollbackAllInited()
{
    RollbackInitedEntries();
}
```

**改动 3**：`fatal.cpp` 调用

```cpp
RollbackAllInited();
```

**顺序**：在日志之后，因为清理可能产生日志。

### 阶段 E：栈使用率

```cpp
size_t unused = 0;
if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
    DUST_LOG_ERR("[fatal] stack unused=%zu", unused);
}
```

需要 `CONFIG_THREAD_STACK_INFO` + `CONFIG_INIT_STACKS`（当前 `prj.conf` 已开）。

**fatal 发生在 ISR 时**：`k_current_get()` 返回被打断的线程，栈信息仍然有效。

### 阶段 F：ISR 停机分支

```cpp
if (k_is_in_isr()) {
    for (;;) { __asm__ volatile("wfi"); }
}
```

**注意**：`wfi` 是 RISC-V / ARM 通用指令。如果某个架构没有 `wfi`，用 `k_cpu_idle()`（Zephyr 的通用空转）。

**更稳的写法**：

```cpp
if (k_is_in_isr()) {
    for (;;) { arch_nop(); }     // 或 k_cpu_idle()
}
```

---

## 7. 验证清单

```text
[ ] 编译通过（HPM / Cortex-M 两块板）
[ ] 正常启动日志不变（fatal 不影响正常路径）
[ ] 故意触发栈溢出（CONFIG_HW_STACK_PROTECTION），观察：
    [ ] [fatal] STACK_CHK_FAIL (reason=2) thread=xxx
    [ ] [fatal] mcause=... mepc=... mstatus=...   （RISC-V）
    [ ] 日志完整发出（PC 串口能看到全部内容）
    [ ] 外设已清理（蜂鸣器停、PWM 停）
    [ ] **业务线程全部 paused；shell 仍是 running**
    [ ] **fatal 后仍能在串口敲 thread list / thread show**
    [ ] 系统不空转（CPU 占用下降）
[ ] 故意触发空指针，观察 pc / mepc 指向出错位置
[ ] 在 ISR 中触发异常（例如某中断里故意除零），观察 [ISR] 标记
```

---

## 8. 风险与回退

| 风险 | 影响 | 应对 |
|---|---|---|
| `k_thread_suspend` 在 fatal 上下文不可用 | 线程冻结失败 | 用 `k_is_in_isr()` 分支；ISR 中跳过冻结 |
| 轮询发送与 DMA 冲突 | 日志乱码 | `FlushBlocking` 开头等 DMA 完成（最多 100ms） |
| `RollbackAllInited` 里某个 cleanup 阻塞 | 停机卡住 | cleanup 契约要求快速返回；加超时不现实（fatal 里无法打断） |
| Cortex-M / RISC-V 的 `arch_esf` 字段名不对 | 编译失败 | 对照 `zephyr/arch/arm/cortex_m/exc.h`、`zephyr/arch/riscv/...` 实际结构 |
| 板卡忘了开 `DUST_FATAL_ESF_*` | 少打印寄存器（不报错） | `__weak` 兜底打一行提示；文档要求新板卡配置 |
| `Stream::Dev()` 破坏了抽象 | 所有 Stream 子类都要改 | 默认返回 `nullptr`，只有 UartDma 需要真实设备 |
| `__weak` 在某些链接器下失效 | 新架构链接失败 | 已验证 GCC/LD 支持；如失效改为弱符号表 |

**回退**：阶段 A~F 相互独立，每个阶段可单独回退。A 回退 = 恢复 `#if` 分支。

---

## 9. 与旧规划的关系

`doc/fatal架构化规划.md`（2026-08-09）只提出了「per-arch 拆分」和「zbus 发布失败日志」两件事，**从未执行**。

本规划：

- **继承** 它的 per-arch 拆分方案（阶段 A）
- **扩展** 线程冻结、日志保底、安全清理（阶段 B/C/D）
- **放弃** 它的「zbus 发布失败日志」部分（与本任务无关，可单独做）

**旧规划文档保留**，作为历史记录；执行以本文为准。

---

## 10. 执行清单

```text
[ ] 阶段 A-1：架构层新增 framework/cmd/fatal/esf_dump.hpp
[ ] 阶段 A-2：fatal.cpp 删 #if，改调 DumpEsf + __weak 兜底
[ ] 阶段 A-3：子树新增 drivers/fatal/hpm/esf_dump_riscv.cpp
[ ] 阶段 A-4：子树新增 drivers/fatal/st/esf_dump_cortex_m.cpp
[ ] 阶段 A-5：drivers/Kconfig 加 DUST_FATAL_ESF_HPM / _STM32
[ ] 阶段 A-6：drivers/CMakeLists.txt 按 CONFIG_ 装配
[ ] 阶段 A-7：两块板卡 conf 打开对应符号
[ ] 阶段 A-8：编译验证（HPM + Cortex-M）
[ ] 阶段 A-9：关掉符号再编，验证 __weak 兜底生效
[ ] 阶段 B-1：ThreadBase 加 Tid() / KeepAlive()，Thread<> 实现
[ ] 阶段 B-2：Shell 转发 SetKeepAlive 并在 thread_start 置位
[ ] 阶段 B-3：fatal.cpp 加 FreezeBusinessThreads
[ ] 阶段 B-4：上板验证业务线程 paused、shell 仍 running
[ ] 阶段 C-0：Log::Pending() + 两个队列的 NotEmptyLocked
[ ] 阶段 C-1：Stream 加 Dev()，各子类实现
[ ] 阶段 C-2：Log::FlushBlocking()（含 DMA 等待）
[ ] 阶段 C-3：fatal.cpp 按 in_isr 分支选择 FlushBlocking / WaitLogDrained
[ ] 阶段 C-4：上板验证日志完整发出 + fatal 后仍可敲命令
[ ] 阶段 D-1：Init_entry 暴露 RollbackAllInited
[ ] 阶段 D-2：fatal.cpp 调用
[ ] 阶段 D-3：上板验证外设已安全
[ ] 阶段 E：栈使用率打印
[ ] 阶段 F：ISR 停机分支
[ ] 文档同步：framework/cmd/README.md 补 fatal 章节
[ ] 记忆同步：project-state-snapshots
```

---

## 11. 附：为什么 fatal 不能用 `printk` 或 Zephyr LOG

| 方案 | 问题 |
|---|---|
| `printk` | `CONFIG_PRINTK=n` 时是空实现；开了也走 Zephyr console，与自研 shell 抢同一个 UART |
| `LOG_ERR` | `CONFIG_LOG=n` 时为空；本项目正是因此才自研 DUST_LOG |
| `arch_system_halt()` | 只关中断死循环，不打印任何东西 |
| `coredump` | `CONFIG_COREDUMP` 未开；且需要额外的 flash 分区 |

**线程上下文**：`DUST_LOG_ERR` + 让 shell 消费（主路径）
**ISR 上下文**：`DUST_LOG_ERR` + `uart_poll_out` 轮询兜底

---

## 12. 附：fatal 上下文的能力边界

| 能做什么 | 不能做什么 |
|---|---|
| `k_is_in_isr()` / `k_current_get()` | `k_msleep`（ISR 中） |
| `k_thread_suspend` / `k_thread_resume` | `k_thread_abort` 当前线程 |
| `irq_lock` / `irq_unlock` | 长时间阻塞（会掩盖现场） |
| `uart_poll_out` | 依赖中断的 UART 发送 |
| `k_thread_stack_space_get` | 动态分配（fatal 时堆可能已损坏） |
