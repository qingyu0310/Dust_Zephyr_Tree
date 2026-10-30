# EtherCAT 从站整体 C++ 重构规划

日期：2026-10-30。

范围：`framework/drivers/communication/ethercat/` 与 `drivers/ethercat/` 的整体重构，以及必要的构建装配和测试线程适配。本文只交付规划，不实施源码改动、不编译、不烧录。

## 1. 本次到底重构什么

本次不是给 SSC 再套一层 C++ 外壳，也不是把 `.c` 改名成 `.cpp`。最终将 AL 状态机、SM 管理、PDO、邮箱、CoE、SDO、对象字典、SII 命令和同步时序分别放入有明确所有权的 C++ 类，同时重写当前 HAL 和顶层的连接方式。

旧文档 `EtherCAT按USB分层风格重构规划.md` 以“不碰 SSC”为边界，解决的是外围分层；本规划接替其后续工作，**不再以现有 EcatStack、APPL 桥和 HPM C 端口层为不可改边界**。旧文档保留作为历史参照，不按其中的旧目标继续执行。

保留的是已验证的协议行为、报文布局、硬件访问约束和初始化顺序。现有封装结构、全局变量、宏展开对象字典和反向依赖都允许替换。

最终目标：

- 业务使用对象实例，不通过协议栈单例和 C 全局钩子连接。
- core 直接实现协议，不调用旧 SSC 函数来假装完成重构。
- HAL 只负责 ESC/PHY/存储硬件，不包含协议状态、不调用 SSC、不引用 core。
- 顶层负责业务 PDO 镜像和通知；协议核心负责总线状态和交换时序。
- 自研及迁入维护的 EtherCAT 实现使用 `.hpp/.cpp`；HPM SDK 原有 C 驱动继续作为外部依赖，不改官方源码。
- 保留 Beckhoff 派生代码的版权和许可说明。C++ 化不改变派生关系，不将其标为 Apache-2.0 自研原创。

## 2. 参照 USB 的具体规则

已核对的参照包括 `usb.hpp/cpp`、`usb_dev_port.hpp/cpp`、`usb_cdc_acm.hpp/cpp`、描述符/配置类、HAL/types 和 RX 队列。

| USB 当前做法 | EtherCAT 对应做法 |
|---|---|
| `Usb` 持有 `UsbDevPort device_` | `EthercatSlave` 持有 `EcatDevice device_`，不再持有全局单例引用 |
| `UsbDevPort` 持有 CDC 协议对象 | `EcatDevice` 组合 AL、SM、PDO、同步、邮箱、CoE/SDO、字典、SII 对象 |
| `UsbHal::EventCallback(context,event)` | ESC IRQ 与 1 ms tick 经带上下文的事件转发，禁止全局 SSC 回调 |
| CDC 配置统一驱动描述符和查询 | 同一 PDO/对象配置驱动字典、长度和顶层镜像容量检查 |
| 线编码显式编解码，不直接投射 wire struct | 邮箱/CoE/SDO 报文使用字节偏移与 LE 编解码，运行状态与报文分离 |
| HAL 不知道 `Usb` 或 CDC | HAL 不知道 `EcatDevice`、AL 状态和 SDO；只依赖 interface |
| 顶层接收队列解决业务生命周期 | 顶层 PDO 镜像解决 ISR/线程数据交接；不把 PDO 当无边界字节流排队 |

借鉴职责、组合和事件分发，不机械照抄 USB 的端点/EP0/CDC 数量，也不为了套 Stream 改变 EtherCAT 周期镜像语义。当前 EthercatSlave 实际没有继承 Stream，头部却声称继承，还有阻塞 `Run()` 的过时示例；本次统一修正。

## 3. 当前实现的具体问题

