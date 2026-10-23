# 当前框架未收拢语义与跨 RTOS 架构思路调研

> 调研日期：2026-10-23
>
> 调研对象：`zephyr_user` 子树、`framework/`、`project/` 及其现有架构文档
>
> 结论性质：架构审视与后续收拢建议，不是本次实现计划

## 1. 先给结论

当前项目应准确描述为：

> **以 Zephyr 为基础、面向 MCU 电控的通用子树框架；其分层、边界、模块组织和故障处理思路，可以被其他 RTOS 工程直接借鉴或照抄，具体 API 和底层实现再按目标 RTOS 落地。**

这两个判断不能混在一起：

| 层面 | 当前状态 |
| --- | --- |
| 分层思想 | 有较清晰的方向：算法、驱动、模块、topic、cmd、init、project |
| 业务边界 | 已经有约束，但部分所有权和生命周期语义没有完全定死 |
| Zephyr 实现 | 已形成可工作的主路径，但仍有接口遗留、文档漂移和失败路径缺口 |
| 架构思路可迁移性 | 分层、依赖方向、模块组合、数据契约、线程归属和故障边界可直接作为其他 RTOS 工程的设计参考；当前实现基于 Zephyr |

因此，后续应避免把“架构思路可直接借鉴”和“现有代码可直接编译复用”混为一谈。更准确的说法是：

1. 其他 RTOS 可以直接借鉴本框架的领域拆分、依赖方向和模块边界。
2. 线程、启动、设备描述、构建裁剪和消息机制可以照着同一架构原则，在目标 RTOS 中重新实现。
3. 不要求把 Zephyr API 改造成一套“万能 RTOS API”，也不把代码级复用当作本框架目标。

## 2. 调研范围与判断方法

本次判断基于以下内容：

- `README.md`、`子树架构介绍.md`、`doc/线程编写指南.md`。
- `framework/init` 的启动器和 `Thread` 模板。
- `framework/drivers` 的 UART、SPI、CAN、GPIO、PWM、USB 封装。
- `framework/modules` 的 IMU、Remote、注册段和协议实现。
- `framework/topic` 的 zbus / `k_msgq` 通道。
- `project/Kconfig`、`project/CMakeLists.txt`、板级 overlay 和线程入口。
- Zephyr 官方对 DeviceTree、Kconfig、线程和 zbus 的定义，以及 CMSIS-RTOS2 对线程、消息队列和事件对象的抽象。

这里区分三种问题：

- **语义缺口**：代码能够运行，但调用方不知道边界、所有权或失败后的准确含义。
- **实现缺口**：设计已经写出来，当前代码没有闭合或仍有旧路径。
- **平台实现边界**：当前按 Zephyr 机制实现；换到另一个 RTOS 时，直接借鉴架构分层和语义，按其机制重写实现。

## 3. 当前最重要的未收拢语义

### 3.1 模块线程归属已经明确，需保持现有规则

这里不是当前缺口，而是需要明确记录并保持的设计规则：模块是否拥有线程，取决于模块的运行模型。IMU 和 Remote 都是主动数据源，内部线程属于它们自身的运行语义；项目线程只负责初始化、配置和启动装配。

当前实际关系是：

```text
project/thread/trd_imu.cpp
    -> ImuManager::Init()
    -> ImuManager::Start()
        -> ImuManager 内部 Thread

project/thread/trd_remote.cpp
    -> Remote::Init()
    -> Remote::Start()
        -> Remote 内部 Thread
```

当前框架已经采用混合模型：

| 模型 | 含义 | 影响 |
| --- | --- | --- |
| 项目拥有线程 | module 只提供 `Init/Process/Read`，线程对象在 `project/thread` | 适合项目组合线程和控制闭环 |
| 模块拥有执行体 | module 内部持有线程，但项目拥有启动阶段和配置 | 适合主动数据源和内部连续状态机 |
| 混合模型 | 主动数据源自带线程，被动设备由项目线程调度 | 当前框架采用的模型 |

正式规则已经在 `framework/modules/ARCHITECTURE.md` 中说明：IMU、Remote 持有模块线程；Motor、PowerMeter 等被动设备不强行线程化；Chassis、Gimbal 等多模块协同逻辑放在 `project/thread/`。

