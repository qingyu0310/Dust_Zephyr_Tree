# MCU 与 Linux 架构选型调研

日期：2026-10-26。对象为当前 `framework/`、`temp/Dust_Vision_Architecture-main/` 与 `temp/dust-control-main/`。

本文比较架构组织、扩展机制与维护方式。依据为当前源码及设计文档，未进行编译、性能测试或硬件验证。文中的“源码事实”“架构判断”“迁移建议”分别表示已有实现、基于实现的评价和后续方案。

## 1. 结论：以当前 MCU 的架构设计为 Linux 演进主线

**建议继续沿用自己的架构体系：以当前 MCU 的设计为基线，更新已有 Linux 移植版；不把团队 dust-control 整体换成新的架构底座。**

理由是当前 MCU 已经形成相互配合的机制：能力按职责分层，功能从编译期裁剪，组件在本地注册，启动器统一执行，任务外壳统一承担循环与故障处理，运行状态由注册表和 shell 统一管理。它不仅规定“文件放在哪里”，还规定“组件怎样进入系统、怎样运行、怎样扩展、怎样被观察”。

团队工程有独立控制库、明确会话所有权和 Linux 调度等有用实现，但它整体更接近一台具体机器人的控制应用。`DustControl` 集中装配业务模块，`ChassisController` 集中协调动作，`src/chassis` 内部职责分区不足。已有控制功能、ROS 2 接入和回归测试，不能证明这套组织比当前 MCU 更适合成为通用底座。

**MCU 的设计思想可以直接借鉴到 Linux。** 注册扩展、任务外壳、模块运行权、项目装配权、数据契约和调试治理均不由 Zephyr 独占。Linux 使用自己的线程、等待、时钟与 I/O 实现这些机制即可；这不要求先建立一套跨所有操作系统的万能 HAL。

这里的选择包含三个候选，而不只是两个已有 Linux 目录：

| 候选 | 本文判断 |
|---|---|
| 更新后的 MCU 设计移植到 Linux | 推荐主线：保留已有架构的一致性，补回 Linux 版落下的机制 |
| 现有 Linux 移植版原样继续 | 有基础，但不能把当前版本当作 MCU 最新设计的完整代表 |
| 团队 dust-control 整体替代 | 不推荐作为通用底座；按模块或具体机制单独评估借鉴 |

这个判断针对可扩展框架的组织方式，不代表某套代码在任何机器人、任何负载下都性能更好，也不为具体算法正确性背书。

## 2. 先明确当前 MCU 架构的真实模型

### 2.1 它是一套装配与治理体系

当前代码的主要链路是：

```text
项目选择功能与板级资源
    │
    ├─ Kconfig：业务需要哪些框架能力
    ├─ CMake：哪些实现真正进入编译
    └─ overlay / 设备配置：使用哪些硬件资源
                 ↓
组件本地声明：初始化 / 任务 / 设备实现 / 协议 / 命令 / 变量
                 ↓
链接段收集，KEEP 保留，消费者遍历
                 ↓
init 分阶段初始化与启动
                 ↓
模块内部任务或项目业务任务进入 Thread 外壳
                 ↓
统一统计、重试、故障处置、注册表与 shell 管理
```

这条链可分别在 `project/Kconfig`、`project/thread/CMakeLists.txt`、`framework/cmd/linker/dust_init.ld`、`init/startup/Init_entry.cpp`、`init/thread/thread.hpp` 和 `shell/thread/thread_cmd.cpp` 中核对。不是所有注册表都存指针：初始化和线程表存指针，IMU、Remote、命令等表按各自条目布局收集；共同点是组件声明、消费者发现这一模式。

### 2.2 分层按责任，而不是按一台机器人的功能堆积

| 位置 | 承担的责任 | 代码例子 |
|---|---|---|
| `framework/algorithm` | 计算与状态更新，不负责具体设备接线与业务调度 | PID、Timer、滤波、观测器、辨识 |
| `framework/drivers` | 外设访问、字节流、缓冲与底层事件 | Stream、UartDma、Can、Spi |
| `framework/modules` | 完整设备能力及设备内部状态 | ImuManager、Remote、电机、PowerMeter |
| `framework/topic` | 数据结构与交换通道 | imu_to、remote_to、to_can_tx |
| `framework/init` | 启动描述、执行器、线程治理与公共分发 | InitEntry、ThreadEntry、Thread、CAN 分发 |
| `framework/shell`、`cmd` | 调试管理与辅助能力 | 命令、变量、日志、boot timing、蜂鸣、Flash |
| `project` | 项目选择、资源绑定、启动策略和多设备业务组合 | trd_imu、trd_remote、trd_gpio、trd_test |