| 当前位置/符号 | 问题 | 目标归属 |
|---|---|---|
| `EthercatSlave::stack_ = EcatStack::Instance()` | 看似两个对象，实际共享一个协议栈和回调 | 顶层值成员 device |
| `core/ecat_stack.cpp` | 初始化、诊断、线程驱动、ISR 分发、业务回调混合 | device 编排；诊断快照独立接口；业务镜像归顶层 |
| `appl/ecat_appl_hooks.cpp` | 全局 C 符号反查单例；协议靠链接名称接业务 | 有上下文的类型化回调 |
| `core/ecat_pdo.cpp` | `_DIGITAL_IO_` 触发变量定义；读取 packed 字典内存计算长度 | 明确的字典定义与 PDO 映射模型 |
| `core/ecat_eeprom.cpp` | `s_hal` 全局桥、重新安装 pAPPL 钩子 | SII 服务持有存储能力引用 |
| `ssc/ecatslv.c` | AL、SM、DC、看门狗、状态请求处理跨域全局互访 | 状态机、SM、同步三个明确域 |
| `ssc/ecatappl.c` | PDI、Sync0/1、1 ms tick、PDO、EEPROM 命令与循环集中 | device 分发给各协议服务 |
| `ssc/mailbox.c` | 多个邮箱指针别名与队列隐式转移所有权 | 邮箱传输对象显式管理所有权 |
| `ssc/coeappl.c` + `objdef.c` | 字典链表、标准对象、同步参数、pending SDO 交叉操作 | 字典与同步/SDO 的具名接口 |
| `ssc/sdoserv.c` | 分段状态、pending response、分配/释放跨函数共享 | SDO 会话成员状态 |
| `EcatHalHpm::Stop()` | 写 `bRunApplication`，HAL 反向控制协议变量 | HAL 只停止硬件事件/定时器 |
| `hpm_ecat_hw.c` | k_timer 直接调用 `ECAT_CheckTimer`，EEPROM 依赖 `EepromReadSize` | 带上下文 tick 事件；读宽作为参数传入 |
| `ecat_zephyr_port.c` | IRQ 多层 C 跳板 | HPM HAL 实例事件入口 |
| `hpm_ecat_hw.h` | 硬件头包含 SSC 头、HW 宏、全局 pEsc | HAL 类型化寄存器与 DPRAM 访问 |
| `drivers/CMakeLists.txt` | 底层 include core、ssc 才能编译 | 只含 interface、HPM SDK 和自身头 |

### 3.1 必须修正对现有日志的理解

`framework/shell/log/queue/log_record.hpp` 的参数快照上限是 4 槽。此前八参数日志后四个零，以及多参数状态日志里超限字段的零值，不能作为硬件或协议证据。重构后的日志每条最多四个普通整数参数，浮点占两槽；ISR 仅采集快照，线程输出。

### 3.2 本次保留的实际基线

- HPM6E80 + JL1111；PHY 型号/地址仍由设备树决定。
- 两个方向各 8 个 UDINT、32 字节，主站已确认收到从站 0～7。
- PDO 容量上限当前每方向 64 B，不把“有效 32 B”混成“容量只有 32 B”。
- 非 DC 的 SM 同步是本次已验证主路径。
- DC、SDO Complete Access、分段 SDO 和异步 SDO 应答在当前源码中存在，必须迁移，不能因为本次 PDO 测试没用到就删掉。
- AoE/EoE/FoE/SoE/VoE、Bootstrap 等当前禁用的能力，不在重构中假称支持。
- PLIC 上限覆盖 ESC 133～136 的配置保留，不在本次复原到 127。
- 主站 OP 写请求失败/重试属于对照环境行为，不通过从站擅自进入 OP 或伪造状态来掩盖。

## 4. 目标目录和命名

继续使用现有 EtherCAT 路径，不为本次重构批量搬到 stream 下。

```text
framework/drivers/communication/ethercat/
  ethercat.hpp/.cpp                 ecat::EthercatSlave：业务入口、PDO 镜像、通知
  interface/
    ecat_types.hpp                 状态、事件、布局、错误值、回调契约
    ecat_hal.hpp                   ESC 硬件能力
    ecat_storage.hpp               SII 存储能力
    ecat_phy.hpp                   MDIO/PHY 抽象，保留芯片驱动扩展点
  core/
    ecat_device.hpp/.cpp           协议组合根和事件分发
    ecat_config.hpp                当前能力、容量、超时、标识常量
    ecat_registers.hpp             ESC 标准偏移与掩码，零厂商头
    ecat_wire.hpp                  字节序与报文字段访问
    ecat_state_machine.hpp/.cpp    AL 状态转换、错误确认、pending
    ecat_sync_manager.hpp/.cpp     SM 校验、通道操作、邮箱/PDO 区域检查
    ecat_sync.hpp/.cpp             SM/DC 同步、看门狗、周期测量
    ecat_pdo.hpp/.cpp              PDO 映射计算与过程缓冲交换
    ecat_mailbox.hpp/.cpp          SM0/1、队列、repeat、报文所有权
    ecat_coe.hpp/.cpp              CoE 服务分发
    ecat_sdo.hpp/.cpp              SDO 请求/分段/异步应答
    ecat_sdo_info.hpp/.cpp         对象列表与描述查询分片
    ecat_dictionary.hpp/.cpp       字典查询、权限、类型、读写
    ecat_object_config.hpp/.cpp    标准对象与应用对象定义
    ecat_eeprom.hpp/.cpp           SII 命令、reload、延迟 store
    LICENSE-BECKHOFF.md            派生范围、来源、文件对应表

drivers/ethercat/hal/
  hpm/
    ecat_hal_hpm.hpp/.cpp          硬件生命周期和事件上报
    ecat_esc_hpm.hpp/.cpp          MMIO、SM 区域访问、中断注册/使能、MDIO
    ecat_storage_hpm.hpp/.cpp      Flash 模拟/板载 EEPROM 初始化
    ecat_sii_image.hpp             当前 SII 数据及标识，保留字节内容
  phy/                            按现有芯片分目录的 C++ PHY 驱动
```

