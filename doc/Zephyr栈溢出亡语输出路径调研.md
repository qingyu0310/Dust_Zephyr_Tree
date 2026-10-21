# Zephyr 栈溢出「亡语」输出路径调研

> 调研对象：`E:\Zephyr\zephyr`（v4.3.0）、`E:\Zephyr_HPMicro\sdk_glue`、`E:\Zephyr\zephyr_user`
>
> 调研日期：2026-10-21
>
> 触发问题：`var set test_trigger 2`（栈溢出）后只看到 `[fatal] CPU_EXCEPTION (reason=0) thread=`，`thread` 为空、`reason` 不是 `STACK_CHK_FAIL`、后续 `DumpEsf` 一行都没有。

## 1. 结论先行

「按理来说应该没问题」的前提在本工程里**两条都不成立**：

1. **Zephyr 的默认亡语在编译期就整个消失了**。`CONFIG_LOG` 未开（`.config:92`）→ `LOG_ERR()` / `LOG_PANIC()` 展开为空；`CONFIG_CONSOLE` 未开（`.config:105`）→ `printk()` 的输出钩子从未被安装，落回空实现。**Zephyr 自带的 fatal 打印路径本工程一条都不存在**，必须自己 override handler，这一点当前实现已经做了。
2. **本项目这条栈溢出不是「顺序增长」，而是「一步跨过 guard」**。`Thread<2048>` 的栈底只有 512 字节 PMP guard，测试里的 `volatile uint8_t big[4096] {}` 从栈顶往下铺 4KB，**先砸穿 `k_thread` 结构、再跨过 guard**。等异常发生时，`_current->stack_info` 已经被自己清零，`bad_stack_pointer()` 判定失效 → `reason` 退化成 `K_ERR_CPU_EXCEPTION(0)`；`name[]` 被清零 → `thread=` 打印为空。**这两个现象是同一个根因**。

3. **直发方案仍然不彻底**。`Log::SendCommandLine()` 名字叫直发，实际是「入帧池 + DMA 空闲才提交」，DMA 忙时帧留在池里等 TX_DONE 回调驱动 shell 线程续发；而 handler 随后 `while(1)` 自旋，shell 线程永远得不到 CPU。这就是 `trigger=1` 一条都没出、`trigger=2` 只出了第一条的原因。

**唯一可靠的 fatal 输出是 `uart_poll_out()`**：HPM 实现直接轮询 THR 寄存器写字节，不经 DMA、不排队、不等回调、不依赖任何线程。

---

## 2. Zephyr 官方是怎么「抛出亡语」的

### 2.1 完整调用链

```text
硬件异常（PMP / Load / Store fault）
    └─> arch/riscv/core/isr.S  异常入口汇编
          └─> z_riscv_fault(esf)                  arch/riscv/core/fatal.c:203
                ├─ bad_stack_pointer(esf) ?       arch/riscv/core/fatal.c:148
                │     true  -> reason = K_ERR_STACK_CHK_FAIL   (:231)
                │     false -> reason = K_ERR_CPU_EXCEPTION    (:221)
                └─> z_riscv_fatal_error(reason, esf)           (:80)
                      ├─ EXCEPTION_DUMP(...)                    (:89~:136)
                      ├─ z_riscv_unwind_stack()                 (:141，可选)
                      └─ z_fatal_error(reason, esf)             (:144)
                            ├─ arch_irq_lock()                  kernel/fatal.c:91
                            ├─ LOG_ERR(">>> ZEPHYR FATAL ERROR...")   (:98)
                            ├─ LOG_ERR("Current thread: %p (%s)")     (:114)
                            ├─ coredump(reason, esf, thread)          (:117)
                            └─ k_sys_fatal_error_handler(reason, esf) (:119)
                                  └─ 默认弱实现 (:37)
                                        LOG_PANIC()                   (:42)
                                        LOG_ERR("Halting system")     (:43)
                                        arch_system_halt(reason)      (:21)
                                              └─ 关中断 + for(;;) 自旋
```

要点：**Zephyr 的设计是「打印在 halt 之前完成」**。默认 handler 先 `LOG_PANIC()`（`subsys/logging/log_core.c:435`）把日志后端 flush 干净，再 `LOG_ERR`，最后才 `arch_system_halt()` 进死循环。所以官方语境下「亡语不会丢」。

### 2.2 默认 handler 返回时会怎样