当前 shell 已有独立目录，不能只照旧 README 的“六层”目录图描述现状。根目录 `drivers/` 是 Zephyr 模块式驱动，和 `framework/drivers/` 的 C++ 使用封装也不是一层。

### 2.3 模块拥有运行权，项目拥有装配权

这是当前设计的重要边界，不能把“模块内部有线程”误判为混乱。

- **IMU 是周期主动数据源。** `ImuManager` 持有 Source、Heater、Processor、采样状态和 Thread。采样、温控、真实 dt 与姿态处理属于同一条设备内部链。
- **Remote 是事件主动数据源。** Remote 持有 UART 引用、拼帧、探测、锁定、超时和 Thread。协议类负责 Validate/Decode，不分别创建线程。
- **电机和功率计是被动数据源。** CAN 回调更新状态，上层通过快照读取；没有为每台设备强行增加独立线程。
- **跨设备的项目控制属于 project。** 控制任务决定何时消费设备数据、怎样调用控制算法和提交输出。

实际例子：`project/thread/remote/trd_remote.cpp` 负责 UART 配置、设备树绑定、Remote 初始化、优先级和启动失败策略；Remote 内部负责协议状态机。这不是同一件事被写两遍，而是设备运行与项目接线分开。

设计说明见 `framework/modules/ARCHITECTURE.md`，实现见 `imu/drivers/imu.hpp/.cpp`、`remotes/remote.hpp/.cpp`、`motors/dji/dji_c6xx.hpp` 和 `powermeter/powermeter.hpp`。

## 3. 当前 MCU 的优势具体体现在哪里

### 3.1 功能选择、编译裁剪与运行接入形成可追踪链

**源码事实：** `project/Kconfig` 的 TRD_REMOTE 选择遥控模块与协议集合；`modules/Kconfig` 声明所需通信和 topic；对应 CMake 按 CONFIG 符号加入核心及协议实现。`trd_remote.cpp` 自己注册初始化和启动项，启动器不硬编码 Remote。

**架构判断：** 功能是否存在、实现是否编入、注册是否进入映像、启动是否成功，可以分别追踪。裁剪发生在能力和源文件层，新增模块不需要让运行入口维护一个所有业务类型的总表。

**Linux 价值：** 现有 Linux 工程已经有 Kconfig 求值、条件源文件和 ELF 注册段。这一设计已经落地了一部分，更新它无需放弃原有路线。Linux 的运行参数可以继续使用 YAML；“编哪些能力”与“实例使用什么数值参数”应分开。

边界：注册免除的是中央运行表修改，新增能力仍要接入 Kconfig/CMake，不能写成新增组件完全不改任何已有文件。

### 3.2 同一种扩展模式贯穿多个子系统

| 扩展点 | 组件侧声明 | 公共消费者 |
|---|---|---|
| 初始化 | REGISTER_INIT | RunInitStage |
| 任务启动 | REGISTER_THREAD | RunThreadStage |
| IMU 后端 | REGISTER_IMU | ImuManager::InitSource |
| 遥控协议 | REGISTER_REMOTE | Remote 探测与锁定流程 |
| CAN 接收处理 | CAN_RX_HANDLER | user_can*_rx_callback |
| 命令族 | REGISTER_SHELL_CMD | Shell::ProcessLine / PrintHelp |
| 调试变量 | REGISTER_SHELL_VAR | Var 查找、读取与设置 |

**架构判断：** 通用消费者依赖描述和接口，具体实现各自登记。新增能力不会自然堆积到一个越来越大的 switch 或中央创建函数里。命令帮助和命令注册也使用同一个来源，减少两份名单漂移。

这比“目录划分合理”更实质：它约束了新增功能的修改位置。它也说明当前架构不是几种孤立技巧的拼接，而是在启动、设备发现和调试管理中重复使用一套扩展原则。

边界：链接顺序不应承担隐含业务依赖。当前 IMU 明确要求单个映像恰好注册一个后端，不是多 IMU 自动择优框架；Remote 的 Priority 字段也不能仅凭存在就认定探测过程已经按优先级排序。

### 3.3 单拍业务与公共执行机制分开

当前 `Thread::Run()` 承担循环、计数、计时、结果处理和两拍之间的等待。Task 返回 Continue、Retry 或 Exit，业务无需重复写公共循环。

```cpp
static TaskResult Task(void* context)
{
    // 做一拍业务；发生可处理问题时返回 Retry。
    return TaskResult::Continue;
}
```

周期式 IMU 与事件式 Remote 均已使用同一外壳。IMU 通过 kPeriod 配置间隔；Remote 传 K_NO_WAIT，在 Task 内通过有界信号量等待决定节奏。**受管任务并不等于所有任务都必须周期轮询。**

