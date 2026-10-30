# EtherCAT 从站与 SSC 整体静态审查

日期：2026-10-31。范围：当前 C++ 从站协议核心、ESC 事件入口、HPM HAL 与 SII 后端接口。

## 1. 结论与证据边界

当前已得到用户提供的双向 32 字节 PDO 运行证据：从站收到八个 `0～7`，主站收到八个同步递增的计数值。这说明本次 SM 同步、固定映射的通信路径已通；不能据此推定错误恢复、SDO 写入、SDO Info、DC、邮箱重发和 SII 写入全部正确。

本次只阅读源码、追踪调用及数据布局、核对常量和原版分支，并生成本文。没有修改生产源码，没有编译、执行测试、烧录或进行新的硬件验证。文中的“确认”表示源码能证明存在对应缺陷，不表示已经在板上复现。

审查发现的优先风险是：

1. 对象字典用协议紧凑偏移访问自然对齐的 C++ 成员，存在字段错位及非对齐类型访问；`0x10F8` 还直接别名到其他对象数据。
2. 状态转换没有按 SSC 区分校验范围，INIT 恢复、SM Change 和 pending 转换收尾仍有错误。
3. 邮箱直接发送后的缓冲没有正确保留，重发握手不完整；线程和 ISR 还可以进入同一组可变状态。
4. SDO Info 的布局、列表类型、长度和名称边界不正确。
5. DC 对外宣称支持，但看门狗阈值、锁存模式和用户配置通知尚未完整接入。

### 1.1 参考版本

通过 GitHub 连接核验远端 `HNUST-Dust/hpm_support` 的 `codex/dust-hpm5e31` 分支，审查时 HEAD 为：

`d1e9fbfbdca40c3ecc965795917735cb92bd6fd2`

本地参考目录：`temp/hpm_support-codex-dust-hpm5e31/samples/ecat/EtherCAT/ssc/`。将该目录全部 21 个文件的 Git blob SHA 与远端同提交目录列表逐项比对，均一致。因此下文的本地 SSC 行号对应此固定提交。参考目录不是独立 Git 工作树，不能使用在其目录内执行 `git rev-parse HEAD` 得到的父仓库提交冒充 SSC 版本。

主要参考入口：

