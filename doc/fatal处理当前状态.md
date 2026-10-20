# fatal 处理当前状态

> 日期：2026-10-20
> 规划文档：[fatal处理重构规划.md](fatal处理重构规划.md)（985 行，A~F 六阶段）
> 本文记录**实际执行到哪、遇到了什么、下一步怎么办**。

---

## 1. 已完成

### 1.1 阶段 A：esf dump 下沉子树 ✅

| 文件 | 状态 |
|---|---|
| `framework/cmd/fatal/esf_dump.hpp` | 新增：`void DumpEsf(const struct arch_esf* esf)` 接口 |
| `framework/cmd/fatal/fatal.cpp` | 删除 `#if CONFIG_CPU_CORTEX_M / #elif CONFIG_RISCV`，改为调用 `DumpEsf(esf)` + `__weak` 兜底 |
| `drivers/fatal/hpm/esf_dump_riscv.cpp` | 新增：`mcause` / `mtval` / `mepc` / `mstatus` / `sp` / `ra` / `a0` / `a1` |
| `drivers/fatal/st/esf_dump_cortex_m.cpp` | 新增：`pc` / `xpsr` / `lr` |
| `drivers/Kconfig` | 新增 `DUST_FATAL_ESF_HPM`（`depends on RISCV`）、`DUST_FATAL_ESF_STM32`（`depends on CPU_CORTEX_M`） |
| `drivers/CMakeLists.txt` | 两个 `if(CONFIG_...)` 块 |
| 三个板卡 conf | `CONFIG_DUST_FATAL_ESF_HPM=y` / `CONFIG_DUST_FATAL_ESF_STM32=y` |

**分层原则照抄 USB HAL**：

```text
framework/cmd/fatal/esf_dump.hpp        ← 接口（架构层）
drivers/fatal/hpm/esf_dump_riscv.cpp    ← RISC-V 实现（子树，按厂商分目录）
drivers/fatal/st/esf_dump_cortex_m.cpp  ← Cortex-M 实现
```

当前 `fatal.cpp` 的形态：

```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?");

    DumpEsf(esf);

    while (1) {}
}

__weak void DumpEsf(const struct arch_esf* esf) { ... }
```

### 1.2 阶段 B 的部分接口（未接线）

以下改动已落地但 **fatal 未使用**：

| 文件 | 改动 |
|---|---|
| `framework/init/thread/thread.hpp` | `ThreadBase` 新增 `Tid()` / `SetKeepAlive()` / `KeepAlive()`；`Thread<>` 实现 + `keep_alive_` 成员 |
| `framework/cmd/shell/shell.hpp` | `SetKeepAlive(bool)` 转发 |
| `framework/cmd/shell/shell.cpp` | `thread_start()` 里 `shell_.SetKeepAlive(true)` |

**保留状态**：接口就绪，但 `fatal.cpp` 没有调用（回退过）。后续做阶段 B 时可直接接线。

### 1.3 测试工具

`project/thread/test/trd_test.cpp`（1.0）：通过 `var set test_trigger <n>` 触发异常。

| 值 | 行为 |
|---|---|
| 1 | 写 `0x90000000`（无映射地址）→ Load/Store access fault |
| 2 | 局部 `big[4096]` → 栈溢出 |
| 3 | 整数除零 |

---

## 2. 实测发现的问题

### 2.1 问题 1：HPM5361 的 `0x0` 是合法内存 ✅ 已解决

```c
// hpm_misc.h:11-12
#define ILM_LOCAL_BASE  (0x0U)
#define ILM_SIZE_IN_BYTE (0x20000U)   // 128 KB
```

最早用 `*(volatile uint32_t*)0x0 = ...` 测空指针，在 HPM5361 上**是合法写入**——它写进了 ILM 起始处（中断向量表），导致后续异常处理全部失效，连 fatal handler 都进不去。