最终删除旧 `ssc/` 实现目录、`appl/ecat_appl_hooks.cpp` 和 SSC C 端口适配文件。许可文件迁入 core，派生源码文件保留原声明并追加本项目修改说明。迁移阶段的旧引擎只在最终切换前存在，不作为长期双实现。

## 5. 所有权和依赖方向

```text
test Task / 业务
    ↓
ecat::EthercatSlave
    ├ PDO 业务收发镜像、通知
    └ EcatDevice device_
         ├ EcatStateMachine / EcatSyncManager / EcatSync
         ├ EcatPdo / EcatDictionary / 对象定义
         ├ EcatMailbox → EcatCoe → EcatSdo + EcatSdoInfo
         └ EcatEeprom
                  ↓ 只用 interface
              EcatHal / EcatStorage
                  ↑ 实现
          EcatHalHpm / EcatStorageHpm → HPM SDK、Zephyr、PHY
```

1. 所有协议类放 `namespace ecat`，名称用项目已有 PascalCase 类/方法、snake_case_ 私有成员。
2. `EthercatSlave` 是唯一业务所有者，禁止拷贝；其成员持有 EcatDevice。硬件通过 `GetDefaultEcatHal()` 获取默认实例，也支持 Init 显式注入用于验证。
3. `EcatDevice` 值成员组合各协议服务；子服务只保存依赖引用或非拥有指针，不自己创建 device，不回查全局 Instance。
4. AL 状态与 pending 归 EcatStateMachine；SM 通道快照归 EcatSyncManager；同步计数/时间戳归 EcatSync；PDO 实际布局及缓冲归 EcatPdo；邮箱缓冲归 EcatMailbox；分段/pending 请求归 EcatSdo。
5. 字典里的 0x1C32/0x1C33 读写绑定同步服务的真实字段或读写方法，不能再复制一份状态。
6. 字典权限校验经只读 AL 状态访问器取得当前状态，不包含状态机实现头；同步类型写入经具名回调通知 EcatSync。禁止 Dictionary 与 StateMachine 相互持有完整对象形成环。
7. 邮箱不知道具体 SDO 对象：保存协议分发回调；CoE 返回应答/待处理结果，由邮箱处理发送与重发。所有权交接由结果类型表达。
8. 最终没有公开的总 Runtime 大结构供各层任意写，也不把旧全局变量全部搬进一个 public Context 就宣布完成。

## 6. 接口定案

### 6.1 业务入口

保留现有 `Init/Poll/SetRxHandler/SetTxProvider/SetCycleHandler/GetPdoLayout` 的业务含义，测试线程只做必要签名适配。新接口不暴露 SSC 类型：

```cpp
namespace ecat {
class EthercatSlave final
{
public:
    bool Init();
    bool Poll();
    bool IsReady() const;
    PdoLayout GetPdoLayout() const;

    using RxHandler = void (*)(void*, const uint8_t*, uint16_t);
    using TxProvider = void (*)(void*, uint8_t*, uint16_t);
    using CycleHandler = void (*)(void*);
    void SetRxHandler(RxHandler handler, void* context);
    void SetTxProvider(TxProvider handler, void* context);
    void SetCycleHandler(CycleHandler handler, void* context);

private:
    EcatDevice device_;
};
}
```

此处展示业务 API 形状，不是可直接编译的完整头；完整头必须含镜像成员、事件入口、禁止拷贝和依赖声明。短访问器内联；复杂实现进入 cpp。`IsReady` 明确表示初始化完成，OP 状态通过状态快照独立查询，二者不能混用。

不在本次引入阻塞 Run、线程停止/join 语义；Thread 继续负责循环和故障治理。若保留显式硬件停用能力，命名为 `Disable`，只作为生命周期清理，不宣称具备完整 Stop→Init 热重启能力，除非完成独立验证。

### 6.2 HAL 拆掉 SSC 形状

当前 `EepromRead()` 既像返回数据又被调用者丢弃、`TimerClear()` 模拟全局时基、`EnterCritical()` 无保存值，这些接口不直接照搬。

最终 HAL 提供：

| 能力 | 接口约定 |
|---|---|
| 初始化 | `Init(config, callback, context)`，不启用协议事件；回调先装好 |
| 硬件运行 | `StartEvents()` / `StopEvents()`，只操作中断和定时器 |
| 寄存器 | `Read8/16/32`、`Write8/16/32`；明确对齐与平台支持策略 |
| DPRAM | `ReadMemory/WriteMemory(address, buffer, byte_count)`，不偷偷取整地址 |
| 事件 | `Event{Pdi,Sync0,Sync1,Reset,Tick1ms}`，带 instance context |
| 屏蔽 | `SaveIrqState()` 返回 token，`RestoreIrqState(token)` 恢复原状态 |
| 链路 | `PollLinks()`，100 ms 调用，PHY 状态写 ESC 的行为不变 |
| MDIO | 平台 PHY 能力接口，不放进 AL/SDO 层 |