- [SSC ecatslv.c：状态转换、SM 校验、同步及错误处理](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatslv.c)
- [SSC ecatappl.c：PDI/SYNC 入口、定时器、EEPROM 命令](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/ecatappl.c)
- [SSC objdef.c：字典访问、权限、对象列表、名称](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/objdef.c)
- [SSC sdoserv.c：SDO 与 SDO Info](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/sdoserv.c)
- [SSC sdoserv.h：报文结构、权限位及列表编号](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/sdoserv.h)
- [SSC mailbox.c：缓冲所有权和重发](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/mailbox.c)
- [SSC coeappl.c：标准对象与专用访问处理](https://github.com/HNUST-Dust/hpm_support/blob/d1e9fbfbdca40c3ecc965795917735cb92bd6fd2/samples/ecat/EtherCAT/ssc/coeappl.c)

### 1.2 审查对应表

| 当前模块 | SSC 对应内容 | 本次重点及问题编号 |
|---|---|---|
| `core/ecat_device.cpp` | `ECAT_Init`、`ECAT_Main`、`PDI_Isr`、`Sync0_Isr`、`Sync1_Isr`、`ECAT_CheckTimer` | 事件上下文、同步分发、复位：A02、C01～C03、F01～F02 |
| `core/al/ecat_state_machine.cpp` | `AL_ControlInd`、`AL_ControlRes`、`CheckSmSettings`、`StartInputHandler`、`StopInputHandler` | 转换范围、恢复、SM 区域、pending：A01～A05 |
| `core/al/ecat_sync.cpp` | `StartInputHandler`、`DC_CheckWatchdog`、SYNC ISR、标准对象处理 | DC 监测与配置：C01～C03 |
| `core/al/ecat_sm.cpp` | `GetSyncMan`、`Enable/DisableSyncManChannel` | 使能字节读确认、PDI 控制写入：A05、F03 |
| `core/model/ecat_dictionary.cpp` | `OBJ_Read`、`OBJ_Write`、`OBJ_GetEntryOffset`、`OBJ_GetObjectList` | 真实内存偏移、CA、权限：B01～B05 |
| `core/spec/ecat_object.hpp`、`ecat_application_objects.hpp` | `sdoserv.h`、`coeappl.c`、`digital_ioObjects.h` | 对外访问标志、名称与对象存储：B01～B03、E04 |
| `core/mailbox/ecat_mailbox.cpp` | `MBX_Main`、`MBX_CopyToSendMailbox`、`MBX_MailboxReadInd`、`MBX_MailboxRepeatReq` | 缓冲生命周期、重发、接收边界：D01～D03 |
| `core/mailbox/ecat_coe.cpp` | `COE_ServiceInd`、`COE_ContinueInd` | 服务路由及后续分片：E03、F01 |
| `core/mailbox/ecat_sdo.cpp` | `SDOS_SdoInd`、上传/下载/分段处理 | 长度、发送失败、分段、Abort：B05、G01～G03 |
| `core/mailbox/ecat_sdo_info.cpp` | `SDOS_SdoInfoInd`、`TSDOINFO*` | 布局、枚举、边界：E01～E04 |
| `core/mailbox/ecat_sii_service.cpp`、HPM HAL | `EEPROM_CommandHandler` 及板级 EEPROM 后端 | 字地址、传输宽度、定时落盘：H01～H02 |
| `core/pdo/ecat_process_data.cpp` | `APPL_GenerateMapping`、`PDO_*Mapping`、区域检查 | 固定映射计算、业务回调与区域检查：A05、B03、C02 |

这是一份跨模块行为差异审查，不是“全部 SSC 特性已逐行等价”的声明。FoE、EoE、未实现的动态映射等不因 SSC 文件中存在就自动成为本次必须新增的功能；应让设备能力声明与实际实现一致。

## 2. 已修复项，不再当作当前缺陷

| 项目 | 当前源码情况 | 证据范围 |
|---|---|---|
| expedited 应答截断 | `ecat_sdo.cpp` 的 `kExpeditedLength`、`kInitiateNormalLength` 已包含末尾 4 字节，总长度为 16 | 已读当前定义；用户随后报告连接及双向 PDO 成功 |
| CA 漏掉逻辑映射/分配表 | 字典 CA 读已调用 `MappingEntryOf`、`AssignmentEntryOf` | 解决的是从子索引 0 开始的这一条路径，CA 起始索引仍有 B04 |
| SM_ACT 确认地址读成 `+2` | 当前使用 `reg::kSyncManagerEnableOffset = 6` | 地址已改对；状态调度条件仍有 A02 |
| 校验参数 1/3 与数量语义混用 | 两处调用目前传 `kMailboxIn + 1` / `kProcessIn + 1`，即 2/4 | 注释仍称“最后索引”，应改为不含上界的通道数量，避免回归 |
| 邮箱满状态掩码 | 当前 `kStatusMbxFull = 0x00000800` | 与 SSC 一致 |
| 正常看门狗检查错误清首帧 | 清首帧已放回看门狗异常且输出未运行分支 | 不代表 DC 和 watchdog 配置校验已经完整 |

## 3. 状态机与 SM

### A01【高】所有不同状态转换统一先验 SM，导致 INIT 恢复被配置错误阻塞

**位置：** `ecat_state_machine.cpp`，`Transition()`，约 281～330 行。

**SSC：** `AL_ControlInd()` 的 `stateTrans` 分支（ecatslv.c 1559～1596）：进 PREOP/同 PREOP 检查邮箱；PREOP→SAFEOP 先生成映射再检查全部；SAFEOP/OP 相应分支检查全部。返回 INIT 的分支不执行这段 SM 校验。

**当前：** 仅根据 `target == PreOp` 决定 2 或 4，连 `target == Init` 也检查四个通道；失败直接返回，执行不到 `LeavePreOp()` / `BackToInit()`。之前日志中 INIT 请求连续报 `0x001e`、`0x0016` 与此路径一致。

**修改：** 按 `(current, target)` 明确选择“无校验/邮箱校验/全部校验”，不要只按目标状态二分。降 INIT 必须能进行清理；保留 SSC 对非法跨级升态和错误确认的约束。`Configure()` 也只应放在需要重新生成映射的转换中，不能对 OP→SAFEOP 一概重新配置。

### A02【高】SM Change 缺少 SSC 的门控，并会重复处理同一 AL Control 快照

**位置：** `ecat_device.cpp:361` `HandleAlEvents()`；`ecat_state_machine.cpp:911` `Recheck()`。

**SSC：** ecatslv.c 2695～2711：处理 AL Control 时从本地事件快照同时去掉 AL_CONTROL、SM_CHANGE；单独处理 SM_CHANGE 需要满足“无 pending、无 AL 错误、当前不是 INIT”。

**当前：** 两个独立 `if` 复用同一快照；`0x0011` 会先转换再立即 Recheck；`Recheck()` 在 INIT 也检查邮箱/PDO，没有错误锁存和 pending 限制。主站在 INIT 重配 SM 时会被过早判成非法。

**修改：** 对照 SSC 的全部判断恢复门控；AL Control 已覆盖校验时，不再消费旧快照中的 SM Change。无需为了消灭日志而在 INIT 强制校验邮箱。修正“SSC 不跳过 INIT”的错误注释。

**说明：** `0x0010` 是事件，不是 AL 错误码。重复日志既可能是挂起事件，也可能是新写入；仅凭打印次数不能认定链路坏。

### A03【高】旧 pending 转换未随新请求或退态取消，首帧可把状态直接改回 OP

**位置：** `HandleControl()`、`BackToInit()`、`MarkFirstOutputs()`（约 641 行）、`RequestLocalState()`。

**SSC：** `AL_ControlInd/AL_ControlRes` 维护具体转换标识及等待条件，完成 OP 前仍检查当前转换、DC 等前置条件。

**当前：** `transition_pending_` 在等待 OP 时置位；之后成功转 INIT/PREOP 或本地错误退态并不统一清除它。`MarkFirstOutputs()` 只看 `pending_target_ == Op`，就直接置 `state_=Op`、清错误并打开输出，没有检查当前仍为 SAFEOP、是否超时、同步是否健康。

**修改：** 新 AL 请求接管旧请求时，明确取消/替换旧 pending；退出当前转换和本地故障时统一清理目标与计时器。首帧只报告事件，由状态机验证当前转换后完成 EnterOp，不能自行清掉无关错误。

### A04【高】SM 校验失败后的状态及资源不一致；SAFEOP 进入失败缺少回滚

**位置：** `Transition()` 失败收尾、`Recheck()`、`EnterSafeOp()`。

**SSC：** ecatslv.c `AL_ControlInd()` 失败收尾及 `StartInputHandler/StopInputHandler` 让 AL 状态与邮箱、输入、输出开关一致。

**当前：** OP 下 Recheck 失败调用 `LeaveOp()` 和 `LeaveSafeOp()`，却发布 SAFEOP；此时输入已经关闭、SM2/SM3 已被 PDI 禁用。PREOP 邮箱失效时又不退 INIT。另一条路径中 `EnterSafeOp()` 先使能 SM、写初始输入和事件掩码，再 `PrepareSm()`，后者失败可留下已经生效的硬件副作用。

**修改：** 以 SSC 对具体失败类别的退态策略决定最终状态；进入动作先完成可失败校验，再提交 SM/掩码；失败回滚已启用的资源。不要用统一“退一级”的方法替代转换表。

### A05【高】SM 地址冻结、重叠检查和实际 DPRAM 边界未完整落实

**位置：** `CheckSmSettings()`、`EnterSafeOp()`、`ecat_process_data.cpp:122` `ValidateAreas()`。

**SSC：** `CheckSmSettings` 开头校验最大地址定义不超实际 DPRAM；SAFEOP/OP 要求 SM 地址仍等于保存的 `nEscAddrInputData/OutputData`。`StartInputHandler` 读缓冲模式，检查过程数据区与邮箱、彼此之间重叠。

**当前：** PREOP 检查起始地址，非 PREOP 直接放行地址；没有已提交地址快照。`ValidateAreas()` 没有调用点；实现固定按三缓冲且未完整使用输入跨度判断所有区间。初始化只取实际 SM 数，未建立实际 DPRAM 上界。固定 `0x2fff` 只约束起点，不能证明起点加缓冲跨度合法。

**修改：** 在进入 SAFEOP 时保存验证后的 SM2/SM3 地址和模式；在 SAFEOP/OP 对比快照。使用实际硬件容量与实际 1/3 缓冲跨度，采用足够宽整数计算所有区间的结束位置，提交前统一验证不重叠、不过界。不存在的成员应新增，不能用“非 PREOP 放行”近似。

## 4. 对象字典与 SDO 数据一致性

### B01【严重】协议偏移被直接用于自然对齐结构体，读写了错误字段

**位置：** `ecat_dictionary.cpp:108` `ByteOffsetOfEntry()`、`DataOf()`；`ecat_application_objects.hpp` `ObjectData`。

**SSC：** `OBJ_GetEntryOffset()` 操作与 SSC 打包规则匹配的对象存储。该偏移是存储模型的结果，不能脱离布局照搬。

**当前：** 子索引 1 一律按对象起点 `+2` 计算，后续累加描述位长；但 `ObjectData` 和 `SyncParameters` 是普通 C++ 结构。`uint16_t` 后的 `uint32_t` 常有对齐空隙。

例如 `input_counter_count` 后的 `uint32_t input_counter[8]`、`identity_sub_count` 后的 `vendor_id`，不能假定紧邻 `+2`。`SyncParameters` 的 `sync_types_supported` 后也有 32 位字段对齐空隙。当前再将偏移指针转成 `uint32_t*` 解引用，还引入非对齐访问问题。

**影响：** SDO 身份、计数器、同步参数可能读错；写参数可能损坏邻近字段。PDO 当前走独立缓冲回调，因此 PDO 正常不证明字典成员访问正确。

**修改：** 为对象子项建立指向真实成员的绑定或基于 `offsetof` 的存储描述；协议串行化仍按协议位宽进行。内存布局与线上布局分别计算。不要仅给整个 `ObjectData` 加 packed 后继续任意宽类型解引用；这样仍存在对齐和严格别名风险，也不解决缺失对象前门。

### B02【严重】时间戳对象没有前门，实际别名到 ObjectData 起点

**位置：** `ecat_dictionary.cpp` 的 `kSlots`，`index::kTimestamp` 项约 50 行。

**SSC：** `0x10F8` 经时间戳相关处理读写实际时间值，参见 `coeappl.c` 标准对象定义与 Read/Write 处理。

**当前：** 此项为 `DataKind::Member`、`data_offset=0`，访问钩子为空；`DataOf()` 返回整个 ObjectData 起点。读 8 字节会把设备类型及相邻数据当时间戳；写 8 字节会覆盖它们。`EcatSync::timestamp_` 只在同步类内部更新，没有接到该对象。

**修改：** 为 `0x10F8` 绑定实际访问实现，读时获取一致的 DC/软件时间快照，写时按 SSC 定义更新允许部分。读写钩子需要分别接入；只补读钩子不够，因为当前 `Write()` 根本没有调用 slot 的访问钩子。

### B03【高】对外访问位定义与 SSC 不兼容，运行态写权限也没有生效

**位置：** `ecat_object.hpp` 的 `access`；`AllowedInCurrentState()`、`Write()`；SDO Info 返回 `entry->access_flags`。

**SSC：** sdoserv.h：读 PREOP/SAFEOP/OP 为 `0x01/0x02/0x04`，写为 `0x08/0x10/0x20`；RxPDO=`0x40`，TxPDO=`0x80`。`OBJ_Read/Write` 按当前 AL 状态检查相应权限位。

**当前：** `kRead=1`，写权限从 `2` 起并包含 Boot，Tx/Rx 映射位反向；这些内部标志又原样发到 SDO Info。`AllowedInCurrentState()` 对带读权限的条目只判断状态 ≥PREOP，导致声明“仅 PREOP 可写”的 `0x1C32/33:01` 在 OP 仍可写。

**修改：** 采用标准线上位定义，或建立显式内部标志到标准位的转换；读与写分别按 AL 状态判权限，不能复用“有读权限即可”的函数。修正所有描述表引用和列表筛选。映射位变动会影响 E02，需一并修改，不能只换常量。

### B04【高】CA 忽略起始子索引，并删除保留字段，返回长度与内容不一致

**位置：** `Read()` CA 循环约 498 行、`Write()` CA 循环；`TotalLengthOf(slot, sub_index)`。

**SSC：** `OBJ_Read/Write` 从请求的子索引开始，CA 子索引 0 用 16 位计数；保留/对齐字段按对应协议布局占位。

**当前：** 长度函数会按 `sub_index=1` 不计子索引 0，但读写循环都从 0 开始。以 `0x1C12` 的 CA 子索引 1 为例：上层给对象长度 2，实际读仍写 4，可能发送错误内容或产生错误码。`0x1C32/33` 中非零位长但无访问权限的保留项被 `continue` 删除；TotalLengthOf 却仍计入位长，Normal Upload 又按 object_length 发包，未用 written 核实，可能把旧缓冲内容发出去。

**修改：** 统一请求起始子索引、逐项线上偏移、实际写入长度；保留字段按 SSC 写零/占位，不能压缩；构包前确认所需长度与 written 一致。CA 写必须逐项检查权限和状态。只读 CA 写目前可能“跳过全部字段后返回成功”，应按 SSC 对该对象的规则拒绝。

### B05【高】单项 SDO 写入没有拒绝短数据，可能读取请求有效范围之外

**位置：** `ecat_dictionary.cpp` `Write()`，长度判断只检查 `size > length + (length & 1)`。

**SSC：** `SDOS_SdoInd` 与 `OBJ_Write` 共同检查对象长度、请求长度及特殊类型规则。

**当前：** 目标为 32 位而传入 1～3 字节时，随后仍调用 `ReadLe32()`；64 位亦然。有效数据不足也可能返回成功。缓冲池通常还有物理空间，因此不一定立刻崩溃，但读到的是无效/旧字节。

**修改：** 对固定宽度对象先验证有效数据长度符合协议；字符串、域、CA 单独执行各自规则。跨 SDO 服务与字典入口建立同一长度契约，不能只依赖正常主站。

## 5. 同步模式与 DC

### C01【高】DC 看门狗和同步序列阈值没有装载，却对外宣称支持 DC

**位置：** `ecat_sync.cpp` `ResetParameters/PrepareSm/StartSm/Tick`；`ecat_sync.hpp` 对应成员。

**SSC：** ecatslv.c 1039～1080、1159～1239 根据 Sync0/1 周期装载 SM/Sync0 比率、锁存点、Sync0/1 看门狗阈值；SAFEOP→OP 还检查相应同步运行条件。

**当前：** `sync0_wd_value_`、`sync1_wd_value_`、`sm_sync0_value_` 只有初始化为 0 的写入，没有按 DC 配置计算的路径。`Tick()` 在阈值为 0 时将 DC 当作运行正常；`EnterOp()` 只等首帧输出，不验证 DC 已稳定。输入独立使用 DC 的判断也只根据输出 sync_type 设置 `dc_sync_active_`。

**修改：** 完整落实周期、同步类型兼容性、锁存、序列及阈值装载，再允许相应 DC 组合进 OP；仅输出侧判断 DC 不够。若暂不支持某模式，应同步收紧能力声明和请求校验，不能仍报支持。

### C02【高】PDO 输入提交没有按同步类型选择唯一触发源

**位置：** `ecat_device.cpp` `HandleAlEvents()`、`OnSync0/OnSync1()`。

**SSC：** ecatappl.c `PDI_Isr` 只有输入 sync_type 为 SM/SM2 同步才在该路径映射输入；`Sync1_Isr` 在 DC SYNC1 且锁存参数满足条件时映射输入。

**当前：** 收到 SM2 时只要输入运行就 `PublishInputs()`，DC 模式同样执行；SYNC0 还会再次提交。`OnSync1()` 只复位计数，从不提交输入。仅输入 SM 同步时，SM3 分支只读确认，也没有按相应模式刷新下一帧。

**修改：** 按输入同步类型和锁存参数分配唯一发布点；恢复 SYNC1 锁存路径；无输出的输入同步需要自己的提交路径。普通 SM 模式仍保持已验证的每周期 32 字节行为。

### C03【高】同步参数写入没有通知同步域，配置和监测状态脱节

**位置：** 字典 `Write()`；`EcatSync::sync_set_by_user_`、`SetSyncErrorLimit()`、`UpdateErrorStatus()`。

**SSC：** `coeappl.c` 对 `0x1C32/33`、`0x10F1` 的专用处理，以及 `COE_UpdateSyncErrorStatus`。

**当前：** `sync_set_by_user_` 没有置 true 的调用路径；`SetSyncErrorLimit()`、`UpdateErrorStatus()` 没有调用点。主站写了同步类型后 PrepareSm 仍自动推导覆盖；写 `0x10F1:02` 不更新实际阈值。周期测量字段有成员但没有完整测量推进，不能将 `:08` 写成功等同于测量已实现。

**修改：** 建立对象写入到同步域的明确通知，按协议校验类型组合和状态，更新阈值及“用户已配置”标志；在周期执行点维护错误状态和测量结果。先修 B01/B03，再接这些通知，否则通知可能读取错误字段。

## 6. 邮箱传输与重发

### D01【高】直接发送应答后没有保留 read_frame，Service 随即释放缓冲

**位置：** `ecat_mailbox.cpp` `Submit()`、`Service()`、`IsQueued()`、`OnSendMailboxRead()`。

**SSC：** `MBX_CopyToSendMailbox` 保存当前发出帧到 `psReadMbx`；主站读走后转到 `psRepeatMbx`，再根据别名关系释放。mailbox.c 开头专门说明所有权流程。

**当前：** `Submit()` 直接 `Transmit()` 成功就返回 Queued，却没有 `read_frame_=stable`。随后 Service 检查 `IsQueued(frame)`，该函数只搜队列；直接发送的帧不在队列，就被 Release。主站通常仍能读到 ESC 已复制的数据，所以基础通信可以正常，但软件已丢失重发所需帧，也可能立即复用该槽。

**修改：** 明确 Submitted/Queued/InFlight/Repeat/Stored 的所有权；直接发成功和队列发成功都保存当前帧，Service 不得释放已经转移所有权的缓冲。不要只把 IsQueued 改成永远 true。失败入队、重发、Stop 路径都要按同一所有权关系清理。

### D02【高】重发把请求位当电平，缺少 toggle 比较和 ACK 写回

**位置：** `IsRepeatRequested()`、`Service()`、`OnRepeatRequest()`；`ecat_sm.cpp` `WritePdiControl()`。

**SSC：** ecatslv.c 2751～2769：请求位与 `bMbxRepeatToggle` 不同才处理，结束后把确认状态写到 SM1 PDI 控制的 Repeat ACK 位；`MBX_CopyToSendMailbox` 负责保存正在发送帧。

**当前：** 请求位只要为 1 就每次 Service 都处理；1→0 的下一次请求被忽略。`repeat_toggle_` 只翻转，不与请求比较、也不写 ACK。重发后清 `repeat_frame_` 却未转入 read_frame。启停 SM 时 `WritePdiControl(...,0/1)` 覆盖整个 PDI 字节，还可能清掉 Repeat ACK。

**修改：** 按 toggle 边沿比较，完成后写回对应 ACK；成功重发帧转为 in-flight，Transmit 失败不能丢掉所有权。PDI 使能/禁用只修改 disable 位，保留 ACK 等其他合法位；具体写宽遵守 HPM 访问约束。

### D03【高】Receive 缺少运行态、最小帧和长度溢出保护

**位置：** `ecat_mailbox.cpp` `Receive()`；`ecat_coe.cpp` `Service()`。

**SSC：** `MBX_CheckAndCopyMailbox/MBX_MailboxWriteInd` 与 `COE_ServiceInd/SDOS_SdoInfoInd` 分层检查邮箱长度和服务最小尺寸。

**当前：** Receive 没有 `running_` 判断；`payload_length + 6` 先转换成 uint16_t，非法大长度可能回绕为很短帧；之后仍读协议字节。CoE 路由读取偏移 6 的两字节也没有先验证 frame 长度至少为 8。未运行时旧事件还可能访问保存地址。

**修改：** 宽整数先验证加法、邮箱上限、至少 6 字节头，再读取类型；CoE/SDO/Info 各自校验完整所需请求字段。非法请求按 SSC 返回适当错误，不能靠截断后继续读取解决。

## 7. SDO Info

### E01【高】列表及对象/条目描述线上布局不符合 SSC

**位置：** `ecat_sdo_info.cpp` `HandleListFragment/HandleObjectDescription/HandleEntryDescription`。

**SSC：** sdoserv.h 的 `TSDOINFOHEADER` 是 InfoHead 2 + FragmentsLeft 2，随后：

- 列表首片：ListType 2，然后索引数组；
- 对象描述：Index 2、DataType 2、ObjFlags 2，然后名称；
- 条目描述：Index 2、SubIndex/ValueInfo 2、DataType 2、BitLength 2、ObjAccess 2，然后可选信息/名称。

**当前：** `kDataOffset=12` 已经越过公共 Info 头，却又用 `kListHeaderSize=4` 添加空位；对象描述把 DataType/flags 放前面、Index 放后面，条目描述也没有按上述顺序构造 Index/Info。对象类型还根据“总位长≤8”粗略选择 U8/U32，变量 TotalLengthOf 返回 0 导致 32 位变量可能被描述成 U8。

**修改：** 先按结构体算出字段位置表，再逐字段构造；保留请求 Index、SubIndex 和 ValueInfo 的语义。对象类型来自对象描述，不能通过总字节数猜测。与 SSC 比较实际输出字节，不只比较字段名。

### E02【高】列表编号错位、缺少 type 5，全部对象列表实际未返回全部对象

**位置：** `kMaxListType=4`、`HandleList()` 与字典 `CollectIndices()`。

**SSC：** 线上 ListType=0 返回数量，1=全部，2=RxPDO，3=TxPDO，4=Backup，5=Settings。SDOS_SdoInfoInd 先将非零线上编号减 1，再传给 OBJ_GetObjectList；内部 0 才是全部。

**当前：** 把线上编号直接传 CollectIndices，内部只在 0 返回全部；请求 type 1 会被映射标志筛选。数量列表仅返回四类，且从内部 1 开始计数，漏掉“全部”；另有 B03 的 Tx/Rx 位反向。

**修改：** 将线上编号与内部筛选编号显式区分，支持 0～5 并返回五类数量；修复 B03 后再按标准位筛选。对象枚举结果按索引升序输出，当前 kSlots 的存储顺序并非升序，不能直接依赖表顺序进行 next_index 分片。

### E03【高】列表应答长度少计字段，容量计算也可能越过帧末尾；后续分片未接入

**位置：** `HandleListFragment()`、`EcatCoe::Continue()` 及邮箱发送完成路径。

**当前计算可直接证明：** 数据写入位置为 `kDataOffset + kListHeaderSize + 2*n`，即从 16 开始；最终长度却为 `6+2+4+2*n = 12+2*n`。写出的索引尾部有 4 字节不在发送长度内。writable 按 `capacity-12` 算，而实际从 16 起写，满容量时可越界 4 字节。`Continue()` 存在但当前邮箱/Device 没有调用推进它。

**SSC：** SDOS_SdoInfoInd 按实际发送邮箱尺寸计算首片/后续片容量，并通过 FRAGMENTS_FOLLOW、COE_ContinueInd 继续。

**修改：** 修 E01 后从统一偏移推导发送长度和 writable，禁止手工重复扣头；实现发送完成驱动的后续片推进。当前 19 个对象不一定触发分片，不代表该功能已正确。

### E04【严重】名称查找越过 0xff 终止符，且终止字节写入没有容量保护

**位置：** `HandleEntryDescription()` 名称遍历及两个描述处理函数末尾。

**SSC：** `OBJ_GetDesc()` 识别终止标记、缺失名称，并使用默认名称规则；SDOS_SdoInfoInd 先检查应答尺寸。

**当前：** 跳过第 N 个名称时只找 `\0`，不检查 `0xff`；`0x6000/7010` 名称仅含对象名与终止标记，但对象有八个子项，访问较高子索引可扫描到数组之外。复制名称有容量判断，随后 `frame->data[name_offset + name_length] = '\0'` 却无条件执行，名称过长时仍可写越界。入口也只检查 total≥12，没有校验对象请求所需 14 字节、条目请求所需 16 字节及 total≤frame.length。

**修改：** 使用带边界的名称访问，遇终止标记返回默认名称；先验证子索引再访问描述；每个请求类型单独检查长度。先计算完整应答可用空间，再决定发送名称或返回错误，不能只保护 memcpy。

## 8. 调度、生命周期和硬件访问

### F01【高，运行表现待验证】线程与 PDI ISR 执行同一完整事件分发，没有临界保护

**位置：** `EcatDevice::Poll/OnPdi/HandleAlEvents`、邮箱 pool/queue；`EcatEsc::MaskEvents/UnmaskEvents`。

**SSC：** 线程 ECAT_Main 处理 AL/邮箱管理，PDI ISR 主要处理过程数据和同步；邮箱发送/重发关键操作使用 ENTER_MBX_CRITICAL 等保护。

**当前：** Poll 和 OnPdi 都执行 HandleAlEvents；可并发修改状态机、邮箱计数、队列头尾、池位图、SII 命令。虽然 HAL 有 MaskEvents 接口，core 没有调用它。单核也可能在“读位图→修改→写回”间被 ISR 抢占；不能因为没有两个 CPU 就认为安全。

**修改：** 明确一个上下文拥有 AL/邮箱/SII 可变状态，ISR 只处理必须即时完成的 PDO/SYNC 并提交事件；必要的共享操作采用足够短的临界区。仅 volatile 不能保证所有权或复合操作正确；ESC 中断屏蔽也不会自动屏蔽 Zephyr 1 ms timer ISR。

### F02【高】Disable/Reset 未完整复位协议会话，复位后事件可能永久停用

**位置：** `EcatDevice::Disable/OnReset/Init`；`LeavePreOp/BackToInit`。

**SSC：** ECAT_Init、StopMailboxHandler、SDOS_ClearPendingResponse 等清理模块状态，硬件恢复后需重新建立运行条件。

**当前：** Disable 只停事件、邮箱和 sync，没有重置状态机/SDO/Info；Init 可重用同一实例。OnReset 停止所有事件后只 Reset 状态机，不清 ready_，也不重新 StartEvents；轮询还认为设备 ready，但计时和 IRQ 已停。退 INIT 不取消 SDO 分段会话及 Info 分片。

**修改：** 定义统一停止/复位顺序，清除 pending、分段会话、分片、队列及有效标志；硬件复位选择明确的“重新初始化后恢复”或“置未就绪等待重开”，不得 ready=true 且事件永久关闭。不能仅加 StartEvents 而忽略已丢失的硬件配置。

### F03【条件风险】HAL 的部分 DWORD 读写可能碰到额外寄存器，需按真实驱动契约核实

**位置：** `ecat_sm.cpp` PDI 控制 DWORD 回写；`ecat_hal_hpm.cpp` `Read/Write/ReadAlEvent`。

通用 Read 有非对齐补偿，ReadAlEvent 却直接走 hpm_esc_read；非对齐 Write 采用读改写，SM 使能也回写整个控制/状态/使能/PDI DWORD。寄存器具有读确认、只读、写专属含义，不能只按普通 RAM 处理。

**SSC 对照：** HW_Esc* 是端口约束，SSC 自身也可能使用 DWORD SM 访问，因此“读写 DWORD”本身不能直接判错。本项需要沿当前实际 SDK 的 hpm_esc_read_block/hpm_esc_write、芯片能力宏和 PDI 权限确认；不在缺证据时断言会清空 SM 主站配置。

**修改方向：** 记录哪些寄存器需要何种访问宽度及副作用；统一封装相同规则。需要改 SDK 时以可复现补丁交付，不直接让 core 引用厂商 API。

## 9. SDO 服务其他问题

### G01【高】回复提交失败被当作成功吞掉

**位置：** `EcatSdo::BuildUpload/BuildDownload/Handle*Segment()`、`Reply()`。

**SSC：** SDOS/COE 与邮箱服务维护未能发送应答的状态及继续发送机制。

**当前：** Reply 返回 sink 的错误，但多数构包函数调用后固定返回 0；队列满或 Submit 失败时，协议层报告成功，邮箱随后可能释放帧，没有保存待发送响应。简单低负载请求通常不触发。

**修改：** 区分 SDO abort code、邮箱提交结果与异步待发送状态；提交失败要保留完整应答及其所有权，或明确让上层执行受支持的失败流程。不能直接把 mailbox 的 8 位错误当作 32 位 SDO abort code。

### G02【中】上传短末段和下载分段确认的报文长度与 SSC 不一致

**位置：** `HandleUploadSegment()`、`HandleDownloadSegment()`。

**SSC：** sdoserv.c `SdoUploadSegmentInd()`：末段 size<7 时编码 `7-size`，并将邮箱载荷设为固定 `SEGMENT_NORM_RES_SIZE`，数据区补齐。

**当前：** 头部编码了 `7-chunk`，但总长仍为 `kSdoOffset + 1 + chunk`，未补齐七字节数据区。

下载分段确认也仅发送 CoE 头加一个命令字节，总长 9 字节；SSC 的 `SdoRes()` 对 DOWNLOADSEGMENTREQ 使用 `DOWNLOAD_NORM_RES_SIZE=10` 字节邮箱载荷，即含邮箱头 16 字节。

**修改：** 按 SSC 的短末段布局补齐并清零 padding，发送长度与头编码共同修改。当前字典对象大多不超过一个邮箱，此缺陷属于分段功能路径，不能称为已在当前 PDO 中复现。

**不列为缺陷：** `remaining == chunk_capacity` 时先发满段、再发结束段，与本次 SSC 的严格 `<` 分支一致；不能仅凭个人习惯改成 `<=` 并称为对齐。

### G03【中】SDO Abort 构包的 CoE 服务号及命令位与 SSC 不一致

**位置：** `EcatSdo::BuildAbort()`。

**SSC：** `SdoRes()` 的 abort 分支将 CoE 服务设置为 `COESERVICE_SDOREQUEST`，命令设置为 `SDOSERVICE_ABORTTRANSFER`，并写入四字节 abort code。

**当前：** BuildAbort 调用 `SetCoeResponse()`，把 CoE 设成 SDO Response；命令又额外 OR `kSizeIndicated`，得到 `0x81`，而 SSC 此处是 `0x80`。

**修改：** 独立构造 Abort 的 CoE/SDO 头，不复用普通上传成功应答的服务号；按 SSC 保留索引、子索引及标准 abort code。主站是否容忍当前差异需实测，本文不把它说成必然导致拒收。

**排除误报：** 本次 SSC 的 SDOS_SdoInd 同样没有显式主站 Abort 分支，而是默认返回未知命令；因此不能仅以“没有 Abort case”为由声称当前与该 SSC 不一致。异常分段与退出会话的清理仍应结合 F02 和具体错误路径分别核对。

## 10. SII 与 Flash 模拟接口

### H01【高，启用模拟路径时】读写接口长度与后端固定搬运宽度不一致

**位置：** `EcatEscHpm::ReadSiiWord/WriteSiiWord`、`kSiiChunkSize=8`；`EcatSiiService::HandleRead/HandleReload`。

**已核实后端：** `E:/Zephyr_HPMicro/sdk_glue_user/drivers/ethercat/ecat_e2p_emulation.c` 的 ecat_flash_eeprom_read/write 使用 `EEPROM_WRITE_SIZE` 作为长度；对应头默认定义为 8。HAL 循环忽略本次剩余 length，始终调用固定宽度后端。

因此若该宏最终为 8，`ReadSiiWord(..., alias[4], 4)` 仍要求后端向四字节缓冲搬八字节；一般长度非 8 倍数也有越界风险。循环将字地址 `word_address` 加字节 offset，跨块时单位也不一致。小 EEPROM reload 使用的 `alias[4]` 就是实际存在的四字节调用点。

**SSC：** EEPROM_CommandHandler 按协议读宽度及字地址访问介质；SSC 默认 EEPROM_WRITE_SIZE 是 2。当前板级头改成 8 是另外一套后端约定，不能假定两者天然一致。

**修改：** 先明确后端 e2p 的索引单位及单条记录大小，再让 HAL 用足够大的 scratch 精确拷贝请求长度，处理跨记录字地址；写入需要保留请求范围外的字节。协议写命令、后端记录宽度、Flash 擦写粒度必须分开，不能直接把三个常量统一改成 8 或 2。

**关联风险：** 模拟初始化代码中还有用 uint16_t 局部变量调用固定 EEPROM_WRITE_SIZE 读取的路径；若有效宏为 8，同样需要处理。最终宏值须按实际构建配置确认，本文未重新编译。

### H02【高/条件】容量、板载模式门控和定时落盘上下文不一致

**位置：** `ecat_limits.hpp` `kSiiSizeBytes=0x800`；`HandleReload()`；`EcatDevice::OnTick1ms()`、`EcatSiiService::Tick()`、HAL `StoreSii()`。

**SSC：** EEPROM_CommandHandler 运行在 ECAT_Main 路径；ECAT_CheckTimer 达到延迟阈值时也会直接调用 `pAPPL_EEPROM_Store()`。因此不能声称“SSC 必定将存储放在主循环”。容量与介质配置应一致，字地址检查要与完整访问长度配合。

**当前：** reload 把字地址直接与名为 SizeBytes 的容量比较；板级配置另有 `ECAT_EEPROM_SIZE_BYTE=16*1024`，后端还有 ESC_EEPROM_SIZE，尚未形成单一契约。GetSiiMode 存在但 core 未用于阻止板载模式的软件命令处理；OnBoard 的 ReadSiiWord 返回 true 却不填数据。Zephyr k_timer 回调直接执行 OnTick1ms→Tick→StoreSii→Flash flush，把存储工作放到 timer ISR 上。

**修改：** 以实际介质容量校验 `word_address*2 + length`，并对齐默认镜像和后端记录范围；仅模拟模式执行软件 EEPROM 命令。对于 Zephyr 当前实际回调上下文，建议定时 ISR 只发布“需要落盘”，由线程执行实际存储，并核对 Flash 驱动是否允许 ISR 调用；这是端口上下文审查项，不是 SSC 本身保证的调度差异。结合停机 Flush 处理并发写入和错误返回。板载模式到底是否产生需软件响应的事件应依据驱动配置验证，不将其假设为必然触发。

## 11. 建议修改顺序与验收条件

以下是后续实施建议，不表示本次已经执行这些修改或测试。

| 顺序 | 修改范围 | 需要证明的行为 |
|---|---|---|
| 1 | B01/B02/B03/B04/B05：字典地址、权限与 CA | `0x1018` 身份四项准确；`0x6000/7010` 单项与 CA 一致；CA 起始 0/1 正确；只读和状态写限制有效；短写拒绝；时间戳不覆盖身份或设备类型 |
| 2 | A01～A05：转换、恢复、pending、SM 区域 | PREOP/SAFEOP/OP 往返；任意阶段允许合法退 INIT；等待 OP 时收到 INIT 不会被旧首帧拉回 OP；非法 SM 配置按 SSC 停止相应资源 |
| 3 | D01/D02/F01：邮箱所有权、重发及并发 | 一次发送只持有一份有效所有权；重复请求正确处理 0→1 和 1→0；ACK 对应；饱和队列不丢帧/重复释放；ISR 抢占不损坏队列 |
| 4 | E01～E04/G01～G03：Info 与 SDO 会话 | 全部 19 对象按标准列表可枚举；类型、位宽和映射位准确；高子索引名称不越界；分片/短末段/Abort/发送失败正确处理 |
| 5 | C01～C03：DC 和同步配置 | SM 模式保留已验证行为；DC 输入只在配置锁存点发布；同步丢失能被检测且禁止/退出 OP；主站写入参数真实生效 |
| 6 | H01/H02/F02/F03：SII、复位和端口契约 | 4/8 字节读取、字地址跨边界、写入邻字保护；实际 Flash 在允许上下文执行；重启与 ESC reset 后能重新建链 |

每项实施前先读完对应 SSC 原函数和所依赖结构定义，列出全部分支差异，再修改；完成后逐条核对差异清单。不要通过降低校验、固定强写 32 字节、吞掉错误或删日志来掩盖布局与协议问题。

## 12. 本次未作出的结论

- 没有认定“必须刷 EEPROM 才能解决”；本次已经证明过字典与 SDO 应答本身足以破坏主站 PDO 识别。
- 没有把 INIT 下反复 `0x0010` 或正常 PDO 的 `0x0c00` 单独认定为故障。
- 没有认定 SDK 的整 DWORD 访问必然破坏寄存器；F03 需要真实端口和芯片访问约束证据。
- 没有把当前未触发的 DC、重发、分段或 SII 缺陷说成已经在板上复现。
- 没有声称整套 C++ 栈已与 SSC 完全等价；本文列出的差异仍需逐项实施和分别验收。

## 13. 当前源码导航

行号对应本次审查快照，后续修改后应按函数名定位。

| 问题 | 当前源码入口 |
|---|---|
| A01～A05 | [状态转换](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/al/ecat_state_machine.cpp:232)、[SAFEOP 进入](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/al/ecat_state_machine.cpp:419)、[SM 配置检查](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/al/ecat_state_machine.cpp:767)、[SM 重检](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/al/ecat_state_machine.cpp:911) |
| B01～B05 | [真实存储偏移](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/model/ecat_dictionary.cpp:108)、[权限检查](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/model/ecat_dictionary.cpp:363)、[读取](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/model/ecat_dictionary.cpp:386)、[写入](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/model/ecat_dictionary.cpp:585) |
| C01～C03 | [同步实现](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/al/ecat_sync.cpp:113)、[事件发布输入](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/ecat_device.cpp:361) |
| D01～D03 | [邮箱 Service](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_mailbox.cpp:200)、[Submit](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_mailbox.cpp:271)、[Receive](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_mailbox.cpp:417)、[重发](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_mailbox.cpp:599) |
| E01～E04 | [列表](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sdo_info.cpp:133)、[列表分片](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sdo_info.cpp:218)、[对象描述](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sdo_info.cpp:271)、[条目描述](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sdo_info.cpp:326) |
| F01～F03 | [Poll](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/ecat_device.cpp:128)、[OnReset](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/ecat_device.cpp:212)、[HAL 访问](/E:/Zephyr/zephyr_user/drivers/ethercat/hal/hpm/ecat_hal_hpm.cpp:221) |
| G01～G03 | [SDO 服务](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sdo.cpp:204) |
| H01～H02 | [SII 服务](/E:/Zephyr/zephyr_user/framework/drivers/communication/ethercat/core/mailbox/ecat_sii_service.cpp:1)、[实际模拟后端](/E:/Zephyr_HPMicro/sdk_glue_user/drivers/ethercat/ecat_e2p_emulation.c:240) |
