# EtherCAT 首次进入 OP 缓慢调研

日期：2026-10-30。范围：Linux SOEM 主站建链、Zephyr 从站 SAFE-OP→OP 状态转换及本次双方日志。本文仅调研，不修改通信代码、不编译、不烧录。

## 1. 结论与证据边界

1. 之前的 `SPURIOUS_IRQ` 与本次重试慢是两件事。前者已有明确配置错误：HPM6E80 ESC IRQ 为 133～136，而原 PLIC 配置上限为 127。当前板卡配置已改为 `CONFIG_MAX_IRQ_PER_AGGREGATOR=192`，本次日志显示 PDI ISR 能运行，且最终进入 OP。
2. 本次主要耗时是三次 OP 请求各等待约 5 秒失败，再重新枚举；第四次进入 OP 只用 2 ms。不能把它描述为“一次正常握手本来就需要十几秒”。
3. **主站在等待 OP 时确实持续发送、接收过程数据。此前提出的“是否没有发 PDO”已经通过源码排除为这份代码的行为。** 但发送和接收的结果被丢弃，现有日志不能证明失败尝试中的 PDO 被从站正确接收。
4. **已确认主站建链期的 PDO 交换节拍远慢于配置的 1 ms。** 等待 OP 的循环执行一次收发，再等待最多 10 ms 的状态查询，最后休眠 10 ms。未进 OP 时，典型间隔会接近 20 ms，还叠加接收与状态读取耗时。这与从站每个 100 ms 报告间隔约增加 4～5 次 ISR 相符，但两份日志没有共同时间戳，不能当成精确周期测量。
5. 从站在非 DC 模式、有输出 PDO 时，需要 `bEcatFirstOutputsReceived` 为真才完成 OP 转换。主站当前丢弃了启动阶段 WKC，从站又没有打印这个标志、实际同步模式、看门狗状态及状态转换内部进度，因此**尚不能唯一判定前三次为什么始终没有完成这个条件**。
6. 当前日志不足以证明 EEPROM 的 4/32 字节配置冲突，更不能得出必须重刷 EEPROM。此次每次均映射 65 字节，SM 缓存和 CoE 长度均报告 32/32。

最优先的验证是：在同一次失败尝试中，把主站 PDO 收发结果与从站 OP 门槛状态对应起来；然后对照试验建链期间维持 1 ms 收发、降低状态查询频率，确认是否能稳定首次进入 OP。不能仅把 5 秒超时调长、或把重试间隔缩短当作根因修复。

## 2. 资料与路径约定

本文使用以下缩写，后文行号均指本次读取的源码快照：

| 缩写 | 路径 |
|---|---|
| M | `temp/Dust_Linux_Architecture-main/framework/drivers/communication/ethercat/ethercat.cpp` |
| MH | `temp/Dust_Linux_Architecture-main/framework/drivers/communication/ethercat/ethercat.hpp` |
| MT | `temp/Dust_Linux_Architecture-main/project/thread/ethercat/trd_ethercat.cpp` |
| MTH | `temp/Dust_Linux_Architecture-main/framework/init/thread/thread.hpp` |
| S | `framework/drivers/communication/ethercat/ssc/ecatslv.c` |
| A | `framework/drivers/communication/ethercat/ssc/ecatappl.c` |
| H | `drivers/ethercat/hal/hpm/esc_hpmicro.c` |
| HAL | `drivers/ethercat/hal/hpm/ecat_hal_hpm.cpp` |

运行证据：用户粘贴的主站 attempt 1～4 日志，以及附件 `C:/Users/33527/.codex/attachments/5b15f6b5-ef9f-4b32-8913-d8f7c41281b6/已粘贴的文本.txt` 中的从站串口日志。

主站源码是本地解压目录，不等同于已核实部署版本。本仓库只通过 `find_path/find_library` 查找安装好的 SOEM，未附带该主站实际链接的 SOEM 源码、版本和运行二进制。因此不把某个 SOEM 版本的内部实现当作本次已验证事实。

## 3. 本次实际发生了什么

### 3.1 主站时间线

| 尝试 | 扫描与映射 | 初次报告状态 | 本次结果 |
|---|---|---|---|
| 1 | 1 从站，65 B，SM2/SM3=32/32 | 11 ms，SAFE-OP，AL=0 | 5021 ms 超时 |
| 2 | 同上 | 11 ms，SAFE-OP，AL=0 | 5006 ms 超时 |
| 3 | 同上 | 11 ms，SAFE-OP，AL=0 | 5003 ms 超时 |
| 4 | 同上 | 2 ms，OP，AL=0 | 成功 |

