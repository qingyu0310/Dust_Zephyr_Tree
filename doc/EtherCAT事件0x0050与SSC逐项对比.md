# EtherCAT 事件 0x0050 与 SSC 逐项对比

日期：2026-10-31。

范围：当前从站的 AL 事件处理、SM 校验、过程数据看门狗、PDO 映射顺序、pending 转换与邮箱状态位。本文记录源码对比结果，不修改代码、不编译、不烧录。

## 1. 对比来源

本次通过 GitHub 连接直接读取远端源码，没有将本地旧解压目录视为最新版本。

- 仓库：`HNUST-Dust/hpm_support`。
- 含 SSC 的分支：`codex/dust-hpm5e31`。
- 本次核实的分支最新提交：`d1e9fbfbdca40c3ecc965795917735cb92bd6fd2`。
- SSC 路径：`samples/ecat/EtherCAT/ssc/`。
- 仓库默认分支为 `main`，但本次查询的默认分支对应路径没有 SSC 文件；本文所称最新 SSC，是上述含 SSC 分支的最新提交，不混称为 main 的实现。

主要原文件：

| 原文件 | 本次对比内容 |
|---|---|
| `ecatslv.c` | ECAT_Init、ECAT_Main、CheckSmSettings、状态转换、看门狗检查 |
| `ecatappl.c` | PDI_Isr、ECAT_CheckTimer |
| `ecatslv.h` | AL 事件位定义 |
| `esc.h` | ESC 寄存器、SM 状态位与看门狗状态掩码 |
| `mailbox.c` | 邮箱状态、重复请求与发送完成处理 |

当前代码行号以本次读取版本为准，后续修改可能使行号移动。本文不是整个 SSC 功能等价性验收，也不代表 DC、SDO 等所有分支已通过实测。

## 2. 现象与事件含义

从站持续打印：

```text
[inf] ecat: al_event 0x0050
```

`0x0050 = 0x0010 | 0x0040`。

| 位 | 含义 | 对应确认操作 |
|---|---|---|
| `0x0010` | 至少一个 SM 的激活配置发生变化 | 从 PDI 读取相应 SM 激活寄存器，如 `0x0806 + 8*n` |
| `0x0040` | 过程数据看门狗事件 | 从 PDI 读取 Watchdog Status Process Data，即 `0x0440` |

确认语义同时可见于本地 HPM SDK 的 `soc/HPM6E00/ip/hpm_esc_regs.h`：`AL_EVT_REQ.SM_ACT` 和 `AL_EVT_REQ.WDG_PD` 的字段说明。

**AL Event 是事件汇总/镜像，不是读一次 0x0220 就清空所有来源。** 各事件有各自的确认操作。SSC 的 ECAT_Main 和 PDI_Isr 都会重新读取事件值，而不是假定读过一次便全部处理完成。

`0x0050` 也不是 AL Status Code。它本身不能说明当前处于 INIT、PREOP、SAFEOP 还是 OP；要另看 AL Status 和 AL Status Code。

当前 `EcatDevice::HandleAlEvents()` 对所有非零事件值都输出日志，所以未确认、仍然挂起的位会在后续 Poll 中反复显示。这不是持续产生新事件次数的直接证明。

## 3. SM Change 的确认流程遗漏

### SSC 原行为

`CheckSmSettings()` 在必要通道校验成功之后，还读取其余硬件 SM 通道的激活信息，用于确认剩余 SM Change 来源。原实现注释明确说明，这轮读取用于确认 SM-Change 中断。

来源：[ecatslv.c：673～680](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L673-L680)。

### 当前行为

`core/al/ecat_state_machine.cpp`：

- `CheckSmSettings()` 约第 730 行：先读 SM0/SM1，按当前检查范围决定是否继续读 SM2/SM3。
- PREOP 检查邮箱后可直接返回，没有 SSC 末尾那轮“读取其余 SM 激活寄存器”的处理。
- `Recheck()` 约第 791 行：INIT 状态直接返回，不进行 SM 确认读取。
- 未见与 SSC `nMaxSyncMan` 配套的完整剩余通道扫描。

### 影响

例如主站在 PREOP 配置 SM2/SM3，当前 Recheck 只校验邮箱通道，相关 SM Change 来源可能未被读取确认，`0x0010` 因而可以持续存在。

这是当前源码中明确的处理缺口。具体哪一个 SM 通道产生本次挂起位，仍需要当时的各通道激活状态才能确定，不能单凭 0x0050 指认某一个通道。

## 4. 看门狗事件确认与错误判断混在一起

### SSC 原行为

`StartInputHandler()` 读取过程数据看门狗时间 `0x0420`、分频 `0x0400`，结合 SM2 的看门狗触发使能，检查配置是否一致。

`CheckIfEcatError()` 在实际计算的 `EcatWdValue` 非零时读取 `0x0440`，再判断是否需要退态或清除首帧标志。

