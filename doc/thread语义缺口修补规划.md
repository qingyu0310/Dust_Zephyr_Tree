# thread 语义缺口修补规划

> 日期：2026-10-23
> 前置：`doc/thread语义缺口调研.md`（11 条缺口 A~K，三条高危 A/B/C）
> 目标：把调研里确认的缺口逐条修掉。**不是重新设计线程层**，是把已有语义收干净。
> 铁律：**MCU 上线程常驻，没有"任务跑完就退"的语义**——`TaskResult` 保持两态，不加第三态。

---

## 0. 总览

| 阶段 | 修什么 | 涉及文件 | 风险 |
|---|---|---|---|
| P1 | **A** Retry 无钩子静默死 | `thread.hpp`、`imu.hpp`、`imu.cpp` | 低（删退出路径 + imu 补钩子） |
| P2 | **C** 64 位统计撕裂 | `thread.hpp` | 低（临界区保护） |
| P3 | **B** KeepAlive 假保护 | `thread.hpp`、`thread_cmd.cpp`、`shell.cpp`、`shell.hpp`、`fatal.cpp` | 中（要删 API） |
| P4 | **D** `IsAlive()` 死条件 | `thread.hpp` | 低 |
| P5 | **I** 文档失效 | `doc/线程编写指南.md` | 无 |
| P6 | **G** StackSize 语义 | `thread.hpp`（注释） | 无 |
| P7 | **H** 周期语义 + 观测 | `thread.hpp`（注释 + 可选统计） | 低 |
| P8 | **J / K** 注册表满 / fail 策略边界 | `thread_registry.hpp`、`Init_entry.cpp`、`Init_entry.hpp` | 低 |
| P9 | **E / F** 停止原语 / Start 幂等 | 无 | **不做**（P1 之后动机已消失，见 §9） |

**P1~P8 只修现有语义，不新增能力。P9 明确不做。**

---

## 1. P1 — 三套生命周期名字分干净 + 统一故障处理器（高危）

> **P1 拆成 11 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改下面表格里对应的**那一个文件**，改完停下报告，等"继续"。
> 不许一次做多步，不许跨步连带改（必要时见每步的「连带」栏）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **1-1** | `framework/init/startup/Init_entry.hpp` | `CleanupFunc`→`RollbackFunc`、`InitEntry::cleanup`→`rollback`、`REGISTER_INIT` 参数 `cleanup_`→`rollback_`；`REGISTER_THREAD` 的 fail 注释 | 无 | 宏是位置参数，12 处调用**不用改**；但 `Init_entry.cpp` 此时会编译不过（下一步修） |
| **1-2** | `framework/init/startup/Init_entry.cpp` | 两处 `cleanup`→`rollback`；删 `RegisterThreadInstance` 与 `#include "thread_registry.hpp"` | 1-1 | 删掉函数后 `thread.hpp` 的调用会链接失败（1-6 才恢复） |
| **1-3** | `framework/init/thread/thread_fault.hpp`（新建） | 故障类型 + `ThreadFaultCenter` 声明 | 无 | 无 |
| **1-4** | `framework/init/thread/thread_fault.cpp`（新建） | `DefaultHandle` + `Report` 实现 | 1-3 | 无 |
| **1-5** | `framework/init/thread/thread_registry.hpp` | 加 `Remove()` | 无 | 无 |
| **1-6** | `framework/init/thread/thread_registry.cpp`（新建） | `RegisterThreadInstance` + `UnregisterThreadInstance` | 1-5 | 补回 1-2 删掉的函数 |
| **1-7** | `framework/init/thread/thread.hpp` | include `thread_fault.hpp`+`log.hpp`、删 `RecoveryHook`、加 `FaultPolicy()`、`SetFaultPolicy`/`SetRetryHook`/`SetExitHook`、`ExitThread()`、`Run()` 接故障分流 | 1-3 1-6 | 此步做完 `remote.hpp` 的 `SetRecoveryHook` 会编译不过（1-9 修） |
| **1-8** | `framework/init/CMakeLists.txt` | `target_sources` 加两个新 cpp | 1-4 1-6 | 无 |
| **1-9** | `framework/modules/remotes/remote.hpp` + `remote.cpp` | `Recover`→`OnRetry`、`SetRecoveryHook`→`SetRetryHook`、加 `SetFaultPolicy(Recover)`；删"停止检查"旧注释 | 1-7 | 两个文件同一概念，必须一起改 |
| **1-10** | `framework/modules/imu/drivers/imu.hpp` + `imu.cpp` | `SetFaultPolicy(Recover)` + `SetRetryHook(OnRetry)`、`fail_count_`、`kFailLogInterval`、Task 成功清零 | 1-7 | 同上 |
| **1-11** | `project/thread/gpio/trd_gpio.cpp` + `project/thread/test/trd_test.cpp` | 各加一行 `SetFaultPolicy(Isolate)` | 1-7 | 两个文件同性质，必须一起改 |

**编译状态说明**：1-1 ~ 1-7 期间**工程编不过是预期的**（改名/删函数/加依赖是分步的）。**1-8 之后可编译**；1-9 ~ 1-11 是把调用方接上。**如果想任何时候都能编过，就按 1-8 → 1-9 → 1-10 → 1-11 顺序做，且 1-7 和 1-9 不能隔太久。**

---

### 1.1 三个生命周期，三套名字，谁也不许冒充谁

| 生命周期 | 何时发生 | 职责 | 命名 | 签名 | 归属文件 |
|---|---|---|---|---|---|
| **初始化回滚** | 启动期某项 init 失败，或停机前逆序恢复 | 把硬件放回安全态 | `RollbackFunc` / `rollback` | `void (*)()` | `startup/Init_entry.hpp` |
| **Retry** | 运行期 Task 返回 `TaskResult::Retry` | 就地处理可恢复错误，回答"我自己扛住了吗" | `RetryHook` / `retry_hook` | `bool (*)(void* context)` | `thread/thread.hpp` |
| **线程退出** | 统一处理器判 `Exit` 之后 | 线程真正消失前最后一次收尾 | `ExitHook` / `exit_hook` | `void (*)(void* context)` | `thread/thread.hpp` |

**三条铁律：**

1. **`cleanup` 这个词只属于启动期，而且要改成 `rollback`。** 它的语义是"初始化回滚"，不是"收尾"。改完之后全仓库搜 `cleanup` 应为零命中。
2. **`RetryHook` 与 `ExitHook` 是两件事，不许合并。** 前者回答"扛住了吗"（返回 `bool`），后者负责"要死了，收尾吧"（返回 `void`）。合并成一个 `bool` 钩子会让"退出"变成"返回值的一个分支"，语义再次糊掉。
3. **参数形态不同是刻意的**：启动期回滚是 `void(*)()`（服务全局条目表，无上下文）；线程钩子是 `void(*)(void*)`（每线程的 context 是自己的）。不要为了"统一"把启动期也塞一个 `void*`。

### 1.2 文件怎么分：一个文件一件事

P1 涉及的东西**不许全塞进 `thread.hpp`**。按职责拆成五个文件：