SII 存储单独提供 `Mode/Read/Write/Reload/Store`。读写返回错误与实际数据分离；Flash 仿真模式与板载 EEPROM 自动加载模式显式区分。core 负责 ESC EEPROM 命令与应答寄存器，storage 负责实际数据/介质，不再让 HAL 替 SSC 完成半套协议应答。

### 6.3 C ABI 的最终边界

协议类及其互调全部为 C++，没有 APPL_*、HW_*、PROTO/VARMEM 等适配宏。HPM SDK 的 C API 继续通过其原声明调用；Zephyr 设备/IRQ 注册可保留必要的静态 thunk，但 thunk 带设备/实例上下文。不要为追求零 `extern "C"` 去改 SDK ABI。

## 7. 旧函数到新类的迁移清单

迁移以函数为单位逐项核对返回码、寄存器访问顺序和异常分支；不是凭协议印象重新写最简版本。

| 原文件/函数族 | 新所有者和方法 | 必须保持的细节 |
|---|---|---|
| `ECAT_Init/Main` | EcatDevice::Init/Poll + 各服务 Init/Poll | 字典、邮箱、AL 初始化和事件消费先后 |
| `AL_ControlInd/Res`、`ECAT_StateChange`、`SetALStatus` | EcatStateMachine::HandleRequest/CompletePending/RequestLocalState/PublishStatus | 低四位状态与错误位分开；错误确认、降态、pending 和超时 |
| `CheckSmSettings/GetSyncMan/Enable/DisableSyncManChannel` | EcatSyncManager::Validate/Read/Enable/Disable | SM 地址/长度/控制位、区域重叠、mailbox 与 PDO 区分 |
| `Start/StopInputHandler` | StateMachine 编排 SM、PDO、Sync | 首帧清零、使能顺序、初始输入提交 |
| `Start/StopOutputHandler/BackToInitTransition` | EcatStateMachine | 先准备后提交状态，退出 OP 的输出语义 |
| `DC_CheckWatchdog/CheckIfEcatError` | EcatSync + StateMachine 本地错误入口 | DC/SM 计数、看门狗退态、错误码 |
| `PDI_Isr/Sync0_Isr/Sync1_Isr` | EcatDevice::OnPdi/OnSync0/OnSync1 | 事件读取与确认顺序、首/尾 DPRAM 访问、PDO/应用调用顺序 |
| `ECAT_CheckTimer` | EcatDevice::OnTick1ms 分发 Sync/AL/SII | 每毫秒只推进一次，不重复累计 |
| `PDO_InputMapping/OutputMapping` | EcatPdo::PublishInputs/ConsumeOutputs | 实际 rx/tx 长度、SM 首末字节访问、副作用 |
| `HandleCycleTimeMeasurement/GetSystemTimeDelay` | EcatSync | 测量开关、计数回绕、读寄存器开销补偿 |
| `EEPROM_CommandHandler` | EcatEeprom::PollCommand | busy/error/CRC、read size 4/8、reload/store |
| `MBX_*` 全族 | EcatMailbox | duplicate counter、repeat toggle、锁定接收、续发和销毁别名 |
| `COE_ServiceInd/ContinueInd/Init` | EcatCoe | SDO/Info 分发及不支持协议的原错误码 |
| `SDOS_SdoInd/Res/ClearPendingResponse` | EcatSdo | expedited/normal/segmented、toggle、abort、pending 所有权 |
| `SDOS_SdoInfoInd` | EcatSdoInfo | 对象/条目描述、对象列表分片、继续请求 |
| `OBJ_Get* / OBJ_Read/Write` | EcatDictionary | 类型宽度、CA/subindex0、状态权限、映射修改约束 |
| `COE_ObjInit/Add/Remove/Clear` | EcatDictionary + EcatObjectConfig | 标准/应用对象合并、查找顺序、同索引拒绝 |
| `COE_Main` pending 处理 | EcatSdo::PollPending | 异步完成后应答与分段缓冲释放 |
| `COE_SyncTimeStamp/UpdateSyncErrorStatus` | EcatSync | 0x10F8 与 0x1C32/33 的一致性 |
| `APPL_*` | 顶层业务回调或具名配置回调 | 不改变调用上下文，不反查单例 |

## 8. 协议与资源迁移的硬约束

### 8.1 AL 和同步