来源：

- [ecatslv.c：1099～1148](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L1099-L1148)。
- [ecatslv.c：2341～2368](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L2341-L2368)。

### 当前行为

`core/al/ecat_state_machine.cpp:833` 的 `CheckErrors()`：

1. 仅当 `output_running_` 为真且有输出 PDO 时读取 `0x0440`。
2. 代码注释写着“主站配置了看门狗时间时才检查”，实际条件却没有这个配置判断。
3. `EcatSync::PrepareSm/StartSm` 没有迁入上述完整的过程数据看门狗配置检查。
4. `HandleAlEvents()` 没有独立处理 `0x0040` 的分支。

### 影响

未进入 OP、或者退回 SAFEOP 后，`output_running_` 为假，看门狗事件可能没有相应的 `0x0440` 读取来确认，`0x0040` 持续挂起。

此外，不能绕过“是否启用看门狗”的配置检查，直接用状态位判断所有场景都故障。**事件确认、当前状态读取、是否构成协议故障、是否要求退态，是需要分清的四件事。**

## 5. 首帧标志清零分支错误

### SSC 原行为

在看门狗检查已经启用、状态异常且存在输出 PDO 时：

- 若输出正在运行，要求退到 SAFEOP 并报告 SM 看门狗错误。
- 否则清除 `bEcatFirstOutputsReceived`。

这是错误路径中的条件动作，不是在每次正常检查后清零。

### 当前行为

`core/al/ecat_state_machine.cpp` 约第 845 行，在读取看门狗后：异常分支退态并 return；正常分支反而继续执行 `first_outputs_received_ = false`。

### 影响

正常通信也会丢掉“已经收到首帧”的状态。此前将这个字段变化直接归因于没有收到 PDO，证据不足；当前软件逻辑本身会主动清掉它。

## 6. PDO 映射与 SM 校验顺序反了

### SSC 原行为

PREOP→SAFEOP 先调用 `APPL_GenerateMapping()` 更新实际 PDO 长度，再调用 `CheckSmSettings()` 对照主站配置的 SM 长度。

来源：[ecatslv.c：1572～1593](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L1572-L1593)。

### 当前行为

- `EcatProcessData` 的 `layout_` 默认初始化为零。
- `Transition()` 先调用 `CheckSmSettings()`。
- 之后进入 `EnterSafeOp()`，才调用 `process_data_->Configure(*dictionary_)`。
- 本次搜索到的 Configure 调用就在 EnterSafeOp 内，没有找到初始化阶段预先配置布局的调用。

### 影响

首次尝试进入 SAFEOP 时，可能先用默认 0 字节布局校验主站已经配置好的 32 字节 SM。校验返回错误后，根本到不了计算 32 字节映射的步骤。

这不是 EEPROM 长度不一致的直接证据，而是从站内部校验顺序错误。

## 7. pending 转换超时机制没有接完整

### SSC 原行为

返回 `NOERROR_INWORK` 后，SSC 保存目标转换、装载相应的超时计数，并在主循环调用 `AL_ControlRes()` 继续完成或终止转换。1 ms 定时路径负责递减计数。

该分支还会从配置超时中扣除 10%，不是直接使用完整配置值。

来源：[ecatslv.c：1892～1915](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L1892-L1915)。

### 当前行为

- `HandleControl()` 设置 pending 和目标状态，但没有装载 `timeout_counter_`。
- 本次搜索中，该计数只被初始化为 0 和递减，没有找到装载转换超时的赋值。
- 定义了 `CheckTimeout()`，但没有找到其调用点。
- 首帧到达时直接完成 OP，不能代替所有 pending 情形的完成和超时处理。

### 影响

等待条件不满足时，状态转换缺少与 SSC 等价的超时闭环。只看到了一个 pending 字段，不代表转换治理已经完整。

## 8. DC 看门狗被两个上下文推进

### SSC 原行为

`ECAT_CheckTimer()` 在 1 ms 路径调用 `DC_CheckWatchdog()`；普通错误检查读取相应结果，不再额外递增一次计数。