`z_fatal_error()` 在 `k_sys_fatal_error_handler()` 返回后（`kernel/fatal.c:119` 之后）会走到 `:174` 解锁中断、`:177` `k_thread_abort(thread)`——**只杀故障线程，系统继续跑**。这是一条可选策略：handler 不返回 = 停机；返回 = 只杀线程。

当前工程 `framework/cmd/fatal/fatal.cpp` 是强定义覆盖，**不返回**（`while(1){}`），等于选了「停机保现场」。

### 2.3 栈溢出有两条判定路径

| 路径 | 触发条件 | 代码位置 | 本工程是否可用 |
|---|---|---|---|
| **PMP guard 硬件异常** | `sp`（异常前栈指针）落在 guard 窗口内 | `arch/riscv/core/fatal.c:148` `bad_stack_pointer()` | ✅ 已开（`CONFIG_PMP_STACK_GUARD=y`） |
| **stack sentinel 软件检查** | 栈底 4 字节魔数被踩 | `kernel/thread.c:331` `z_check_stack_sentinel()` | ❌ 未开（`CONFIG_STACK_SENTINEL` not set） |
| **编译器栈保护** | `__stack_chk_fail` | `kernel/compiler_stack_protect.c:40` | ❌ 未开 `CONFIG_STACKPROTECTOR` |

`bad_stack_pointer()` 的判定原文（`arch/riscv/core/fatal.c:173-177`）：

```c
if (sp >= _current->stack_info.start - K_KERNEL_STACK_RESERVED &&
    sp <  _current->stack_info.start - K_KERNEL_STACK_RESERVED + Z_RISCV_STACK_GUARD_SIZE) {
        return true;
}
```

**它读的是 `_current->stack_info`——也就是 `k_thread` 结构里的字段。** 这是本工程判定失效的关键。

---

## 3. 为什么本工程的亡语在编译期就没了

### 3.1 `CONFIG_LOG` 未开 → `LOG_ERR` 展开为空

`include/zephyr/logging/log.h:62` → `Z_LOG(...)` → `Z_LOG2()` → 第一道闸门 `Z_LOG_LEVEL_ALL_CHECK()`（`include/zephyr/logging/log_core.h:199`）→ `Z_LOG_CONST_LEVEL_CHECK()`（同文件 `:146`）：

```c
#define Z_LOG_CONST_LEVEL_CHECK(_level) \
    (IS_ENABLED(CONFIG_LOG) && ...)     // ← 第一项就是 CONFIG_LOG
```

`CONFIG_LOG` 为 n → 整个条件编译期常量 false → `Z_LOG2` 的 `do { ... } while(false)` 里只剩 `break` → **`LOG_ERR(...)` 一个字节都不生成**。

同理 `LOG_PANIC()`：`include/zephyr/logging/log_ctrl.h:300` 要求 `CONFIG_LOG && !CONFIG_LOG_MODE_MINIMAL`，否则走 `:311` 的 `#define LOG_PANIC() /* Empty */`。

### 3.2 `CONFIG_CONSOLE` 未开 → `printk` 输出到虚空

`arch/riscv/core/fatal.c` 里那几十条 `EXCEPTION_DUMP(...)` 走的是 `include/zephyr/arch/exception.h:12`：

```c
#if defined(CONFIG_LOG)
#define EXCEPTION_DUMP(...) LOG_ERR(__VA_ARGS__)
#else
#define EXCEPTION_DUMP(format, ...) printk(format "\n", ##__VA_ARGS__)
#endif
```

`CONFIG_LOG` 未开 → 走 `printk`。但 `printk` 的字符出口是函数指针 `_char_out`（`lib/os/printk.c:56`），初值指向弱定义：

```c
__attribute__((weak)) int arch_printk_char_out(int c)   // lib/os/printk.c:47
{
    ARG_UNUSED(c);
    do nothing;
    return 0;
}
```

要让它真正输出，必须有驱动调用 `__printk_hook_install()`。全树唯一的串口路径是 `drivers/console/uart_console.c:597`，**编译前提是 `CONFIG_UART_CONSOLE`**。本工程 `CONFIG_CONSOLE` 未开（`.config:105`），`CONFIG_UART_CONSOLE` / `CONFIG_STDOUT_CONSOLE` 也不存在 → **钩子从未安装 → `printk` 逐字符调空函数**。

> `CONFIG_EARLY_CONSOLE=y` 和 `CONFIG_BOOT_BANNER=y` 只控制「初始化优先级」和「打 banner 的意图」，**不安装 printk 钩子**，不能替代 `CONFIG_CONSOLE`。