团队各 Runtime 和现有 Linux 视觉任务多自行维护完整循环。它们同样可以完成业务，但新增任务时，循环、状态、停止、异常和统计更容易分别演化。

当前 MCU 的优势是：公共执行机制改一次，受管任务共同获益；业务单拍更易阅读，统计边界也已有统一位置。

边界：当前 period 在 Task 执行后用于 k_sleep，表示间隔，不是绝对发布周期。把设计移植到 Linux 时可以保留这个模式，也可为固定频率控制另实现绝对周期模式；不能把相对等待误写成硬实时周期保证。

### 3.4 错误分类、恢复与影响范围分开

当前调用链为：

```text
启动返回 false
  → StartFailPolicy：继续还是中止启动

Task 返回 Retry
  → RetryHook：业务尝试就地处理
  → 未处理：DefaultHandle 按线程故障策略兜底

Task 返回 Exit
  → 跳过 RetryHook，交给 DefaultHandle

Thread::Exit
  → ExitHook → 注销 → 终止
```

ThreadFaultConfig 在任务启动处同时声明 retry、exit、policy 与 keep_alive。Isolate 表示隔离退出；Recover 在恢复无法处理时进入整体故障处置，不是“自动重启线程”的同义词。

**架构判断：** 任务结果、设备特有的恢复、故障影响范围和最终收尾分别有归属。团队的整体 finished/error 聚合有价值，但不等同于这套逐任务声明和公共执行机制。

MCU 的常驻模型没有正常进程退出需求。REGISTER_THREAD 不带 Stop/Join 是明确设计，不是架构缺陷。Linux 有进程退出需求，可保留现有停止通知和 Join；两种生命周期可以遵循同一任务治理思想。

### 3.5 调试是横向公共能力，不是每个业务临时拼装

当前已连通两条路径：

```text
ThreadStats → ThreadBase → thread_registry → thread list/show/pause/resume/reset

组件本地变量/命令注册 → shell 发现与分派 → 统一响应输出
```

`project/thread/test/trd_test.cpp` 同时接入 TaskResult、RetryHook、ExitHook、变量注册和启动注册，形成一个小型可操作验证入口。它说明这些机制确实能配合，而不只是分别定义了接口。

日志系统也有自己的结构：Log 对外入口、process 展开与调试选择、record/frame 队列和策略、transport 发送。shell 接管后，事件日志在调用点快照参数，格式化放到 shell；接管前则在调用点格式化并提交发送。

源码中的三档发送优先级区分事件、命令响应和 DBG；epoch 用于淘汰切换前的调试记录；队列与帧池固定容量。设计目的不仅是“能打印”，还包括减少业务格式化工作、限制缓冲占用、区分诊断与流式数据。

这套机制比“每个模块提供一个状态输出，再接入 ROS”更接近统一框架治理。队列容量有限，不能据此宣称日志绝不丢失或对时序完全无影响。

依据：`shell/log/process/log_process.cpp::Emit/Debug/ExpandOneRecord`、`queue/log_queue.hpp`、`queue/log_policy.hpp`、`transport/log_transport.hpp`。

### 3.6 接口契约与所有权支撑底层替换

Shell::Init 接受 Stream 引用；日志 transport 绑定 Stream。当前 shell 默认装配 UartDma，但命令处理与日志队列不以具体 UART 设备为入口。

`UartDma::Send()` 先复制到驱动发送缓冲，再提交 uart_tx。LogTransport 提交帧后立即归还帧池，正是依赖这个载荷移交契约。替换传输实现时，需要保持契约，不能只实现相同函数签名。

**架构判断：** 设备访问与上层消费之间已经有可解释的替换边界。Linux 可以实现自己的字节流或终端传输，并保留 shell/日志的上层职责。

边界：当前 Stream 公开 k_sem，具体实现是 Zephyr 风格；这说明实现需要按 Linux 重写等待机制，不否定接口分层的设计价值。

## 4. 用四个实际扩展场景检验可扩展性

### 4.1 更换或新增 IMU 芯片

当前路径为：Source 定义设备能力；CalibSource 复用原始样本转换与校准流程；具体 BMI088/ICM42688P 类私有持有自己的总线与寄存器实现；源文件 REGISTER_IMU；Kconfig/CMake 选择实现；ImuManager 经注册表获得 Source。

新增后端需要实现对应接口、注册并接入构建，不要求 ImuManager include 新芯片或新增型号分支。温控与姿态处理不因芯片增加重新写一遍。项目仍决定模式和优先级。