```text
framework/init/thread/
├── thread.hpp            线程本体：ThreadBase（能力契约）+ Thread<>（模板）
│                         只管"一个受管线程怎么跑、怎么停、怎么退出"
│
├── thread_fault.hpp      故障类型与处置中心（纯声明 + 两个内联函数）
│                         ThreadFaultPolicy / ThreadFaultAction / RetryHook /
│                         ExitHook / ThreadFaultHandler / ThreadFaultCenter
│
├── thread_fault.cpp      故障中心的默认策略实现（DefaultHandle）
│                         唯一需要 .cpp 的部分：它要 include log.hpp、buzzer
│
├── thread_registry.hpp   线程注册表（已有，本阶段只加 Remove）
│
└── thread_registry.cpp   注册表的实例管理（新增）
                          RegisterThreadInstance / UnregisterThreadInstance
                          从 Init_entry.cpp 挪出来：它俩是"线程台账"的事，
                          不属于"启动阶段编排"
```

**为什么这样分：**

| 判据 | 结论 |
|---|---|
| 谁需要 include 什么 | `thread.hpp` 只需要看到类型和 `ThreadFaultCenter` 的声明 → include `thread_fault.hpp`（头） |
| 谁能不看见 log/buzzer | `thread.hpp` 只要 `DUST_LOG_WRN` 一行 → include `log.hpp`；`DefaultHandle` 要 `EXEC_BUZZER_LONG` 和 `k_msleep` → 放 `.cpp`，头文件不背这两个依赖 |
| 谁是"线程台账" | `Register/UnregisterThreadInstance` 操作的是注册表，不是启动流程 → 从 `Init_entry.cpp` 挪到 `thread_registry.cpp` |
| 谁是"启动编排" | `Init_entry.cpp` 只管阶段遍历、失败策略、回滚 → **rollback 改名的活在这里，注册表的活不在这里** |

### 1.3 命名污染现状（证据）

| 位置 | 现在叫什么 | 真实语义 | 问题 |
|---|---|---|---|
| `Init_entry.hpp:66` | `using CleanupFunc = void (*)();` | 启动期回滚 | 也叫 cleanup |
| `Init_entry.hpp:84` | `InitEntry::cleanup` | 同上 | 同上 |
| `thread.hpp:42` | `using RecoveryHook = bool (*)(void*);` | Retry 处理 | 名字说的是"恢复"，返回的是"扛住了吗"，和"退出收尾"无关 |
| `thread.hpp:150` | `SetRecoveryHook` | 同上 | 同上 |
| 上一稿规划 | `ThreadCleanupHook` / `SetCleanupHook` | 线程退出收尾 | **与启动期 `CleanupFunc` 撞名** |

改完之后，三个词各归各：`rollback` = 启动期，`retry` = 运行期出错处理，`exit` = 运行期退出。

---

### 1.4 文件 1：`startup/Init_entry.hpp` — `cleanup` → `rollback`

**改 1 — typedef（第 66 行）**

old：
```cpp
using CleanupFunc = void (*)();
```
new：
```cpp
using RollbackFunc = void (*)();    // 初始化回滚（仅启动期，勿与线程退出钩子混用）
```

**改 2 — 结构体字段（第 84 行）**

old：
```cpp
    CleanupFunc       cleanup;   // 失败回滚与退出清理；可为 nullptr
```
new：
```cpp
    RollbackFunc      rollback;  // 初始化失败/停机前的逆序回滚；可为 nullptr
```

**改 3 — `REGISTER_INIT` 宏（第 99-127 行）**

宏参数 `cleanup_` → `rollback_`，展开里 `cleanup_,` → `rollback_,`；Doxygen `@param cleanup_` → `@param rollback_`。

**宏是位置参数，所有调用点不用改**（12 处传 `nullptr` / `buzzer_deinit` / `flash_deinit`）。

---

### 1.5 文件 2：`startup/Init_entry.cpp` — 回滚调用改名 + 移除注册表函数

**改 1 — `RollbackInitedEntries()`（第 50-58 行）**

old：
```cpp
        if (inited_entries[i]->cleanup != nullptr)
        {
            inited_entries[i]->cleanup();
            DUST_LOG_WRN("[boot] %s rolled back", inited_entries[i]->name);
        }
        else
        {
            DUST_LOG_WRN("[boot] %s has no cleanup", inited_entries[i]->name);
        }
```
new：
```cpp
        if (inited_entries[i]->rollback != nullptr)
        {
            inited_entries[i]->rollback();
            DUST_LOG_WRN("[boot] %s rolled back", inited_entries[i]->name);
        }
        else
        {
            DUST_LOG_WRN("[boot] %s has no rollback", inited_entries[i]->name);
        }
```

**改 2 — 部分初始化失败回滚（第 167 行）**

old：
```cpp
                if (e->cleanup != nullptr) e->cleanup();    	// 部分初始化失败也回滚本项资源
```
new：
```cpp
                if (e->rollback != nullptr) e->rollback();    	// 部分初始化失败也回滚本项资源
```

**改 3 — 删掉 `RegisterThreadInstance`（第 31-38 行）**

old：
```cpp
/**
 * @brief 线程登记入口：Thread::Start 创建线程后调用
 * @param thread 已创建的线程对象
 */
void RegisterThreadInstance(ThreadBase* thread)
{
    (void)thread_registry::Add(thread);
}
```
new：**整段删除**（挪到 `thread_registry.cpp`，见 §1.9）。

**改 4 — 删除 `#include "thread_registry.hpp"`（第 15 行）**

改完之后 `Init_entry.cpp` 不再碰注册表。

---

### 1.6 文件 3：新建 `thread/thread_fault.hpp` — 故障类型 + 处置中心

**新文件全文：**

```cpp
/**
 * @file thread_fault.hpp
 * @author qingyu
 * @brief 线程故障类型与统一处置中心
 * @version 0.1
 * @date 2026-10-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <cstdint>

class ThreadBase;

/**
 * @brief 线程故障策略：声明本线程出故障时的影响面
 *
 * 同一个故障对不同线程的影响面不同：心跳挂了系统照跑，姿态挂了必须继续。
 * 线程在 Start 之前用 SetFaultPolicy() 声明，统一处理器据此决定处置方式。
 */
enum class ThreadFaultPolicy : uint8_t
{
    Isolate = 0,    // 影响小：收尾后退出，不拖累系统（心跳、测试等）
    Recover = 1,    // 影响大：统一处理器接管，记录后继续运行（姿态、遥控等）
    Halt    = 2,    // 致命：整机停机，保留现场
};

/**
 * @brief 统一故障处理器给出的处置动作
 */
enum class ThreadFaultAction : uint8_t
{
    Continue = 0,   // 已处置，线程继续下一拍
    Exit     = 1,   // 收尾后退出（处理器认可）
    Halt     = 2,   // 停机（处理器内部已停机，不返回）
};

/**
 * @brief Retry 钩子：Task 返回 Retry 时由线程自己就地处理
 *
 * 与退出钩子无关：本钩子回答"扛住了吗"，返回 true 表示继续下一拍。
 * 返回 false 时上报 ThreadFaultCenter，由统一处理器按策略处置。
 */
using RetryHook = bool (*)(void* context);

/**
 * @brief 退出钩子：线程确定退出前最后一次收尾
 *
 * 只在统一处理器判 ThreadFaultAction::Exit 时执行。
 * 与启动期的 RollbackFunc 无关：那个服务初始化回滚，这个服务线程退出。
 */
using ExitHook = void (*)(void* context);

/**
 * @brief 统一故障处理器原型
 * @param thread  出故障的线程
 * @param context 线程上下文（原样传入）
 * @return 处置动作；返回 Halt 表示处理器内部已停机（不会返回）
 */
using ThreadFaultHandler = ThreadFaultAction (*)(ThreadBase* thread, void* context);

/**
 * @brief 统一故障处置中心
 *
 * 所有线程的故障都汇总到这里：Task 返回 Retry 且 RetryHook 处理不了时上报。
 * 项目可在启动期用 SetHandler() 替换默认处理器（统一上报、联动降级等）。
 *
 * 默认处理器实现见 thread_fault.cpp。
 */
class ThreadFaultCenter
{
public:
    /**
     * @brief 安装全局故障处理器
     * @param handler 处理器；传 nullptr 恢复默认（按线程策略兜底）
     */
    static void SetHandler(ThreadFaultHandler handler) { handler_ = handler; }

    /**
     * @brief 上报一次线程故障
     * @param thread  出故障的线程
     * @param context 线程上下文（原样带给处理器）
     * @return 处理器给出的动作
     */
    static ThreadFaultAction Report(ThreadBase* thread, void* context);

private:
    static inline ThreadFaultHandler handler_ = nullptr;   // 全局唯一处理器
};
```