### 3.3 `coredump` 也是空

`CONFIG_COREDUMP_DEVICE` not set → `coredump()` 是弱空实现，不产生任何输出。

### 3.4 小结：三条官方输出路径全部失效

| 路径 | 依赖符号 | 本工程 | 结果 |
|---|---|---|---|
| `LOG_ERR` / `LOG_PANIC` | `CONFIG_LOG` | not set | 编译期消失 |
| `EXCEPTION_DUMP` → `printk` | `CONFIG_CONSOLE` + `CONFIG_UART_CONSOLE` | not set | 调空函数 |
| `coredump` | `CONFIG_COREDUMP_DEVICE` | not set | 空实现 |

**所以「按理来说应该没问题」的「理」在本工程不成立**——Zephyr 官方那套亡语依赖日志/控制台子系统，本工程为了省 RAM 把 `LOG`/`CONSOLE` 全关了（历史决策见 `doc/fatal处理当前状态.md`）。自己 override handler 是**必需**的，不是可选优化。

---

## 4. 为什么 `reason=0` 而不是 `STACK_CHK_FAIL`

### 4.1 先算清楚内存布局

`CONFIG_PMP_STACK_GUARD=y`、`CONFIG_PMP_POWER_OF_TWO_ALIGNMENT=y`、`CONFIG_PMP_STACK_GUARD_MIN_SIZE=256`、`CONFIG_PMP_GRANULARITY=4`、`ARCH_STACK_PTR_ALIGN=16`。

`include/zephyr/arch/riscv/arch.h:51-59`：

```c
#define Z_RISCV_STACK_PMP_ALIGN  MAX(CONFIG_PMP_GRANULARITY, ARCH_STACK_PTR_ALIGN)   // = 16
#define Z_RISCV_STACK_GUARD_SIZE \
    Z_POW2_CEIL(MAX(sizeof(struct arch_esf) + CONFIG_PMP_STACK_GUARD_MIN_SIZE, Z_RISCV_STACK_PMP_ALIGN))
```

`struct arch_esf`（`include/zephyr/arch/riscv/exception.h:57`）在「无 USERSPACE、无 SOC_CONTEXT_SAVE、开 EXCEPTION_DEBUG」下是 20 个 `unsigned long` = **80 字节**。

- `MAX(80 + 256, 16) = 336`
- `Z_POW2_CEIL(336) = 512`

**`Z_RISCV_STACK_GUARD_SIZE = 512`**，`K_KERNEL_STACK_RESERVED = 512`（`arch.h:74`）。

`Thread<2048>` 的栈数组长度（`include/zephyr/kernel/thread_stack.h:107`）：

```c
K_KERNEL_STACK_LEN(2048) = ROUND_UP(ROUND_UP(2048,16) + 512, 512) = 2560
```

从 `zephyr.map` 实测（`project/build/zephyr/zephyr.map:4923`）：

```
.bss._ZN6thread4testL7thread_E
        0x0000000000081a00      0xe00     app/libapp.a(trd_test.cpp.obj)
```

`0xE00 = 3584`，减去栈数组 2560 → **`k_thread` 结构 1024 字节**。于是：

```text
0x81A00 ┌──────────────────────────┐
        │ k_thread (1024B)         │  ← 含 stack_info / name[32] / 调度链
0x81E00 ├──────────────────────────┤  ← stack_ 起点（stack_obj）
        │ PMP guard (512B)         │  ← 受保护，访问即 fault
0x82000 ├──────────────────────────┤  ← stack_info.start（可用栈底）
        │ 可用栈 (2048B)           │
0x82800 └──────────────────────────┘  ← 初始 SP
```

### 4.2 `big[4096]` 的踩踏顺序

`project/thread/test/trd_test.cpp:58`：

```cpp
volatile uint8_t big[4096] {};
```

初始 SP = `0x82800`，`big` 占 `0x81800 ~ 0x82800`。`{}` 零初始化按地址从低往高写，于是**先写坏 `k_thread`（0x81A00~0x81E00），再进 guard（0x81E00~0x82000）**。

写 `k_thread` 的那一刻，`stack_info.start` / `stack_info.size` / `name[32]` 全部变成 0。

### 4.3 判定链的连锁崩塌

写进 guard 时 PMP 硬件确实抛异常，进入 `z_riscv_fault()` → `bad_stack_pointer(esf)`：