**证据：** `framework/modules/imu/drivers/imu.hpp`、`framework/modules/remotes/remote.hpp`、`project/thread/imu/trd_imu.cpp`、`project/thread/remote/trd_remote.cpp`。

### 3.2 线程退出语义是常驻 MCU 模型，不是缺少 Stop/Join

当前设计明确采用 MCU 常驻线程模型：

- `REGISTER_THREAD` 不带 `stop/join` 回调，因为 MCU 运行期没有进程退出语义。
- 线程正常情况下常驻；受管任务只有 `Fatal` 或恢复钩子返回 `false` 时退出。
- 对外提供的是 `Pause/Resume`，用于调试和运行控制，并非进程式生命周期管理。

因此，不能把“没有 Stop/Join”列为架构缺口。真正需要检查的是：源码实现、线程指南和 shell 统计是否始终保持同一套常驻线程语义。

此外，当前实现的 `TaskResult` 只有 `Continue` 和 `Retry`，但 README 和线程指南已经描述了 `Fatal`、`fatal_count` 以及裸任务 `Start(k_thread_entry_t, ...)`。这些内容在当前 `thread.hpp` 中没有对应实现，属于明确的文档-代码漂移。

**证据：** `framework/init/thread/thread.hpp:35-36,126-143,301-322`；`README.md:374-402`；`doc/线程编写指南.md:411-557`。

当前应保持的生命周期是：

```text
Created -> Started -> Running -> Paused -> Running
                           -> Fatal/Recovery-failed
```

`Stop/Join` 不属于当前 `REGISTER_THREAD` 契约，不能按通用操作系统线程模型强行补入。

### 3.3 周期线程是“固定周期”还是“执行后延时”

当前 `Thread::Run()` 在任务体返回后执行 `k_sleep(period_)`：

```text
Task 执行时间 + period
```

这不是严格的固定周期调度。任务执行耗时变化时，下一次启动时间会漂移。IMU 的 `dt` 通过真实 cycle 差值修正，说明代码已经意识到这个问题，但线程调度本身仍然是“完成后延时”。

需要明确两种语义：

| 语义 | 适用场景 |
| --- | --- |
| Delay-after-work | 普通轮询、低精度状态检查 |
| Deadline / fixed-rate | IMU、控制环、周期采样 |

建议受管线程配置显式区分 `periodic` 和 `delay`，或至少把当前接口命名为 `delay`，不要让调用方把它误解为固定频率。

### 3.4 初始化失败有回滚，线程启动失败没有对称回滚

`InitEntry` 有 `cleanup`，成功初始化项会进入逆序回滚表；但 `ThreadEntry` 没有 cleanup。

当前行为是：

```text
线程 A 已启动
线程 B 启动失败且 HaltOnFail
    -> startup_aborted_ = true
    -> 跳过后续阶段
    -> A 仍然运行
    -> main() 进入停机循环
```

这会留下一个“系统已经宣布启动失败，但部分业务线程仍在运行”的中间状态。对只读采样线程问题不大，对已经打开 PWM、CAN、DMA 或执行控制输出的线程则不安全。

需要在架构层继续收拢的是启动失败后的安全态，而不是引入 `Stop/Join`：

- 已启动的常驻线程在系统中止后是否允许继续运行。
- 控制输出线程是否必须立即进入安全态。
- `cleanup` 继续只承担初始化失败回滚，还是增加专门的故障安全动作。
- 失败停机前是否允许保留 shell / log 线程。

**证据：** `framework/init/startup/Init_entry.cpp:196-232`；`framework/init/startup/Init_entry.hpp:73-96`。

### 3.5 阶段、等级和链接顺序的关系仍然不够严格

当前 Init 有 `Stage + Level`，Thread 只有 `Stage`。同一阶段内的线程顺序依赖链接段排列，而链接段顺序不是业务契约。

这适合“这些线程都可以并行启动”的场景，不适合表达：

```text
A 必须先启动
B 才能启动
C 必须等待设备校准完成
```

建议把依赖分成两类：

- **初始化依赖**：用显式阶段、依赖 token 或 ready barrier 表达。
- **运行期数据依赖**：用 topic 的有效性 / 状态字段表达，不靠启动顺序猜测。