前三次等待 OP 的耗时合计为 **15030 ms**。除此以外还有网口打开、扫描、CoE 映射、SM 改写和重连退避时间。主站没有记录每个阶段耗时，不能从这份日志精确计算上电到连通的总时长。

`cycle=1000 us` 是成功以后线程打印的配置值，不是 `RequestOperational()` 内部实际使用的循环周期，也不是实际收发周期测量值。

### 3.2 从站时间线

- PHY 报告 port0 link up。
- `ESC ISR irq=0` 的计数由 2 持续增加，说明 PDI 已经能经过 PLIC 进入 ESC 驱动；这里的 `irq=0` 是 ESC 驱动枚举值，不是 PLIC 中断号 0。
- SAFE-OP 阶段计数通常每条增加 4～5；在 236、471、708 附近出现一串计数不变的报告。
- 后续出现 `ecat rx: 0 1 2 3 0 0 0 0`，ISR 快照也最终为 `AL_STAT=0x0008`。

这些停顿与主站失败后停止发 PDO、关闭总线并退避的行为相符。由于串口日志无时间戳，不能严格把每个停顿逐一绑定到某个 attempt。

### 3.3 为什么反复打印 SAFE-OP 不等于反复读到 SAFE-OP

H:328～341 只在进入 ISR 时保存 `AL_STAT / AL_STAT_CODE / GPR_CFG1`。H:347～358 的 `esc_isr_report()` 打印保存值，HAL:153～154 每次链路轮询调用它。

所以 `n=236` 不变时，反复出现的 `AL_STAT=0x0004` 是同一个旧 ISR 现场，不能证明这段时间内寄存器始终没有变化。日志里的状态采样发生在 SSC 回调之前，也不是该次 SSC 处理完成后的状态。

## 4. 主站建链流程逐段核对

### 4.1 枚举与 PDO 映射

M:98～197 的 `Master::Init()` 顺序为：

```text
选择网口
→ 分配 SOEM context
→ ecx_init
→ ecx_config_init
→ 分配 4096 B IOmap
→ ecx_config_map_group
→ 打印缓存中的 SM 长度与 CoE 长度
→ 整块重写 SM2、SM3
→ 计算 expected WKC
→ 可选 ecx_configdc
→ initialized=true
```

M:158～159 打印的 SM 长度来自 `ctx->slavelist[i].SM[]`，是 **SOEM 缓存**；并非在该位置直接从 ESC 寄存器读回的值。`SM2 rewrite ... wkc=1` 表示该寄存器写命令有响应，也不等价于后续 PDO 交换 WKC 正确。

M:153～154 关于“EEPROM 是 4 字节、固件是 32 字节”的注释是代码里的既有判断，不能代替实际 SII 导出和寄存器读回证据。

### 4.2 OP 请求前后确实有 PDO 交换

M:251～253：

```cpp
(void)ecx_send_processdata(ctx);
(void)ecx_receive_processdata(ctx, config_.timeout_us);
(void)ecx_writestate(ctx, 0);
```

M:258～305 循环内：

```cpp
(void)ecx_send_processdata(ctx);
(void)ecx_receive_processdata(ctx, config_.timeout_us);
(void)ecx_statecheck(ctx, 0, EC_STATE_OPERATIONAL, kStateCheckUs);
(void)ecx_readstate(ctx);
// 检查状态、成功或超时退出
std::this_thread::sleep_for(std::chrono::milliseconds(kOperationalPollMs));
```

因此“主站先等 OP 才开始发送 PDO，双方互相等”的简单死锁，不符合这份代码。

但是发送、接收、写状态和读状态的返回值在这里全部被忽略。失败尝试中可能出现的发送失败、WKC 不足和状态读取失败都不会出现在当前日志中。正常运行的 `Master::Receive()` 会校验 WKC（M:345～350），启动循环却绕开这个校验。

### 4.3 建链循环为什么大约只有几十 Hz

M:34～36 固定为：

| 参数 | 值 | 用途 |
|---|---:|---|
| `kOperationalPollMs` | 10 ms | 每轮结尾休眠 |
| `kStateCheckUs` | 10000 µs | 状态检查超时参数 |
| `kOperationalTimeoutMs` | 5000 ms | 整次 OP 请求超时 |