来源：[ecatappl.c：349～360](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatappl.c#L349-L360)。

### 当前行为

- `EcatDevice::OnTick1ms()` 调用 `sync_.Tick()`。
- `EcatStateMachine::CheckErrors()` 在 DC 且输出运行时，又调用 `sync_->Tick()`。

### 影响

推进计数的频率不再只有 1 ms，而是还受业务 Poll 调用频率影响。DC 看门狗可能提前判断失败。非 DC 路径不能据此被认定为本次刷屏的直接原因，但这是已确认的 DC 行为差异。

## 9. 邮箱缓冲满状态位定义错误

| 实现 | 32 位 SM 控制/状态快照中的掩码 |
|---|---|
| SSC `SM_STATUS_MBX_BUFFER_FULL` | `0x00000800` |
| 当前 `sync_manager_reg::kStatusMbxFull` | `0x00000100` |

来源：[esc.h：171](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/esc.h#L171)。

当前定义位于 `core/spec/ecat_registers.hpp`，被 `core/mailbox/ecat_mailbox.cpp` 的 `IsSendBufferFull()` 与 `IsReceiveBufferFull()` 使用。

状态字节位于整个 DWORD 的 bit8～15，但“缓冲满”不是状态字节 bit0；SSC 使用 bit3，因此整个 DWORD 对应 `0x0800`。当前位置和掩码不能因为状态字节从 bit8 开始就直接取 `0x0100`。

该错误影响邮箱接收是否完整、发送是否被主站读走等判断，会导致 CoE/SDO 通信异常。它不是 0x0050 的组成位，不能将两者混称为同一个事件。

## 10. 其他已见差异与风险

### 10.1 PDI 与线程共用完整事件处理函数

当前 `Poll()` 与 `OnPdi()` 都调用完整的 `HandleAlEvents()`，其中包括 AL 状态转换、SII 命令和邮箱收发。

SSC 的 `PDI_Isr()` 主要处理同步与过程数据，低优先级 AL/邮箱流程由 `ECAT_Main()` 推进。当前上下文分工已改变；如果线程处理过程中被 PDI 打断，必须有相应的重入/所有权约束，不能因为函数相同就认为行为等价。

来源：[ecatappl.c：482～585](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatappl.c#L482-L585)。

### 10.2 SM 检查只剩部分条件

SSC 的 `CheckSmSettings()` 还检查方向、缓冲模式、地址范围和运行中地址一致性。当前方法主要检查使能与长度，对照覆盖不完整。

`EcatProcessData::ValidateAreas()` 虽然定义了区域重叠检查，但本次没有搜索到调用点。存在一个检查函数，不等于初始化流程已经执行该检查。

### 10.3 INIT 初始化缺少部分硬件状态重置

SSC 的 `ECAT_Init()` 读取 SM 数量和 DPRAM 能力、禁用全部 SM、初始化邮箱并重置 AL Event Mask。当前 `EcatStateMachine::Reset()` 主要复位软件字段并发布状态，没有对应完整流程。

来源：[ecatslv.c：2555～2625](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c#L2555-L2625)。

需要核对这些硬件准备在当前其他初始化层是否有等价实现，不能擅自假设由硬件复位自动代替所有步骤。

### 10.4 Recheck 的退态行为不一致

当前 Recheck 在 OP 检查失败时，既调用 `LeaveOp()` 又调用 `LeaveSafeOp()`，随后却把软件状态设为 SAFEOP。

`LeaveSafeOp()` 会关闭输入更新并禁用过程 SM，因此可能出现“报告 SAFEOP，但输入路径已经关闭”的状态不一致。这需要与 SSC 相应错误回退分支分别对照，不能仅按状态枚举值向下减一来实现。

## 11. 对本次 0x0050 的判断

**与持续刷屏直接相关的源码缺口：**

1. SM Change 没有完整确认剩余 SM 通道的激活变化。
2. 看门狗事件没有独立、覆盖当前状态的确认路径，读取 0x0440 又被输出运行条件限制。
3. 非零事件值每次 Poll 都打印，所以未确认的事件持续显示。

**会同时妨碍建链和正常通信的其他明确差异：**

- PDO 映射生成晚于 SM 长度校验。
- 过程数据看门狗配置判断缺失，首帧标志清零位置错误。
- pending 转换超时机制没有接完整。
- DC tick 重复推进。
- 邮箱缓冲满状态位掩码错误。

这些结论来自源码对比。仅凭用户给出的重复事件日志，还不能确定本次现场是哪一个 SM 通道未确认、是否存在当前有效的看门狗超时，以及当时具体处于哪一个 AL 状态。

不能将删日志、减少日志频率、改 PHY、或延长主站超时当作上述处理缺口的修复。

## 12. 后续修正顺序

以下只记录范围，不在本次实施：

1. 修正寄存器语义和邮箱满位掩码，避免后续判断继续建立在错误常量上。
2. 对齐 SM Change 的确认读取、检查范围与硬件 SM 数量，不能只扫描固定的两个邮箱通道。
3. 区分看门狗事件确认与错误判断，补齐时间/触发配置检查和首帧标志分支。
4. 调整 PREOP→SAFEOP 的 PDO 计算顺序，并接通现有区域校验。
5. 接完整 pending 的装载、推进、完成与超时，DC 看门狗只保留一个计时推进源。
6. 对齐 ISR/线程分工以及错误退态的收尾行为。
7. 用户编译、上板后对照实际 AL 状态、状态码与事件位，确认不再重复处理未确认来源。

**本文不宣称问题已修复，也不把源码对比等同于运行验证。**
