# fatal 处理架构化规划（板卡多了怎么办）

> 2026-08-09。当前 `framework/cmd/fatal/fatal.cpp` 用 `#if CONFIG_CPU_CORTEX_M / #elif CONFIG_RISCV` 打印异常栈帧（esf）字段。板卡少尚可，**板卡/架构多了不允许**——每加一个架构就在 fatal.cpp 加一个 `#elif`。本文参考 Zephyr 的分层做法，把 esf 打印拆成 per-arch 独立实现，并规范 zbus/消息队列发布失败的日志。

## 0. 背景与问题

**当前写法**（fatal.cpp:44-64）：

```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

#if defined(CONFIG_CPU_CORTEX_M)
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s pc=%lx xpsr=%lx",
                 ..., (unsigned long)esf->basic.pc, (unsigned long)esf->basic.xpsr);
#elif defined(CONFIG_RISCV)
    unsigned long mcause = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    DUST_LOG_ERR("[fatal] %s ... mcause=%lx mepc=%lx mstatus=%lx", ..., mcause, esf->mepc, esf->mstatus);
#else
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s", ...);
#endif

    while (1) {}
}
```

**问题**：`arch_esf` 是 per-arch 结构（ARM 的 `basic.pc/xpsr`、riscv 的 `mepc/mstatus`），集中在一个文件里 `#if` 会随架构数量线性膨胀；新架构要改 fatal.cpp 主干。fatal.cpp 是"无条件编译防呆"（记忆 RAM 诊断），主干不该被架构细节污染。

## 1. Zephyr 是怎么做的（调研结论）

| 层 | 做什么 | 证据 |
| --- | --- | --- |
| kernel | `k_sys_fatal_error_handler` 是 `__weak`，默认只 `LOG_ERR("Halting system")`，esf 用 `ARG_UNUSED`——**kernel 不打印 esf 字段** | `zephyr/kernel/fatal.c:37-44` |
| kernel | `z_fatal_error` 只打**通用信息**：`LOG_ERR(">>> ZEPHYR FATAL ERROR %d: %s on CPU %d")` + 线程名 + 嵌套异常检测；esf 转储交给 `coredump(reason, esf, thread)` | `zephyr/kernel/fatal.c:85-119` |
| arch（ARM） | `fault_show(const struct arch_esf *esf, int fault)` 用 `PR_EXC(...)`（=`EXCEPTION_DUMP` 宏，`CONFIG_EXCEPTION_DUMP` 开关）打故障详情 | `zephyr/arch/arm/core/cortex_m/fault.c:28,101,120` |
| arch（riscv） | 独立的 `arch/riscv/core/fatal.c`、`coredump.c` | `zephyr/arch/riscv/core/` |
| 子系统 | `CONFIG_COREDUMP`：per-arch backend（logging/flash/in_memory）转储现场 | `zephyr/subsys/debug/coredump/` |

**结论**：Zephyr **跨架构不 ifdef 打印 esf**——kernel 只打通用信息，esf 细节由**各架构自己的实现文件**负责（arch 层 `fault_show` 或 coredump backend）。新架构接入 = 新增 arch 层实现，不碰 kernel。

项目自研 fatal 用 `DUST_LOG`（Zephyr console/log 已关，arch 层的 `EXCEPTION_DUMP` 走 Zephyr log 会被吞），所以不能直接依赖 `CONFIG_EXCEPTION_DUMP`——需要自研 per-arch 打印，但**分层方式照抄 Zephyr**：通用部分 + per-arch 实现文件，不用 ifdef。

## 2. 项目方案：fatal 拆 per-arch

### 目标结构

```text
framework/cmd/fatal/
├── fatal.cpp              通用：reason 字符串 + 线程名 + 调 DumpEsf()；删掉 ifdef
├── esf_dump.hpp           void DumpEsf(const struct arch_esf* esf) 声明 + __weak 默认空实现
├── esf_dump_cortex_m.cpp  CONFIG_CPU_CORTEX_M：打印 basic.pc/xpsr
└── esf_dump_riscv.cpp     CONFIG_RISCV：csrr mcause + mepc/mstatus
```

- fatal.cpp **无条件编译不变**（防呆），只打通用信息（reason/线程名）后调 `DumpEsf(esf)`，最后 `while(1){}`。
- `esf_dump.hpp` 提供 `__weak` 默认 `DumpEsf`（空实现）——**新架构没写 esf_dump 时不链接炸**（仿 Zephyr `__weak` handler），写了就覆盖打印。
- 各架构 esf_dump 文件按 `CONFIG_` 条件编译进 app（framework/cmd/CMakeLists.txt），与 drivers 的 Kconfig 门禁装配一致。

**新架构接入**：加 `esf_dump_xxx.cpp` + CMake 两行，fatal.cpp 一行不改。

### old → new 逐文件

**① 新建 `esf_dump.hpp`**：