- INIT=1、PREOP=2、SAFEOP=4、OP=8，状态错误位独立保存，不用 `enum class` 强转吞掉错误位。
- 迁移 `stateTrans=(old<<4)|new` 的转换表及所有非法转换分支；不能只写 INIT→PREOP→SAFEOP→OP 正向链。
- 保留 SAFEOP→OP 的首帧输出条件；不允许收到 OP 请求就直接设置 OP。
- 当前 SAFEOP2OPTIMEOUT 配置 9000 ms，原状态机还扣除 10%，实际计数为 8100 ms；迁移按原计算保留，不能只抄 9000 作为最终等待值。
- 原 MainLoop 有一段 FreeRun/首帧兜底被注释；先记录为当前行为，不在结构迁移中悄悄恢复。后续是否修复单列验证，不与重构混为一件事。
- DC 代码迁移保留，但目前硬件只验证了非 DC 路径，不能在完成说明中声称 DC 已验收。

### 8.2 内存与报文

- PDO 两方向各保留 64 B 协议缓冲上限，有效映射各 32 B。
- Mailbox 当前 MIN=0x24、MAX=0x80，队列深度取当前定义；分配策略第一阶段保持原行为，不临时改大缓冲、不新建堆池。
- `psReadMbx/psRepeatMbx/psStoreMbx` 可能指同一缓冲。迁移先列状态所有权表，再设计句柄，禁止三个 unique_ptr 各自释放同一地址。
- 分段 SDO 的缓冲与保留应答必须分别标明谁创建、谁持有、何时转交、谁释放；错误、超时和退 INIT 都覆盖。
- mailbox 头 6 B、CoE/SDO 字段偏移、toggle 和 abort code 全部按原值保留；CA 的 subindex0 占位行为不能按普通 uint8_t 数组理解。
- 运行对象采用自然对齐；wire 使用显式 ReadLe/WriteLe。不能把成员对象、虚表或编译器 padding 写到 ESC。
- 对象字典的应用/标准对象定义集中，移除 `_DIGITAL_IO_/_OBJD_/PROTO` 的多遍 include 定义机制；标识和字符串内容保持不变。
- SII 镜像与对象字典之间记录一致性字段，但本次不自动重刷板载 EEPROM。
- 全部新增常驻 RAM 列出 `sizeof` 预算，与旧缓冲对照；不在迁移中私自扩大容量。

### 8.3 并发与线程

| 上下文 | 执行内容 | 禁止行为 |
|---|---|---|
| 业务 Thread 单拍 | device.Poll、邮箱/AL 低频处理、到期链路刷新、快照输出 | 内部常驻 while、重复启动第二个协议循环 |
| ESC PDI/Sync ISR | 按旧顺序处理同步事件和 PDO；短回调 | sleep、分配新大缓冲、串口格式化诊断 |
| 1 ms 定时器 ISR | AL 超时、同步看门狗、SII 延迟计数 | 再调用整个 Poll、重复启动同一计数源 |

保持事件所属上下文，不为“全在一个线程更清晰”把 1 ms tick 或 DC ISR 搬到可能延迟的 Poll。跨上下文共享字段使用短临界区/原子操作，不依赖 volatile 当同步。IRQ 屏蔽必须保存并恢复，不能退出临界区时无条件开 PDI。

顶层 PDO 双镜像每方向采用两个容量 64 B 的数组：已提交与业务暂存，共 256 B；core 保留原两方向各 64 B 交换缓冲。净增/复用量必须在整合步核账；若能够以所有权证明复用既有数组，应复用并在实现记录说明。不能把这部分数组同时放进每个服务类重复分配。

业务 Rx 默认由 Poll 转交通知，ISR 只提交完整帧镜像；TxProvider 在线程准备下周期镜像，ISR 发布已提交内容。**这与当前 ISR 直接调业务的上下文不同，是本规划明确的接口变化**，必须在测试线程适配与验收中检查，不能称为纯搬文件。周期同步业务回调若需要精确 ISR 语义，独立命名 `SetRealtimeCycleHandler`，明确限制；默认业务回调不得隐式跑 ISR。

## 9. 迁移方式与编译边界

采用“新实现旁路落地 → 单次切换 → 清旧”的迁移方式。旧引擎在切换前仍是唯一运行引擎；新类可被编译验证但不初始化、不占用 ESC。禁止双栈同时读事件、写 AL 或驱动 SM。

新实现不会长期转发到旧 SSC。切换前以原函数作行为对照；切换后所有协议路径必须调用新类。旧全局 C 引擎只作为迁移期参照，最终清除。

每步只按下表修改指定文件/同概念文件组。用户说“开始第 N 步”就是只执行该全局步号，改完停，等“继续”。hpp/cpp 是同一类的必需原子文件组；不借“连带”跨到后续业务或平台步骤。

### P1：固定契约与协议数据类型（步骤 1～6）

