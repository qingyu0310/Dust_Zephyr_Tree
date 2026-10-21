# Zephyr 异常处理与 fatal 日志调研

> 调研对象：当前 `zephyr_user` 工程、HPM RISC-V 路径、`framework/cmd/fatal/fatal.cpp` 和 `project/thread/test/trd_test.cpp`
>
> 调研日期：2026-10-21

## 1. 结论先行

当前测试“触发异常但看不到日志”，主要不是异常没有发生，而是异常后的输出链路不适合 fatal 上下文：

1. `test_trigger` 先用 `uart_poll_out()` 直接输出 `[test] firing`，所以这条提示可以立即出现。
2. 真正进入 `fatal.cpp` 后，`DUST_LOG_ERR()` 在 shell 线程接管发送后走的是“入队 + 信号量唤醒”，并不保证调用点立即发串口。
3. `fatal.cpp` 随后执行 `while (1) {}`。该 handler 占住当前 CPU，shell 线程无法继续运行，队列里的 fatal 日志因此没有机会格式化和发送。
4. `trigger=2` 是栈溢出。handler 仍在故障线程/异常上下文上进行函数调用、局部栈使用和日志处理，可能再次触发栈保护，形成 fatal 重入。
5. `trigger=3` 的 C/C++ 整数除零在 HPM 使用的 RISC-V 上不应当作为可靠的硬件异常测试。RISC-V `DIV` 对除数为零定义了结果，通常不会进入 Zephyr fatal 路径。

因此，当前最可靠的异常测试是 `trigger=1` 的无映射地址访问；当前最可靠的 fatal 现场输出方式应当是故障上下文可用的直接输出路径，而不是依赖低优先级 shell 线程的异步日志泵。

## 2. 当前测试到底触发了什么

测试文件是 `project/thread/test/trd_test.cpp`。

`test_trigger` 通过 shell 变量注册，测试线程每 10 ms 检查一次：

```text
var set test_trigger 1
    -> 访问 0x90000000

var set test_trigger 2
    -> 在 2048 字节线程栈上声明 4096 字节数组

var set test_trigger 3
    -> 执行 100 / 0
```

触发前的 `[test] firing` 使用 `Trace()` 和 `uart_poll_out()` 逐字节发送，绕开了 DUST_LOG 队列。因此它只能证明测试线程已经执行到 `TriggerFault()` 前面，不能证明后续 fatal handler 的日志链路正常。

### 2.1 `trigger=1`：非法地址访问

当前地址是 `0x90000000`。在 HPM5361 上，地址 `0x0` 属于 ILM 起始地址，不能用作空指针异常测试；`0x90000000` 才是当前测试所用的无映射地址。

RISC-V 异常入口会读取 `mcause` 和 `mtval`，然后调用 `z_riscv_fatal_error(K_ERR_CPU_EXCEPTION, esf)`，最终进入 `z_fatal_error()`。

### 2.2 `trigger=2`：栈溢出

测试线程的栈是 `Thread<2048>`，但函数中声明了 `volatile uint8_t big[4096]`。这会触发栈保护或栈检查，通常对应 `K_ERR_STACK_CHK_FAIL`。

该路径比非法地址更容易重入：异常处理本身也需要栈，而故障线程的栈已经越过保护边界。当前文档中的实测现象是 `[fatal] enter` 重复出现多次，说明 handler 处理期间又触发了异常入口。

### 2.3 `trigger=3`：整数除零

这不是跨架构可靠的 fatal 测试。RISC-V 指令集对整数除零定义了结果，不要求产生同步异常；编译器也可能生成正常的 `DIV` 指令。因此 HPM RISC-V 上执行 `100 / zero` 可能只是得到一个定义结果，既不进入 `z_riscv_fault()`，也不会调用 `k_sys_fatal_error_handler()`。

## 3. Zephyr 的异常/致命错误入口

Zephyr 把“硬件异常入口”和“统一 fatal 收口”分开。当前工程最相关的调用链如下：

```text
硬件异常/非法访问
    -> arch fault handler
    -> arch-specific fatal function
       例如 z_riscv_fatal_error()
    -> z_fatal_error(reason, esf)
    -> k_sys_fatal_error_handler(reason, esf)
    -> handler 返回后按策略 abort；当前项目 handler 不返回
```

### 3.1 统一原因码

`zephyr/include/zephyr/fatal_types.h` 定义了基础原因：