不要继续依赖 CMake 源文件顺序、目标文件顺序或链接器偶然排列。

### 3.6 topic 只有数据结构，没有完整的数据契约

当前 `imu_to::Message` 没有：

- 时间戳或采样序号。
- 数据有效性标志。
- 校准状态 / 传感器健康状态。
- 数据来源或协议版本。
- 单位、坐标系和刷新周期的机器可检查字段。

`remote_to::Message` 有 `version`，但没有明确它代表“每次有效帧”“每次发布”还是“连接状态变化”。

同时，`ImuManager::Task()` 中 zbus 发布调用当前被注释掉；`Remote::Task()` 则在每次读到 UART 数据块后发布，即使该数据块还没有形成完整有效帧，也可能重复发布旧的 `pub_`。

因此 topic 目前更像“共享结构体 + 传输调用”，还不是完整的数据契约。至少应定义：

```text
Message = payload + sequence + timestamp + validity + health + source/version
```

并明确每类通道是：

- latest-value：允许覆盖和丢中间值。
- event/command：不能丢，必须排队或返回背压。
- sampled-stream：允许丢帧，但必须能检测丢帧和时序异常。

### 3.7 zbus / msgq 的选型语义需要继续写成架构判断

当前 topic 层直接暴露：

- `#include <zephyr/zbus/zbus.h>`。
- `ZBUS_CHAN_DECLARE` / `ZBUS_CHAN_DEFINE`。
- `K_MSGQ_DEFINE`。
- `struct k_msgq`。

这在 Zephyr 内是合理的，但它意味着 `framework/topic` 是 Zephyr 实现，而不是 RTOS 中立层。这里值得迁移的是“如何选择通道”的架构判断，不是把 zbus / msgq 包装成一套跨 RTOS API：

| 抽象 | 必须定义的语义 |
| --- | --- |
| LatestValue | 发布是否覆盖、读是否阻塞、是否有序、是否可读到旧值 |
| EventQueue | 满时丢弃/覆盖/阻塞/报错，发送者是否可在 ISR 调用 |
| Observer | 回调在哪个上下文执行，是否允许阻塞，是否保证不丢通知 |

不能把 `zbus_chan_pub()` 简单替换成另一个 RTOS 的函数名，然后宣称架构已经跨平台；不同 RTOS 需要重新实现这些语义。

### 3.8 ISR、回调和线程上下文没有统一写进接口契约

UART、CAN、USB 的回调可能运行在 ISR 或驱动回调上下文。当前文档有零散约束，但公共接口没有标注：

- 是否允许在 ISR 调用。
- 是否允许阻塞。
- 是否允许日志。
- buffer 的所有权和有效期。
- 回调执行完成前是否允许重新配置设备。
- 回调是否只负责唤醒线程，还是允许执行协议处理。

例如 `Stream::sem_` 只表示“可能有数据”，并不表达帧边界；`Can::Send()` 返回的是提交结果，不是总线发送完成。此类差异必须成为 API 契约，而不能只留在实现注释里。

### 3.9 静态注册段是 Zephyr 实现，但声明式注册是可借鉴思路

当前使用：

```text
__attribute__((used, __section__(...)))
KEEP(*(.user_init))
__user_init_start / __user_init_end
```

它依赖 GCC/Clang 属性、GNU ld 或兼容链接器、Zephyr 链接脚本接入方式和 ELF section 边界符号。

这不是问题，但要准确归类：

- “声明式注册表”是可迁移的架构思想。
- “链接段收集并通过边界符号遍历”是当前 Zephyr/GNU 工具链实现。

其他 RTOS 适配可以使用：

- 编译期生成表。
- 显式 `Register()` 调用。
- 静态对象构造后注册。
- 各 RTOS 的组件注册机制。

公共层不应把 `.user_init`、`.remote`、`.imu` 当成跨平台 API。

### 3.10 Kconfig、CMake、DeviceTree 是三种不同的配置语义

当前配置链清晰，但全部是 Zephyr 工具链语义：

```text
Kconfig -> CMake if(CONFIG_...) -> DeviceTree / DT_ALIAS -> C++ Init
```

需要补充一个重要边界：