这是“设备实现变化被限制在设备边界内”的具体证据。当前单后端数量校验则明确了适用范围，不能夸大为任意多设备管理。

### 4.2 新增遥控协议

当前协议实现自己的 Validate/Decode 与线路配置，在对应 cpp 注册；Remote 负责共享 UART、拼帧、探测、锁定、解锁和超时。新增协议不需要新增解析线程，也不应修改项目控制消费者去理解新字节布局。

探测阶段负责找协议，锁定后直接调用所选协议 Decode。它把扩展性主要放在选择阶段，正常路径仍围绕一个已选实例执行。

团队虽编译 DR16、VT03、WFLY 协议，但当前正式链路围绕 WFLY 解析与路由组织。协议源码数量不等于已经具备相同的自动发现与统一状态机。

### 4.3 新增一个业务任务

当前需要任务目录/源文件、项目功能开关和 CMake 接入；源文件本地声明对象、初始化、单拍任务、恢复钩子和启动项。init 不增加该业务类型分支，shell 的 thread 命令可通过公共注册表观察新任务。

团队新增一种独立 Runtime，通常需要增加成员、创建与资源绑定，并检查 Start、Stop、finished、error 等中央聚合处。显式装配对固定产品易读，但通用接入工作的重复面更大。

项目特有资源绑定仍应由项目代码负责，不能为了“零修改中央文件”把硬件关系藏进全局注册副作用。当前 MCU 的优势是公共启动与执行机制独立于具体业务，项目接线保持局部可读。

### 4.4 新增调试命令

当前命令族在本地 REGISTER_SHELL_CMD，Shell::ProcessLine 遍历匹配，PrintHelp 同样遍历。新增命令不必修改 shell 分派和手写帮助列表。

现有 Linux 移植版的 Shell::ProcessLine 仍直接判断 log、var、h/?，帮助也是显式列举。这里存在一个明确、可同步的 MCU 更新，而非笼统的“Linux 版老了”。

## 5. 现有 Linux 移植版继承了什么、落下了什么

### 5.1 已继承的设计基础

- framework/project 分开，设备能力、视觉算法与项目任务有位置区分。
- Kconfig 与 CMake 条件接入仍在；线程注册源直接编入 vision_app。
- ELF `.user_init/.user_thread` 指针表、KEEP 和分阶段执行已经实现。
- Thread 提供公共异常入口与可选恢复 hook，相机实际接入重连逻辑。
- 启停注册项覆盖 request_stop/join，统一退出先通知、再等待、最后释放资源。
- topic::Channel 提供单槽最新消息、订阅者独立游标和覆盖计数，符合视觉“旧帧可被替换”的时效需求。
- 日志已保留 DBG 选择、优先级队列、epoch 和启动/运行阶段切换等语义。

因此它不是一条需要因更新落后就抛弃的路线，而是有既有设计基础、需要补齐公共机制的版本。

### 5.2 与当前 MCU 的明确差距

| 项目 | 当前 MCU | 当前 Linux 版 | 应怎样更新 |
|---|---|---|---|
| 单拍受管执行 | TaskResult + 公共循环 | Entry 内多为完整业务循环 | 增加受管单拍入口，保留必要的特殊执行形式 |
| 任务治理 | ThreadBase、统计、注册表、shell | 未见对等的通用管理链 | 接入统一任务状态与统计消费接口 |
| 故障策略 | 启动失败与运行故障分开，逐任务配置 | 初始化有失败策略，线程启动失败统一停；异常 hook 负责恢复 | 明确公共故障语义，再映射既有恢复逻辑 |
| 命令扩展 | 本地命令族注册 | shell 中显式列举 | 同步注册与自动帮助机制 |
| 启动耗时 | BootTimingSlot 随注册生成 | 注册描述没有同等耗时槽 | 按注册项补统计，避免每组件手写计时 |
| 日志调用开销 | 运行阶段参数快照后异步格式化 | 调用点格式化自有文本 | 根据时序需求选择是否延后格式化 |

日志不能机械复制：MCU 参数快照使用 uint32_t 槽并含指针约定；64 位 Linux 的指针宽度、字符串生命周期和格式参数类型需要自己的实现。可迁移的是职责与队列策略，不是未经检查的二进制布局。

### 5.3 应保留的 Linux 特性

RequestStop/Join、可解除的 poll 等待、SDK 资源释放和 GUI 主线程事件不是“落后 MCU”的表现。它们解决 Linux 进程与库资源生命周期，应和受管任务机制组合。

例如视觉任务改成一帧一拍后，仍应先退出并 Join，再释放 detector/tracker/CUDA 资源。Channel 的共享图像内容也必须维持发布后不原地修改的所有权约定。