| 原因码 | 含义 | 当前 `fatal.cpp` 字符串 |
|---|---|---|
| `K_ERR_CPU_EXCEPTION` | 通用 CPU 异常 | `CPU_EXCEPTION` |
| `K_ERR_SPURIOUS_IRQ` | 未处理的硬件中断 | `SPURIOUS_IRQ` |
| `K_ERR_STACK_CHK_FAIL` | 故障上下文栈溢出/栈检查失败 | `STACK_CHK_FAIL` |
| `K_ERR_KERNEL_OOPS` | 中等严重的软件错误 | `KERNEL_OOPS` |
| `K_ERR_KERNEL_PANIC` | 高严重度 kernel panic | `KERNEL_PANIC` |
| `K_ERR_ARCH_START` 及以上 | 架构专用错误码起点 | 当前映射为 `UNKNOWN` |

### 3.2 Zephyr 内核统一入口

`zephyr/kernel/fatal.c` 中有几个关键函数：

- `arch_system_halt()`：默认关闭中断后永久自旋。
- `k_sys_fatal_error_handler()`：Zephyr 提供的 `__weak` 默认 handler。默认行为是 `LOG_PANIC()`、`LOG_ERR("Halting system")`，然后调用 `arch_system_halt()`。
- `z_fatal_error()`：统一收口函数。它会锁中断、记录 Zephyr fatal banner、记录当前线程、调用 coredump，然后调用 `k_sys_fatal_error_handler()`。
- `k_fatal_halt()`：直接进入架构停机。

当前工程的 `framework/cmd/fatal/fatal.cpp` 用强定义覆盖了 Zephyr 的 weak handler，所以实际不会使用 Zephyr 默认的 `LOG_PANIC() + LOG_ERR() + arch_system_halt()` 组合。

### 3.3 RISC-V 架构入口

当前 HPM 路径使用 `zephyr/arch/riscv/core/fatal.c`：

- `z_riscv_fault()` 先判断异常前的栈指针是否落入栈保护区。
- 栈异常会转换成 `K_ERR_STACK_CHK_FAIL`；普通非法访问通常使用 `K_ERR_CPU_EXCEPTION`。
- `z_riscv_fatal_error()` 读取 `mcause`、`mtval`，可选输出异常寄存器和调用栈，然后调用 `z_fatal_error()`。
- RISC-V 异常汇编入口通过 `tail z_riscv_fault` 或 `tail z_riscv_fatal_error` 进入 C 路径，异常处理不会回到原指令继续执行。

### 3.4 软件触发入口

除硬件异常外，Zephyr 还可以通过软件路径进入 fatal：

- `z_except_reason(reason)`：由栈 canary 失败、显式异常等路径调用。
- `_StackCheckHandler()` / `__stack_chk_fail`：编译器栈保护检测失败时调用，最终进入 `z_except_reason(K_ERR_STACK_CHK_FAIL)`。
- userspace 的 `user_fault()` / `z_impl_user_fault()`：可产生 `K_ERR_KERNEL_OOPS` 或栈检查错误。
- `k_panic()`、`k_oops()`：主动触发 kernel panic/oops。
- `z_irq_spurious()`：未处理的中断进入 `K_ERR_SPURIOUS_IRQ`。

这些入口最终都可能汇入 `z_fatal_error()`，所以应用层通常只需要关注统一的 `k_sys_fatal_error_handler()` 和 `reason/esf`。

## 4. 当前 `fatal.cpp` 的真实行为

当前实现位于 `framework/cmd/fatal/fatal.cpp`：

```cpp
extern "C" void k_sys_fatal_error_handler(
    unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s", ...);
    DumpEsf(esf);

    while (1) {}
}
```

它做了三件事：

1. 读取当前线程名并记录基础原因。
2. 调用 `DumpEsf()` 输出架构寄存器。
3. 永久停机。

`DumpEsf()` 的板级实现位于 `drivers/fatal/hpm/esf_dump_riscv.cpp` 或 `drivers/fatal/st/esf_dump_cortex_m.cpp`；没有启用板级实现时，weak 空实现只输出一条 warning。

## 5. 为什么触发异常后没有日志

### 5.1 `DUST_LOG_ERR()` 不是始终立即发送

`framework/cmd/shell/log/log.hpp` 中，DUST_LOG 只有在 `CONFIG_DUST_CMD_SHELL_LOG` 开启时才是真实实现；关闭时宏展开为空。

当前 `project/Kconfig` 中 `PRJ_MAIN -> USE_CMD_SHELL` 会 select `DUST_CMD_SHELL_LOG`，因此正常工程配置通常会启用它。但最终应以构建产物的 `.config` 为准；如果该符号没有进入最终配置，`fatal.cpp` 中的 `DUST_LOG_ERR()` 和 `DUST_LOG_WRN()` 会完全消失，只有 `DumpEsf()` 的其他输出路径可能可见。

### 5.2 shell 接管后，fatal 日志走异步队列

`Log::PrintColoredLog()` 在 shell 线程接管发送后会：

```text
格式化参数快照
    -> 放入 LogRecordQueue
    -> k_sem_give(shell UART sem)
    -> shell 线程 FormatOnePendingRecord()
    -> QueueFrameForShellSend()
    -> UartDma 发送
```