- Kconfig 决定“能力是否编译”。
- DeviceTree 决定“硬件实例和连接关系”。
- C++ Init 决定“运行时是否 ready、如何失败和如何恢复”。

当前仍存在把板级实例写进业务线程的情况，例如 `trd_gpio.cpp` 直接固定 `gpioa + pin 18`。这会让 `project/thread` 同时承担业务语义和板级硬件实例选择，削弱通用 MCU 子树的复用性。

### 3.11 资源生命周期和多实例语义不完整

当前多数驱动和模块采用静态对象、单通道、单实例假设。这个选择适合 MCU，但需要显式回答：

- 一个 UART 是否允许多个协议对象共享。
- 一个 CAN 控制器是否允许多个模块注册过滤器。
- 一个 topic 是否允许多个生产者。
- 一个 Remote 是否允许多个 UART 实例。
- `Reconfigure()` 是否能在接收回调、发送在途或消费者阻塞时调用。
- `Init()` 失败后能否再次调用。
- `Stop()` 后能否重新 `Init()`。

目前 `DUST_MOD_DEV_REMOTE_DUAL` 已存在，但双实例的资源隔离、topic 隔离和命令可见性还没有闭合成契约。

## 4. 已确认的实现和文档缺口

这些不是推测，而是当前仓库可以直接定位到的具体问题：

| 优先级 | 缺口 | 位置 |
| --- | --- | --- |
| P0 | IMU 的 `zbus_chan_pub()` 被注释，IMU topic 开关和实际发布路径不一致 | `framework/modules/imu/drivers/imu.cpp:116` |
| P0 | `Thread` 源码、线程指南和 shell 统计需要继续保持 `Fatal`、裸任务、恢复失败等常驻线程语义一致 | `framework/init/thread/thread.hpp`、`README.md`、`doc/线程编写指南.md` |
| P0 | 线程启动失败后的已启动线程和控制输出安全态没有统一收拢 | `framework/init/startup/Init_entry.cpp:196-232` |
| P1 | `Thread::Start()` 可重复调用，没有生命周期状态保护 | `framework/init/thread/thread.hpp:126-143` |
| P1 | `RegisterThreadInstance()` 的容量失败被忽略，且注册表容量硬编码为 16 | `framework/init/thread/thread_registry.hpp:27-50` |
| P1 | 周期实际是“任务完成后延时”，文档和调用方容易理解为固定周期 | `framework/init/thread/thread.hpp:316-322` |
| P1 | `COM_UART` 有 Kconfig 声明但没有独立 CMake 源文件映射 | `framework/drivers/Kconfig`、`framework/drivers/CMakeLists.txt` |
| P1 | RS485 仍引用旧的 `RxStream`，当前公共接口是 `Stream` | `framework/drivers/communication/stream/rs485/rs485.hpp` |
| P1 | Remote 在每个 UART 数据块后发布，发布条件不是“有效帧已更新” | `framework/modules/remotes/remote.cpp:24-43` |
| P2 | `project/Kconfig` 当前 `TRD_REMOTE` 默认关闭，部分架构文档仍按默认开启描述 | `project/Kconfig`、`README.md`、`子树架构介绍.md` |
| P2 | `System_Startup()` 固定每阶段休眠 500ms，阶段延迟与硬件稳定需求没有配置化 | `framework/init/startup/Init_entry.cpp:248-276` |

## 5. 哪些架构思路可以迁移到其他 RTOS

### 5.1 可作为平台无关设计参考

这些内容体现的是领域建模和模块边界，换到其他 RTOS 时可以保留设计思路；不代表现有源码可以直接编译或直接接入：

- PID、滤波器、四元数、EKF、RLS、缓冲区算法。
- Remote 协议校验、解码、探测状态机。
- IMU 寄存器协议和工程量转换逻辑。
- 电机协议打包、反馈帧解析和状态换算。
- 设备模块内部的状态机和错误分类。
- topic 消息结构本身，但需要补齐时间戳、有效性和版本契约。

### 5.2 必须按目标 RTOS 重新设计的部分

以下内容属于 Zephyr 运行时和工具链实现，不应被包装成“拿来即用”的通用核心：