```cpp
/**
 * @file esf_dump.hpp
 * @author qingyu
 * @brief 异常栈帧打印接口 — 各架构独立实现（参考 Zephyr arch 层分层）
 * @version 0.1
 * @date 2026-08-09
 *
 * @copyright Copyright (c) 2026
 */
#pragma once

#include <zephyr/kernel.h>

/**
 * @brief 打印异常栈帧（esf）关键字段
 *
 * 各架构各自实现（esf_dump_cortex_m.cpp / esf_dump_riscv.cpp），
 * 覆盖此弱定义即可。未实现的架构走默认空实现，不链接炸。
 * @param esf 异常栈帧（per-arch：ARM basic.pc/xpsr，riscv mepc/mstatus）
 */
__weak void DumpEsf(const struct arch_esf *esf)
{
    ARG_UNUSED(esf);
}
```

**② 新建 `esf_dump_cortex_m.cpp`**（`CONFIG_CPU_CORTEX_M`）：

```cpp
#include "log.hpp"
#include "esf_dump.hpp"

void DumpEsf(const struct arch_esf *esf)
{
    DUST_LOG_ERR("[fatal] pc=%lx xpsr=%lx",
                 (unsigned long)esf->basic.pc, (unsigned long)esf->basic.xpsr);
}
```

**③ 新建 `esf_dump_riscv.cpp`**（`CONFIG_RISCV`）：

```cpp
#include <zephyr/kernel.h>
#include "log.hpp"
#include "esf_dump.hpp"

void DumpEsf(const struct arch_esf *esf)
{
    unsigned long mcause = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    DUST_LOG_ERR("[fatal] mcause=%lx mepc=%lx mstatus=%lx",
                 mcause, esf->mepc, esf->mstatus);
}
```

**④ `fatal.cpp`（old → new）**：

old（44-64 行整段）：
```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

#if defined(CONFIG_CPU_CORTEX_M)
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s pc=%lx xpsr=%lx",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?",
                 (unsigned long)esf->basic.pc, (unsigned long)esf->basic.xpsr);
#elif defined(CONFIG_RISCV)
    unsigned long mcause = 0;
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s mcause=%lx mepc=%lx mstatus=%lx",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?",
                 mcause, esf->mepc, esf->mstatus);
#else
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?");
#endif

    while (1) {}
}
```

new：
```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char* thread = k_thread_name_get(k_current_get());

    // 通用信息（所有架构一致）
    DUST_LOG_ERR("[fatal] %s (reason=%u) thread=%s",
                 FatalReasonStr(reason), reason, (thread != nullptr) ? thread : "?");

    // esf 字段由各架构独立实现（esf_dump_*.cpp），无实现走 weak 空实现
    DumpEsf(esf);

    while (1) {}
}
```

fatal.cpp 顶部补 `#include "esf_dump.hpp"`。

**⑤ `framework/cmd/CMakeLists.txt`（old → new）**：

old：
```cmake
target_include_directories(app PRIVATE fatal)
target_sources(app PRIVATE fatal/fatal.cpp)
```

new：
```cmake
target_include_directories(app PRIVATE fatal)
target_sources(app PRIVATE fatal/fatal.cpp)
if(CONFIG_CPU_CORTEX_M)
    target_sources(app PRIVATE fatal/esf_dump_cortex_m.cpp)
endif()
if(CONFIG_RISCV)
    target_sources(app PRIVATE fatal/esf_dump_riscv.cpp)
endif()
```

## 3. zbus / 消息队列发布失败日志

### 调研结论（Zephyr 现状）

- **zbus**（`zephyr/subsys/zbus/`，发布/订阅 IPC）：`zbus_chan_pub` 失败返回错误码（`-ENOMSG` 等，`zbus.c:393,524`）；内部对 observer 通知失败打 `LOG_ERR`（`zbus.c:203`，`LOG_MODULE_REGISTER(zbus, CONFIG_ZBUS_LOG_LEVEL)`）。**正常发布失败靠返回码，日志由调用方决定**。
- **消息队列**：`k_msgq_put` 满返回 `-ENOMSG`（`kernel/msg_q.c:187,320,368,414`）；**kernel 不打日志**，错误码完全由调用方处理。

**项目现状（都没检查返回值，失败静默丢）**：

| 位置 | 调用 | 结果 |
| --- | --- | --- |
| `framework/modules/remotes/remote.cpp:36,49` | `zbus_chan_pub(&pub_remote_to, &pub_, K_MSEC(1))` | 未检查返回值 |
| `project/thread/gimbal/trd_gimbal.cpp:59,68` | `k_msgq_put(gimbal_tx, &msg, K_NO_WAIT)` | 未检查返回值 |
| `framework/modules/imu/drivers/imu.cpp:111` | `zbus_chan_pub` | 已注释 |
| `project/thread/chassis/trd_chassis.cpp:199` | `k_msgq_put` | 已注释 |

### 规范（old → new）

发布失败日志铁律：**检查返回值；丢帧（队列满 `-ENOMSG`）打 `DUST_LOG_WRN`；`DUST_LOG_WRN/ERR` 需限频防刷屏**（嵌入式日志通道宝贵）。

**remote.cpp:36,49（old → new）**：