**说明**：
- 头文件只依赖 `<cstdint>`，**不含 log / buzzer / kernel**。
- `ThreadBase` 前向声明即可（只用在函数指针签名里）。
- `Report()` **只有声明**，实现在 `.cpp`——因为它要调 `DefaultHandle`，而后者要打日志和长鸣。

---

### 1.7 文件 4：新建 `thread/thread_fault.cpp` — 默认策略实现

**新文件全文：**

```cpp
/**
 * @file thread_fault.cpp
 * @author qingyu
 * @brief 线程故障处置中心的默认实现
 * @version 0.1
 * @date 2026-10-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "thread_fault.hpp"
#include "thread.hpp"
#include "log.hpp"
#include "buzzer.hpp"
#include <zephyr/kernel.h>

#pragma message "Compiling Init/Thread/Thread_fault"

/**
 * @brief 默认处置：按线程声明的策略兜底
 * @param thread 出故障的线程
 * @return Continue=继续运行 / Exit=收尾后退出；Halt 时内部停机（不返回）
 */
static ThreadFaultAction DefaultHandle(ThreadBase* thread)
{
    if (thread == nullptr) {
        return ThreadFaultAction::Continue;
    }

    switch (thread->FaultPolicy())
    {
        case ThreadFaultPolicy::Isolate:
            DUST_LOG_WRN("thread %s fault, isolating", thread->Name());
            return ThreadFaultAction::Exit;

        case ThreadFaultPolicy::Recover:
            DUST_LOG_WRN("thread %s fault, holding", thread->Name());
            return ThreadFaultAction::Continue;

        case ThreadFaultPolicy::Halt:
        default:
            DUST_LOG_ERR("thread %s fatal, halting", thread->Name());
            EXEC_BUZZER_LONG();
            for (;;) {
                k_msleep(1000);             // 休眠停机：让 shell 把日志发完
            }
    }
}

/**
 * @brief 上报一次线程故障
 * @param thread  出故障的线程
 * @param context 线程上下文（原样带给处理器）
 * @return 处理器给出的动作
 */
ThreadFaultAction ThreadFaultCenter::Report(ThreadBase* thread, void* context)
{
    if (handler_ != nullptr) {
        return handler_(thread, context);
    }
    return DefaultHandle(thread);
}
```

**说明**：`DefaultHandle` 是 `.cpp` 内的 `static` 函数，**不进头文件**，外部看不到它。

---

### 1.8 文件 5：`thread/thread.hpp` — 只留线程本体

`thread.hpp` 删掉一切与"故障处置中心实现"相关的东西，只保留线程自己的东西。

**改 1 — include（第 12-17 行）**

old：
```cpp
#include <zephyr/kernel.h>
#include <cstdint>
#include <cstring>
```
new：
```cpp
#include <zephyr/kernel.h>
#include <cstdint>
#include <cstring>
#include "thread_fault.hpp"     // ThreadFaultPolicy / ThreadFaultAction / RetryHook / ExitHook
#include "log.hpp"              // ExitThread 的退出日志
```

**改 2 — 删 `RecoveryHook` 别名（第 41-42 行）**

old：
```cpp
using TaskFunc     = TaskResult (*)(void* context);
using RecoveryHook = bool (*)(void* context);
```
new：
```cpp
using TaskFunc = TaskResult (*)(void* context);
```

（`RetryHook` / `ExitHook` 已在 `thread_fault.hpp` 定义，这里不再重复。）

**改 3 — `RegisterThreadInstance` 声明保留、新增注销声明（第 102-108 行）**

old：
```cpp
/**
 * @brief 线程登记入口（定义在 Init_entry.cpp，供 Thread::Start 调用）
 * @param thread 已创建的线程对象
 */
void RegisterThreadInstance(ThreadBase* thread);
```
new：
```cpp
/**
 * @brief 线程登记入口（定义在 thread_registry.cpp，供 Thread::Start 调用）
 * @param thread 已创建的线程对象
 */
void RegisterThreadInstance(ThreadBase* thread);

/**
 * @brief 线程注销入口（定义在 thread_registry.cpp，供 Thread::ExitThread 调用）
 * @param thread 已退出的线程对象
 */
void UnregisterThreadInstance(ThreadBase* thread);
```

**改 4 — `ThreadBase` 增加策略查询（`Paused()` 之后，第 98 行附近）**

```cpp
    /**
     * @brief 本线程的故障策略
     */
    virtual ThreadFaultPolicy FaultPolicy() const = 0;
```

**改 5 — 公开接口（替换 `SetRecoveryHook` 那一段，第 146-150 行）**

old：
```cpp
    /**
     * @brief 注册恢复钩子：任务返回 Retry 时调用
     * @param hook true=重进任务入口；false=线程退出
     */
    void SetRecoveryHook(RecoveryHook hook) { hook_ = hook; }
```
new：
```cpp
    /**
     * @brief 注册 Retry 钩子：Task 返回 Retry 时就地处理
     * @param hook true=已处理继续；false=处理不了，上报统一处理器
     */
    void SetRetryHook(RetryHook hook) { retry_hook_ = hook; }

    /**
     * @brief 注册退出钩子：统一处理器判 Exit 后、线程消失前执行
     * @param hook 收尾函数；nullptr 表示无需收尾
     */
    void SetExitHook(ExitHook hook) { exit_hook_ = hook; }

    /**
     * @brief 声明本线程的故障策略（影响面）
     * @param policy Isolate=收尾后退出 / Recover=统一处理并继续 / Halt=停机
     */
    void SetFaultPolicy(ThreadFaultPolicy policy) { fault_policy_ = policy; }

    ThreadFaultPolicy FaultPolicy() const override { return fault_policy_; }
```

**改 6 — 成员（第 263-273 行区域）**

old：
```cpp
    TaskFunc     task_    		  = nullptr;   		  // 受管任务入口（裸任务为空）
    k_timeout_t  period_          = K_NO_WAIT;        // 两拍间隔
    void*        context_ 		  = nullptr;   		  // 任务上下文
    const char*  name_    		  = nullptr;   		  // 线程名（日志与命令用）
    RecoveryHook hook_    		  = nullptr;   		  // 恢复钩子（Retry 时调用）
```
new：
```cpp
    TaskFunc     task_    		  = nullptr;   		  // 受管任务入口
    k_timeout_t  period_          = K_NO_WAIT;        // 两拍间隔
    void*        context_ 		  = nullptr;   		  // 任务上下文
    const char*  name_    		  = nullptr;   		  // 线程名（日志与命令用）

    RetryHook          retry_hook_   = nullptr;                        // Retry 处理钩子
    ExitHook           exit_hook_    = nullptr;                        // 退出收尾钩子
    ThreadFaultPolicy  fault_policy_ = ThreadFaultPolicy::Recover;     // 故障策略（默认按大影响）
```