```text
线程创建 / 优先级 / 挂起恢复 / 退出
时间获取 / 延时 / 周期调度
信号量 / 队列 / 互斥量 / 事件通知
发布订阅 / latest-value / event queue
日志输出和 shell 线程
启动阶段和失败回滚
静态注册表和链接脚本
设备树、Kconfig、CMake 裁剪
GPIO / UART / SPI / CAN / PWM / USB HAL
```

它们在本项目中可以继续使用 Zephyr 原生机制；如果未来参考这套思路设计 FreeRTOS 或其他 RTOS 工程，应在各自工程中重新定义对应实现，而不是先承诺一套共同 API。是否建立 `runtime/` 或 `port/` 目录，属于未来具体工程的实现选择，不是本框架当前必须新增的结构。

### 5.3 不能把 CMSIS-RTOS2 当成完整的跨平台解决方案

CMSIS-RTOS2 能提供线程、线程标志、事件、信号量、互斥量、消息队列和定时器等 RTOS 原语，但它不替代：

- Zephyr 的 DeviceTree。
- Kconfig 和 CMake 配置裁剪。
- zbus 的 channel/observer 模型。
- Zephyr 的设备模型和异步驱动 API。
- GNU linker section 注册机制。
- 板卡级 HAL 和 DMA buffer 生命周期。

因此 CMSIS-RTOS2 只能作为另一个架构对照：它可以帮助验证“线程、同步、队列应当有哪些语义”，但不应被当作本框架的统一跨平台后端。

## 6. 建议的收拢路线

这里的建议按语义优先级排序，不要求一次性重写。

### P0：先定契约，再动实现

1. 固化常驻 MCU 线程契约：线程不提供 `Stop/Join`，支持 `Pause/Resume`，受管任务通过 `Fatal` 或恢复失败退出。
2. 保持 `TaskResult`、`fatal_count`、裸任务和 shell 统计的语义一致，不得继续保留文档和代码两套语义。
3. 定义周期模型：`delay-after-work` 还是 `fixed-rate`。
4. 定义 topic 类型：latest-value、event、sampled-stream，并写明丢失、阻塞、背压和上下文限制。
5. 保持既有模块线程规则：IMU/Remote 等主动数据源由模块持有线程，Motor/PowerMeter 等被动设备不强行线程化，项目组合线程放在 `project/thread`。
6. 定义启动失败后的安全状态：已启动线程、PWM、CAN、DMA、日志和 shell 分别如何处理。

### P1：把 Zephyr 当前主路径闭合

1. 修正 IMU 实际发布路径和 topic 配置的关系。
2. 统一 `Stream` / `RxStream`，处理 `COM_UART` 的真实定位。
3. 让 Remote 仅在有效帧、连接状态变化或明确的周期心跳时发布。
4. 增加 `Thread::Start()` 重入保护和线程终态可见性。
5. 处理线程注册表满、初始化表满、重复注册和重复启动。
6. 把板级 GPIO/SPI/UART 实例尽量移到 DeviceTree alias/spec，不在业务线程中固定控制器和 pin。
7. 对固定 500ms 阶段延迟给出配置来源，区分“硬件上电等待”和“启动阶段节奏提示”。
8. 同步更新 README、线程指南和源码，建立文档漂移检查。

### P2：明确架构思路与 Zephyr 实现的边界

如果未来在另一个 RTOS 上重新实现同类框架，可以参考下面的职责分法；这不是要求当前仓库立即改目录：

```text
framework/
├── algorithm/             平台无关
├── domain/                协议、设备状态、消息契约
├── runtime/
│   ├── interface/         可讨论的语义：Thread、Clock、Queue、LatestValue、Logger
│   ├── zephyr/            当前 Zephyr 工程的具体实现
│   └── other-rtos/        其他 RTOS 工程按自身机制重新实现
├── drivers/
│   ├── interface/         GPIO、UART、SPI、CAN、PWM 的调用契约
│   └── zephyr/            当前 Zephyr device API 封装
├── modules/               设备能力与协议
└── project/               产品组合线程
```

这里表达的是职责边界，不是要求所有 RTOS 使用同一个 API。每个 RTOS 工程可以有自己的线程模型、配置系统、注册机制和消息实现，但都可以参考同一套架构问题清单。