old：
```cpp
				zbus_chan_pub(&pub_remote_to, &pub_, K_MSEC(1));
```
new（首次最小实现，直接检查）：
```cpp
				if (zbus_chan_pub(&pub_remote_to, &pub_, K_MSEC(1)) != 0) {
					DUST_LOG_WRN("pub_remote_to failed");
				}
```

**trd_gimbal.cpp:59,68（old → new）**：

old：
```cpp
            k_msgq_put(topic::to_can_tx::gimbal_tx, &msg, K_NO_WAIT);
```
new：
```cpp
            if (k_msgq_put(topic::to_can_tx::gimbal_tx, &msg, K_NO_WAIT) != 0) {
                DUST_LOG_WRN("gimbal_tx msgq full");
            }
```

**限频（可选，刷屏才加）**：若某发布点高频且常满，用计数限频，例如：

```cpp
static uint32_t drop_cnt;
if (k_msgq_put(...) != 0 && (++drop_cnt % 100 == 1)) {
    DUST_LOG_WRN("gimbal_tx msgq full (drop=%u)", drop_cnt);
}
```

> 说明：`log.hpp` 在 `framework/cmd/shell/`，remote/gimbal 如需 `DUST_LOG_*` 检查 include 与编译门禁（`target_include_directories` 是否含 shell/log 路径）。发布失败日志可并入 fatal 主题（"总线/队列丢数据"是 fatal 监控的一部分），也可单独做——本文按并入处理。

## 4. 分阶段执行

### 阶段 1：fatal 拆 per-arch

**目标**：fatal.cpp 去掉 ifdef，esf 打印拆成 per-arch 文件。

**任务**：
1. 新建 `framework/cmd/fatal/esf_dump.hpp`（weak 默认空实现）。
2. 新建 `framework/cmd/fatal/esf_dump_cortex_m.cpp`、`esf_dump_riscv.cpp`（从 fatal.cpp 原 ifdef 分支平移，**内容逐字节一致**）。
3. `fatal.cpp` 按 §2④ 改（删 ifdef + `#include "esf_dump.hpp"` + 调 `DumpEsf`）。
4. `framework/cmd/CMakeLists.txt` 按 §2⑤ 加两个条件编译块。

**产出**：fatal/ 下 4 个文件；fatal.cpp 无架构 ifdef。
**验证**：hpm（riscv）与 stm32（cortex-m）都能编译；fatal 触发时各架构打印对应 esf 字段；新架构无 esf_dump 时不链接炸（weak 空实现）。

### 阶段 2：发布失败日志规范落地

**目标**：remote/gimbal 发布点检查返回值并打 DUST_LOG_WRN。

**任务**：
1. `framework/modules/remotes/remote.cpp:36,49` 按 §3 改。
2. `project/thread/gimbal/trd_gimbal.cpp:59,68` 按 §3 改。
3. 确认两文件 include `log.hpp` 及编译门禁（缺则补 include，不自行改结构）。

**产出**：发布失败有日志，不再静默丢。
**验证**：队列满/发布失败时 DUST_LOG 出 WRN；正常时不打扰。

### 阶段 3：验证 + 子模块提交

**目标**：编译验证 + framework/cmd 与 project 子模块提交上传。

**任务**：
1. 用户编译 hpm5361icb + stm32 板验证。
2. framework/cmd 子模块提交（fatal 拆分）、project 子模块提交（gimbal 发布日志），直推。
3. 主仓库更新子模块指针，走 PR（GH013 门禁 + auto-merge）。

**产出**：两子模块 + 主仓库指针落地。

## 5. 验证标准

- [ ] hpm5361icb（riscv）编译通过；`CONFIG_RISCV` 分支下 fatal 触发打印 `mcause/mepc/mstatus`
- [ ] stm32f407（cortex-m）编译通过；`CONFIG_CPU_CORTEX_M` 分支下打印 `pc/xpsr`
- [ ] 新架构板卡无 esf_dump 实现时不链接炸（weak 默认空实现兜底）
- [ ] remote/gimbal 发布失败有 `DUST_LOG_WRN`，正常时不刷屏
- [ ] framework/cmd 无架构 ifdef 残留

## 6. 执行清单（逐条勾）

- [ ] 阶段1：建 `esf_dump.hpp`（weak 默认空实现）
- [ ] 阶段1：建 `esf_dump_cortex_m.cpp` / `esf_dump_riscv.cpp`（内容从 fatal.cpp 平移）
- [ ] 阶段1：`fatal.cpp` 删 ifdef，改通用信息 + `DumpEsf(esf)`
- [ ] 阶段1：`framework/cmd/CMakeLists.txt` 加两个条件编译块
- [ ] 阶段2：`remote.cpp` 两处 zbus 发布加返回值检查
- [ ] 阶段2：`trd_gimbal.cpp` 两处 k_msgq_put 加返回值检查
- [ ] 阶段2：确认两文件 log.hpp include 与门禁
- [ ] 阶段3：用户编译 hpm + stm32 验证
- [ ] 阶段3：framework/cmd、project 子模块提交直推 + 主仓库 PR