**改 7 — `Run()` 的 Retry 分支（第 311-318 行）**

old：
```cpp
            if (result == TaskResult::Retry) 
			{
                atomic_inc(&retry_count_);
                if (hook_ == nullptr || !hook_(context_)) 
				{
                    return;
                }
            }
```
new：
```cpp
            if (result == TaskResult::Retry)
            {
                atomic_inc(&retry_count_);

                // 一级：Retry 钩子就地处理（钩子自己决定要不要节流打日志）
                if (retry_hook_ == nullptr || !retry_hook_(context_))
                {
                    // 二级：钩子处理不了 → 统一处理器按线程策略处置
                    if (ThreadFaultCenter::Report(this, context_) == ThreadFaultAction::Exit)
                    {
                        ExitThread();
                        return;                 // 唯一合法退出点（已收尾、已注销）
                    }
                }
            }
```

**改 8 — 新增 `ExitThread()`（`Run()` 之后）**

```cpp
    /**
     * @brief 线程退出：执行 ExitHook → 注销注册表 → 打日志
     *
     * 这是唯一的合法退出路径。直接 return 会跳过 ExitHook，
     * 并在注册表里留下永久的 dead 条目。
     */
    void ExitThread()
    {
        if (exit_hook_ != nullptr) {
            exit_hook_(context_);
        }

        UnregisterThreadInstance(this);

        DUST_LOG_WRN("thread %s exited", Name());
    }
```

---

### 1.9 文件 6：`thread/thread_registry.hpp` — 加 `Remove`

**位置**：`Add()` 之后（第 49 行之后）

```cpp
    /**
     * @brief 从注册表移除一个已退出的线程
     *
     * 用尾部元素填补空位，保持数组紧凑。
     *
     * @param thread 线程对象
     * @return true=找到并已移除
     */
    static bool Remove(ThreadBase* thread)
    {
        if (thread == nullptr) return false;

        for (uint8_t i = 0; i < count_; ++i)
        {
            if (threads_[i] == thread)
            {
                threads_[i] = threads_[count_ - 1];
                threads_[--count_] = nullptr;
                return true;
            }
        }
        return false;
    }
```

---

### 1.10 文件 7：新建 `thread/thread_registry.cpp` — 注册表实例管理

**新文件全文：**

```cpp
/**
 * @file thread_registry.cpp
 * @author qingyu
 * @brief 线程注册表的实例登记与注销
 * @version 0.1
 * @date 2026-10-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "thread_registry.hpp"

#pragma message "Compiling Init/Thread/Thread_registry"

/**
 * @brief 线程登记入口：Thread::Start 创建线程后调用
 * @param thread 已创建的线程对象
 */
void RegisterThreadInstance(ThreadBase* thread)
{
    if (!thread_registry::Add(thread)) {
        /* 静默失败会让线程在 shell 里不可见，只能靠这里的警告发现 */
        printk("[boot] thread registry full, thread not visible to shell\n");
    }
}

/**
 * @brief 线程注销入口：Thread::ExitThread 收尾后调用
 * @param thread 已退出的线程对象
 */
void UnregisterThreadInstance(ThreadBase* thread)
{
    (void)thread_registry::Remove(thread);
}
```

**说明**：
- 用 `printk` 而非 `DUST_LOG_WRN`——`thread_registry.cpp` 会被所有板卡编译，而 `DUST_LOG` 依赖 shell 子系统；注册表满属于启动期早期问题，`printk` 一定可用。
- 注册表满的日志本来在 P8 里要加，**挪到这里更合适**（谁管台账谁打日志），§8 因此只保留 `REGISTER_THREAD` 的注释改动。

---

### 1.11 文件 8：`init/CMakeLists.txt` — 加两个新源文件

**位置**：`target_sources` 块（第 3-8 行）

**old：**

```cmake
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/Init_entry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/boot_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/Irq_handlers.cpp
)
```

**new：**

```cmake
target_sources(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/Init_entry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/boot_timing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/startup/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thread/thread_fault.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thread/thread_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/irq/Irq_handlers.cpp
)
```

`target_include_directories` 已经包含 `thread/`，新增的 `thread_registry.cpp` / `thread_fault.cpp` 能直接 `#include "thread.hpp"`，**不用改 include 路径**。

---

### 1.12 文件 9：`modules/imu` — 设为 Recover + RetryHook

**imu.hpp `Start`（第 40-47 行）**

old：
```cpp
    bool Start(ThreadPrio prio = ThreadPrio::Normal)
    {
        if (!ready_) {
            return false;
        }
        thread_.Start(TaskEntry, kPeriod, prio, this, "imu");
        return true;
    }
```
new：
```cpp
    bool Start(ThreadPrio prio = ThreadPrio::Normal)
    {
        if (!ready_) {
            return false;
        }
        thread_.SetFaultPolicy(ThreadFaultPolicy::Recover);   // 姿态链路：故障不停机
        thread_.SetRetryHook(OnRetry);
        thread_.Start(TaskEntry, kPeriod, prio, this, "imu");
        return true;
    }
```

**imu.hpp 私有成员（第 66-67 行附近）**

old：
```cpp
    Thread<2048>            thread_     {};
    bool                    ready_      = false;
```
new：
```cpp
    Thread<2048>            thread_     {};
    bool                    ready_      = false;
    uint32_t                fail_count_ = 0;        // 连续读失败次数（成功即清零）
```

**imu.hpp 私有声明（第 72 行 `TaskResult Task(void* context);` 之后）**

```cpp
    static bool OnRetry(void* context);
```

**imu.cpp 顶部常量（`namespace imu {` 之后）**

```cpp
static constexpr uint32_t kFailLogInterval = 100;   // 连续失败 100 次（1kHz 下约 100ms）打一条
```

**imu.cpp 新增实现（`Task()` 之后）**

```cpp
/**
 * @brief Retry 钩子：读失败后由本线程自行消化，不上报统一处理器
 *
 * IMU 是 SPI 从设备，单次读失败多为瞬态（总线抖动/片选时序），
 * 继续下一拍即可恢复。日志按 kFailLogInterval 节流，避免 1kHz 刷屏。
 *
 * @param context ImuManager 实例
 * @return 恒 true=已处理，外壳继续下一拍
 */
bool ImuManager::OnRetry(void* context)
{
    auto* self = static_cast<ImuManager*>(context);

    self->fail_count_++;

    if (self->fail_count_ >= kFailLogInterval)
    {
        DUST_LOG_ERR("imu read failed x%u (spi link?)", (unsigned)self->fail_count_);
        self->fail_count_ = 0;
    }

    return true;
}
```

**imu.cpp `Task()` 成功路径清零（第 105-107 行）**

old：
```cpp
    if (self->source_ == nullptr || !self->source_->Read(self->sample_)) {
        return TaskResult::Retry;
    }
```
new：
```cpp
    if (self->source_ == nullptr || !self->source_->Read(self->sample_)) {
        return TaskResult::Retry;       // 交给 OnRetry：计数 + 节流打日志，然后继续
    }

    self->fail_count_ = 0;              // 读成功：连续失败计数清零
```

---

### 1.13 文件 10~13：其余线程