## 6. 团队架构的实际组织问题

### 6.1 chassis 根目录混合了不同层次

| 实际类别 | 文件示例 | 主要变化原因 |
|---|---|---|
| 几何/力学计算 | leg_kinematics、leg_vmc、gas_spring_model | 模型与计算方法 |
| 估计 | body_motion_estimator、support_force_estimator | 估计策略 |
| 控制计算 | balance_controller、unified_lqr_schedule、joint_output | 控制方法与约束 |
| 产品动作 | recovery、jump、flight、climb_stairs、stool_controller | 具体机器人的行为 |
| 行为协调 | chassis_controller、chassis_state_machine | 动作切换与业务状态 |
| 输入输出策略 | chassis_input_reader、chassis_output_policy | 输入解释、许可与输出规则 |
| 执行与观测 | chassis_runtime、chassis_observation | 线程、周期、设备接入、遥测 |
| 配置 | chassis_config、chassis_config_loading | 参数模型与 YAML 加载 |

**源码事实：** 上述类别大部分平铺在 src/chassis 根目录；CMake 将大部分核心实现加入同一个 dust_chassis，另建 dust_chassis_runtime。

**架构判断：** 目录边界只表达“和这台底盘有关”，没有充分表达计算能力、产品行为、执行机制和配置加载的关系。阅读和复用时需要靠文件名前缀及 include 手工恢复层次。继续增加动作或更换产品，中央目录与配置更容易继续增长。

不是把这些文件移动到几个子目录就会自动解耦，也不是所有 chassis 计算都应移到全局 algorithm。通用数学/控制方法可进入 framework/algorithm；轮腿专用模型可以是独立领域组件；爬楼、恢复等产品动作仍属于项目业务；线程接线与调度放项目运行层。先确定复用范围和接口，再安排目录与构建边界。

### 6.2 中央控制类承担动作汇集与切换协调

ChassisController 头文件直接包含并持有 BalanceController、RecoveryController、ClimbStairsController、FlightController、JumpController、输入读取、估计器、状态机和输出策略。

UpdateControlState() 集中执行各状态进入时的 Reset；ComputeControlOutput() 按状态组织各动作输入输出。新增动作需要考虑中央成员、状态枚举、进入动作、重置和输出分支，扩展会触及多个协调位置。

状态机本来就要知道业务状态，不能仅因存在分支判定设计错误。这里的问题是：这套具体机器人的行为集合被整体作为 chassis 核心组织，缺少进一步说明哪些计算可独立复用、哪些组合只属于该产品。相较之下，当前 MCU 先分算法、设备能力与项目组合，更适合用作多个项目共同演进的基础。

### 6.3 数据契约仍与具体设备文件组织耦合

`controller_inputs.h` 直接包含 motor_protocol.hpp、remote_input_state.hpp、hi91_protocol.h，并将 MotorFeedback[6]、RemoteInputState、ImuSample 作为输入。

控制器不调用这些头中的 Decoder/Encoder 服务，这是已有隔离；但控制输入的类型来源仍由设备协议文件决定。更换 IMU 或复用底盘核心时，需要继续带入这些头，或重新抽取/适配数据。

`tests/architecture_boundaries.py` 明确为这些被动数据 include 设置例外，说明检查通过也不等于已经消除这一耦合。当前 MCU topic 独立声明跨线程消息的方向更清楚；其 Linux 实现同样应保持明确的消息拥有者与转换边界。

### 6.4 配置按底盘汇集，行为参数边界较粗

ChassisConfig 同时收纳几何与质量、遥控死区、平衡偏置、支撑力、飞行推力、跳跃力矩、腿长 PID、关节控制等字段，中央构造过程将配置传给多个子组件。

这可以简化固定产品的配置传递，但当行为和算法持续增长时，大配置对象更容易成为共同依赖。不同模块应依赖哪些参数、哪些字段只属于某个动作，当前类型与文件组织表达得不够细。

这不是反对 YAML 或参数校验，而是区分“配置文件可校验”和“配置职责已经模块化”。

### 6.5 通用辅助文件也混入具体业务知识

用户当前打开的 `src/yaml_validation.hpp` 中，ValidateKeys/ParseUnsignedInteger 是通用校验；LoadSensorFile 却固定校验 schema_version、hi91、remote，携带具体传感器配置结构。

同一个根级辅助头承载了通用规则与产品 schema。新增传感器 schema 时，可能要修改通用工具文件。这个例子较小，却说明不仅 chassis 目录，其他位置也存在职责收拢不足。合理方向是通用校验留公共层，传感器 schema 加载归对应配置模块。