> 每次只执行一步，不跨步。此阶段新文件不参与运行，现有工程保持原行为。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `core/ecat_config.hpp` | 从 ecat_def 提取启用能力、容量、标识、原超时；逐项列旧宏→constexpr | 本文 | 不删旧宏，保持可编译 |
| 2 | `interface/ecat_types.hpp` | 增加事件、错误/布局/状态快照、带 ctx 回调；保留旧类型至切换 | 1 | 不破坏旧签名 |
| 3 | `core/ecat_wire.hpp` | LE 16/32/64 编解码、mailbox/CoE/SDO 字节视图与边界检查 | 1 | 未接入旧栈 |
| 4 | `core/ecat_registers.hpp` | esc.h 偏移/掩码分类迁移，标注访问宽度和确认副作用 | 1 | 保留旧 esc.h |
| 5 | `interface/ecat_storage.hpp` | 定义数据与错误分离的存储契约、模式和 reload/store | 2 | 暂不要求旧 HAL 实现 |
| 6 | `interface/ecat_hal.hpp` | 新能力接口与旧接口迁移共存；新方法暂不改成逼迫旧实现的纯虚方法 | 2～5 | 切换前旧 HAL 可编译 |

### P2：真正的协议类（步骤 7～18）

> 每次只执行一步；每行的 hpp/cpp 为一个类的一组文件。尚不接入硬件，不和旧引擎并行运行。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 7 | `core/ecat_dictionary.hpp/.cpp` | 迁移 OBJ 查询、位偏移、权限、读写、CA；以状态访问器和同步写回接口代替全局依赖 | 1～4 | 声明回调契约供后续类实现 |
| 8 | `core/ecat_object_config.hpp/.cpp` | 标准对象+应用对象唯一实体，含 0x1600/1A00/1C12/1C13/7010/6000；描述表与数据分离 | 7 | 不再多遍 include 生成全局 |
| 9 | `core/ecat_sync_manager.hpp/.cpp` | SM 读写校验、地址区域/容量校验、通道使能 | 2、4、6 | busy-wait 原行为先保留并显式记录 |
| 10 | `core/ecat_sync.hpp/.cpp` | SM/DC/watchdog/时间戳/周期测量成员化；连接字典参数 | 7～9 | tick 和 IRQ 入口仅声明，暂不调硬件 |
| 11 | `core/ecat_pdo.hpp/.cpp` 新类版暂以 `ecat_pdo_engine` 命名 | 长度计算、两方向缓冲和原始搬运、实际长度回调 | 7～10 | 保留旧静态 EcatPdo 避免旧栈断编 |
| 12 | `core/ecat_mailbox.hpp/.cpp` | 队列、发送、接收、repeat、缓冲生命周期；分配策略保持原值 | 3、9 | 用回调分发协议，避免循环 include |
| 13 | `core/ecat_sdo.hpp/.cpp` | 请求/应答/分段/toggle/abort/pending 全量迁移 | 7、12 | 通过明确邮箱发送结果转交所有权 |
| 14 | `core/ecat_sdo_info.hpp/.cpp` | SDO Info 查询、续发与分片状态成员化 | 7、12 | 不共享旧 pSdoInfoObjEntry |
| 15 | `core/ecat_coe.hpp/.cpp` | CoE 分发和继续处理，连接 SDO 与 Info | 12～14 | 不支持服务按原错误返回 |
| 16 | `core/ecat_eeprom_engine.hpp/.cpp` | EEPROM 命令与 4/8 B 应答、reload/store；新类不再安装全局钩子 | 4～6 | 旧 EcatEeprom 仍用于旧栈 |
| 17 | `core/ecat_state_machine.hpp/.cpp` | 原全部转换/错误确认/pending/超时/退态分支迁移，调用 SM/Sync/PDO/Mailbox | 9～16 | 字典只读状态回调由 device 装配 |
| 18 | `core/ecat_device.hpp/.cpp` | 实例组合、Init/Poll/IRQ/tick、能力绑定与快照 | 7～17 | 不注册硬件，不调用旧 SSC |

此阶段结束前逐文件核对原函数清单；所有原函数必须标记为“迁入哪个方法/编译禁用功能不迁/示例 main 删除”，不能只有几条主流程能跑。

### P3：HPM HAL 与存储整体替换（步骤 19～25）