```c
sp = esf + sizeof(arch_esf);                       // ≈ 0x81E00 附近
_current->stack_info.start                         // 已被清零 = 0
0 - 512 = 0xFFFFFE00（无符号下溢）
sp >= 0xFFFFFE00 && sp < 0xFFFFFE00 + 512          // false
```

→ `reason` 保持初值 `K_ERR_CPU_EXCEPTION`（`arch/riscv/core/fatal.c:221`），**永远走不到 `:231` 的 `K_ERR_STACK_CHK_FAIL`**。

同理 `k_thread_name_get(_current)` 读的是已被清零的 `name[32]`，返回空串 → 打印 `thread=` 后面什么都没有。

> 两个「异常现象」是**同一个根因**：栈溢出把 `k_thread` 元数据砸了。

### 4.4 官方为什么「看起来没问题」

Zephyr 的栈溢出检测假设**栈是顺序增长的**（递归、局部变量逐层下探），`sp` 会先落入 guard 窗口，此时 `_current->stack_info` 完好，判定有效。而：

- 一次 `volatile uint8_t big[4096]` 是**跳跃式**：一跳跨过整个 512 字节 guard 窗口；
- `k_thread` 恰好就在 guard **正下方**（栈数组前面），必然先被砸。

`CONFIG_STACK_SENTINEL` 本来能兜住这种情况（栈底魔数检查，`kernel/thread.c:331`），但本工程没开；`CONFIG_STACKPROTECTOR` 也没开。

---

## 5. 为什么直发还是只有第一条

### 5.1 `SendCommandLine` 的真实行为

`framework/cmd/shell/log/log.hpp:83` 声明为「命令响应直发」，实现（`framework/cmd/shell/log/log.cpp:175`）调用 `QueueFrameAndSendNow()`（`:242`）：

```cpp
if (txq_.PushFrameLocked(data, len, prio, epoch, !k_is_in_isr())) {
    pump_.PumpTxQueue(txq_, dbg_.ActiveEpoch());   // DMA 空闲 → 提交；忙 → 留在池里
}
```

`PumpTxQueue` 只在 DMA **空闲**时提交。DMA 忙时帧停在帧池，后续要靠：

```text
UART TX_DONE 中断
    └─> UartDma tx_cb
          └─> Log::HandleTxDone()                        log.hpp
                └─> shell 线程被唤醒
                      └─> Log::SendNextFrameIfIdle()      log.hpp
                            └─> pump_.PumpTxQueue(...)    ← 此时才发下一帧
```

**发送续接依赖 shell 线程调度。** 而 fatal handler 里 `while(1){}` 自旋，且 `z_fatal_error()` 在 `:91` 已经 `arch_irq_lock()` 关中断、handler 不返回所以永不解锁（`:174` 走不到）——shell 线程永远得不到 CPU。

### 5.2 两次实测对得上

| 场景 | 触发瞬间 DMA 状态 | 结果 |
|---|---|---|
| `trigger=1` | shell 刚回显 `ok`，DMA 忙 | 第一条就进池未提交 → **一行都没有** |
| `trigger=2` | DMA 恰好空闲 | 第一条提交成功，`DumpEsf` 两条进池 → **只出第一行** |

### 5.3 `uart_poll_out` 才是真同步

HPM 实现（`E:\Zephyr_HPMicro\sdk_glue\drivers\serial\uart_hpmicro.c:473`）：

```c
static void uart_hpm_poll_out(const struct device *dev, unsigned char c)
{
    const struct uart_hpm_cfg *const cfg = dev->config;
    uart_send_byte((UART_Type *)cfg->base, c);
}
```

`uart_send_byte`（`E:\Zephyr_HPMicro\sdk_env\hpm_sdk\drivers\src\hpm_uart_drv.c:231`）：

```c
while (!(ptr->LSR & UART_LSR_THRE_MASK)) { ... }   // 轮询发送保持寄存器空
ptr->THR = UART_THR_THR_SET(c);                    // 直接写数据寄存器
```

不经 DMA、不入队列、不等回调、不依赖中断和线程调度。**fatal 上下文里唯一可靠的路。**

---

## 6. 三条输出路径对比