### 6.6 显式所有权优点不能替代框架扩展机制

DustControl 使用对象拥有会话和模块，业务借用设备/发送接口，先停止借用者再回收会话。这是清楚的局部设计。

但 CreateChassis/CreateGimbal/CreateUi、StartModulesFor、Stop、finished、error 等仍显式枚举业务种类。通用生命周期聚合和产品资源绑定交织在中央应用内，没有形成当前 MCU 那种按描述统一发现、执行和管理任务的完整链路。

结论是它有可取的所有权实践，不能据此推断整体架构比当前 MCU 更先进。反过来，注册也不能替代资源所有权；自己的 Linux 架构需要同时保留组件描述与清楚的资源持有关系。

## 7. 团队代码中可以独立借鉴什么

| 局部机制 | 源码依据 | 放入自有架构后的归属 |
|---|---|---|
| 绝对时刻周期等待 | chassis/runtime/realtime_loop.cpp::SleepUntilNs | Linux 任务执行机制 |
| CPU/FIFO/内存锁定配置 | concurrency/thread.cpp::ConfigureThread | Linux 线程实现 |
| 短临界区快照 | PublishedSnapshot + PriorityMutex | 有明确使用约束的通信/并发工具 |
| 有界非阻塞观测队列 | chassis_observation.hpp | 通用队列机制与项目观测类型分开 |
| 控制与 ROS 发布分离 | chassis_runtime.cpp / dust_control_node.cpp | 控制业务与观测适配分开 |
| 参数验证 | yaml_validation.hpp 中通用函数 | 配置工具，去除具体 schema 依赖 |
| 自动依赖约束 | tests/architecture_boundaries.py | 根据自己的分层重新定义检查规则 |

这些实现值得按需求审查，不要求整体接收团队工程。桥接会话和控制算法属于另一层评估：必须核对实际协议和机器人模型，不能凭架构选型顺带决定采用。

已有测试表示有回归入口，本文未执行，也没有用测试数量给架构打分。

## 8. 统一标准下的比较

| 标准 | 当前 MCU 设计 | 现有 Linux 版 | 团队工程 |
|---|---|---|---|
| 通用能力与产品业务分开 | 有明确职责分类及项目接线 | 继承主要框架结构 | 外层已有分区，chassis 内混合较多层次 |
| 新实现本地接入 | 启动、设备、协议、命令等均有注册入口 | 启动保留，命令等有旧式中央分派 | 主要依赖显式应用装配 |
| 公共任务执行 | 单拍外壳、结果、统计、策略相互配合 | 公共线程入口存在，单拍治理未跟进 | 各 Runtime 自行组织公共流程 |
| 设备运行模型 | 主动源内部线程，被动源回调/快照 | 相机能力与项目运行分开 | Decoder 与 Runtime 分开，但输入类型耦合设备头 |
| 运行管理 | 注册表、命令、变量、日志和 boot 统计 | 部分能力已有，缺口明确 | 业务观测完善程度另议，缺对等统一任务治理 |
| 进程级资源回收 | MCU 常驻模型，按自身需求设计 | 已有完整停止/Join/释放路径 | 显式拥有与借用，清理顺序可借鉴 |
| 架构约束落地 | Kconfig、条件构建、注册和接口；构建多汇入 app | 条件构建与注册，部分独立库 | 有目标拆分和自动 include/链接检查 |

当前 MCU 并非每项都已经最强：例如它的多数框架源汇入 app，分层更多依赖职责、接口和维护约定；团队独立目标和自动边界检查是值得补充的手段。但这些局部手段可以加强当前体系，不构成整体换底座的理由。

综合判断：**当前 MCU 的主要优势在框架机制的完整性和一致性，团队工程主要提供具体产品实现与若干局部工程机制。现有 Linux 版应补齐前者，而不是因后者已有更多控制代码就替换架构。**

## 9. Linux 版更新方向：保留原则，明确差异

### 9.1 保留的原则

1. project 决定使用什么能力、资源和策略；framework 提供通用机制与设备能力。
2. 组件本地声明，公共机制发现；构建负责是否存在，配置负责实例参数。
3. 业务只写单拍或一次事件处理，循环、统计和公共故障分派收进执行器。
4. 主动设备拥有内部连续运行逻辑；被动设备不强行线程化；跨设备组合留项目层。
5. 消费者面向消息与设备能力契约，避免依赖具体生产线程或原始协议布局。
6. 调试管理是框架能力，新增任务可以直接进入共同观察入口。

### 9.2 Linux 具体落地

