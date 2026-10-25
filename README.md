# zephyr_user 子树架构

> 更新日期：2026-10-20
> 适用路径：`E:\Zephyr\zephyr_user`
> 规模：6 个架构层 / 约 1.7 万行框架代码 / 4 块板卡 / 7 个工作区

一套面向 RoboMaster 的 Zephyr 电控架构。

**本文不是使用手册。** 使用看 [快速上手.md](快速上手.md)，线程细节看 [doc/线程编写指南.md](doc/线程编写指南.md)。本文讲的是：**这套架构为什么长成这样，每一处设计解决的是什么问题。**

---

## 目录

1. [设计目标](#1-设计目标)
2. [六层分工与依赖方向](#2-六层分工与依赖方向)
3. [编译期决定一切：Kconfig 门禁链](#3-编译期决定一切kconfig-门禁链)
4. [启动层：注册式生命周期](#4-启动层注册式生命周期)
5. [线程层：受管任务外壳](#5-线程层受管任务外壳)
6. [调试台：一套自研的异步日志系统](#6-调试台一套自研的异步日志系统)
7. [shell 命令表](#7-shell-命令表)
8. [驱动层：Stream 抽象与双缓冲 DMA](#8-驱动层stream-抽象与双缓冲-dma)
9. [模块层：IMU 前后端隔离](#9-模块层imu-前后端隔离)
10. [模块层：Remote 协议自动探测](#10-模块层remote-协议自动探测)
11. [横切能力：没有也能编](#11-横切能力没有也能编)
12. [数据契约层：topic](#12-数据契约层topic)
13. [业务线程：一个文件装下全部接线](#13-业务线程一个文件装下全部接线)
14. [为什么 `test` 线程是点睛之笔](#14-为什么-test-线程是点睛之笔)
15. [目录结构](#15-目录结构)
16. [新增一个线程](#16-新增一个线程)

---

## 1. 设计目标

只有两条，但每一条都贯穿全部代码：

### 1.1 编译期决定一切

**没有运行期注册、没有工厂、没有 `new`、没有反射。**

一个线程存不存在，在 `prj.conf` 里就能看出来；一段代码编不编进来，在 `CMakeLists.txt` 的 `if()` 里就能看出来；链接后固件里到底有什么，`map` 文件里一行不差。

```text
prj.conf / 板卡 .conf
      ↓  Kconfig 符号
CMakeLists 的 if(CONFIG_TRD_XXX)
      ↓  源文件是否参与编译
链接段 .user_init / .user_thread
      ↓  符号是否进表
启动器遍历
```

**为什么这么做**：嵌入式调试最痛苦的是"我明明写了为什么没跑"。这条链上任何一环断了，都能在编译期或链接期看到，不会拖到板子上。

### 1.2 每一层都不需要知道相邻层是谁

| 层 | 它认识谁 | 它**不认识**谁 |
|---|---|---|
| `algorithm` | 标准库 | Zephyr、任何设备 |
| `drivers` | Zephyr device API | 业务 |
| `modules` | drivers + topic | 谁在用它、什么时候启动 |
| `topic` | 无 | 全部 |
| `cmd` | drivers | 业务调度 |
| `init` | cmd（日志/蜂鸣） | 有哪些业务线程 |

**单向依赖**：`project/` → `framework/*`。任何反向引用都是 bug。

**具体体现**：

- `framework/init` 不知道有 IMU 这个线程，它只遍历链接段
- `ImuManager` 不知道后端芯片是 BMI088 还是 ICM42688P，它只拿到一个 `Source*`
- `Remote` 不知道用户插的是 DR16 还是 SBUS，它自动探测
- shell 不知道有哪些命令族，它遍历 `.shell_cmd` 段
- `buzzer` / `flash` 不知道谁在调用，宏展开成空也能编译

---

## 2. 六层分工与依赖方向

```text
framework/
├── algorithm/   纯计算：PID / LQR / Kalman / EKF / 四元数 / ESO / RLS / 缓冲 / 定时器
├── drivers/     通用外设封装：Stream / UartDma / Can / Spi / Usb / Gpio / Pwm / Rs485
├── modules/     设备能力：IMU（前后端） / Remote（协议探测） / 电机 / 功率计
├── topic/       跨线程数据契约：zbus 通道 / msgq
├── cmd/         横切：shell(log/var/thread/boot) / buzzer / flash / fatal / 构建脚本
└── init/        启动层：main + 注册表 + 阶段调度 + 线程模板
```

| 层 | 文件数 | 约行数 | 绝对不做的事 |
|---|---|---|---|
| `algorithm` | 19 | 3.8k | 不 include 任何 Zephyr 设备头，可在 PC 上单测 |
| `drivers` | 27 | 3.7k | 不知道业务是什么 |
| `modules` | 26 | 4.3k | 不接管线程生命周期（线程由 init 层拉起） |
| `topic` | 6 | 0.2k | 不实现任何逻辑，只定义数据 |
| `cmd` | 23 | 3.5k | 不参与业务调度 |
| `init` | 10 | 1.2k | 不知道有哪些业务线程 |

### 2.1 子树 `drivers/` ≠ 架构 `drivers/`

这是最容易混淆的一处：

| | 位置 | 内容 | 谁来维护 |
|---|---|---|---|
| **架构 drivers** | `framework/drivers/` | 通用外设 **C++ 用法封装**（`Stream` / `Can` / `Spi` / `Pwm`） | 架构维护者 |
| **子树 drivers** | `zephyr_user/drivers/` | 板卡专属 **Zephyr 模块式驱动**（`fsmc/` / `usb/hal/stm32/`），带 `module.yml` | 子树维护者 |

区别在**层级**：子树 drivers 是 Zephyr 驱动框架的实现（注册到 `DEVICE_DT_DEFINE`），架构 drivers 是对这些设备的 C++ 使用封装。业务只碰架构 drivers。

---

## 3. 编译期决定一切：Kconfig 门禁链

**这是整个项目的骨架。**

### 3.1 三级符号体系

```kconfig
# ① 业务门禁（project/Kconfig）—— 不带 DUST_ 前缀
config TRD_IMU
    bool "IMU thread"
    default n
    select DUST_MOD_DEV_IMU          # 拉入模块能力
    select DUST_TPC_IMU_TO            # 拉入数据契约

# ② 模块门禁（framework/modules/Kconfig）
config DUST_MOD_DEV_IMU
    bool "IMU module"
    default n
    select DUST_COM_SPI               # 再拉入驱动
    select DUST_FLT_QUATERNION        # 再拉入算法

# ③ 驱动门禁（framework/drivers/Kconfig）
config DUST_COM_SPI
    bool "SPI stream"
    default n
    select SPI                        # 最后落到 Zephyr 原生符号
```

**打开一个 `CONFIG_TRD_IMU=y`，会自动拉入整条链：** SPI 驱动 → 四元数 EKF → IMU topic 通道。

反过来，某一个能力被关掉，所有依赖它的业务自动编不进来——Kconfig 依赖不满足 → 符号不存在 → CMake 的 `if()` 不成立 → 源文件不参与编译。

### 3.2 命名规则（强制）

| 前缀 | 归属 | 例子 |
|---|---|---|
| `TRD_*` / `USE_*` | 业务层（**不带** `DUST_`） | `TRD_IMU` / `USE_CMD_SHELL` |
| `DUST_DEV_*` / `DUST_COM_*` | 架构 drivers | `DUST_COM_UART_DMA` |
| `DUST_MOD_DEV_*` | 架构 modules | `DUST_MOD_DEV_IMU_BMI088` |
| `DUST_TPC_*` | 架构 topic | `DUST_TPC_IMU_TO` |
| `DUST_CMD_*` | 架构 cmd | `DUST_CMD_SHELL` |
| `DUST_CTL_*` / `DUST_FLT_*` / `DUST_ID_*` / `DUST_MATH_*` / `DUST_BUF_*` / `DUST_OBS_*` | 架构 algorithm | `DUST_FLT_QUATERNION` |

**业务门禁不带 `DUST_` 前缀**，是刻意的：一眼就能看出"这个开关属于业务，不属于架构"。

### 3.3 CMake 条件编译

```cmake
# project/thread/CMakeLists.txt
if(CONFIG_TRD_IMU)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/imu/trd_imu.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/imu
    )
endif()
```

**关键点**：`.cpp` 里**不需要 `#ifdef` 包整个文件**。

因为**编译单元本身就是条件编译**——文件根本不参与编译，`REGISTER_INIT` 从没被展开过。

只有写在头文件里、会被多个编译单元展开的宏，才需要 `#ifdef` 保护。

### 3.4 三层筛选与排查顺序

```text
① Kconfig 决定符号      ② CMake 决定是否编译      ③ 链接器决定是否进段
   prj.conf / 板卡 .conf    thread/CMakeLists.txt     __user_init_start
```

"我的线程怎么没跑"按这个顺序查：

| 现象 | 查哪里 |
|---|---|
| 日志里完全没有这个线程 | ① `build/zephyr/.config` 里 `CONFIG_TRD_XXX` 是否是 `y` |
| 符号是 y 但没日志 | ② `thread/CMakeLists.txt` 有没有对应的 `if()` 块 |
| CMake 有但链接段没东西 | ③ `build/zephyr/zephyr.map` 搜 `kInitEntryPtr_` / `kThreadEntryPtr_` |

**这就是"编译期决定一切"的价值**：问题在板子上电之前就已经定死了。

---

## 4. 启动层：注册式生命周期

### 4.1 两个注册宏

```cpp
REGISTER_INIT  (thread_init,  EarlyInit, High, HaltOnFail, nullptr, "imu_init");
REGISTER_THREAD(thread_start, EarlyThread, HaltOnFail, "imu_start");
```

**展开成"结构体 + 指向它的指针"两份对象**（`Init_entry.hpp`）：

```cpp
// ① 结构体本体：普通 const 区
static const InitEntry kInitEntry_thread_init = {
    thread_init, nullptr, &kTiming_thread_init,
    InitStage::EarlyInit, InitLevel::High, StartFailPolicy::HaltOnFail, "imu_init"
};

// ② 指向它的指针：放进 .user_init 段
static const InitEntry* const kInitEntryPtr_thread_init
    __attribute__((used, __section__(".user_init"), aligned(sizeof(void*)))) =
        &kInitEntry_thread_init;
```

**为什么段里存指针而不是结构体**：

| 项 | 段内存结构体 | 段内存指针 |
|---|---|---|
| 每项占段空间 | `sizeof(InitEntry)` = 28 B | 4 B |
| 结构体布局变化 | 段内字节布局跟着变 | 恒定 4 B |
| 遍历步长 | 依赖 `sizeof(InitEntry)` | 恒定 `sizeof(void*)` |

结构体字段增删不再影响段内布局——这是**防御性设计**，避免"改了结构体忘了改遍历"的隐式耦合。

### 4.2 `used` 属性：最容易漏的一环

```cpp
__attribute__((used, __section__(".user_init")))
```

`used` 是**强制保留**。这些对象在源码里没有任何引用，`-O2` + LTO 下编译器完全有理由整个删掉。`used` 是唯一手段。

### 4.3 启动流程

```text
main()
 └── System_Startup()
      PreInit    [High → Mid → Low]     ← 调试台 / buzzer / flash / GPIO
      PreThread                          ← shell 线程
      EarlyInit  [High → Mid → Low]     ← IMU / 遥控器
      EarlyThread                        ← IMU / 遥控器线程
      MidInit / MidThread
      LateInit / LateThread
      AppInit / AppThread
      （每阶段间隔 500ms 给硬件上电稳定 + 蜂鸣提示）
```

**先 Init 后 Thread**，成对推进：保证线程启动时它依赖的资源已经就绪。

**等级只作用于 Init**：同一阶段跑三轮（High → Mid → Low），线程项没有等级，一遍遍历。

### 4.4 失败处理：回滚而不只是停机

| 场景 | 行为 |
|---|---|
| 初始化 false + `ContinueOnFail` | 橙色告警，继续 |
| 初始化 false + `HaltOnFail` | 红色告警 → **逆序回滚所有已成功项** → 长鸣 → 休眠停机 |
| 线程启动 false + `ContinueOnFail` | 橙色告警，继续启动其它线程 |
| 线程启动 false + `HaltOnFail` | 红色告警 → 置 `startup_aborted` → 后续阶段全部跳过 |

**回滚是按反向顺序执行的**：

```text
[err] [boot] imu_init fail
[wrn] [boot] output_init has no rollback
[wrn] [boot] w25q128 rolled back
[wrn] [boot] buzzer rolled back
[wrn] [boot] dbg_init has no rollback
        （长鸣两声）
```

逆序保证两件事：

1. **依赖反转**：后初始化的项可能依赖先初始化的项，先关依赖方再关被依赖方
2. **`dbg_init` 最后处理**：调试台是打日志的，它必须活到最后，否则回滚日志发不出去

**没有 `rollback` 的项也要打日志**（`has no rollback`）——停机现场不能有盲区，要能一眼看出哪一项没被安全处理。

### 4.5 停机为什么用 `k_msleep(1000)`

```cpp
if (halt) {
    RollbackInitedEntries();
    EXEC_BUZZER_LONG();          // 长鸣两声约 2.5s，给日志发送留窗口
    for (;;) { k_msleep(1000); } // 休眠停机
}
```

**不能空转**。shell 线程优先级是 `Lowest`(7)，主线程是 `0`（最高）。主线程空转会把 shell 线程饿死，**失败日志全部堵在队列里发不出去**——现场静默，什么都看不到。

长鸣 + 休眠，是"让低优先级日志线程有机会把现场发完"。

### 4.6 启动期耗时：链接段槽

`REGISTER_INIT` / `REGISTER_THREAD` 每次展开，会**额外生成一个耗时槽**：

```cpp
static BootTimingSlot kTiming_thread_init
    __attribute__((used, __section__(".boot_timing"), aligned(4))) = { "imu_init", 0, false };
```

启动器在每项执行前后读 `k_cycle_get_64()`，把差值写进槽。

**槽数量由链接器按实际注册项决定**——没有固定数组、没有容量上限、不会浪费。

```text
boot timing
BOOT TIMING (total 42 ms, 8 items)
  imu_init          25.134 ms   ok
  w25q128            8.021 ms   ok
  dbg_init           3.552 ms   ok
```

**`.boot_timing` 用 `DATA_SECTIONS`**（可写），因为槽在启动时要写入；而注册表用 `ROM_SECTIONS`（只读）。

---

## 5. 线程层：受管任务外壳

**这是整个框架的核心。**

### 5.1 任务只写一拍

```cpp
static TaskResult Task(void* context)
{
    /* 这一拍做什么 */
    return TaskResult::Continue;
}
thread_.Start(Task, K_MSEC(10), ThreadPrio::High, nullptr, "foo");
```

循环、周期等待、统计、故障处置全部由外壳负责。事件驱动线程不用另写循环，把阻塞点放进 `Task`、周期传 `K_NO_WAIT` 即可。

### 5.2 外壳替 Task 做的事

```cpp
void Run()
{
    for (;;)
    {
        atomic_inc(&run_count_);                        // ① 计数
        const uint64_t begin = k_cycle_get_64();        // ② 开始时刻
        const TaskResult result = task_(context_);      // ③ 调一拍
        const uint64_t elapsed = k_cycle_get_64() - begin;

        run_cycles_ += elapsed;                         // ④ 耗时统计
        last_run_cycles_ = elapsed;
        if (elapsed > max_run_cycles_) max_run_cycles_ = elapsed;

        switch (result)                                 // ⑤ 结果分派
        {
            case Continue: break;
            case Retry:    retry_count++; hook...; break;
            case Exit:     DefaultHandle(this);            break;
        }

        if (!K_TIMEOUT_EQ(period_, K_NO_WAIT)) {
            k_sleep(period_);                           // ⑥ 周期等待
        }
    }
}
```

**Task 里消失的代码**：

| 原本写在 Task 里 | 现在 |
|---|---|
| `for (;;)` | 外壳的循环 |
| `k_msleep(N)` | 外壳的 `k_sleep(period_)` |
| `if (error) { log; return; }` | `return TaskResult::Retry / Exit` |
| 重连逻辑 | Retry 钩子 |
| 运行次数 / 耗时统计 | 外壳自动采集 |

**效果**：`ImuManager::Task` 现在只做 **读一帧 → 控温 → 姿态解算**，然后 `return Continue`。没有任何线程管理代码。

### 5.3 `TaskResult` 三态

| 返回值 | 外壳行为 | 统计 |
|---|---|---|
| `Continue` | 本拍成功，等一个周期再调 | — |
| `Retry` | 先调 Retry 钩子，`true` 则重进 Task 函数体；`false` 则交给 `DefaultHandle` | `retry_count++` |
| `Exit` | 跳过 Retry 钩子，直接交给 `DefaultHandle` 按线程策略处置 | — |

`Retry` 与 `Exit` 的最终处置都由 `DefaultHandle` 按 `ThreadFaultPolicy` 决定（`Isolate` 自杀 / `Recover` 全杀），差别只在于要不要先给 Retry 钩子一次自救机会。

### 5.4 Retry 钩子：错误处理从业务里剥离

```cpp
static bool OnRetry(void* ctx)
{
    auto* self = static_cast<Remote*>(ctx);

    if (self->detect_.last_valid_ms != 0) {
        DUST_LOG_ERR("lost %s", ...);       // 只打一次
        self->detect_.last_valid_ms = 0;    // 一次性标记
    }
    self->pub_ = {};                        // 清零发布数据
    zbus_chan_pub(&pub_remote_to, &self->pub_, K_MSEC(1));
    return true;                            // 重进 Task
}
```

钩子在**编译期用注册宏**声明，不再调 setter：

```cpp
```

**契约**：

1. Task 每拍**重新进入函数**，跨拍状态必须放在函数外（`static` 或类成员）
2. 钩子在**被管理线程自己的上下文**执行，不要在里面等别的线程
3. 钩子拿不到错误码，需要分类时用 `context` 里的字段传递

### 5.5 运行统计：每条线程都能被观察

```text
thread list
THREAD     STATE     RUN      RETRY   DROP    STACK
imu        running   12453    0       0       512
remote     running   9871     3       0       744
test       running   4521     0       0       1900
```

```text
thread show imu
imu:
  state        running
  run          12453
  retry        0
  drop         0
  last         1.002 ms
  max          2.310 ms
  total        12492.4 ms
  stack        512 B unused
```

**口径**：`run_ns` / `max_run_ns` 只测 **Task 函数体**，不含 `k_sleep`、不含统计本身。

**栈剩余量**：`k_thread_stack_space_get()`，需要 `CONFIG_INIT_STACKS` + `CONFIG_THREAD_STACK_INFO`。栈给小了以前只能靠崩溃发现，现在一眼看得见。

### 5.6 暂停 / 恢复

```cpp
void Pause()  { k_thread_suspend(&thread_); atomic_set(&paused_, 1); }
void Resume() { k_thread_resume(&thread_); atomic_clear(&paused_); }
```

**内核级挂起，栈、局部变量、程序指针全部保留**，恢复后从挂起的那条指令继续。

**风险**：如果被暂停的线程持有互斥量，等它的线程会永久阻塞。当前工程只用 `irq_lock` 短临界区（关中断，不跨调度点），所以安全。

### 5.7 线程登记表：模板类型如何统一访问

```cpp
class ThreadBase          // 非模板基类
{
    virtual const char* Name() const = 0;
    virtual bool        GetStats(ThreadStats& out) const = 0;
    virtual void        ResetStats() = 0;
    virtual void        Pause() = 0;
    virtual void        Resume() = 0;
    virtual bool        Paused() const = 0;
    virtual void        Exit() = 0;
};
```

`Thread<2048>` / `Thread<128>` 是**不同的类型**，无法放进同一个数组。用非模板基类 + 虚函数解决。

**登记入口是自由函数**：

```cpp
// thread.hpp 里声明
void RegisterThreadInstance(ThreadBase* thread);

// Init_entry.cpp 里定义（避免 thread.hpp include thread_registry.hpp）
void RegisterThreadInstance(ThreadBase* thread) { thread_registry::Add(thread); }
```

这样 `thread.hpp` 不需要知道 `thread_registry` 的存在，**避免头文件循环依赖**。

### 5.8 线程名：让命令能按名字找人

`Start()` 把 `name_` 存下来：

```cpp
name_ = name;
if (name != nullptr) k_thread_name_set(&thread_, name);
```

- `k_thread_name_set` 给 **Zephyr 内核**看（`k_thread_name_get`、调试器）
- `name_` 给 **shell 命令**看（`thread_registry::Find`）

两者都要，缺一个就有一边是 `?`。

---

## 6. 调试台：一套自研的异步日志系统

### 6.1 为什么要自研

Zephyr 的 `LOG` 走 `printk` / console，和自研 shell 共享 `uart3` 时会产生 **`uart_callback_set` 槽位冲突**——两个模块抢同一个 UART 回调，直接卡死。

自研方案：**log 与 shell 共用同一个 `Stream` 的 DMA 发送通道**，不碰 Zephyr 的回调槽位。

### 6.2 六文件分层（全部头文件内联，只有一个 .cpp）

| 文件 | 层 | 内容 |
|---|---|---|
| `log.hpp` | 门面 | `Log` 类 + `DUST_LOG_*` 宏族 |
| `log_policy.hpp` | 策略 | 通道策略表 + 优先级策略表 |
| `log_debug.hpp` | DBG 注册 | `LogEntry` + `DebugSourceRegistry`（名字 / 选中 / 选择代数） |
| `log_record.hpp` | 记录与格式化 | 参数快照 / `FormatRecordToText` / `WrapOutputFrame` |
| `log_queue.hpp` | 队列 | `LogRecordQueue` + `TxFrameQueue` |
| `log_transport.hpp` | 发送泵 | DMA 空闲续发 / 发送完成回调 |
| `log.cpp` | 实现 | 门面入口 + `log` 命令 |

### 6.3 三段时间模型：boot 早期直发，线程接管后异步

| 时段 | `shell_own_` | 行为 |
|---|---|---|
| boot 早期 | `false` | 调用点 `vsnprintf` + **入帧池 + 立即提交 DMA**（不依赖线程） |
| shell 接管后 | `true` | 调用点只做**参数快照入队**，shell 线程格式化 + 驱动 DMA |

**为什么必须分两段**：shell 线程在 `PreThread` 阶段才启动。boot 早期若只入队，无人消费，帧池（8 帧）很快塞满，Early/Mid/Late 阶段的日志**全被吞**。

**收益（实测）**：异步段调用点耗时从 ~96µs 降到 ~9µs（省 87µs，**~10x**）。

### 6.4 双链表：两级队列 + 三档优先级

```text
调用点（任务或 ISR）
  │  BuildRecordFromVaList（扫 fmt + va_arg，~µs 级）
  ▼
LogRecordQueue        静态池 8 条 × 26B，FIFO，池满直接丢
  │  shell 线程 FormatRecordToText 展开
  ▼
TxFrameQueue          静态池 8 帧 × 128B，三档优先级插队
  │  SendNextFrameIfIdle
  ▼
DMA
```

**为什么两级**：

- 帧池容量留给"**已格式化待发**"的帧
- 未格式化的请求单独排队，**不占帧池**
- 异步段的调用点开销被压到极限（只存指针 + 参数值）

**三档优先级**（`log_policy.hpp` 的表）：

| 档位 | 谁 | 池满时 |
|---|---|---|
| `Event` | INF/ERR/OK/WRN | **永不挤** |
| `Cmd` | 命令响应 | 可被挤 |
| `Dbg` | DBG 流式 | **优先被挤** |

同档位保持 FIFO。池满时：**线程上下文会挤出最低优先级帧**（`EvictLowestPriorityFrameLocked`），**ISR 上下文直接丢**（不遍历链表，保证 ISR 安全）。

**全链路零动态分配**：所有池都是静态数组 + `uint8_t` 索引链表，链表的"空值"是 `255`。

### 6.5 策略表：改行为不改代码

```cpp
inline constexpr LogChannelPolicy kLogChannelPolicies[] = {
    { LogChannel::Info,    TxPriority::Event, LogColor::Black,  "[inf] ", true,  false, false, false },
    { LogChannel::Error,   TxPriority::Event, LogColor::Red,    "[err] ", true,  false, false, false },
    { LogChannel::Ok,      TxPriority::Event, LogColor::Green,  "[ok] ",  true,  false, false, false },
    { LogChannel::Warning, TxPriority::Event, LogColor::Orange, "[wrn] ", true,  false, false, false },
    { LogChannel::Debug,   TxPriority::Dbg,   LogColor::Mint,   "",       true,  true,  true,  true  },
    { LogChannel::Command, TxPriority::Cmd,   LogColor::Black,  "",       false, false, false, false },
};
```

新增一个通道：**加一行表项**，不碰任何逻辑代码。

### 6.6 选择代数（epoch）：切换时不遍历队列

DBG 是"选一条流式打印"。切换选中时，队列里可能还残留旧条目的帧。

**常规做法**：遍历整个队列，给旧条目打作废标记——O(n) 且要处理并发。

**这里的做法**：注册表每次 `Select` / `Deselect` 递增 `epoch_`。记录和帧携带创建时的 epoch，**出队时比对**：

```cpp
if (frames[i].prio == TxPriority::Dbg && frames[i].epoch != active_epoch)
{
    ReleaseFrameLocked(&frames[i]);    // 作废帧：回收不发送
    continue;
}
```

**O(1) 作废，零遍历**。代价是最多残留一帧"已在 DMA 中"的旧帧（无法取消，可接受）。

### 6.7 DBG 输出模式：按名字独立切换

```text
log mode <name> on      纯文本（VOFA+ FireWater 能画曲线）
log mode <name> off     上色（终端可读）
```

**模式存在条目标志里**，入队时快照进记录。所以：

- 同一时间只有一个条目被"选中"（打不打）
- 但每条各自记着自己的模式（打成什么形态）
- 两者**独立**

**为什么 VOFA+ 模式必须关 ANSI**：ANSI 转义串 `\x1b[38;2;R;G;Bm` 里的数字（`38/2/123/247/205`）会被 VOFA+ 当成通道数据，曲线全乱。

### 6.8 容量模型

```text
B_max = K + 1        同一时间允许的最大日志调用数（1 DMA 中 + K 排队）
K_min = max(1, B-1)  给定突发需求 B 所需最少帧数
不丢帧 ⇔ R_p < baud/10 且 B ≤ K+1
```

| 项 | 实测值 |
|---|---|
| 单帧周期（30/64/127B） | 371 / 743 / 1430 µs |
| 固定开销/帧 | ~40 µs |
| 瞬时容量（8 帧池） | 最多 9 条同时发送，第 10 条起丢 |
| 无限闭环 | 稳态 < 92160 B/s 时零丢帧 |

---

## 7. shell 命令表

### 7.1 词法解析 + 查表匹配

```cpp
// 各命令族在自己的文件里注册
REGISTER_SHELL_CMD("thread", "thread list|show|pause|resume|exit|reset",
                   ThreadCmd::Process);

// shell.cpp 只负责遍历
void Shell::ProcessLine(uint8_t* line)
{
    const char* cmd = nullptr;
    uint8_t* rest   = NextToken(line, cmd);
    if (*cmd == '\0') return;

    for (const ShellCmdEntry* e = ShellCmdBegin(); e < ShellCmdEnd(); ++e)
    {
        if (std::strcmp(e->name, cmd) == 0) { e->handler(rest); return; }
    }
    Log::SendCommandLine("unknown command");
}
```

**新增命令族不需要改 `shell.cpp`**。帮助列表 `h` 也是遍历生成的——**不会漏**。

### 7.2 为什么用链接段而不是中央表

中央表的问题：**"有哪些命令"分散在实现和表两处**。漏加一行，编译不报错，运行时才发现命令不存在。

链接段的性质：**实现和注册是同一行代码**，没有遗忘的可能。

### 7.3 变量名由宏自己生成

```cpp
REGISTER_SHELL_CMD(name_, help_, handler_)
```

变量名用 `_CONCAT(kShellCmd_, __COUNTER__)`（`<zephyr/toolchain.h>`，与 `REGISTER_SHELL_VAR` 同款）：`__COUNTER__` 保证唯一，调用方不必再传一个标识符。

`handler_` 可能是 `ThreadCmd::Process` 这种带 `::` 的限定名，不能直接拼进标识符（`kShellCmd_ThreadCmd::Process` 非法），所以用 `__COUNTER__` 而不是拼 handler 名。


### 7.4 段边界必须声明为 C 符号

```cpp
// shell_cmd.hpp
extern "C" {
extern const ::debug::ShellCmdEntry __shell_cmd_start[];
extern const ::debug::ShellCmdEntry __shell_cmd_end[];
}
```

**写进 `namespace debug` 会报 `undefined reference to debug::__shell_cmd_start`**——C++ 名字修饰让符号变成 `_ZN5debug18__shell_cmd_startE`，而链接脚本导出的是 C 符号。

### 7.5 `NextToken`：把重复的切分代码收成一处

```cpp
inline uint8_t* NextToken(uint8_t* line, const char*& out)
{
    line = SkipSpace(line);
    out  = reinterpret_cast<const char*>(line);

    while (*line && *line != ' ') ++line;
    if (*line == ' ') { *line = '\0'; ++line; }

    return SkipSpace(line);
}
```

原地把 token 末尾改成 `'\0'`，返回下一个 token 起点。

**替换掉了此前 5 个文件里各自手抄的 5 行空白跳过代码**——语义统一，不会再出现"某处允许多个空格、某处不允"。

### 7.6 五个命令族

| 命令族 | 数据来源 | 作用 |
|---|---|---|
| `var` | `.shell_var` 段 | 调试变量 list/get/set（支持 12 种类型） |
| `log` | `DebugSourceRegistry` | 日志条目选中 + 输出模式 |
| `thread` | `thread_registry` | 运行统计 / 暂停 / 恢复 / 清零 |
| `boot` | `.boot_timing` 段 | 启动期各项耗时 |
| `h` | `.shell_cmd` 段 | 帮助（自动生成） |

---

## 8. 驱动层：Stream 抽象与双缓冲 DMA

### 8.1 `Stream`：调试通道可整体替换

```cpp
class Stream
{
public:
    using RxCallback = void (*)(uint8_t* data, uint16_t len);
    using TxCallback = void (*)();

    virtual uint16_t Read(uint8_t* buf, uint16_t max_len) = 0;
    virtual bool     Send(const uint8_t* data, uint32_t len) = 0;

    k_sem sem_ {};      // 通道事件信号量（接收数据 / 日志发送需要驱动）
};
```

shell 和 log 都持 `Stream*` 而非 `UartDma*`。`UartDma` / `Usb` / `Rs485` 都是子类——**调试通道可以从 UART 换成 USB，上层一行不改**。

`sem_` 的名字刻意叫"通道事件信号量"而不是"接收信号量"：它同时承载**接收数据到达**和**日志发送需要驱动**两种事件。

### 8.2 UART 双缓冲 DMA

```cpp
uint8_t  dma_buf_[2][kMaxBufSize] {};       // 硬件 DMA 双缓冲
bool     buf_free_[2] = {true, true};       // DMA buffer 空闲跟踪
BipBuffer<kMaxBufSize * 2> rx_bip_ {};      // 软件缓冲队列
```

**双缓冲轮转**：DMA 填满一块时，回调把数据拷进 `BipBuffer` 并归还该块，同时告诉硬件"下一块用这个"。

```cpp
auto on_buf_request = [self, dev]()
{
    int next = self->buf_free_[0] ? 0 : (self->buf_free_[1] ? 1 : -1);
    if (next >= 0) {
        self->buf_free_[next] = false;
        uart_rx_buf_rsp(dev, self->dma_buf_[next], UartDma::kMaxBufSize);
    }
};
```

**为什么用 `BipBuffer` 而不是普通环形缓冲**：BipBuffer（双区）保证**任何可读数据块在物理内存中连续**，解析器不需要处理回绕。`alignas(64)` 对齐利于 DMA 缓存一致性。

### 8.3 发送：契约是"返回前已完成拷贝"

```cpp
bool UartDma::Send(const uint8_t* data, uint32_t len)
{
    if (atomic_get(&tx_busy_)) return false;
    memcpy(tx_buf_, data, len);          // ← 先拷贝
    atomic_set(&tx_busy_, 1);
    uart_tx(dev_, tx_buf_, len, 0);      // ← 再提交
    return true;
}
```

**这个契约支撑了日志系统的帧池设计**：`Send()` 返回后调用方可以立刻回收帧（帧即取即还），不需要等 DMA 完成。

`tx_busy_` 忙时直接返回 `false`——**不排队、不阻塞**，由日志层的帧池负责缓冲。

### 8.4 CAN 与 SPI 的定位

| 外设 | 设计 | 原因 |
|---|---|---|
| `Can` | `K_NO_WAIT` 发送，硬件 32 个 TX 邮箱 | 经典 CAN 帧 ≤ 8B，硬件就是队列，不需要 DMA |
| `Spi` | 同步阻塞 `spi_transceive_dt()` | 小事务（IMU 14B）同步够用；DMA 是底层开关，框架层零改动 |

---

## 9. 模块层：IMU 前后端隔离

### 9.1 结构

```text
后端（芯片驱动）                    前端（管理器）
  bmi088.cpp                        ImuManager
  icm42688p.cpp                       │
  实现 Source 接口                    ├── InitSource()  遍历 .imu 段
  自己做寄存器读、单位换算             ├── heater_.Update()
  自己 REGISTER_IMU                  ├── attitude_.Process()（四元数 EKF）
                                     └── zbus 发布 imu_to
```

### 9.2 前端彻底不知道后端是谁

```cpp
class Source
{
    virtual bool Init() = 0;
    virtual bool LateInit() = 0;
    virtual bool Read(Sample& sample) = 0;
    virtual bool Calibrate() { return true; }
};
```

`Sample` 是**统一工程量**（`gyro[3]` / `accel[3]` / `temp` / `dt`），单位换算和驱动侧校准由后端完成。

**`InitSource()` 只做三件事**：

```cpp
ptrdiff_t count = __imu_end - __imu_start;

if (count != 1) {                    // ① 恰好一个
    DUST_LOG_ERR("imu source count=%d", count);   // 0 个或 2+ 个都是配置错误
    return false;
}
source_ = __imu_start->source;       // ② 拿到唯一那个
return source_->Init();              // ③ 初始化
```

**换芯片 = 换一行 Kconfig，业务代码零改动**：

```kconfig
CONFIG_DUST_MOD_DEV_IMU_BMI088=y       # 或
CONFIG_DUST_MOD_DEV_IMU_ICM42688P=y
```

### 9.3 为什么用链接段而不是 Kconfig 直接选择

**链接段在这里提供的是"自动发现"能力**：前端不需要 include 任何具体芯片的头文件，也不需要在 `#ifdef` 里列一遍所有芯片型号。

新增一款 IMU：

1. 写一个类实现 `Source`
2. 文件末尾 `REGISTER_IMU(NewChip, newchip);`
3. CMake 加一个 `if(CONFIG_DUST_MOD_DEV_IMU_NEWCHIP)`

**前端一行不改。**

### 9.4 校准参数持久化

后端在 `LateInit()` 里从 flash 读校准数据：

```cpp
ImuOffsetData calib{};
if (!EXEC_FLASH_READ(flash::kPartCalib.offset, &calib, sizeof(calib))) {
    DUST_LOG_ERR("read calib from flash failed");
}
```

`EXEC_FLASH_READ` 是横切宏——**没有 flash 的板卡上它展开成 `false`**，代码路径自然退化到"用寄存器默认值"。

---

## 10. 模块层：Remote 协议自动探测

**这是整个架构里状态机最复杂的一块，但它的复杂全部锁在冷路径里。**

### 10.1 冷路径复杂，热路径直连

这是 Remote 最核心的设计取舍：

| 路径 | 频率 | 做什么 | 开销 |
|---|---|---|---|
| **冷路径**（Detecting） | 只在开机 / 掉线重连时 | 逐协议试探、切 UART 参数、累计命中 | 复杂但很少走 |
| **热路径**（Locked） | 每帧都走（100Hz+） | `Decode()` → 发布 | **一条虚函数调用，直连** |

**锁定之后，探测逻辑完全不参与**：

```cpp
void Remote::Dispatch()
{
    if (detect_.state == DetectState::Locked) {
        HandleLocked();          // ← 热路径：while (帧够) { Decode(); Consume(); }
    } else {
        HandleDetecting();       // ← 冷路径：试探、切换、累计
    }
}
```

`HandleLocked` 里没有任何协议比较、没有任何状态机分支：

```cpp
while (frame_.frame_pos_ >= entry->frame_size)
{
    if (entry->protocol->Decode(...)) { ... }    // 直连当前协议
    else { fail_count++; }

    if (fail_count >= kUnlockFailLimit) { ResetDetect(); break; }
}
```

**代价与收益**：

- 代价：进入锁定时多一次判断（`state == Locked`）——每帧一次，可忽略
- 收益：**锁定后每帧只做一次虚函数调用 + 一次 memmove**，与"直接用单协议写死"没有区别

### 10.2 只有一个协议时，自动退化成正常逻辑

这是这套设计的第二个精妙之处：**协议数从 1 变 N，代码路径不需要改**。

```text
注册 1 个协议（例如只编 SBUS）：
  __remote_start ──► __remote_end          （只有 sbus 一项）
        │
        ├─ 第一次 Detecting：probe.entry = __remote_start（就是 sbus）
        ├─ 校验通过 3 帧 → 锁定
        └─ 之后永远在 Locked 分支
     
  如果一直校验不过：
        └─ next = e + 1 ≥ __remote_end → probe 清零、帧缓冲清空 → 下一轮从头
           （还是同一个协议，等价于"单协议 + 无限重试"）
```

**没有任何"if (协议数 == 1)"的特判**——退化是**数据结构自然导致**的：

| 机制 | 单协议时的行为 | 多协议时的行为 |
|---|---|---|
| `probe.entry = __remote_start` | 指向唯一协议 | 指向第一个 |
| `probe.retry >= need_hits` 换下一个 | `next >= __remote_end` → 重置从头 | 切到下一个协议 |
| `SwitchProto()` | 切到自己（`Reconfigure` 同一套参数） | 切到不同参数 |
| `Locked` 后的解码 | 完全相同 | 完全相同 |

**结论**：单协议场景下，这套"通用探测"退化成"固定协议 + 持续重试"，**没有额外的分支开销，也没有额外的代码路径**。

### 10.3 用户不需要关心自己插的是哪种遥控器

```text
Detecting 状态：按顺序试探协议，命中 need_hits 帧后锁定
Locked 状态：解码；连续失败 kUnlockFailLimit 次 → 回 Detecting
失联超时：返回 TaskResult::Retry → 钩子清零发布数据
```

### 10.4 协议注册与编译选择的分工

```cpp
// 协议实现用链接段注册
REGISTER_REMOTE(SbusProtocol, kFrameSizeSBUS, remote::Priority::High, 3, sbus);
```

```cmake
# 编哪些协议由 Kconfig 决定
if(CONFIG_DUST_MOD_DEV_REMOTE_AUTO OR CONFIG_DUST_MOD_DEV_REMOTE_SBUS)
    target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/remotes/sbus/sbus.cpp)
endif()
```

**为什么分两层**：

| 层 | 决定 | 理由 |
|---|---|---|
| Kconfig | 哪些协议**进固件** | 协议实现有体积成本（DR16/SBUS/VT12/VT13 各有解析表），不该全编 |
| 链接段 | 哪些协议**可用** | 运行时自动探测，用户不该关心 |

`CONFIG_DUST_MOD_DEV_REMOTE_AUTO` 打开全部协议，适合"不知道会插什么"的场景；单开某个协议可以省 flash。

### 10.5 自动探测状态机（冷路径）

```cpp
void Remote::HandleDetecting()
{
    // ① 无候选 → 从第一个协议开始驻留
    if (detect_.probe.entry == nullptr) {
        detect_.probe.entry = __remote_start;
        if (detect_.probe.entry >= __remote_end) return;
        SwitchProto(detect_.probe.entry);
        return;
    }

    const auto *e = detect_.probe.entry;

    // ② 帧不够 → 继续接收
    if (frame_.frame_pos_ < e->frame_size) return;

    // ③ Validate
    if (e->protocol->Validate(frame_.frame_buf_, e->frame_size)) {
        detect_.probe.hits++;
        if (detect_.probe.hits >= e->need_hits) {
            DUST_LOG_INF("%s: hit %d/%d, lock", e->name, ...);
            detect_.locked = e;
            detect_.state  = DetectState::Locked;
        }
        Consume(e->frame_size);
        return;
    }

    // ④ Validate 失败 → 累计 retry
    detect_.probe.retry++;
    Consume(e->frame_size);

    // ⑤ retry 够了 → 换下一个协议
    if (detect_.probe.retry >= e->need_hits) {
        const auto *next = e + 1;
        if (next >= __remote_end) {
            detect_.probe = {};           // 全部试完 → 从头
            frame_.frame_pos_ = 0;
            return;
        }
        detect_.probe.entry = next;
        SwitchProto(next);
    }
}
```

**`need_hits = 3` 的意义**：连续 3 帧校验通过才认为锁定，避免误锁。**VB 遥控器的帧格式允许随机数据碰巧通过一次校验**，但连续 3 次通过的概率极低。

**注意 ⑤ 分支的两个出口**：

```cpp
if (next >= __remote_end) {
    detect_.probe = {};           // 全部协议试完：清零候选
    frame_.frame_pos_ = 0;        // 丢弃残帧
    return;                       // 下一轮从 __remote_start 重新开始
}
```

- 多协议：反复轮询所有协议，直到某个锁定
- **单协议**：`next` 立刻越界 → 清零 → 下一轮还是它 → 等价于"对同一协议无限重试"

这就是 §10.2 说的"自动退化"——**同一个分支，两种场景都正确**。

### 10.6 UART 自动重新初始化

不同协议的 UART 参数完全不同：

| 协议 | 波特率 | 校验 |
|---|---|---|
| DR16 | 100000 | 8E1 |
| SBUS | 100000 | 8E2（反相） |
| VT12 / VT13 | 115200 | 8N1 |

**切换协议时**：

```cpp
void Remote::SwitchProto(const RemoteEntry *e)
{
    DUST_LOG_INF("switch to %s", e->name);
    frame_.frame_pos_ = 0;                          // 丢弃旧数据
    uart_->Reconfigure(e->protocol->GetLineCfg());  // 原子：Stop → 改线路参数 → Start
}
```

**`Reconfigure` 是原子的**——停 DMA、改参数、重启，中间不会收到半帧。

### 10.7 解码失败与失联是两条路

| 场景 | 路径 | 结果 |
|---|---|---|
| 锁定中，`Decode` 连续失败 5 次 | `HandleLocked` → `fail_count` 累加 → `ResetDetect()` | 回到 Detecting，日志 `lost xxx, redetect` |
| 锁定中，**完全没数据**超过 100ms | `Task` 返回 `Retry` | 钩子清零发布数据，日志 `lost xxx` |

**两条路的日志不同**（`redetect` vs 无），一眼能区分是"协议不对了"还是"遥控器关机了"。

### 10.8 为双 UART 冗余预留的接口

```kconfig
config DUST_MOD_DEV_REMOTE_DUAL
    bool "Dual remote support"
    default n
    depends on DUST_MOD_DEV_REMOTE
```

**当前只有 Kconfig 符号，没有实现**。设计上已经为它留好位置：

- `Remote` 是普通类（不是单例），可以实例化两个
- 每个实例持有自己的 `UartDma*`、自己的 `detect_` 状态
- 发布通道 `pub_remote_to` 是共享的，主备切换只需要决定"谁有资格发布"

**升级路径**：新增一个 `RemoteDual` 壳，持有两个 `Remote`，主实例失败时切到备实例。协议层、UART 层都不需要改。

---

## 11. 横切能力：没有也能编

### 11.1 buzzer

```cpp
#define EXEC_BUZZER_SHORT()    buzzer::Instance().Short()
#define EXEC_BUZZER_LONG()     buzzer::Instance().Long()
#define EXEC_BUZZER_ERR(...)   buzzer::Instance().Err(__VA_ARGS__)
#else
#define EXEC_BUZZER_SHORT()
#define EXEC_BUZZER_LONG()
#define EXEC_BUZZER_ERR(...)    while(1) {}
#endif
```

启动器里的失败处理这么写：

```cpp
EXEC_BUZZER_LONG();
for (;;) { k_msleep(1000); }
```

**关闭 `CONFIG_DUST_CMD_BUZZER` 时，宏展开为空**——启动器代码一行不改，照样编得过，逻辑自然退化成"不响但停机"。

### 11.2 flash

```cpp
#define EXEC_FLASH_READ(addr, data, len)   w25q128::Instance().Read(addr, data, len)
#else
#define EXEC_FLASH_READ(addr, data, len)   ((void)(addr), (void)(data), (void)(len), false)
#endif
```

IMU 的校准参数读取：

```cpp
if (!EXEC_FLASH_READ(flash::kPartCalib.offset, &calib, sizeof(calib))) {
    DUST_LOG_ERR("read calib from flash failed");
}
```

没有 Flash 的板卡上，这段代码照常编译，只是**永远走"没有校准数据"的分支**。

### 11.3 fatal：用 DUST_LOG 上报，不走被关闭的 Zephyr LOG

```cpp
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
#if defined(CONFIG_CPU_CORTEX_M)
    DUST_LOG_ERR("[fatal] %s thread=%s pc=%lx xpsr=%lx", ...);
#elif defined(CONFIG_RISCV)
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    DUST_LOG_ERR("[fatal] %s thread=%s mcause=%lx mepc=%lx", ...);
#endif
    while (1) {}
}
```

Zephyr 默认实现走 `LOG_ERR` + `printk`，本项目 `CONFIG_LOG` / `CONSOLE` 都关着，输出直接被吞。覆盖后用 `DUST_LOG_ERR` 上报原因 + 线程名 + 现场寄存器。

**停机用 `while(1){}` 而不是复位**——保留现场，等人工接调试器。

### 11.4 设计价值

这三处是同一个模式：**横切能力只暴露宏，不暴露实现细节**。

- 有硬件时：宏展开成真实调用
- 没有硬件时：宏展开成空 / `false` / 死循环

**调用方不需要知道自己在哪个板子上。**

---

## 12. 数据契约层：topic

### 12.1 只定义数据，不实现逻辑

```cpp
// topic/imu_to/imu_to.hpp
namespace topic::imu_to {

struct Message
{
    float quaternion[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float gyro[3]       = {0.0f, 0.0f, 0.0f};
    float temperature   = 0.0f;
    float roll = 0.0f, pitch = 0.0f, yaw = 0.0f, yaw_total = 0.0f;
};

} // namespace topic::imu_to

ZBUS_CHAN_DECLARE(pub_imu_to);
ZBUS_OBS_DECLARE(sub_imu_to);
```

**生产者（`ImuManager`）和消费者（底盘 / 云台线程）都只依赖这个头文件**，互相不知道对方是谁。

### 12.2 三种通道形态

| 通道 | 机制 | 适用 |
|---|---|---|
| `pub_imu_to` | zbus | 一发多收，最新值覆盖 |
| `pub_remote_to` | zbus | 同上 |
| `user_can1_msgq` | msgq（深度 16） | 有队列语义的发送请求 |

**zbus vs msgq 的选择**：状态类数据（IMU 姿态、遥控器摇杆）用 zbus——消费者只关心最新值；命令类数据（CAN 发送请求）用 msgq——不能丢。

---

## 13. 业务线程：一个文件装下全部接线

### 13.1 完整内容

```cpp
// project/thread/imu/trd_imu.cpp —— 全部 33 行
#include "imu.hpp"
#include "Init_entry.hpp"

namespace thread::imu {

static ::imu::ImuManager imu_ {};

bool thread_init()  { return imu_.Init(); }
bool thread_start() { return imu_.Start(ThreadPrio::High); }

REGISTER_INIT  (thread_init,  EarlyInit,  High, HaltOnFail, nullptr, "imu_init");
REGISTER_THREAD(thread_start, EarlyThread, HaltOnFail, "imu_start");

}
```

### 13.2 这个文件回答了全部问题

| 问题 | 答案在这个文件里 |
|---|---|
| 这个线程依赖哪个模块 | `#include "imu.hpp"` + `ImuManager` |
| 它什么时候初始化 | `EarlyInit` |
| 初始化失败怎么办 | `HaltOnFail` |
| 它什么时候启动 | `EarlyThread` |
| 启动失败怎么办 | `HaltOnFail` |
| 它叫什么名字 | `"imu"` / `"imu_init"` / `"imu_start"` |
| 它的优先级 | `ThreadPrio::High` |
| 它的栈多大 | `Thread<2048>`（在模块内部） |

**不用翻三个文件找"谁初始化了它、谁启动了它、失败怎么处理"。**

### 13.3 为什么这样设计

业务线程文件是**调试时最常打开的文件**。

如果接线信息分散在：
- `main.cpp` 的初始化顺序里
- 某个中央注册表里
- 线程类自己的构造函数里

那么每次排查都要跨文件对照。**压缩到一个文件，等于把"这个线程的全部事实"变成一次阅读。**

---

## 14. 为什么 `test` 线程是点睛之笔

```cpp
// project/thread/test/trd_test.cpp
static Thread<2048> thread_ {};
static float t_ = 0.0f;

static TaskResult Task(void* context)
{
    (void)context;

    t_ += 0.01f;
    float ch0 = std::sinh(t_);
    float ch1 = std::cosh(t_) * 2.0f;

    DUST_LOG_DBG("vofa", "%f,%f", (double)ch0, (double)ch1);
    return TaskResult::Continue;
}

bool thread_start()
{
    thread_.Start(Task, K_MSEC(100), ThreadPrio::High, nullptr, "test");
    return true;
}
```

### 14.1 它同时验证五件事

| 验证项 | 怎么看 |
|---|---|
| 受管任务外壳能跑 | `thread list` 里 `test` 的 `RUN` 在涨 |
| 周期等待准确 | `thread show test` 的 `last` ≈ 0.01ms，`RUN` 每秒涨 10 |
| 日志异步链路通 | VOFA+ 能画出双通道曲线 |
| DBG 输出模式切换有效 | `log mode vofa on` 后变成纯文本，曲线正常 |
| 全套线程命令有真实数据 | `show` / `pause` / `resume` / `reset` 全部可验证 |

### 14.2 换板卡时第一个跑的是它

**它不依赖任何外设**——不碰 UART、不碰 SPI、不碰 CAN、不碰 GPIO。

只依赖：
- 调试台（shell + log）
- 线程模板

**如果 `test` 线程跑不起来，问题一定在架构层，不在业务层。**

这就是它的价值：**最小可验证单元**。新板卡第一次上电，`test` 一跑，就知道六模块是否装配成功。

### 14.3 它是最小可用的受管任务模板

新写一个周期线程，直接复制 `trd_test.cpp` 改三处：

1. `Task` 里的业务
2. `Start` 的周期和名字
3. 注册宏的阶段

**比看文档快。**

---

## 15. 目录结构

```text
zephyr_user/
├── framework/          架构层（6 个独立 git 子模块，只读）
│   ├── algorithm/      纯计算：PID / LQR / Kalman / EKF / 四元数 / ESO / RLS / 缓冲
│   ├── drivers/        通用外设封装：Stream / UartDma / Can / Spi / Usb / Pwm / Rs485
│   ├── modules/        设备能力：IMU（前后端） / Remote（协议探测） / 电机 / 功率计
│   ├── topic/          数据契约：zbus 通道 / msgq
│   ├── cmd/            横切 + shell + 构建脚本
│   └── init/           启动层 + 线程模板
├── drivers/            子树板卡底层（Zephyr 模块式：fsmc / USB HAL）
├── boards/             用户自研板卡（stm32f407igh6 / stm32g431cbtx）
├── dts/bindings/       自定义 devicetree binding
├── platform/cmsis/     CMSIS 兼容垫片
├── project/            维护者验证工程（gpio / imu / remote / test）
└── doc/                架构、规划、调研文档
```

### 15.1 用户区（`projects/<user>/`）

用户区是**独立的 git 仓库**，结构**镜像**架构的六层：

```text
projects/qingyu/
├── algorithm/    用户自己的算法层（可为空）
├── cmd/          用户自己的横切层（可为空）
├── drivers/      用户自己的驱动（可为空）
├── init/         用户自己的启动扩展（可为空）
├── modules/      用户自己的模块（可为空）
├── topic/        用户自己的数据契约（可为空）
└── project/      业务线程（必填）
```

**空目录会跳过**（CMake 用 `if(EXISTS)` 保护）。

**为什么镜像**：用户在需要时可以覆盖任何一层，不需要时直接留空——**不需要改架构层**。

**依赖方向是单向的**：用户区 → zephyr_user，禁止用户之间互相依赖。

---

## 16. 新增一个线程

### 四步

```text
① project/Kconfig              加 TRD_XXX，select 所需的 DUST_* 符号
② project/thread/CMakeLists    加 if(CONFIG_TRD_XXX) 块
③ project/thread/xxx/trd_xxx.cpp   写 Task + thread_init + thread_start + 两个注册宏
④ 板卡 .conf                   加 CONFIG_TRD_XXX=y
```

### 模板

```cpp
/**
 * @file trd_xxx.cpp
 * @author <你>
 * @brief XXX 线程 — 一句话说明
 * @version 0.1
 * @date 2026-10-20
 */

#include "thread.hpp"
#include "Init_entry.hpp"
#include "log.hpp"

namespace thread::xxx {

static Thread<2048> thread_ {};
static XxxDriver    xxx_ {};        // 跨拍状态：必须放在函数外

/**
 * @brief 任务单拍：做一次业务
 * @param context 未使用
 * @return Continue=正常；Retry=可恢复错误
 */
static TaskResult Task(void* context)
{
    (void)context;

    if (!xxx_.Read()) {
        return TaskResult::Retry;
    }
    return TaskResult::Continue;
}

/**
 * @brief Retry 钩子：可恢复错误后重新初始化设备
 * @param context 未使用
 * @return true=重进任务；false=放弃
 */
static bool OnRetry(void* context)
{
    (void)context;
    return xxx_.Reopen();
}

bool thread_init()  { return xxx_.Init(); }

bool thread_start()
{
    thread_.Start(Task, K_MSEC(10), ThreadPrio::High, nullptr, "xxx");
    return true;
}

REGISTER_INIT  (thread_init,  EarlyInit,  High, HaltOnFail, nullptr, "xxx_init");
REGISTER_THREAD(thread_start, EarlyThread, HaltOnFail, "xxx_start");

} // namespace thread::xxx
```

### 检查清单

```text
[ ] project/Kconfig 有 TRD_XXX，select 了依赖的 DUST_* 符号
[ ] project/thread/CMakeLists.txt 有 if(CONFIG_TRD_XXX) 块
[ ] trd_xxx.cpp 里有 REGISTER_INIT + REGISTER_THREAD
[ ] REGISTER_INIT 六个参数齐全（rollback 可为 nullptr）
[ ] REGISTER_THREAD 四个参数（fn / stage / fail / name）
[ ] Task 返回 TaskResult，没有 for(;;)
[ ] 跨拍状态在函数外（static 或 namespace 级对象）
[ ] 板卡 conf 里 CONFIG_TRD_XXX=y
```

**完整细节见 [doc/线程编写指南.md](doc/线程编写指南.md)**（编译期 / 链接期 / 启动期 / 运行期四层讲解）。

---

## 附：构建

```powershell
dust build hpm5361icb        # 自动找 project/boards/*/hpm5361icb/*.overlay
dust build puzhong
dust flash                   # 定位项目根后透传 west flash
```

直接用 `west`：

```powershell
west build -b hpm5361icb -- -DBOARD_CFG=hpm5361icb
west build -b stm32f407igh6 -- -DBOARD_CFG=puzhong
```

---

## 附：推荐阅读顺序

1. **本文** —— 先建立整体印象与设计意图
2. [doc/线程编写指南.md](doc/线程编写指南.md) —— 线程从注册到运行的完整链路
3. [快速上手.md](快速上手.md) —— 30 分钟跑起来
4. `framework/init/README.md` + `ARCHITECTURE.md` —— 启动器细节
5. `framework/cmd/shell/README.md` —— 调试台（log 分层 / var / thread / boot）
6. `framework/drivers/ARCHITECTURE.md` —— Stream / UART DMA / CAN / SPI
7. `framework/modules/ARCHITECTURE.md` —— IMU 前后端 / Remote 探测
8. [子树架构介绍.md](子树架构介绍.md) —— 逐层展开