> 每次只执行一步。新实现暂使用不同类名/工厂名旁路存在，避免与旧默认工厂重定义。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 19 | `hal/hpm/ecat_esc_hpm.hpp/.cpp` | 迁移 esc_hpmicro 的 MMIO/IRQ/MDIO/控制寄存器，采用实例 ctx 与静态 thunk | 4、6 | 保留原 Zephyr 设备定义至切换；新文件不重复定义同一设备 |
| 20 | `hal/hpm/ecat_sii_image.hpp` | 从原 eeprom.h 迁移镜像，逐字节核对、记录长度与校验值 | 1 | 不烧写 EEPROM |
| 21 | `hal/hpm/ecat_storage_hpm.hpp/.cpp` | 迁移 Flash 模拟与板载 I2C 初始装载；无 EepromReadSize 全局依赖 | 5、19、20 | 读宽通过请求参数传入 |
| 22 | `hal/phy/` 全部 PHY 文件 | 整组移动现有按芯片驱动，只适配 MDIO 能力，不改芯片配置值 | 19 | CMake 尚未切换，旧路径在切换步前以保留副本维持旧栈 |
| 23 | `hal/hpm/ecat_hal_hpm.hpp/.cpp` 新版暂名 `ecat_hal_hpm_next` | 组合 ESC/PHY/storage，注册 IRQ/tick 上下文，完全删除 SSC 依赖 | 19～22 | 不注册第二个设备、不启动第二个 timer |
| 24 | `drivers/CMakeLists.txt` | 新实现加入编译，旧栈仍运行；精确列源文件，不 glob | 19～23 | 新组件无启动副作用；厂商 C 源继续保留 |
| 25 | `framework/drivers/CMakeLists.txt` | 新协议类加入编译，旧 7 个 SSC C 单元仍在，区分命名空间/符号 | 7～18、24 | 首个完整新实现编译检查点，由用户编译 |

第 25 步之后才可认为新实现完成了实际 C++ 编译验证；第 25 步之前新文件是否能编译仅是静态审查，不作已通过声明。

### P4：顶层与切换（步骤 26～30）

> 每次只执行一步。切换采取显式编译断点，禁止为了维持表面可编译长期保留全局桥。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 26 | `ethercat.hpp/.cpp` | 顶层改为值成员 device，加入镜像和回调上下文；移除 EcatStack 引用 | 25 | 现有 test 回调签名会不匹配，至 29 前暂不可编译 |
| 27 | `drivers/CMakeLists.txt` + 新 HAL 工厂文件 | 切到新硬件实现，停编旧 C 适配/旧设备定义；厂商依赖不动 | 26 | 为避免双设备/双工厂，此为同概念原子组；至 29 前暂不可编译 |
| 28 | `framework/drivers/CMakeLists.txt` | 停编旧 SSC、APPL 桥和 stack，启用新 device；新旧 PDO/EEPROM 文件名收敛 | 27 | 不在此步改 test；至 29 前暂不可编译 |
| 29 | `project/thread/test/trd_test.cpp` | ctx 回调适配；Thread 单拍 Poll，两个 32 B 结构体及 0～7 测试保持；日志≤4槽 | 28 | 本步后恢复完整可编译，等待用户编译 |
| 30 | 用户硬件验证记录 | 按第 11 节检查建链、PDO/SDO、掉线与复位；记录新旧差异 | 29 | 验证不通过只修对应差异，不清旧参考 |

26～29 中间不编译是规划中的明确状态；若用户要求全程可编译，应把这四步作为一次授权的切换批次执行，不能自行把“一步”扩大为整阶段。

### P5：删除旧实现与收尾（步骤 31～36）

> 每次只执行一步。此前新实现已经通过基础上板验证，旧文件只是参照，不参与链接。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 31 | `ssc/`、`appl/`、旧 stack/PDO/EEPROM 过渡文件 | 先逐项清点旧→新映射，再删除已迁移旧源；许可先迁移后删目录 | 30 | 停编文件删除不改变当前链接 |
| 32 | `drivers/ethercat/hal/ports/` 与旧 C 设备文件 | 删除被新版替代的 C 端口、旧头、PHY 副本；保留仍用的第三方声明 | 30、31 | 不删 SDK 原文件 |
| 33 | `interface/ecat_hal.hpp` | 删除迁移期旧接口，最终纯虚契约；清掉默认 no-op 占位 | 32 | 检查所有派生实现，确保无占位运行路径 |
| 34 | `drivers/Kconfig`、双方 CMake | 清除 SSC include、旧文件名和过期描述；保留有效板卡/PHY/EEPROM 门禁 | 33 | 不改 unrelated 配置和 dts |
| 35 | `core/LICENSE-BECKHOFF.md` | 最终来源、派生文件与旧文件对应表；核对每个派生文件头 | 31～34 | 不改变授权结论 |
| 36 | 本文及 EtherCAT 状态说明文档 | 回填完成状态、接口用法、功能覆盖和未验证项目 | 用户验收 | 不提交、不上传，除非用户单独要求 |

## 10. 典型 old→new 对照

### 10.1 顶层不再是单例别名

```cpp
// old
EcatStack& stack_ = EcatStack::Instance();

// new
EcatDevice device_ {};
```