| 内容 | 落地方向 |
|---|---|
| 受管任务 | 在现有 init::Thread 基础上实现 TaskResult 单拍入口；明确一拍边界 |
| 周期执行 | 区分执行后间隔与绝对周期；事件任务保留可解除的阻塞等待 |
| 故障处理 | 统一结果/异常到框架故障语义，保留设备特有恢复 hook |
| 运行管理 | 补任务基类、状态/计数/耗时、注册表与命令消费者 |
| 启动与退出 | 保留 ELF 注册与阶段；Linux 保留先通知、再 Join、后释放资源 |
| 命令系统 | 同步本地命令注册，帮助由描述生成 |
| 日志 | 保留通道策略与有界队列，按 Linux ABI 实现快照或自有文本 |
| 数据交换 | 最新帧通道、快照和 FIFO 按契约选择，不强行统一成一种机制 |

视觉里“一帧处理”可作为单拍。GUI 主线程回调保持特殊执行要求，不为形式一致强行放进普通工作线程。修改时也不要求先重排全部目录、重做算法或引入 ROS 2。

## 10. 设计优势与实现完成度必须分开

这部分限定结论，避免把设计意图写成已实现保证：

- **文档可能落后源码。** 线程指南仍有旧重载/结果描述；当前 thread.hpp 的 TaskResult 与 Start 签名为准。旧目录图和计数不作选型依据。
- **注册发现有明确约束。** IMU 只允许一个注册后端；Remote 当前 HandleDetecting 按段条目前后推进，未见按 Priority 排序。不能照设计介绍扩写不存在的能力。
- **故障清理要看实际发布。** Remote 的 OnRetry/OnExit 调用 zbus 发布归零消息；IMU 对应 hook 当前清本地 pub_，OnExit 关闭加热，未执行同等发布。不能把两者笼统写成已对消费者完整通知失效。
- **治理接口不等于并发问题已解决。** Linux 迁入注册表与统计时，需要明确登记/退出/查询、对象生命周期和快照同步；当前实现不能未经审查直接复制为多核线程安全版本。
- **有限注册容量影响治理覆盖。** MCU 注册表上限为 16，满时仍可创建任务；Halt 遍历该表。Linux 若以其承担完整故障管理，要明确容量与登记失败策略。
- **常驻模型不是缺陷。** 不因 MCU 没有正常 Stop/Join，或 IMU/Remote 拥有线程，就否定既定架构；只为 Linux 的实际需求补进程收尾。

以上不展开成无关重构清单，也不改变“沿用设计主线”的建议；它们说明迁移要验证哪些实现契约。

## 11. 建议的更新顺序与验收方式

下面是后续建议，本次未修改代码。

1. **先同步任务治理。** 以当前 MCU 的 TaskResult、故障配置、统计和注册表为设计参考，在 Linux Thread 上补齐。验收：新增最小任务不改公共启动分派，能够查询计数/耗时，Retry 和故障策略有明确表现，停止可解除等待。
2. **再同步调试扩展。** 把 Linux shell 的显式命令列表换成本地描述与统一发现，接通任务管理、启动耗时；日志是否异步格式化按实际需求决定。验收：新增命令族无需修改 shell 分派与帮助两份名单。
3. **按设备/业务边界更新视觉接入。** 保留相机 SDK、检测器与跟踪器的能力接口；项目组织一帧处理和消息交换。验收：换相机实现不改检测算法，增加消费者不改生产者业务，慢消费者有明确覆盖统计。
4. **按需吸收团队局部机制。** 调度、并发工具、观测队列与检查规则分别进入对应职责层。验收：没有因复用某个工具引入整套 chassis 或具体设备 schema。
5. **最后进行目标主机运行验证。** 检查正常退出、断连恢复、推理异常、部分启动失败和负载下周期/帧龄。性能和硬件结论以实测为准。

先看架构是否真正限制修改范围：新增协议是否只影响协议与构建，新增任务是否复用公共治理，项目改接线是否不用修改算法和模块内部。再看目标系统的性能和工程成本。

## 12. 主要源码索引

以下链接相对于本文，符号名称对应本次阅读的入口。