| 方案 | 是否同步 | DMA 忙时 | 依赖线程 | fatal 上下文可用 | 备注 |
|---|---|---|---|---|---|
| `DUST_LOG_ERR()` | ❌ | 入队 | ✅ 依赖 shell 线程 | ❌ | shell 接管后走异步（`log.cpp:159-168`） |
| `Log::SendCommandLine()` | ⚠️ 半同步 | 帧留池，等 TX_DONE + shell | ✅ 续发依赖 shell 线程 | ⚠️ 仅 DMA 空闲时成功 | 本工程已改用它，实测不稳 |
| `uart_poll_out()` | ✅ | 无影响 | ❌ 不依赖 | ✅ | 逐字节轮询 THR，~10 字节/ms 级 |

---

## 7. 证据总表

| 判断 | 源码证据 |
|---|---|
| 默认 handler 是 `LOG_PANIC + LOG_ERR + arch_system_halt` | `zephyr/kernel/fatal.c:37-46` |
| handler 返回则只杀线程，不返回则停机 | `zephyr/kernel/fatal.c:174-177` |
| `CONFIG_LOG` 关闭时 `LOG_ERR` 编译期消失 | `zephyr/include/zephyr/logging/log_core.h:146-154` |
| `CONFIG_LOG` 关闭时 `LOG_PANIC()` 为空 | `zephyr/include/zephyr/logging/log_ctrl.h:311` |
| `EXCEPTION_DUMP` 无 LOG 时走 printk | `zephyr/include/zephyr/arch/exception.h:12-14` |
| printk 默认字符出口是空实现 | `zephyr/lib/os/printk.c:47-53` |
| 只有 `uart_console.c` 装钩子 | `zephyr/drivers/console/uart_console.c:597` |
| 本工程 `CONFIG_LOG` / `CONFIG_CONSOLE` 均未开 | `project/build/zephyr/.config:92`、`:105` |
| 本工程 `CONFIG_STACK_SENTINEL` 未开 | `project/build/zephyr/.config:1039` |
| guard 大小 = 512（POW2 对齐） | `zephyr/include/zephyr/arch/riscv/arch.h:51-59` |
| `bad_stack_pointer` 读 `_current->stack_info` | `zephyr/arch/riscv/core/fatal.c:173-177` |
| 判定失败则 `reason` 保持 `CPU_EXCEPTION` | `zephyr/arch/riscv/core/fatal.c:221`、`:231` |
| `thread_` 对象 3584B（k_thread 1024 + 栈 2560） | `project/build/zephyr/zephyr.map:4923` |
| `SendCommandLine` 是「入池 + 空闲才发」 | `framework/cmd/shell/log/log.cpp:242-252` |
| 续发依赖 shell 线程 | `framework/cmd/shell/log/log.hpp` `SendNextFrameIfIdle()` |
| HPM `poll_out` 直接写 THR | `sdk_glue/drivers/serial/uart_hpmicro.c:473`、`hpm_sdk/drivers/src/hpm_uart_drv.c:231` |

---

## 8. 结论

1. **Zephyr 官方的亡语机制在本工程是「零输出」**——`LOG` / `CONSOLE` / `coredump` 三条路全关，override handler 是唯一出路。
2. **`reason=0` 和 `thread=` 空是同一个根因**：`volatile uint8_t big[4096]` 从栈顶下探时先砸穿 guard 正下方的 `k_thread` 结构，`stack_info` 与 `name[]` 一起被清零，导致 `bad_stack_pointer()` 判定失效、线程名读空。
3. **要测出真正的 `STACK_CHK_FAIL`**，必须让栈「顺序」增长到 guard（例如有限深度递归），或用远小于 512 的越界数组；一次性大数组必然绕过硬件判定。
4. **fatal 输出必须用 `uart_poll_out`**。`SendCommandLine` 在 DMA 忙时只是「入队」，续发靠 shell 线程，而 handler 自旋会把它饿死。

---

## 9. 附：本次实测记录

| 命令 | 输出 | 判读 |
|---|---|---|
| `var set test_trigger 1` | `[test] firing`，无 `[fatal]` | 触发瞬间 DMA 被 `ok` 回显占用，帧未提交，handler 自旋后无法续发 |
| `var set test_trigger 2` | `[test] firing` + `[fatal] CPU_EXCEPTION (reason=0) thread=` | 第一条赶上 DMA 空闲提交成功；`reason` 应为 2 却为 0 → `stack_info` 已被清零；`thread` 空 → `name[]` 已被清零；`DumpEsf` 两行进池未发 |

> 本节记录的是改用 `Log::SendCommandLine` 之后的实测；换 `uart_poll_out` 后的结果待补。