这不是 fatal 上下文中的同步串口输出。handler 中的 `DUST_LOG_ERR()` 最多只能说明“记录入队”，不能说明“字符已经发出”。

### 5.3 `while (1)` 阻断了发送者

`framework/cmd/shell/shell.cpp` 中 shell 线程负责 `FormatOnePendingRecord()` 和 `SendNextFrameIfIdle()`。当前 shell 线程优先级是低优先级调试线程，而 `fatal.cpp` 在当前线程中直接自旋。

因此典型结果是：

```text
[test] firing       // uart_poll_out，立即发出
进入 fatal handler
DUST_LOG_ERR 入队    // 可能已完成
while (1)            // 当前 CPU 永久占用
shell 线程无法取队列和 DMA 发送
```

这解释了“已经触发异常，但串口没有 `[fatal] ...`”的主要现象。

### 5.4 栈溢出会让 handler 自身再次异常

`trigger=2` 下，故障线程的栈已经耗尽。下面这些操作都会继续消耗栈或调用复杂代码：

- `k_thread_name_get(k_current_get())`
- 可变参数格式化
- `DUST_LOG_ERR()` 的队列、锁和参数快照逻辑
- `DumpEsf()` 的函数调用和日志

如果栈保护仍然有效，handler 内部再次越过保护区会重新走 RISC-V 异常入口，从而出现多次 fatal 进入。当前状态文档已经记录了约 32 次 `[fatal] enter` 的实测现象。

### 5.5 Zephyr 自带异常打印也可能不可见

RISC-V 的 `EXCEPTION_DUMP()` 在不同配置下可能映射到 `LOG_ERR()` 或 `printk()`。寄存器详细输出还受 `CONFIG_EXCEPTION_DEBUG` 控制，调用栈受 `CONFIG_EXCEPTION_STACK_TRACE` 控制。

因此，即使 `z_riscv_fatal_error()` 已经执行：

- 没有启用对应 debug 配置时，不会打印完整寄存器；
- `LOG_ERR()` 后端未启用或未及时 flush 时，日志不可见；
- 当前自定义 handler 随后死循环，也会阻止本工程自己的异步日志发送。

## 6. 证据与判断边界

| 现象 | 源码证据 | 判断 |
|---|---|---|
| 能看到 `[test] firing` | `trd_test.cpp` 使用 `uart_poll_out()` | 测试线程已执行到触发点 |
| `trigger=1` 后无 DUST fatal 行 | handler 使用异步 DUST_LOG，随后永久自旋 | 最可能是日志未被 shell 线程泵出；还需用直接 UART 验证 handler 是否完整执行 |
| `trigger=2` 重复进入 | 4096 字节数组 + 2048 字节线程栈；handler 仍使用栈和函数调用 | 栈溢出上下文中的 handler 重入风险已被当前实测支持 |
| `trigger=3` 不进入 fatal | RISC-V DIV 对零有定义结果 | 不能用它证明 fatal 路径失效 |
| Zephyr 默认 banner 看不到 | 工程覆盖 weak handler；当前 handler 不调用 `LOG_PANIC()` | 默认 Zephyr 输出路径已被替换 |

## 7. 建议的验证顺序

本调研不修改代码。后续验证应保持测试和修复分开，按下面顺序收集证据：

1. 查看最终构建 `.config`：确认 `CONFIG_DUST_CMD_SHELL_LOG=y`、`CONFIG_DUST_FATAL_ESF_HPM=y`、`CONFIG_EXCEPTION_DEBUG` 和相关日志后端状态。
2. 只测 `trigger=1`，避免把栈溢出重入混入普通非法访问验证。
3. 在 handler 入口和 `while (1)` 前使用最小、同步、直接 UART 打点，确认 handler 是否走完整。
4. 若直接 UART 能看到而 DUST_LOG 看不到，根因就是异步日志发送链或停机策略。
5. 再单独测 `trigger=2`，观察是否出现入口重复；不要把重复入口当成普通单次 fatal。
6. 需要测试除零时，使用该架构明确保证会产生异常的指令/外设错误，不能直接把 C/C++ 整数除零当成跨架构测试。

## 8. 参考源码

- `framework/cmd/fatal/fatal.cpp`
- `framework/cmd/fatal/esf_dump.hpp`
- `drivers/fatal/hpm/esf_dump_riscv.cpp`
- `drivers/fatal/st/esf_dump_cortex_m.cpp`
- `project/thread/test/trd_test.cpp`
- `framework/cmd/shell/log/log.cpp`
- `framework/cmd/shell/shell.cpp`
- `zephyr/kernel/fatal.c`
- `zephyr/arch/riscv/core/fatal.c`
- `zephyr/arch/riscv/core/isr.S`
- `zephyr/include/zephyr/fatal_types.h`