| 文件 | 线程 | 改动 |
|---|---|---|
| `framework/modules/remotes/remote.hpp` | `remote` | `Start()` 里 `SetRecoveryHook(Recover)` → `SetFaultPolicy(ThreadFaultPolicy::Recover); SetRetryHook(OnRetry);`；第 140 行 `static bool Recover(void* context);` → `OnRetry` |
| `framework/modules/remotes/remote.cpp` | — | 第 64 行 `Remote::Recover` → `Remote::OnRetry`；第 22 行注释删掉"停止检查由 Thread 外壳负责"（外壳没有停止检查） |
| `project/thread/gpio/trd_gpio.cpp` | `output` | `thread_start()` 里 `thread_.Start(...)` **之前**加 `thread_.SetFaultPolicy(ThreadFaultPolicy::Isolate);` |
| `project/thread/test/trd_test.cpp` | `test` | 同上，`Isolate` |

**`shell` 不用设**：它从不返回 `Retry`，默认 `Recover` 不影响行为。

---

### 1.14 语义对比

| 场景 | 改动前 | 改动后 |
|---|---|---|
| `gpio` 返回 `Retry`（无钩子） | 线程静默死，注册表留 dead | 中心判 `Isolate` → `ExitThread()`（ExitHook → 注销 → 打日志） |
| `imu` 返回 `Retry` | 线程静默死，姿态断 | `OnRetry` 消化 → 节流打日志 → 继续跑 |
| `remote` 返回 `Retry` | 走 `Recover` 钩子（恒 true） | 走 `OnRetry`（改名不改行为） |
| 线程退出后 `thread list` | 永久 `dead` 行 | **不留残余**（已注销） |
| 项目想统一处置 | 无处可挂 | `ThreadFaultCenter::SetHandler(my_handler)` |
| 搜 `cleanup` | 命中启动期 | **零命中**（已改 `rollback`） |
| 搜 `RecoveryHook` | 命中 Retry 处理 | **零命中**（已改 `RetryHook`） |
| `thread.hpp` 看到的东西 | 线程 + 注册表 + 未来会有的故障中心 | **只有线程**，故障中心在 `thread_fault.hpp` |

### 1.15 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | `grep -rn "cleanup" framework project --include=*.hpp --include=*.cpp` | 零命中 |
| 2 | `grep -rn "RecoveryHook\|SetRecoveryHook\|ThreadCleanupHook"` | 零命中 |
| 3 | `ls framework/init/thread/` | 五个文件：`thread.hpp` / `thread_fault.hpp` / `thread_fault.cpp` / `thread_registry.hpp` / `thread_registry.cpp` |
| 4 | `grep -n "include" framework/init/thread/thread_fault.hpp` | 只有 `<cstdint>`（无 log/buzzer/kernel） |
| 5 | `dust build` | 编译通过 |
| 6 | 正常启动 | 五行全 `running`，行为与改前一致 |
| 7 | 拔掉 IMU 再上电 | 每 ~100ms 一条 `[err] imu read failed x100`，**imu 仍 `running`，`RUN` 增长** |
| 8 | 插回 IMU | 日志停止，姿态恢复 |
| 9 | 让 `gpio` 线程返回 `Retry` | `[wrn] thread output fault, isolating` + `[wrn] thread output exited`，`thread list` **少一行** |
| 10 | 给某线程设 ExitHook 后再触发 Isolate | ExitHook 被调用一次，之后才注销 |
| 11 | `ThreadFaultCenter::SetHandler(自定义)` 后触发故障 | 自定义处理器被调用，返回值决定动作 |
| 12 | 触发 `Halt` 策略线程故障 | `[err] ... fatal, halting`，长鸣，系统停 |

---

## 2. P2 — 64 位统计撕裂（高危）

> **P2 只有 1 步**：三处改动全在 `thread.hpp`，必须一次改完才一致。
> 用户说"开始第 2 步"= 只改 `framework/init/thread/thread.hpp` 的三处（§2.2 / §2.3 / §2.4），改完停下。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **2-1** | `framework/init/thread/thread.hpp` | ① `Run()` 写入侧加 `irq_lock` 临界区；② `GetStats()` 读取侧加临界区；③ `ResetStats()` 加临界区 | 无 | 无 |

### 问题复述

`thread.hpp:278-280` 三个 `uint64_t` 由工作线程写、shell 线程读，RV32 上会撕裂（`thread.hpp:251-253` 是读点）。

### 2.1 方案选择

| 方案 | 做法 | 代价 |
|---|---|---|
| **A（推荐）** | 读写都套 `irq_lock` / `irq_unlock` | 写入每次 2 条指令的开销，Task 每拍 3 次写 → 6 条指令/拍 |
| B | 拆成 `atomic_t` 高低 32 位 + 序号校验 | 代码复杂，读侧要重试循环 |
| C | 改成 32 位（`uint32_t` 存 ms） | 精度降、会回绕 |

**选 A**。理由：这一组统计只在 `Run()` 里每拍写 3 次，`irq_lock` 的代价（关中断 + 2 条指令）相对于一次 Task 调用可忽略；读侧在 shell 命令里，本来就是低频。

### 2.2 改动 1：写入侧加临界区

**文件**：`framework/init/thread/thread.hpp`
**位置**：`Run()`，第 303-309 行

**old：**

```cpp
            const uint64_t elapsed = k_cycle_get_64() - begin_cycle;
            run_cycles_ 	+= elapsed;
            last_run_cycles_ = elapsed;

            if (elapsed > max_run_cycles_) {
                max_run_cycles_ = elapsed;
            }
```

**new：**

```cpp
            const uint64_t elapsed = k_cycle_get_64() - begin_cycle;

            // 三个 64 位统计量跨线程被 shell 读：RV32 上 64 位读写非原子，
            // 必须与前一次读（GetStats）互斥，否则读到半新半旧的值。
            {
                const unsigned int key = irq_lock();
                run_cycles_     += elapsed;
                last_run_cycles_ = elapsed;
                if (elapsed > max_run_cycles_) {
                    max_run_cycles_ = elapsed;
                }
                irq_unlock(key);
            }
```

### 2.3 改动 2：读取侧加临界区

**位置**：`GetStats()`，第 251-253 行

**old：**

```cpp
        out.run_ns       = k_cyc_to_ns_floor64(run_cycles_);
        out.max_run_ns   = k_cyc_to_ns_floor64(max_run_cycles_);
        out.last_run_ns  = k_cyc_to_ns_floor64(last_run_cycles_);
```

**new：**

```cpp
        {
            const unsigned int key = irq_lock();
            const uint64_t run_cycles      = run_cycles_;
            const uint64_t max_run_cycles  = max_run_cycles_;
            const uint64_t last_run_cycles = last_run_cycles_;
            irq_unlock(key);

            out.run_ns      = k_cyc_to_ns_floor64(run_cycles);
            out.max_run_ns  = k_cyc_to_ns_floor64(max_run_cycles);
            out.last_run_ns = k_cyc_to_ns_floor64(last_run_cycles);
        }
```

**注意**：`k_cyc_to_ns_floor64` 放在临界区**外**，减少关中断时长。

### 2.4 改动 3：`ResetStats()` 也要保护

**位置**：第 229-238 行

**old：**

```cpp
    void ResetStats() override
    {
        atomic_clear(&run_count_);
        atomic_clear(&retry_count_);
        atomic_clear(&drop_count_);

        run_cycles_      = 0;
        max_run_cycles_  = 0;
        last_run_cycles_ = 0;
    }
```