**修法**：改用 `0x90000000`（无映射地址），`mcause=5/7` 正确触发。

### 2.2 问题 2：栈溢出时 fatal handler 反复重入 ❌ 未解决

**现象**（`var set test_trigger 2`）：

```text
[test] firing

[fatal] enter    × 32 次
[fatal] frozen
[fatal] logged
[fatal] esf:
[fatal] halt
```

**原因**：`k_sys_fatal_error_handler` 跑在**故障线程已溢出的栈**上。handler 每次写局部变量、每次函数调用都会再次越过栈保护线，触发新的 fatal → 重入。

32 次之后 SP 落到保护区内不再越界，函数才勉强跑完。

**已尝试但失败**：在 handler 开头 `mv sp, s_fatal_stack_top` 切换专用栈。**现象完全没变（仍然 32 次 `enter`）**，说明切换没生效或被覆盖。

**可能原因（待验证）**：

1. `SwitchToFatalStack()` 被内联优化掉
2. `z_fatal_error()` 在调用 handler 前会重置 SP（`arch_irq_lock` 等）
3. 重入不是从 handler 内部触发的，而是**每次 fatal 都重新走一遍完整调用链**（`z_riscv_fatal_error` → `z_fatal_error` → handler）

第 3 点最可能：RISC-V 的 `do_fault` 在 `isr.S` 里 `tail z_riscv_fatal_error`，如果 handler 内部再次触发异常，会**重新进入 ISR**，形成完整重入链，而不是在 handler 内部循环。

### 2.3 问题 3：`[fatal] logged` 之后没有日志输出 ❌ 未解决

`DUST_LOG_ERR` 入队成功（`[fatal] logged` 打出来了），但串口上没看到 `[err] [fatal] ...`。

**可能原因**：

1. shell 线程没被调度（`while(1){}` 空转会饿死低优先级线程）
2. `irq_lock` 在 fatal 上下文中的状态不干净，队列操作有问题
3. 帧池/日志队列本身已被破坏

**注意**：问题 2 的 `while(1){}` 正是原因 1 的直接来源——handler 空转占满 CPU，shell（`ThreadPrio::Lowest`=7）永远得不到调度。

---

## 3. 被回退的改动

为定位问题，以下改动已全部回退到「刚分完层」的状态：

| 改动 | 原因 |
|---|---|
| `PollPrint` / 裸 UART 打点 | 调试用，已完成使命 |
| `FreezeBusinessThreads` | 阶段 B，与当前问题无关 |
| `SwitchToFatalStack` / `s_fatal_stack` | 尝试失败（问题 2），且切栈代码本身有风险 |
| `k_is_in_isr` 分支 / `k_msleep` 停机 | 阶段 F，待定 |

**当前 `fatal.cpp` 是 0.3，只有阶段 A 的成果。**

---

## 4. 下一步

### 4.1 优先级

| 优先级 | 问题 | 建议 |
|---|---|---|
| **高** | 问题 2：handler 重入 | 先确认重入的真实路径（见 4.2） |
| **高** | 问题 3：日志发不出去 | 与问题 2 同源，修好重入后大概率一并解决 |
| 中 | 阶段 B：线程冻结 | 接口已就绪，接线即可 |
| 低 | 阶段 C/D/E/F | 按规划推进 |

### 4.2 问题 2 的排查方法

**先确认重入是从哪里来的**。三种可能，验证方法不同：

| 假设 | 验证方法 |
|---|---|
| A. handler 内部再次异常 | 在 handler 首尾各打一次裸 UART 点，看是「enter 多次但 halt 只一次」还是「enter/halt 成对出现」 |
| B. 每次 fatal 重新走完整 ISR 链 | 数 `enter` 与 `z_riscv_fatal_error` 的调用次数是否一致（需要加计数器 + 裸 UART 打出） |
| C. 栈溢出保护本身在反复触发 | 改用 `var set test_trigger 1`（非法地址，不涉及栈），看是否也重入 |