## 7. 架构思路是否真正可迁移的判断标准

不以“现有代码能否在其他 RTOS 编译”为验收目标，而以“换一个 RTOS 后，是否仍能重新建立同样的边界”为判断标准：

- 能识别算法、协议、设备能力、项目组合和 RTOS 机制分别属于哪一层。
- 换 RTOS 时，知道哪些部分必须重写，而不是把 Zephyr API 逐个替换。
- 线程契约能说明周期、Zephyr 优先级、Pause/Resume、常驻退出条件和错误传播。
- topic 契约能说明最新值、事件、采样流各自的丢失和背压语义。
- 所有 ISR/回调 API 都能说明上下文、阻塞限制和 buffer 所有权。
- 启动失败时不会遗留未定义的控制输出或后台业务线程。
- 不依赖链接段顺序表达业务依赖。
- 目标 RTOS 的配置、板级描述和构建系统可以独立组织。
- 文档中的 API、状态、统计字段与当前 Zephyr 源码一致。

## 8. 不建议做的事情

### 8.1 不要把所有 Zephyr API 包成同名“万能 RTOS HAL”

这种做法通常只会把 `k_sem`、`k_msgq`、`zbus` 的差异藏起来，最后在 ISR、超时、队列满、内存所有权和线程退出时重新暴露。这里要做的是提炼架构问题和契约，不是机械翻译函数名。

### 8.2 不要为了追求“跨 RTOS”删除 DeviceTree 的职责

DeviceTree 是 Zephyr 的硬件描述机制。其他 RTOS 可以使用 board config、静态表或 HAL 描述替代。可迁移的是“硬件实例不要散落在业务代码里”这一原则，不是要求其他 RTOS 也使用 DeviceTree。

### 8.3 不要先做完整多 RTOS 目录，再补语义

如果线程生命周期、topic 丢失规则和失败安全态没有定下来，先复制一套 FreeRTOS 代码只会制造两套不一致实现；当前调研不以复制实现为目标。

## 9. 外部依据

以下资料用于确认 Zephyr 和其他 RTOS 的边界，不代表本框架必须照搬对应实现：

- [Zephyr DeviceTree Guide](https://docs.zephyrproject.org/latest/build/dts/index.html)：DeviceTree 用于描述硬件和生成设备访问配置。
- [Zephyr Kconfig](https://docs.zephyrproject.org/latest/build/kconfig/index.html)：Kconfig 负责配置符号、依赖和裁剪。
- [Zephyr Threads](https://docs.zephyrproject.org/latest/kernel/services/threads/index.html)：线程优先级、挂起、恢复和线程生命周期属于 Zephyr 内核语义。
- [Zephyr zbus](https://docs.zephyrproject.org/latest/services/zbus/index.html)：channel、subscriber、observer 是 Zephyr 的消息总线机制。
- [CMSIS-RTOS2](https://arm-software.github.io/CMSIS_5/RTOS2/html/index.html)：作为另一种 RTOS 抽象的对照，说明线程、同步、消息队列和定时器的语义差异；不覆盖设备树、构建系统和硬件描述。
- [FreeRTOS Kernel Book](https://github.com/FreeRTOS/FreeRTOS-Kernel-Book)：任务、队列、通知、信号量和软件定时器具有与 Zephyr 不同的 API 和优先级语义。

## 10. 最终判断

这套框架当前最值得保留的不是某个 `REGISTER_*` 宏，也不是某个 zbus 封装，而是以下四个方向：

1. 业务线程不直接承载底层设备协议。
2. 设备能力和项目组合逻辑分开。
3. 跨线程数据通过明确契约传递。
4. 编译期装配、运行期观察和故障路径可追踪。

当前最需要收拢的也不是目录，而是契约：

```text
谁拥有线程
线程什么时候算失败
周期到底如何定义
消息允许不允许丢
回调在哪个上下文执行
启动失败后系统如何进入安全态
哪些是 Zephyr 实现，哪些是通用架构思想
```

这些问题解决后，这套设计才具备“换一个 RTOS 重新落地时仍有参考价值”的完整性；它仍然不是要求现有 Zephyr 代码直接变成其他 RTOS 的运行时后端。