**new：**

```cpp
    void ResetStats() override
    {
        atomic_clear(&run_count_);
        atomic_clear(&retry_count_);
        atomic_clear(&drop_count_);

        const unsigned int key = irq_lock();
        run_cycles_      = 0;
        max_run_cycles_  = 0;
        last_run_cycles_ = 0;
        irq_unlock(key);
    }
```

### 2.5 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | `dust build` 编译通过 | 无错误 |
| 2 | `thread show imu` 连续敲 20 次 | `last` / `max` / `total` 单调不倒退，无非理性大值 |
| 3 | `thread reset imu` 后再 `thread show imu` | 三个数归零，随后重新增长 |

---

## 3. P3 — KeepAlive 假保护（高危）

> **P3 拆成 4 步。**
> 用户说"开始第 3 步"= 只做 3-1，改完停下等"继续"。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **3-1** | `framework/init/thread/thread.hpp` | 删 `ThreadBase` 的两个纯虚函数 `SetKeepAlive` / `KeepAlive`（§3.2） | 无 | 此时 `Thread<>` 的 override 会报错（3-2 修）；`shell.hpp` 的转发会报错（3-4 修） |
| **3-2** | `framework/init/thread/thread.hpp` | 删 `Thread<>` 的两个实现（§3.3）+ 删成员 `keep_alive_`（§3.4） | 3-1 | 3-1~3-2 完成前编不过 |
| **3-3** | `framework/shell/shell.hpp` + `shell.cpp` | 删 `SetKeepAlive` 转发 + `SetKeepAlive(true)` 调用（§3.5） | 3-2 | `shell.*` 同概念，合并一步 |
| **3-4** | `framework/cmd/fatal/fatal.cpp` | 文件头注释补一句"fatal 路径上中断自关到停机，任何线程都不会再被调度"（§3.6） | 无 | 无（纯注释） |

### 问题复述

`KeepAlive` 全仓库零读取者；`fatal.cpp` 里也没有冻结逻辑；**且 fatal 路径上物理不可实现**（`z_fatal_error` 关中断 → handler `while(1)` 永不返回 → 中断永不恢复 → 任何线程都不再被调度）。

### 3.1 方案选择

| 方案 | 做法 | 评价 |
|---|---|---|
| **A（推荐）** | **删除** `SetKeepAlive` / `KeepAlive` / `keep_alive_`，同时删 `shell.cpp:179` 的调用和误导注释 | 承认机制不存在，代码与事实一致 |
| B | 保留 API，改成"fatal 时不冻结"的通用开关，等将来真做线程冻结时用 | 保留一个没有消费者的 API = 继续骗人 |

**选 A**。理由：这条语义的**目的**（fatal 时 shell 还能打日志）已经被 `FatalOut` 轮询直发解决了——那才是真实机制。`KeepAlive` 是早期的设想，没有实现，也没有必要实现。

### 3.2 改动 1：删 `ThreadBase` 的两个纯虚函数

**文件**：`framework/init/thread/thread.hpp`
**位置**：第 74-83 行

**old：**

```cpp
    /**
     * @brief 设置「保留标记」：致命错误冻结线程时不暂停本线程
     * @param on true=保留（如 shell 调试通道）
     */
    virtual void SetKeepAlive(bool on) = 0;

    /**
     * @brief 是否带保留标记
     */
    virtual bool KeepAlive() const = 0;

    /**
     * @brief 暂停线程：内核不再调度，保留全部现场
     */
```

**new：**

```cpp
    /**
     * @brief 暂停线程：内核不再调度，保留全部现场
     */
```

### 3.3 改动 2：删 `Thread<>` 的实现

**位置**：第 201-216 行

**old：**

```cpp
    /**
     * @brief 设置「保留标记」：致命错误冻结线程时不暂停本线程
     * @param on true=保留
     */
    void SetKeepAlive(bool on) override
	{
		atomic_set(&keep_alive_, on ? 1 : 0);
	}

    /**
     * @brief 是否带保留标记
     */
    bool KeepAlive() const override
	{
		return atomic_get(&keep_alive_) != 0;
	}

    /**
     * @brief 业务侧计数一次丢帧/丢包
     */
```

**new：**

```cpp
    /**
     * @brief 业务侧计数一次丢帧/丢包
     */
```

### 3.4 改动 3：删成员变量

**位置**：第 267 行

**old：**

```cpp
    atomic_t     keep_alive_      = ATOMIC_INIT(0);   // fatal 冻结时保留（默认冻结）
```

**new：**（整行删除）

### 3.5 改动 4：删 shell 侧转发与调用

**文件**：`framework/shell/shell.hpp`
**位置**：第 47-54 行

**old：**

```cpp
    /**
     * @brief 设置「保留标记」：致命错误冻结业务线程时本线程继续运行
     *
     * shell 是日志的唯一消费者和唯一的现场观察窗口，不能被冻结。
     *
     * @param on true=保留
     */
    void SetKeepAlive(bool on) { thread_.SetKeepAlive(on); }
```

**new：**（整段删除）

**文件**：`framework/shell/shell.cpp`
**位置**：第 176-179 行

**old：**

```cpp
static bool thread_start()
{
    shell_.SetKeepAlive(true);      // fatal 冻结业务线程时保留 shell（日志消费者 + 观察窗口）
    shell_.Start();
```

**new：**

```cpp
static bool thread_start()
{
    shell_.Start();
```

### 3.6 改动 5：`fatal.cpp` 补一句事实说明

**文件**：`framework/cmd/fatal/fatal.cpp`
**位置**：第 36-47 行的文件头注释块

**在该注释块末尾追加一段：**

```
 *
 * 注：fatal 路径上中断自 z_fatal_error 起一直关闭（arch_irq_lock 在调用
 * handler 前执行、arch_irq_unlock 在 handler 返回后执行），本 handler 永不
 * 返回，因此除当前上下文外任何线程都不会再被调度。不存在"保留某个线程
 * 继续运行"的可能，现场输出只能靠轮询直发。
```

### 3.7 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 全仓库 `grep -rn "KeepAlive" --include=*.cpp --include=*.hpp` | 零命中 |
| 2 | `dust build` | 编译通过 |
| 3 | `thread list` | 输出不变（KeepAlive 本来就不显示） |

---

## 4. P4 — `IsAlive()` 死条件 + 未启动误报

> **P4 只有 1 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **4-1** | `framework/init/thread/thread.hpp` | `IsAlive()` 删 `"exited"` 死条件 + 加 `started_` 早退 | 无 | 无 |

### 4.1 改动

**文件**：`framework/init/thread/thread.hpp`
**位置**：第 327-336 行

**old：**

```cpp
    /**
     * @brief 线程是否仍在运行
     * @return true=未退出
     */
    bool IsAlive() const
    {
        char buf[32];
        const char* state = k_thread_state_str(const_cast<k_thread*>(&thread_), buf, sizeof(buf));
        return (state != nullptr) && (std::strstr(state, "dead") == nullptr) && (std::strstr(state, "exited") == nullptr);
    }
```

**new：**

```cpp
    /**
     * @brief 线程是否仍在运行
     * @return true=已启动且未退出
     */
    bool IsAlive() const
    {
        if (!atomic_get(&started_)) {
            return false;                   // 从未启动：k_thread 全零，state 为空串
        }

        char buf[32];
        const char* state = k_thread_state_str(const_cast<k_thread*>(&thread_), buf, sizeof(buf));
        return (state != nullptr) && (std::strstr(state, "dead") == nullptr);
    }
```