**从历史数据看**：`trigger=1`（非法地址）时**只打出一次 `[test] firing` 后完全静默**（连 `[fatal] enter` 都没有），说明非法地址场景下系统在更早的地方就挂了；`trigger=2`（栈溢出）能看到重入。**两种场景表现不同，说明重入与栈溢出强相关**。

### 4.3 参考做法

Zephyr 自己在 `z_riscv_fatal_error()` 里做了两件事：

```c
/* arch/riscv/core/fatal.c */
FUNC_NORETURN void z_riscv_fatal_error(unsigned int reason, const struct arch_esf *esf)
{
    ...
    if (reason == K_ERR_STACK_CHK_FAIL) {
        /* 移除该线程的 PMP 设置，防止再次触发栈溢出 */
        z_riscv_pmp_stackguard_disable();
    }
    ...
    z_fatal_error(reason, esf);
    CODE_UNREACHABLE;
}
```

**`z_riscv_pmp_stackguard_disable()`** —— 栈溢出时先**关掉栈保护**，避免 handler 内部的任何栈使用再次触发保护。这是在 ISR 层做的，我们的 handler 拿不到。

**含义**：如果重入是栈保护反复触发，Zephyr 已经处理了这一步。那么重入的来源更可能是 **handler 内部的 `DUST_LOG_ERR`**——它调用了 `irq_lock`、队列操作、`vsnprintf` 等，栈使用量远超 handler 本身。

### 4.4 建议的修复方向

1. **先不碰栈**：把 `DUST_LOG_ERR` 从 handler 里拿掉，只用 `uart_poll_out` 打印一行，看是否还重入
   - 若不再重入 → 结论：`DUST_LOG_ERR` 的栈开销是触发源
   - 若仍重入 → 结论：重入与 handler 内容无关，是 ISR 层的问题
2. **若确认是 `DUST_LOG_ERR`**：fatal 路径改用「裸 UART 直接格式化打印」，不走日志队列
3. **恢复 `k_msleep` 停机**（不要 `while(1){}`）：让 shell 有机会把已入队的日志发出去

---

## 5. 当前代码清单

| 文件 | 版本 | 状态 |
|---|---|---|
| `framework/cmd/fatal/fatal.cpp` | 0.3 | 阶段 A 完成，`while(1){}` 停机 |
| `framework/cmd/fatal/esf_dump.hpp` | 0.1 | 接口 |
| `drivers/fatal/hpm/esf_dump_riscv.cpp` | 0.1 | RISC-V 实现 |
| `drivers/fatal/st/esf_dump_cortex_m.cpp` | 0.1 | Cortex-M 实现 |
| `framework/init/thread/thread.hpp` | 0.7 | `Tid()` / `KeepAlive()` 已加，fatal 未接线 |
| `framework/cmd/shell/shell.hpp` | 0.5 | `SetKeepAlive` 转发 |
| `framework/cmd/shell/shell.cpp` | 0.6 | `thread_start` 里置位 |
| `project/thread/test/trd_test.cpp` | 1.0 | 异常触发测试 |

---

## 6. 验证状态

| 项 | 结果 |
|---|---|
| 阶段 A 编译 | ✅ 通过（HPM） |
| `DumpEsf` 被链接 | ✅ `nm` 可见 `_Z7DumpEsfPK8arch_esf` |
| `DUST_FATAL_ESF_HPM` 生效 | ✅ `.config` 中为 `y` |
| 非法地址触发（trigger=1） | ⚠️ 有 `[test] firing`，但无 `[fatal]` 输出 |
| 栈溢出触发（trigger=2） | ⚠️ 有 32 次 `[fatal] enter`，handler 重入 |
| 寄存器打印内容 | ❌ 未观察到（重入导致 `DumpEsf` 前就出问题） |
| 日志最终输出 | ❌ 未观察到 |