失败阶段，单轮时间为：

```text
PDO 发送 + PDO 接收 + 状态等待 + 状态读取 + 日志/处理 + 10 ms 休眠
```

只要状态等待接近 10 ms，PDO 发送间隔就会接近 20 ms。由此可以解释 SAFE-OP 阶段中断约几十次每秒，而不是配置期待的每秒 1000 次。

这证明启动与运行节拍不一致，**尚不证明 20 ms 本身必然阻止 OP**。还需要实际看门狗参数、有效 PDO 接收和 SSC 标志才能建立因果关系。

### 4.4 一次失败为什么会重新来一遍

MT:119 调 `Init() && RequestOperational()`；失败后 MT:136～137 执行 `Close()` 并设 `reconnect_wait_=1000`。随后每次 Retry 减一，耗尽后重新扫描、映射和请求 OP。

MTH:523～541 显示 Retry 钩子返回后仍执行线程周期等待；当前周期配置为 1 ms，因此退避约为一秒再加调度和执行开销，并非精确一秒。

`RequestOperational()` 本身最多阻塞约 5 秒；MT 的“退避不阻塞”注释只适用于按拍递减的退避阶段，不能用来形容整个建链钩子。

## 5. 从站到底在等什么

### 5.1 OP 请求由 MainLoop 消费

`EcatStack::Poll()` 调 `MainLoop()`，而 A:913 调 `ECAT_Main()`。S:2668～2699 在存在 `AL_CONTROL_EVENT` 且没有待完成转换时，读 AL Control 并调用 `AL_ControlInd()`；S:2715～2717 对待完成转换调用 `AL_ControlRes()`。

因此 PDI 中断计数增长不等于 AL 状态转换已经被主循环正确消费；需要同时看 AL Control、pending 标志和转换状态。

### 5.2 SAFE-OP→OP 先进入等待，再等条件完成

S:1270～1304 的 `StartOutputHandler()` 默认返回 `NOERROR_INWORK`。非 DC 分支在 S:2217 检查：

```cpp
if (nPdOutputSize == 0 || bEcatFirstOutputsReceived)
```

条件通过后才调用 `APPL_StartOutputHandler()` 并设置 OP。当前应用钩子 `appl/ecat_appl_hooks.cpp:74～77` 直接返回成功，没有业务准备延迟。

A:503～522 显示，PDI ISR 在 `bEscIntEnabled` 为真且发现 `PROCESS_OUTPUT_EVENT` 时设置 `bEcatFirstOutputsReceived=TRUE`。**总 PDI 计数只能证明进入 ISR，不能证明满足了这个分支。**

### 5.3 该标志也可能被清除

S:2341～2368 的 `CheckIfEcatError()` 在配置了过程数据看门狗、其状态未触发且尚未启用输出时，会将 `bEcatFirstOutputsReceived` 清零。

所以需要区分“从未置位”和“置位后又被看门狗路径清零”。现有日志没有记录两者，不能仅凭 AL=0 判定。

`ecat_def.h:391` 中 `SAFEOP2OPTIMEOUT=0x2328`，即 9000 ms。若从站已经进入等待转换且计时正常，主站 5000 ms 就退出，会先于从站这个超时边界结束本次尝试。因而主站在 5 秒时看到 AL=0，并不说明从站没有在等待某个条件，也不证明主从之间完全正常。这个 9 秒不是正常进入 OP 必须等待的时间。

### 5.4 MainLoop 有一段被整体注释，但不能直接定为本次根因

A:851～904 的 FreeRun/首帧兜底处理被整体注释，包括轮询输出事件、应用处理及输入映射。

它会影响 FreeRun 或缺少首次 SM 事件的路径。不过 S:898～1010 在没有用户强制指定同步模式、无 DC 且有输出时，默认选择 SM 同步并使能过程输出事件。因此不能不看实际 `bEscIntEnabled / bSyncSetByUser / 0x1C32 / 0x1C33` 就直接宣称“恢复这段一定修复本次问题”。

## 6. 其他需要单独标注的发现

### 6.1 测试 PDO 内容与主站源码快照不一致

MT:60～63 当前给八个输出依次填 0～7，双方声明均为 32 B。但日志持续显示 `0 1 2 3 0 0 0 0`。