**说明**：
- 删掉永不匹配的 `"exited"`（Zephyr 状态串只有 `dummy/pending/sleeping/dead/suspended/aborting/suspending/queued`）。
- 加 `started_` 早退，修掉"未启动对象报活着"。

### 4.2 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 正常启动后 `thread list` | 五个线程 STATE 都是 `running` 或 `paused`，与改前一致 |
| 2 | 触发 imu 线程退出（P1 验证 #3 的方式）后 `thread list` | imu 行 STATE = `dead` |

---

## 5. P5 — 更新 `doc/线程编写指南.md`

> **P5 只有 1 步**（同一文件内六处删改）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **5-1** | `doc/线程编写指南.md` | §6.1 只留一个 `Start` 重载 · §6.2 删裸任务整节 · §6.3 重编号 · §6.5 改成两态 · §6.6 补"无钩子的后果" · §6.8 删 `fatal_count` | P1 完成后才写（否则描述的语义还没落地） | 无 |

### 5.1 改动清单

| 位置 | 现状 | 改成 |
|---|---|---|
| `:405-441` §6.1 | 三个 `Start` 重载 | 只讲唯一的重载 `Start(TaskFunc, k_timeout_t, ThreadPrio, void*, const char*)`，并说明"2026-10-22 起裸任务入口已删除" |
| `:443-462` §6.2 | "路径 A：裸任务" | **整节删除**，把"路径 B/C"合并成"受管任务" |
| `:464-497` §6.3 | 受管任务 | 保留，重编号为 §6.2 |
| `:542-559` §6.5 | `TaskResult` 四态含 `Fatal` | 改成两态；补一句"MCU 上线程常驻，没有任务跑完就退的语义，因此不设第三态" |
| `:621-650` §6.8 | `ThreadStats` 含 `fatal_count` | 删掉 `fatal_count` 那一行 |
| `:561-596` §6.6 恢复钩子 | 未提"没设钩子会死" | 补一句"未注册恢复钩子时，`Retry` 直接导致线程退出（会打 ERR 日志）" |

### 5.2 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 文档内搜 `Fatal` | 只剩说明"已删除"的那处 |
| 2 | 文档内搜 `k_thread_entry_t` | 零命中 |
| 3 | 文档内搜 `fatal_count` | 零命中 |

---

## 6. P6 — `StackSize` 语义（只改注释）

> **P6 只有 1 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **6-1** | `framework/init/thread/thread.hpp` | 类注释 `@tparam StackSize` 补"不等于可用栈字节"（§6.1） | 无 | 无 |


### 6.1 改动

**文件**：`framework/init/thread/thread.hpp`
**位置**：第 107-111 行类注释

**old：**

```cpp
/**
 * @brief 线程模板，封装 k_thread 创建、暂停/恢复和受管任务外壳
 *
 * @tparam StackSize 线程栈大小
 */
```

**new：**

```cpp
/**
 * @brief 线程模板，封装 k_thread 创建、暂停/恢复和受管任务外壳
 *
 * @tparam StackSize 请求的栈字节数。实际对象大小由 Zephyr 的
 *         K_KERNEL_STACK_LEN 决定：先按 ARCH_STACK_PTR_ALIGN 对齐、
 *         再加 ARCH_KERNEL_STACK_RESERVED（RISC-V 有 PMP guard 时约 512B），
 *         最后按 2 的幂对齐。可用栈 = 对象大小 − reserved，不等于本参数。
 */
```

### 6.2 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 读注释 | 能明确回答"`Thread<128>` 到底给多少栈" |

---

## 7. P7 — 周期语义与观测

> **P7 只有 1 步。**（§7.3 的"实际周期统计"默认不做）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **7-1** | `framework/init/thread/thread.hpp` | `Start` 的 `@param period` 注释（§7.1）+ 成员 `period_` 注释（§7.2） | 无 | 无 |


### 7.1 改动 1：API 注释补语义

**文件**：`framework/init/thread/thread.hpp`
**位置**：第 118-125 行 `Start` 的 Doxygen

**old：**

```cpp
     * @param period  两拍之间的等待时长（K_NO_WAIT=不等待，节拍由任务体自己决定）
```

**new：**

```cpp
     * @param period  上一拍结束到下一拍开始之间的睡眠时长，不是固定周期。
     *                任务体自身耗时会叠加在实际周期上；要固定频率须在 Task 内
     *                自己按经过时间补偿。K_NO_WAIT=不睡，节拍完全由任务体决定。
```

### 7.2 改动 2：成员注释

**位置**：第 270 行

**old：**

```cpp
    k_timeout_t  period_          = K_NO_WAIT;        // 两拍间隔
```

**new：**

```cpp
    k_timeout_t  period_          = K_NO_WAIT;        // 拍间睡眠时长（非固定周期，见 Start 注释）
```

### 7.3 待用户拍板：要不要加"实际周期"统计

`ThreadStats` 现在只有任务体耗时，没有实际周期。要不要加：

| 选项 | 实现 | 代价 |
|---|---|---|
| ①不加 | 保持现状 | 0 |
| ②加 `period_ns`（最近一拍的实际间隔） | `Run()` 里额外记一个 `k_cycle_get_64()` 差值 + 1 个 `uint64_t` 成员 | +8B/线程 + 每次读 cycle 两次 |

**本规划默认选 ①（不加）**——当前没有排查实际周期的需求，加了是凭空增加 RAM 和复杂度。用户要加再补。

### 7.4 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 读 `Start` 注释 | 能回答"period 是不是固定频率" |

---

## 8. P8 — fail 策略边界

> **P8 只有 1 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| **8-1** | `framework/init/startup/Init_entry.hpp` | `REGISTER_THREAD` 的 `@param fail_` 注释补"只管创建那一刻"（§8.2） | 无 | 无 |


### 8.1 注册表满日志已在 P1 完成

**P1 §1.10 新建的 `thread/thread_registry.cpp` 里已经实现了**：

```cpp
void RegisterThreadInstance(ThreadBase* thread)
{
    if (!thread_registry::Add(thread)) {
        printk("[boot] thread registry full, thread not visible to shell\n");
    }
}
```

用 `printk` 而不是 `DUST_LOG_WRN`：注册表满属启动期早期问题，`printk` 一定可用；`thread_registry.cpp` 也不该依赖 shell 子系统。

**P8 因此不再重复这条**，只保留下面的注释改动。

### 8.2 `REGISTER_THREAD` 的 fail 策略补注释

**文件**：`framework/init/startup/Init_entry.hpp`
**位置**：`REGISTER_THREAD` 宏的 Doxygen（第 118-127 行附近）

**old：**

```cpp
 * @param fail_    失败策略：HaltOnFail 中止后续启动，ContinueOnFail 继续
```

**new：**

```cpp
 * @param fail_    失败策略：HaltOnFail 中止后续启动，ContinueOnFail 继续。
 *                 注意：只覆盖 thread_start() 返回 false 这一瞬间。线程启动
 *                 之后在运行期退出（如 Retry 无恢复钩子），本策略不生效。
```

### 8.3 验证

| # | 操作 | 期望 |
|---|---|---|
| 1 | 编译通过 | — |
| 2 | 读 `REGISTER_THREAD` 注释 | 能回答"线程跑起来后死掉会不会触发 HaltOnFail" |

---

## 9. P9 — 停止原语 / Start 幂等（**不做，无步**）