device 内各模块也必须是这个对象的成员，不能在子模块里继续 Instance。硬件单实例限制由 Init 检查并返回明确错误，不靠静态回调被后一个对象覆盖。

### 10.2 IRQ 和 tick 都从能力接口上报

```text
old: k_timer → ECAT_CheckTimer() → 全局计数
new: k_timer → HPM HAL 事件(ctx, Tick1ms) → device → AL/Sync/SII 成员计数

old: PDI → C driver → C port → 静态 HAL ctx → 单例 stack → PDI_Isr()
new: PDI → HPM ESC 静态 thunk(ctx) → HAL event → device.OnPdi()
```

删跳板不等于删事件确认。新 OnPdi 必须逐条对照旧 PDI_Isr 的读事件、同步事件、输出映射、应用阶段、输入发布及周期超限处理。

### 10.3 HAL 不再写协议运行标志

```cpp
// old: EcatHalHpm::Stop()
bRunApplication = FALSE;

// new: 协议生命周期归 device；HAL 只停用硬件事件
device 的 running_ = false;
hal.StopEvents();
```

上述为职责对照，不把中文伪代码作为可编译实现。真正停用时先屏蔽新事件、等待/阻止并发回调，再释放协议资源；不能停一半留下 ISR 引用已释放缓冲。

### 10.4 取实际 PDO 长度

```text
old: SSC nPdOutputSize 决定硬件读取长度，layout_.rx_size 决定业务回调长度
new: EcatPdo 自己计算并持有 layout；SM 校验、硬件读取、业务通知都取同一 layout
```

字典映射变更只有在协议允许的状态提交，提交失败不更新一半。业务缓冲按当前有效长度校验，容量上限单独保存。

## 11. 验证清单

### 11.1 静态与构建检查

- [ ] 最终 core 不含 HPM/Zephyr device 头、旧 SSC 头、HW/APPL/pAPPL/PROTO 宏。
- [ ] HAL 不 include core，不引用 EcatDevice、全局协议标志或旧 SSC 变量。
- [ ] 迁移函数表逐项有归宿，包含降态、错误确认、repeat、分段和 pending 路径。
- [ ] 只有一个 ESC 设备注册、一个活跃 tick、一个协议实例绑定硬件。
- [ ] 字典及 PDO 对象只有一个定义，标识、映射条目和数据宽度与基线一致。
- [ ] 对比 RAM/栈/镜像大小；说明新增镜像、类成员及被删除全局缓冲的净变化。
- [ ] 日志参数不超槽，所有验证结论来自有效字段，快照与实时寄存器明确区分。
- [ ] 由用户完成第 25、29 和清旧后的编译；AI 不自行执行编译烧录。

### 11.2 必须上板的行为验证

| 场景 | 验收证据 |
|---|---|
| 上电/主站后启动 | PHY link、扫描、INIT→PREOP→SAFEOP→OP，不出现 PLIC 越界 |
| 主站写 OP 暂时失败 | 从站不伪造 OP；写成功后正常转换，AL 错误码可对照 |
| 主→从 PDO | 32 B 八个 UINT32 全量核对；日志拆四项，使用变化序号排除旧帧 |
| 从→主 PDO | 主站收到完整八项，和提交镜像一致 |
| SAFEOP 行为 | 输入与输出启用规则和旧栈一致，输出业务不越过 OP 门槛 |
| CoE | 读取 0x1018、1C12/13、1600/1A00、1C32/33，与基线一致 |
| SDO 错误 | 不存在索引、错误长度、只读写入、错误状态访问返回原 abort code |
| SDO 分段/CA | 多段上传/下载、toggle、终段长度、CA 起始 subindex、异步 pending |
| mailbox repeat | 请求重发和 repeat toggle 不丢回复、不重复释放缓冲 |
| 掉线/主站退出/重连 | 记录 AL 与镜像状态，协议不死锁、不重新创建不受管线程 |
| SII | 板载 EEPROM 模式维持加载成功；Flash 仿真在对应硬件单独验收 |
| DC/Sync0/1 | 有主站配置及对应硬件时单独验收；没有则明确“已迁移、未上板验证” |
| 线程治理 | Task 单拍返回，thread 统计持续增长，Pause/故障处理仍由框架管理 |

不能仅以“主站扫到一个从站”或“编译成功”判定重构完成。基础 OP/PDO 通过也不能代替 mailbox/SDO 功能等价验证。

## 12. 完成定义

本规划执行完成时，业务入口、协议对象和硬件能力之间是清晰的 C++ 组合关系；SSC 原有全局过程式内核已经被实际替代，旧 C 桥和反向依赖已删除，许可来源仍可追溯。

本次规划默认保留当前功能集合，不借重构新增协议，也不凭本次非 DC 测试裁掉 DC/SDO。任何行为修复与结构迁移分开记录。只有用户确认上板结果后，才将对应条目标为验收通过。