这意味着还没有验证八个字段都按当前源码传输。可能涉及运行版本、实际写入内容、映射范围等，现阶段不选定其中某个原因。后续应对照同一周期主站 IOmap 与从站收到的完整 32 B，不能只凭前四项正确宣布整帧验收通过。

### 6.2 正常运行日志在中断回调中打印

PDI ISR → `PDO_OutputMapping()` → `APPL_OutputMapping()` → `EcatStack::OnOutputMapping()` → test 注册的 RxHandler → `DUST_LOG_INF`。

它是过程数据中断路径中的每帧日志，而 HAL 的 ISR 报告是线程侧日志，两者不能混淆。该打印主要在已开启输出更新后执行，因此不能直接解释首次 OP 之前的三次超时。它会影响后续性能评估，应另行测量日志调用和实际发送路径，不能未经核实就称日志同步阻塞或擅自删除。

### 6.3 映射完成后再次写整块 SM 配置

M:168～178 无条件重写完整 `ec_smt`，即使缓存长度已经等于 32。S:2705～2712 会处理 SM 配置变化事件。

这是应记录顺序和读回结果的检查点，不是已经证明的冲突。映射和状态转换的时序是否受它影响，需要失败时寄存器与 SOEM 实际版本证据。

## 7. 最小取证与对照试验

以下是后续验证建议，本文没有实施。保持主站 SOEM context 单线程访问，不另起线程同时访问同一个 context。

| 优先级 | 位置 | 需要记录 | 解决什么疑问 |
|---|---|---|---|
| 1 | M 的 OP 等待循环 | 相对时间、实际 send 间隔、send 返回值、receive WKC、expected WKC、写状态返回值 | 是否真的有有效启动 PDO |
| 1 | 从站线程侧状态快照 | AL Control/Status/Code、`nAlStatus`、`bEcatWaitForAlControlRes`、`bApplEsmPending` | OP 请求是否已被 SSC 接收并进入等待 |
| 1 | SSC/从站快照 | `bEcatFirstOutputsReceived`、`bEscIntEnabled`、`bDcSyncActive`、同步类型、`EsmTimeoutCounter` | 卡在首帧、同步还是其他转换条件 |
| 1 | 从站只读诊断 | `EcatWdValue`、看门狗状态 0x0440、事件 mask、SM2/SM3 配置 | 首帧标志是否被清除、事件是否使能 |
| 2 | 双方 PDO | 同周期完整 32 B 和递增序号 | 八个字段是否完整、是否来自同一部署版本 |
| 2 | 主站初始化分段 | 扫描、映射、SM 改写各自时间及 SM 寄存器读回 | 5 秒以外的开销和改写是否生效 |

诊断不要在 ESC ISR 内发送串口日志。事件观察应优先使用 SSC 本来读取到的局部值或计数，在线程侧汇总，避免额外读取/确认事件改变现场；读取 AL Control 等有事件确认语义的寄存器前应先核对访问副作用。

对照试验保持 PDO 布局、SM 配置和从站固件不变，仅改变主站等待 OP 的组织方式：

1. 发送与接收按 1 ms 目标节拍推进，记录真实间隔。
2. AL 状态读取独立降频，使用有界的短查询，避免每个 PDO 周期都等待最多 10 ms 的状态检查；具体 SOEM 调用行为先按运行机器安装版本核实。
3. 对比连续首次连接的 attempt 数、进 OP 耗时、启动阶段 WKC、首帧标志与看门狗状态。

若新节拍下稳定首次进入 OP，且原节拍时能观察到首帧/看门狗条件不满足，才有依据把启动节拍定为原因。若仍失败，则沿 AL Control 消费与 SSC pending 条件继续定位，不能把“第四次碰巧成功”当成修复验证。

## 8. 验收标准与本次交付

- 首次尝试能稳定进入 OP，不靠多次关闭重开来碰时序；记录实际分布，不以单次 2 ms 替代重复验证。
- 启动收发有明确 WKC 与从站门槛证据，失败时能定位到具体未满足条件。
- 所有八个 UINT32 双向内容可核对，排除仅前 16 B 看起来正确的情况。
- 实际周期由时间戳验证；配置打印 `1000 us` 不算周期验收。
- 不复现 PLIC 越界 fatal；OP 后稳定收发期间不出现未解释的退态或看门狗错误。

本次只新增本 Markdown。没有修改 Linux 主站或 Zephyr 从站源码，没有运行主站、编译固件、烧录或替用户确认硬件修复完成。