### 9.1 为什么不做

P1 之后线程有两种终止方式：`Isolate` 策略自杀收尾、`Halt` 全机停机。前者已经有完整收尾路径（ExitHook + 注销），不需要"停止原语"再来一遍。

主动停止在 MCU 上没有正当场景：

| 场景 | MCU 上的答案 |
|---|---|
| "任务干完了要退出" | **不存在**——线程常驻，任务体只写"一拍" |
| "节省 CPU 让别的线程跑" | 用优先级 + `k_sleep`/`k_sem_take`，不是停线程 |
| "出错了要停下来" | 出错走 P1 的策略分流（`Isolate` 收尾退出 / `Recover` 继续），不是停 |
| "运行期换业务逻辑" | 不换。要换就烧固件 |
| "调试时临时停一下" | **已经有 `Pause()` / `Resume()`**（`k_thread_suspend`），保留全部现场 |

最后一条是关键：**调试用的"停"已经有 `thread pause` 了，而且它比"软停止标志"更强**——内核级挂起，任务体一行代码都不用改。再加一套 `RequestStop`/`StopRequested` 是功能重叠，且要求所有 Task 加检查点，纯负担。

`Start()` 幂等同理：P1 之后线程只会有两种归宿——**活着跑**，或者**Isolate 时收尾后消失**。不存在"重新拉起来"的合法场景，所以不需要幂等保护。

### 9.3 结论

**P9 不执行。** 不是"优先级低"，是**没有需求**。

若将来真出现"必须让某线程永久停"的场景（目前想不到），再单独立项——届时也要先回答"为什么不直接用 `k_thread_abort` + `thread pause`"。

---

## 10. 执行顺序与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P1 | 三套生命周期名字分干净 + 统一故障处理器 + 按线程策略分流 | §1.13 九条 |
| P2 | 64 位统计临界区 | §2.5 三条 |
| P3 | 删 KeepAlive | §3.7 三条 |
| P4 | `IsAlive()` 修正 | §4.2 两条 |
| P5 | 文档更新 | §5.2 三条 |
| P6 | StackSize 注释 | §6.2 一条 |
| P7 | 周期注释 | §7.4 一条 |
| P8 | `REGISTER_THREAD` fail 边界注释 | §8.3 两条（注册表满日志已在 P1） |
| P9 | **不做** | — |

**每阶段独立，可按任意顺序执行**；P1 和 P2 涉及同一函数（`Run()` / `GetStats`），建议先 P1 再 P2（避免冲突）。

**P1 与 P8 有交叉**：P1 在 `Init_entry.cpp` 里新增 `UnregisterThreadInstance` 和 `ThreadFaultCenter::DefaultHandle`，P8 在同一个文件里改 `RegisterThreadInstance`——两者改的是不同函数，不冲突，但如果同一次执行，先做 P1。

**编译/烧录是用户的动作，AI 不代跑。**

---

## 11. 汇总：改动文件清单

| 文件 | 阶段 | 改动 |
|---|---|---|
| `framework/init/startup/Init_entry.hpp` | **P1** P8 | **`CleanupFunc`→`RollbackFunc`、`InitEntry::cleanup`→`rollback`、`REGISTER_INIT` 参数改名**；`REGISTER_THREAD` 的 fail 语义补注释 |
| `framework/init/startup/Init_entry.cpp` | **P1** | **`cleanup`→`rollback` 两处调用**；删掉 `RegisterThreadInstance` 与 `#include "thread_registry.hpp"`（挪去 `thread_registry.cpp`） |
| **`framework/init/thread/thread_fault.hpp`** | **P1 新建** | `ThreadFaultPolicy` / `ThreadFaultAction` / `RetryHook` / `ExitHook` / `ThreadFaultHandler` / `ThreadFaultCenter`（只依赖 `<cstdint>`） |
| **`framework/init/thread/thread_fault.cpp`** | **P1 新建** | `DefaultHandle`（static）+ `ThreadFaultCenter::Report` 实现（log / buzzer / kernel 依赖都在这里） |
| **`framework/init/thread/thread_registry.cpp`** | **P1 新建** | `RegisterThreadInstance`（含注册表满 `printk`）+ `UnregisterThreadInstance` |
| `framework/init/thread/thread.hpp` | P1 P2 P3 P4 P6 P7 | P1：include `thread_fault.hpp` + `log.hpp`、删 `RecoveryHook`、加 `FaultPolicy()`、`SetFaultPolicy`/`SetRetryHook`/`SetExitHook`、`ExitThread()`、`Run()` 接故障分流；P2 统计临界区；P3 删 KeepAlive；P4 `IsAlive` 修正；P6/P7 注释 |
| `framework/init/thread/thread_registry.hpp` | P1 | 新增 `Remove()` |
| `framework/init/CMakeLists.txt` | **P1** | `target_sources` 加 `thread/thread_fault.cpp` 和 `thread/thread_registry.cpp` |
| `framework/modules/imu/drivers/imu.hpp` | P1 | `Start` 加 `SetFaultPolicy(Recover)` + `SetRetryHook(OnRetry)`、`fail_count_` 成员、`OnRetry` 声明 |
| `framework/modules/imu/drivers/imu.cpp` | P1 | 新增 `OnRetry` 实现、`kFailLogInterval` 常量、`Task` 成功路径清零 `fail_count_` |
| `framework/modules/remotes/remote.hpp` | P1 | `Recover`→`OnRetry` 改名、`SetRecoveryHook`→`SetRetryHook`、新增 `SetFaultPolicy(Recover)` |
| `framework/modules/remotes/remote.cpp` | P1 | `Remote::Recover`→`OnRetry` 改名、删掉不存在的"停止检查"注释 |
| `project/thread/gpio/trd_gpio.cpp` | P1 | `thread_start` 加 `SetFaultPolicy(Isolate)` |
| `project/thread/test/trd_test.cpp` | P1 | `thread_start` 加 `SetFaultPolicy(Isolate)` |
| `framework/shell/shell.hpp` | P3 | 删 `SetKeepAlive` 转发 |
| `framework/shell/shell.cpp` | P3 | 删 `SetKeepAlive(true)` 调用 |
| `framework/cmd/fatal/fatal.cpp` | P3 | 文件头注释补一句事实说明 |
| `doc/线程编写指南.md` | P5 | §6.1/6.2/6.5/6.6/6.8 更新 |

**跨仓库**：`thread.hpp` / `thread_registry.hpp` / `Init_entry.*` 在 `framework/init`；`imu.*` / `remote.*` 在 `framework/modules`；`shell.*` 在 `framework/shell`；`fatal.cpp` 在 `framework/cmd`；`trd_gpio.cpp` / `trd_test.cpp` 在 `project`；文档在主仓库。

---

## 12. 与调研文档的对应关系

| 调研编号 | 缺口 | 本规划阶段 |
|---|---|---|
| A | Retry 无钩子静默死 | **P1** |
| B | KeepAlive 假保护 | **P3** |
| C | 64 位统计撕裂 | **P2** |
| D | `IsAlive()` 死条件 | **P4** |
| E | 无停止原语 | **P9（不做的理由见 §9）** |
| F | `Start()` 无幂等 | **P9（同上）** |
| G | StackSize 语义 | **P6** |
| H | 周期语义/观测 | **P7** |
| I | 文档失效 | **P5** |
| J | 注册表满静默失败 | **P1**（挪进 `thread_registry.cpp`） |
| K | fail 策略边界 | **P8** |