| 主题 | 文件与入口 |
|---|---|
| 设计说明 | [README](../README.md)、[子树架构介绍](../子树架构介绍.md)、[线程指南](线程编写指南.md)、[模块设计](../framework/modules/ARCHITECTURE.md)，以源码校正旧描述 |
| 构建接入 | [project/Kconfig](../project/Kconfig)、[项目 CMake](../project/CMakeLists.txt)、[线程 CMake](../project/thread/CMakeLists.txt)、[模块 Kconfig](../framework/modules/Kconfig)、[模块 CMake](../framework/modules/CMakeLists.txt) |
| 注册描述与执行 | [Init_entry.hpp](../framework/init/startup/Init_entry.hpp)、[Init_entry.cpp](../framework/init/startup/Init_entry.cpp)、[链接段](../framework/cmd/linker/dust_init.ld) |
| 受管任务与故障 | [thread.hpp](../framework/init/thread/thread.hpp)、[thread_fault.hpp](../framework/init/thread/thread_fault.hpp)、[thread_fault.cpp](../framework/init/thread/thread_fault.cpp) |
| 管理链与最小示例 | [注册表](../framework/init/thread/thread_registry.cpp)、[线程命令](../framework/shell/thread/thread_cmd.cpp)、[trd_test](../project/thread/test/trd_test.cpp) |
| IMU 能力与实现发现 | [Source/CalibSource](../framework/modules/imu/devices/imu_device_layer.hpp)、[ImuManager 声明](../framework/modules/imu/drivers/imu.hpp)、[实现](../framework/modules/imu/drivers/imu.cpp)、[项目接入](../project/thread/imu/trd_imu.cpp) |
| Remote 扩展与运行 | [remote.hpp](../framework/modules/remotes/remote.hpp)、[remote.cpp](../framework/modules/remotes/remote.cpp)、[项目接入](../project/thread/remote/trd_remote.cpp) |
| 被动设备 | [DJI 电机](../framework/modules/motors/dji/dji_c6xx.hpp)、[PowerMeter](../framework/modules/powermeter/powermeter.hpp) |
| 驱动契约与 CAN 分发 | [Stream](../framework/drivers/communication/stream/stream.hpp)、[UartDma](../framework/drivers/communication/stream/uart/uart.cpp)、[CAN 注册](../framework/init/irq/Irq_handlers.h) |
| 命令和变量扩展 | [shell_cmd.hpp](../framework/shell/shell_cmd.hpp)、[shell.cpp](../framework/shell/shell.cpp)、[var.hpp](../framework/shell/var/var.hpp) |
| 日志机制 | [process](../framework/shell/log/process/log_process.cpp)、[record](../framework/shell/log/queue/log_record.hpp)、[queue](../framework/shell/log/queue/log_queue.hpp)、[policy](../framework/shell/log/queue/log_policy.hpp)、[transport](../framework/shell/log/transport/log_transport.hpp) |
| Linux 构建与注册 | [线程构建](../temp/Dust_Vision_Architecture-main/project/thread/CMakeLists.txt)、[注册段](../temp/Dust_Vision_Architecture-main/framework/init/dust_init.ld)、[启动器](../temp/Dust_Vision_Architecture-main/framework/init/Init_entry.cpp) |
| Linux 任务与消息 | [Thread](../temp/Dust_Vision_Architecture-main/framework/init/thread.hpp)、[Channel](../temp/Dust_Vision_Architecture-main/framework/topic/channel.hpp)、[视觉任务](../temp/Dust_Vision_Architecture-main/project/thread/vision/trd_vision.cpp)、[相机恢复](../temp/Dust_Vision_Architecture-main/project/thread/camera/trd_camera.cpp) |
| Linux 旧命令与日志 | [shell.cpp](../temp/Dust_Vision_Architecture-main/framework/cmd/shell/shell.cpp)、[log.hpp](../temp/Dust_Vision_Architecture-main/framework/cmd/shell/log.hpp) |
| 团队应用聚合 | [DustControl 声明](../temp/dust-control-main/src/dust_control.hpp)、[实现](../temp/dust-control-main/src/dust_control.cpp) |
| 团队 chassis 边界 | [构建](../temp/dust-control-main/src/chassis/CMakeLists.txt)、[中央控制声明](../temp/dust-control-main/src/chassis/chassis_controller.h)、[实现](../temp/dust-control-main/src/chassis/chassis_controller.cpp)、[配置](../temp/dust-control-main/src/chassis/chassis_config.h)、[输入](../temp/dust-control-main/src/chassis/controller_inputs.h) |
| 团队配置职责 | [yaml_validation.hpp](../temp/dust-control-main/src/yaml_validation.hpp) |
| 团队可借鉴机制 | [调度](../temp/dust-control-main/src/concurrency/thread.cpp)、[绝对等待](../temp/dust-control-main/src/chassis/runtime/realtime_loop.cpp)、[快照](../temp/dust-control-main/src/concurrency/published_snapshot.hpp)、[锁](../temp/dust-control-main/src/concurrency/priority_mutex.hpp)、[观测队列](../temp/dust-control-main/src/chassis/chassis_observation.hpp)、[ROS 适配](../temp/dust-control-main/src/dust_control_node.cpp)、[边界检查](../temp/dust-control-main/tests/architecture_boundaries.py) |
