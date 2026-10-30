# EtherCAT 从站整体 C++ 重构规划

日期：2026-10-30。
范围：`framework/drivers/communication/ethercat/`、`drivers/ethercat/`、以及 `project/thread/test/` 的适配。

---

## ⚠️ 执行状态（2026-10-30 更新）

**P1~P8 全部 30 步已执行完毕，未编译、未上板。**

| 阶段 | 步 | 状态 |
|---|---|---|
| P1 数据契约 | 1~5 | ✅ |
| P2 对象模型 | 6~8 | ✅ |
| P3 过程数据 | 9~10 | ✅ |
| P4 邮箱协议 | 11~14 | ✅ |
| P5 组合根 | 15~16 | ✅ |
| P6 硬件实现 | 17~22 | ✅ |
| P7 切换 | 23~26 | ✅ |
| P8 清旧 | 27~29 | ✅ |
| 第 30 步（文档回填） | — | ✅ 本节 |

**当前结构**：`framework/.../ethercat/` 34 文件 8085 行；`drivers/ethercat/` 27 文件 1727 行。

**静态验收结果**（§9.1 五项）：
- core/interface 无厂商符号 ✅（仅 `ecat_sync.cpp` 一条注释提到 `HW_EscReadDWord` 作对照说明）
- HAL 无协议符号 ✅
- 零 `friend class` ✅
- 无 `malloc`/`free(` ✅（仅 `ecat_limits.hpp` 一条注释解释为何要换掉它）
- 常量表全部 `constexpr`，无 `ResetStorage` 式运行期初始化 ✅

**未验证项**：
- 从未编译过。P6 第 22 步的编译检查点、P7 第 26 步的完整编译都跳过了（用户要求连续执行到底）。
- 从未上板。§9.3 的 14 项行为验证全部未做。
- `sizeof(EcatDevice)` 与被删全局变量的对账未做（需编译后从 map 文件取）。
- 由于未编译，**必然存在语法/类型错误**，需要一轮编译修正。

**下面是原始规划全文，保留作为设计与验收依据。**

---

## 0. 先读这一节：上一版错在哪

上一版 `EtherCAT按USB分层风格重构规划.md` 的边界是"不碰 SSC"，只重写外围。之后的执行走成了另一条路：**把 SSC 的 7 个 `.c` 文件逐文件改名成 `.hpp/.cpp`，每个文件一个类，成员变量照搬全局变量，跨类访问用 `friend` 打通。** 结果已经还原，但那个结果必须被记下来当反例，否则同样的错会再犯一次。

它的四个病灶，逐条对应本规划要解决的问题：

| 病灶 | 具体形态 | 根因 |
|---|---|---|
| 切割轴错 | 一个 `.c` 文件 → 一个类；13 个头文件顶部重复 13 行前向声明，`EcatDevice` 里挂 13 个 `friend class` | 按**源文件**切，不是按**职责**切。人人 friend 人人 = 没有封装，只是把一棵全局变量树加了 `device_.` 前缀 |
| 数据搬进 RAM | 对象字典的 280 行编译期常量表改成运行期 `memcpy` 到 `char[264]`、手工数 `"\000\377"`、保留全部名称缓冲和 2048 B SII 镜像 | 为了"能塞进类"而付出实打实的 RAM，收益为零。本该是 `constexpr` 常量表 |
| 风格没翻译 | `bCallInputMapping`、`u32TimeValue`、`if (x == true)` 判 `uint8_t`、`u32dummy_` 这种"必须给个目标变量"的占位 | 只做了词法替换，没做语义重述 |
| 死代码继承 | 8 个 `return 0` 空桩被状态机 30 多处调用，状态机以为自己在激活 SM，其实什么都没做 | 把 C 里本来就缺的东西也一起抄了过来，还让调用方以为它有效 |

**本规划的第一原则：切割轴是"谁持有状态 / 谁做决定 / 谁执行"，不是"原来是哪个文件"。**
判据：如果一个类的所有方法都只是把参数转给另一个类，那它不应该存在。

---

## 1. 从站到底在做哪几件事

分层必须从协议事实出发。EtherCAT 从站的全部工作可以归成**两个时间尺度、四个数据域**：

| 数据域 | 时间尺度 | 由谁触发 | 内容 |
|---|---|---|---|
| **过程数据（PDO）** | 硬实时，1 kHz 量级 | SM2 写入事件 / Sync0 | 定长、双向、每周期全量覆盖，丢一帧不影响下一帧 |
| **邮箱（Mailbox）** | 尽力而为，亚毫秒~秒 | SM0 写入事件 | 变长、请求-应答、有重发（repeat）、有队列 |
| **状态机（AL）** | 事件驱动，毫秒~秒 | AL Control 寄存器写入 | 决定上面两条通道**能不能开** |
| **非易失配置（SII）** | 极低频，仅启动/升级 | ESC EEPROM 命令事件 | 从站身份、PDO 默认配置 |

这四件事的**失败模式完全不同**，这就是为什么它们必须分属不同的类：

- PDO 丢帧 → 业务看到旧值，下一帧就恢复，**不能重传、不能排队**。
- 邮箱丢包 → 主站 SDO 超时，**必须能重发、必须能排队**。
- AL 状态 → 决定了 PDO/邮箱的开关，**它不能有"半个状态"**（要么 SM2 开，要么关）。
- SII → 写坏了整个从站身份就变了，**必须延迟落盘、必须能校验**。

把它们塞进同一个类、用同一个内存模型、同一个并发模型处理，就是 SSC 那个 2795 行 `ecatslv.c` 的由来。

**反过来，PDO 和对象字典不是一回事**，这一点容易被误合并：

- 0x1600 映射表把 `0x7010:01..08` 这一组对象映射进 SM2；
- 但 SSC 的 digital_io 示例里，`APPL_OutputMapping` 是空函数，业务直接读 `aPdOutputData` 那块 PDO 缓冲，**并不写进 0x7010 的对象数据**；
- 也就是说，当前实现里"过程数据"和"对象字典里的同名变量"是两份独立内存，SDO 读 0x7010 读到的是对象数据，不是上一帧 PDO。

这是 SSC 示例的既有语义。**本规划保持它不变**（行为零变化原则），并在 §5.4 单列一条"已知语义缺口"，作为重构完成之后独立决定的事项，不在迁移中顺手改。

---

## 2. 分层与依赖方向

### 2.1 五层

```text
┌──────────────────────────────────────────────────────────────────┐
│ L5  业务            test Task / 用户线程                          │
│                    只认识 EthercatSlave 的 5 个方法                │
└───────────────────────────────┬──────────────────────────────────┘
                                │ 值成员
┌───────────────────────────────▼──────────────────────────────────┐
│ L4  业务门面        ecat::EthercatSlave                          │
│                    持有 EcatDevice；持有 PDO 交接镜像；             │
│                    把设备事件翻译成"某方向来了一帧数据"             │
└───────────────────────────────┬──────────────────────────────────┘
                                │ 值成员 + 类型化回调
┌───────────────────────────────▼──────────────────────────────────┐
│ L3  协议核心        ecat::EcatDevice（组合根 + 事件分发）           │
│      ├ EcatStateMachine   AL 状态机（决策者）                      │
│      ├ EcatProcessData    过程数据域                               │
│      ├ EcatMailbox        邮箱域传输                               │
│      │   └ EcatCoe → EcatSdo / EcatSdoInfo    （协议载荷）         │
│      ├ EcatDictionary     对象模型（被查询者）                      │
│      ├ EcatSync           同步与时基                               │
│      └ EcatSiiService     SII 命令                                 │
└───────────────────────────────┬──────────────────────────────────┘
                                │ 只有 interface/ 里的纯虚契约
┌───────────────────────────────▼──────────────────────────────────┐
│ L2  硬件契约        EcatEsc / EcatSii / EcatPhy（纯虚，零厂商符号） │
└───────────────────────────────┬──────────────────────────────────┘
                                │ 实现
┌───────────────────────────────▼──────────────────────────────────┐
│ L1  硬件实现        drivers/ethercat/hal/hpm/  （子树，允许厂商头） │
│                    EcatEscPortHpm + PHY 驱动 + SII 介质            │
└──────────────────────────────────────────────────────────────────┘
```

依赖**只能向下**。三条硬规矩：

1. L3 的任何类都**不许 include** L1 的头，不许出现 `hpm_`、`HW_Esc`、`DISABLE_ESC_INT`、`pEsc`。
2. L1 的任何文件都**不许 include** L3 的头，不许出现 `EcatDevice`、`EcatStateMachine`，不许写"协议是否在运行"这类标志。
3. L3 内部**只有一个方向**：`EcatStateMachine` 主动调用别人；别人只能通过**被注入的接口**回调状态机，不能持有 `EcatStateMachine&`。

### 2.2 每层的职责

| 层 | 持有状态 | 做的决定 | 禁止 |
|---|---|---|---|
| L5 业务 | 业务自己的量 | 业务语义 | 不许知道 SM 地址、AL 状态、对象索引 |
| L4 门面 | PDO 交接镜像、回调指针 | 哪一帧该交给业务的线程侧、哪一帧可以同步回调 | 不许实现任何协议转换 |
| L3 核心 | 全部协议状态 | 状态转换、通道开关、超时、重传 | 不许碰寄存器（全部走 L2）、不许认识板卡 |
| L2 契约 | 无 | 无 | 不许有任何实现、不许 include 厂商头 |
| L1 实现 | 设备句柄、PHY、Flash 分区 | 寄存器怎么读写、引脚怎么配、EEPROM 走哪条路 | 不许有协议状态、不许替 L3 做协议应答 |

### 2.3 L3 内部：各域的状态归属表

这张表是本规划的核心。每一行都是**唯一持有者**，没有第二份拷贝。

| 域 | 类 | 独占持有的状态 | 对外只暴露 |
|---|---|---|---|
| AL 状态机 | `EcatStateMachine` | 当前 AL 状态、AL 状态码、待完成转换、ESM 超时计数、本地错误、是否等待主站确认 | `Current()` / `StatusCode()` 只读；`HandleControl()` 入口；`CompleteTransition()` |
| 对象模型 | `EcatDictionary` | 对象表（只读常量）、对象数据、条目描述、访问权限 | `Read()` / `Write()` / `Find()` / `EntryOffset()` |
| 过程数据 | `EcatProcessData` | 映射表、两方向实际长度、两方向过程缓冲 | `Validate()` / `Layout()` / `PublishInputs()` / `ConsumeOutputs()` |
| 邮箱传输 | `EcatMailbox` | 收发队列、缓冲池、四个缓冲指针（write/read/repeat/store）、toggle、满/锁标志 | `Start()` / `Stop()` / `Service()` / `Transmit()` |
| CoE | `EcatCoe` | 无（纯路由） | `Dispatch()` / `Continue()` |
| SDO | `EcatSdo` | 待处理请求、分段会话、存储的应答 | `Handle()` / `PollPending()` / `Abort()` |
| SDO Info | `EcatSdoInfo` | 分片游标 | `Handle()` |
| 同步 | `EcatSync` | SM 参数快照（0x1C32/33 来源）、DC 计数、看门狗计数、周期测量 | `StartSm()` / `StopSm()` / 参数只读视图 |
| SII | `EcatSiiService` | 延迟落盘计数、读宽度 | `PollCommand()` |

**空桩一律不出现。** 如果某个能力当前确实没实现（例如 `APPL_GetDeviceID` 返回 0、`APPL_AckErrorInd` 空），那就在**调用方**判断"该能力未启用"，而不是留一个"看起来生效、实际什么都不做"的函数让调用方误以为成功。上一版 8 个 `return 0` 空桩就是反例。

### 2.4 为什么不能按 SSC 的 `.c` 文件切

对照表说明"切割轴不同"：

| SSC 源文件 | 里面实际混了什么 | 本规划归到哪里 |
|---|---|---|
| `ecatslv.c` (2795 行) | AL 状态机 + SM 通道使能 + DC 看门狗 + 错误处理 + SM 区校验 | 拆成 3 个：`EcatStateMachine` / `EcatSync` / `EcatProcessData::Validate` |
| `ecatappl.c` (1202 行) | PDI ISR + Sync0/1 ISR + 1ms 定时 + PDO 映射 + 应用调用 + SII 命令 | 拆成 4 个：`EcatDevice`（分发）/ `EcatProcessData` / `EcatSync` / `EcatSiiService` |
| `mailbox.c` (1000 行) | 队列 + 缓冲所有权 + repeat + 协议分发 | 1 个：`EcatMailbox`（协议分发改成注入的回调） |
| `coeappl.c` (945 行) | CoE 分发 + 标准对象初始化 + 字典链表管理 + 时间戳 | 拆成 3 个：`EcatCoe` / `EcatDictionary` / `EcatSync` |
| `objdef.c` (1795 行) | 字典查询 + 描述查询 + 类型转换 + 权限校验 + 同步类型写入钩子 | 1 个：`EcatDictionary` |
| `sdoserv.c` (1349 行) | SDO 请求 + 分段 + pending + Info | 拆成 2 个：`EcatSdo` / `EcatSdoInfo` |
| `ecatcoe.c` (188 行) | CoE 服务分发 | 合并进 `EcatCoe` |
| `digital_io.c` | 应用钩子示例 | 删除（业务钩子归 L4） |
| `digital_ioObjects.h` (438 行) | 对象表 + 名称串 | `EcatApplicationObjects`（`constexpr`） |

**7 个 `.c` → 9 个类，但映射关系不是一对一。** 判断一个类是否该独立，看的是"它独占的状态是否与别人不重叠"，不是"它原来在哪个文件"。

---

## 3. 上下文与并发

这是与 USB 最大的不同点，USB 只有一个线程驱动，EtherCAT 有三个执行上下文。

| 上下文 | 谁在里面跑 | 时延要求 | 允许做什么 | 禁止做什么 |
|---|---|---|---|---|
| **PDI ISR** | `EcatDevice::OnPdi()` → `EcatProcessData` + `EcatSync` | 微秒级 | 读 AL 事件、拷贝 SM2/SM3 缓冲、调用业务 PDO 钩子、更新同步计数 | 邮箱、SDO、日志格式化、任何阻塞、任何动态内存 |
| **1 ms 定时器 ISR** | `EcatDevice::OnTick1ms()` → ESM 超时递减 + DC 看门狗 + SII 延迟计数 | 微秒级 | 只做计数 | 调 `Poll()`、重入 `EcatDevice` 的主流程 |
| **业务线程（1 ms 单拍）** | `EcatDevice::Poll()` → `EcatStateMachine` + `EcatMailbox` + 链路刷新 | 毫秒级 | AL 状态转换、邮箱收发、SDO、链路轮询 | `while` 死循环、`k_sleep`、二次启动协议循环 |

**PDO 业务钩子跑在 PDI ISR 上下文，这是刻意的，不是遗留。**

理由：这棵树的用途是机器人控制，1 kHz 控制环要求"帧到即算"。把业务钩子挪到 1 ms 线程会让抖动从 ISR 的亚微秒级退化到 `k_sleep` 唤醒的几十微秒~数百微秒，是明确的退步。所以 L4 的 `EthercatSlave` 只做一件事：**把 ISR 里已经拷好的缓冲交给已注册的钩子**，钩子契约是"只做拷贝与赋值，不阻塞、不分配"。

**但邮箱、SDO、AL 状态机在线程侧**，因为它们本来就是尽力而为的，而且要拉长处理时间（分段 SDO 可能跨很多周期）。

跨上下文共享的字段清单（必须用短临界区或原子）：

| 字段 | 写方 | 读方 | 保护方式 |
|---|---|---|---|
| 过程数据缓冲 | PDI ISR | PDI ISR（同上下文） | 无需 |
| 业务 PDO 交接镜像（L4） | PDI ISR 写 / 线程侧读 | 双向 | `irq_lock` 短临界区 |
| AL 状态 / 状态码 | 线程 | PDI ISR（只读，决定要不要映射） | `std::atomic<uint8_t>` |
| SM 参数快照（0x1C32/33） | 线程（SDO 写） / ISR（计数） | 双向 | `std::atomic<uint16_t>` 或短临界区 |
| SII 落盘请求标志 | 1 ms ISR | 线程 | `std::atomic<uint32_t>` 交换 |

**`volatile` 不算同步。** SSC 用 `volatile` 掩盖的竞态，本规划一律换成 `std::atomic` 或 `irq_lock` 临界区，并在每个字段上写清楚它属于哪个上下文。

---

## 4. 目标目录

```text
framework/drivers/communication/ethercat/
  ethercat.hpp / ethercat.cpp          L4 门面：EthercatSlave
  interface/
    ecat_types.hpp                     纯值类型：AlState / AlStatus / PdoLayout / Event / SyncType / Result
    ecat_esc.hpp                       ESC 硅片能力（寄存器 + DPRAM + 事件 + MDIO）
    ecat_sii.hpp                       SII 介质能力（读/写/重载/落盘）
    ecat_phy.hpp                       PHY 芯片能力（保持现有扩展点）
  core/
    ecat_limits.hpp                    容量、超时、超限值、身份标识（原 ecat_def 宏的 constexpr 版）
    ecat_registers.hpp                 ESC 标准偏移与掩码（原 esc.h）
    ecat_wire.hpp                      LE 编解码 + 邮箱/CoE/SDO 报文字段视图
    ecat_object.hpp                    对象条目/描述/权限/类型宽度的纯数据定义
    ecat_application_objects.hpp       应用对象表（0x1600/1A00/1C12/1C13/6000/7010/2000 + 名称串），constexpr
    ecat_dictionary.hpp / .cpp         对象模型：查找、读写、CA、条目偏移、状态门
    ecat_process_data.hpp / .cpp       过程数据域：映射校验、布局、两方向缓冲
    ecat_mailbox.hpp / .cpp            邮箱域传输：队列、缓冲池、repeat、toggle
    ecat_sdo.hpp / .cpp                SDO 会话
    ecat_sdo_info.hpp / .cpp           SDO Info
    ecat_coe.hpp / .cpp                CoE 路由
    ecat_sync.hpp / .cpp               同步与时基：SM 参数、DC、看门狗、周期测量
    ecat_sii_service.hpp / .cpp        ESC EEPROM 命令处理
    ecat_state_machine.hpp / .cpp      AL 状态机
    ecat_device.hpp / .cpp             组合根 + 事件分发
  LICENSE-BECKHOFF.md                  派生来源与文件对应表

drivers/ethercat/hal/
  hpm/
    ecat_esc_port_hpm.hpp / .cpp       ESC 端口：MMIO、IRQ、MDIO、DPRAM（原 esc_hpmicro.c + hpm_ecat_hw.c）
    ecat_sii_hpm.hpp / .cpp            SII 介质：Flash 模拟 / 板载 I2C 装载
    ecat_board_hpm.hpp                 板级常量（分区地址、端口数）
  phy/                                 PHY 芯片驱动（jl1111 / rtl8201f / tja1101b），整目录搬迁
```

**相对当前代码的命名变更**（避免新旧并存时符号冲突）：

- 旧的 `core/ecat_pdo.hpp`（`EcatPdo::Generate` 静态工具类）保持不动，最终删除；新类叫 `EcatProcessData`，名字本身就说明"它拥有过程数据这个域"，而不是"一个算长度的工具"。
- `interface/ecat_hal.hpp` → `interface/ecat_esc.hpp`：`EcatHal` 这个名字掩盖了它其实同时管 ESC、SII、时基、临界区。拆成 `EcatEsc` + `EcatSii` 两个契约，时基和临界区不再进契约。

---

## 5. 接口定案

### 5.1 业务入口（L4）

```cpp
namespace ecat
{
	/**
	 * @brief 从站业务入口
	 */
	class EthercatSlave final
	{
	public:
		using PdoHandler = void (*)(void* context, const uint8_t* data, uint16_t length);

		bool Init();
		bool Poll();
		void Disable();

		bool IsReady() const;
		PdoLayout Layout() const;
		AlState State() const;

		void SetRxHandler(PdoHandler handler, void* context);
		void SetTxProvider(PdoHandler provider, void* context);

	private:
		EcatDevice    device_;
		EcatEscPortHpm& port_ = GetDefaultEcatPort();
	};
}
```

变化点（相对当前 `ethercat.hpp`）：

| 现在 | 改后 | 理由 |
|---|---|---|
| `EcatStack& stack_ = EcatStack::Instance()` | `EcatDevice device_` 值成员 | 单例在嵌入式里只带来"静态初始化顺序"和"无法测试"两个副作用 |
| `std::function` 回调 | 函数指针 + `void* context` | `std::function` 有堆分配和间接调用，ISR 里不合适 |
| 回调 `(data, len)` 无 ctx | `(ctx, data, len)` | 多实例/可测试 |
| `Stop()` | `Disable()` | 只做生命周期清理，不宣称能 Stop→Init 热重启 |
| 没有状态查询 | `State()` | 业务要能知道现在是不是 OP |

`Poll()` 仍然单拍返回，线程框架的统计与故障策略照常生效（见 `threads-register-init` 的 `task-must-not-block`）。

### 5.2 ESC 契约（L2）

```cpp
class EcatEsc
{
public:
	using EventHandler = void (*)(void* context, EcatEvent event);

	struct Config
	{
		uint8_t port_count = 2;    // 使能端口数
	};

	virtual ~EcatEsc() = default;

	virtual bool Open(const Config& config, EventHandler handler, void* context) = 0;
	virtual void Close() = 0;
	virtual void StartEvents() = 0;
	virtual void StopEvents() = 0;

	virtual void Read(uint16_t address, void* data, uint16_t length) = 0;
	virtual void Write(uint16_t address, const void* data, uint16_t length) = 0;
	virtual uint16_t ReadAlEvent() = 0;

	virtual bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* value) = 0;
	virtual bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t value) = 0;

	virtual void PollLink() = 0;

	virtual uint32_t MaskEvents() = 0;
	virtual void UnmaskEvents(uint32_t token) = 0;
};
```

**删掉的四个成员，逐个交代去处**：

| 原成员 | 为什么删 | 去哪 |
|---|---|---|
| `TimerGet()` / `TimerClear()` | 这是 SSC 的"全局时基"模拟，真实语义就是"取毫秒时间" | L3 直接用 `k_uptime_get()` |
| `EnterCritical()` / `LeaveCritical()` | 无返回值，嵌套就错；SSC 里用它包住"关 ESC 中断" | 换成 `MaskEvents()` 返回 token + `UnmaskEvents(token)`；嵌套由调用点自己管更清楚 |
| `AlEvent()` | 语义模糊（读 0x0220 会清事件位） | 改名 `ReadAlEvent()`，明确"有确认副作用" |
| `EepromRead/Write/Reload` | 这是**介质**能力，不是 ESC 硅片能力 | 拆到 `EcatSii` |

### 5.3 SII 契约（L2）

```cpp
class EcatSii
{
public:
	enum class Mode : uint8_t
	{
		Emulated,    // Flash 分区模拟，应用要接管读写
		OnBoard,     // 板上 I2C EEPROM，ESC 自己走 I2C
	};

	virtual ~EcatSii() = default;
	virtual Mode GetMode() const = 0;

	/** 读一个 SII 字到 data；返回 false 表示介质错误，与数据分离 */
	virtual bool ReadWord(uint32_t word_address, uint16_t* data) = 0;
	virtual bool WriteWord(uint32_t word_address, uint16_t data) = 0;
	virtual bool Reload() = 0;
	virtual void Store() = 0;
};
```

**这一条是上一版最典型的错误示范**：旧接口 `uint16_t EepromRead(uint32_t addr)` 返回 `uint16_t`，但 `EcatEeprom::EepromReadHook` 里写的是 `(void)s_hal->EepromRead(word_addr); return 0U;` —— **读到的数据被丢掉了**。数据和错误码塞在同一个返回值里，调用方必须猜哪个是哪个。新契约把两者分开。

### 5.4 过程数据域（L3）

```cpp
class EcatProcessData
{
public:
	/** 由 0x1C12/0x1C13 + 0x1600/0x1A00 计算实际长度并校验；返回 AL 状态码 */
	uint16_t Configure();

	PdoLayout Layout() const;

	/** SM 区与邮箱区、两方向之间的重叠校验 */
	uint16_t ValidateAreas(uint16_t sm2_address, uint16_t sm3_address,
	                       uint16_t mbx_out_address, uint16_t mbx_out_size,
	                       uint16_t mbx_in_address, uint16_t mbx_in_size) const;

	/** ISR 上下文：从 ESC 取回本周期输出，交给业务 */
	void ConsumeOutputs(EcatEsc& esc, uint16_t address);

	/** ISR 上下文：取业务填好的输入，写进 ESC */
	void PublishInputs(EcatEsc& esc, uint16_t address);

	void SetOutputSink(PdoHandler handler, void* context);
	void SetInputSource(PdoHandler provider, void* context);
};
```

注意这里**没有** `ResetStorage()`。SSC 需要它是因为全局变量必须显式复位；C++ 对象构造时就已经是正确初值。

**已知语义缺口（本次不改）**：映射进 0x1600 的对象（0x7010）与实际 PDO 缓冲是两块内存，不自动互拷。改这个会改变"SDO 读 0x7010 的值"这一行为，必须单独立项验证。

### 5.5 邮箱传输域（L3）

```cpp
class EcatMailbox
{
public:
	using Dispatch = uint8_t (*)(void* context, MailboxFrame* frame);

	uint16_t Start(EcatEsc& esc);
	void Stop(EcatEsc& esc);
	void SetDispatcher(Dispatch dispatcher, void* context);

	/** 线程上下文：取出主站写来的报文，交给分发器，把应答送回 ESC */
	void Service(EcatEsc& esc);

	/** 目标缓存的提交结果：入队 / 重发槽 / 被拒绝 */
	enum class SubmitResult : uint8_t
	{
		Queued,
		NoMemory,
	};

	SubmitResult Submit(MailboxFrame* frame);
};
```

**关键改动：断掉"邮箱知道 SDO"的依赖。** SSC 的 `MailboxServiceInd` 里直接 `switch(Protocol)` 调 `COE_ServiceInd`；新结构里邮箱只认一个 `Dispatch` 回调，`EcatDevice` 负责把 CoE 接上去。这样 `EcatMailbox` 编译单元里不会出现 `EcatCoe` 的任何符号。

SSC 的 `APPL_AllocMailboxBuffer` 默认展开成 `malloc`/`free`。**重构后必须换成静态池**：`kMailboxBufferCount × kMaxMailboxSize` 的固定数组 + 位图，理由见 §6.2。

### 5.6 AL 状态机（L3）

```cpp
class EcatStateMachine
{
public:
	/** 线程上下文：读取 AL Control 寄存器并推进状态 */
	void Poll(EcatEsc& esc);

	/** ISR 上下文：只读，供过程数据判断"要不要映射" */
	AlState Current() const;
	uint16_t StatusCode() const;

	/** 1 ms 上下文：超时递减 */
	void Tick();

	/** 本地错误上报（由其他域调用，不是设备状态机本身） */
	void ReportLocalError(uint16_t code, AlState state);

private:
	uint16_t ApplyRequest(AlState requested);          // 分派到具体转换
	uint16_t EnterPreOp();
	uint16_t EnterSafeOp();
	uint16_t EnterOp();
	uint16_t LeaveOp();
	uint16_t BackToInit();
	void     Publish(AlState state, uint16_t code);
	...
};
```

**它调用别人，别人不调用它。** 转换到 SafeOp 时它依次调 `process_data_.Configure()`、`mailbox_.Start(esc)`、`sync_.PrepareSm()`；转换失败时按相反顺序回滚。这样"打开通道的顺序"这一条协议知识集中在状态机里，而不是散在 4 个文件。

### 5.7 组合根（L3）

```cpp
class EcatDevice
{
public:
	bool Init(EcatEsc& esc, EcatSii& sii);
	bool Poll();
	void Disable();

	void OnPdi();
	void OnSync0();
	void OnSync1();
	void OnTick1ms();
	void OnEepromCommand();

	PdoLayout Layout() const;
	AlState State() const;

	void SetOutputSink(PdoHandler handler, void* context);
	void SetInputSource(PdoHandler provider, void* context);
	...
};
```

`EcatDevice` 是唯一的 `friend` 目标吗？**不是。它一个 friend 都不需要**。它只调公有方法；各子域之间通过 `EcatDevice` 提供的小接口互相转发（字典状态查询、同步参数写入），而不是互相持有引用。

---

## 6. 硬约束

### 6.1 必须保持的行为（逐条可验证）

| 项 | 值 / 顺序 | 来源 |
|---|---|---|
| 初始化顺序 | `Open` → 关中断 → 装 PDI/RESET 回调 → 配 EEPROM 模式 → SII 首批应答 → 校验 EEPROM → PHY 复位/配置/链路 → `EcatDevice::Init`（字典/邮箱/状态机） → `StartEvents` → 关 SYNC0/1 → 循环 | 已验证的参考实现 main()，11 步 |
| AL 状态位 | INIT=1 / PREOP=2 / SAFEOP=4 / OP=8，低四位是状态，`0x10` 是错误确认位，`STATE_DEVID=0x20` | `ecatslv.h` |
| 合法转换 | 16 个组合（含 INIT→SAFEOP、INIT→OP 的**拒绝**分支），不是只有正向链 | `ecatslv.c` 的 `stateTrans` 分支 |
| ESM 超时 | `SAFEOP2OPTIMEOUT = 0x2328 = 9000 ms`，然后**减 10%** → 实际 8100 ms | `ecatslv.c:1909` 与 `EsmTimeoutCounter -= EsmTimeoutCounter/10` |
| 其他超时 | INIT→PREOP / INIT→BOOT 用 `PREOPTIMEOUT`；默认 200 ms | 同上 |
| SAFEOP→OP 门槛 | 必须收到首帧输出（`first_outputs_received`），不能收到请求就置 OP | `ecatslv.c` |
| PDO 容量 | 每方向 64 B 缓冲上限；实际有效 32 B（8×UDINT） | `MAX_PD_INPUT_SIZE/OUTPUT_SIZE = 0x40` |
| 邮箱尺寸 | SM0 收 = `MIN_MBX_SIZE = 0x24`，SM1 发 = `MAX_MBX_SIZE = 0x80`，头 6 B | `mailbox.c:258` |
| 邮箱队列 | 容量 10（`MAX_MBX_QUEUE_SIZE`）+1 | `mailbox.h:70` |
| 对象表 | 0x1000/1001/1008/1009/100A/1018/10F1/10F8/1C00/1C32/1C33 + 应用对象 0x1600/1A00/1C00/1C12/1C13/2000/6000/7010/F000 | `digital_ioObjects.h` / `objdef.c` |
| PDO 映射 | 0x1600 = 0x7010:01..08，0x1A00 = 0x6000:01..08，各 8 个 UDINT | `digital_ioObjects.h` |
| 身份 | Vendor 0x0048504D、Product 1、Revision 1、Serial 0、DeviceName "digital_io" | `ecat_def.h` |
| 链路刷新 | 100 ms 一次 | 当前 `EcatStack::Poll` |
| SII 尺寸 | 2048 B（`ESC_EEPROM_SIZE = 0x800`） | `ecat_def.h` |
| 首帧 FreeRun 兜底 | `MainLoop` 里那段被注释，**保持注释状态** | `ecatappl.c:854` |

### 6.2 内存

上一版把常量表搬进 RAM 是明确退步。本规划的硬指标：

| 项 | 现在 | 目标 |
|---|---|---|
| 对象表与名称串 | 运行期 `memcpy` 到成员数组（约 1.4 KB RAM） | `constexpr` 常量表，**0 B RAM** |
| SII 初始镜像 | 非 const 全局（2048 B RAM） | 常量模板 + 一份 RAM 副本（保持可写，因为主站可写 SII） |
| 对象数据 | C 全局 | 类的成员（与现在等量） |
| 邮箱缓冲 | `malloc`/`free`（`MAILBOX_QUEUE=1` 时实际调用堆） | 静态池，`kMailboxBufferCount × 0x80`，**不碰堆** |
| PDO 缓冲 | 两方向各 64 B | 不变 |
| 新增虚表 | — | 每个协议类 1 个 vptr（约 4 B/实例），只有 `EcatEsc`/`EcatSii`/`EcatPhy` 有虚函数；协议类**不要虚函数** |

**协议类不要虚函数。** 这些类是值成员、生命周期固定、没有多态替换需求。给它们加虚函数只换来一个 vptr 和不可内联。只有 L2 契约（`EcatEsc` / `EcatSii` / `EcatPhy`）需要虚函数，因为每个平台要换实现。

实施前必须算出 `sizeof(EcatDevice)` 并与现在所有全局变量总和对账，写进 `doc/` 的完成记录里。

### 6.3 报文

- 邮箱头 6 B、CoE/SDO 字段偏移、toggle 位、abort code 全部按原值保留。
- 一律用显式 LE 编解码（`ecat_wire.hpp`），**不许**把寄存器结构体直接 `reinterpret_cast` 到 ESC DPRAM。
- 对象条目偏移是**位**，`OBJ_GetEntryOffset` 返回的是 bit 偏移，转字节要 `>>3`；SSC 里 `>>4` 的地方是 `UINT16` 指针运算，迁移时逐处核对，不许照抄位移量。
- Complete Access 的 subindex0 占位行为不能当普通 `uint8_t` 数组处理。

### 6.4 许可

SSC 5.13 是 Beckhoff 的 Slave Stack Code，派生使用必须保留许可与来源说明。本规划的路径是：

1. 迁移期间 SSC 源码原样保留在 `ssc/`，作为**行为对照参考**，不参与"新实现调用旧实现"。
2. 新实现独立书写，但行为等价。凡是直接照搬了 SSC 的数据结构、常量表、状态转换表的地方，在 `core/LICENSE-BECKHOFF.md` 里登记"来源：SSC 5.13 `<文件>`"。
3. 全部切换验证通过后删除 `ssc/`，`LICENSE-BECKHOFF.md` 保留并记录派生范围。
4. 不把派生代码标成 Apache-2.0 自研原创。

---

## 7. 迁移步骤

**迁移方式：新旧并存 → 单次切换 → 清旧。**

- P1~P6 期间旧栈是唯一运行引擎，新类只被编译、不被实例化、不碰硬件。
- 新类**不许**调用旧 SSC 函数来"先跑起来"。每个新类自己完整实现。
- 一个域写完不等于能跑，整条链直到 P7 才能替换。这是刻意的：分层的价值就在于可以分域独立验证，不需要"半个协议栈"。

每步只改表里指定的文件（hpp+cpp 是同一个类的原子组）。用户说"开始第 N 步"= 只做那一步，改完停下报告，等"继续"。**不许一次做多步，不许跨步连带。**

### P1 数据契约（步骤 1~5）

> **P1 拆成 5 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 不许一次做多步，不许跨步连带改（必要时见每步的「连带」栏）。
> 本阶段全部可编译：新文件是纯定义，没有调用方。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `interface/ecat_types.hpp` | 增加 `EcatEvent` / `AlState` / `PdoLayout` / `SyncType` / `MailboxProtocol` / `AlStatusCode` 常量；保留现有 `EcatIrq` / `PdoLayout` 一段时间以维持旧栈编译 | 本文 | 无（纯新增） |
| 2 | `core/ecat_limits.hpp` | 把 `ecat_def.h` 里被实际用到的值迁成 `constexpr`：容量、超时、身份、邮箱尺寸、队列深度；每个常量注释标原宏名 | 1 | 不删旧宏 |
| 3 | `core/ecat_registers.hpp` | 把 `esc.h` 的偏移/掩码分类迁成 `constexpr`；标注每项的访问宽度和"读取是否有确认副作用" | 1 | 保留 `esc.h` |
| 4 | `core/ecat_wire.hpp` | LE 16/32/64 编解码；邮箱头 / CoE 头 / SDO 头 / SDO 分段的字段视图（偏移常量 + get/set） | 1、2 | 未被任何文件包含 |
| 5 | `core/ecat_object.hpp` | 对象条目/描述/权限/类型宽度的纯数据定义；`EntryOffset` 的位→字节换算；**不含查表逻辑** | 2 | 无 |

### P2 对象模型（步骤 6~8）

> **P2 拆成 3 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 本阶段全部可编译：`EcatDictionary` 尚未被任何运行路径调用。
> 不可编译的步骤：无。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 6 | `core/ecat_application_objects.hpp` | 应用对象表 + 标准对象表，**全部 `constexpr`**：0x1000/1001/1008/1009/100A/1018/10F1/10F8/1C00/1C32/1C33/1600/1A00/1C12/1C13/2000/6000/7010/F000。名称串用带内嵌 `\0` 的字面量，逐字节与 `digital_ioObjects.h` 对齐 | 5 | 无；**不许留 `memcpy` 初始化** |
| 7 | `core/ecat_dictionary.hpp` / `.cpp` | 查表（含对象列表分片）、按 index/sub 读写、Complete Access、条目描述查询、权限与类型宽度校验、状态门（通过注入的 `AlStateQuery` 回调，不持有状态机） | 5、6 | 无 |
| 8 | `core/ecat_sync.hpp` / `.cpp` | SM 参数快照（0x1C32/33 的真实来源）、DC 计数、看门狗、周期测量；**只声明 `PrepareSm/StartSm/StopSm/Tick`，暂不接硬件** | 1~4 | 无 |

> 步骤 8 归 P2 是因为 0x1C32/0x1C33 这两个对象必须绑定真实字段，字典才能算完。

### P3 过程数据（步骤 9~10）

> **P3 拆成 2 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 本阶段全部可编译：旧 `EcatPdo` 静态工具类仍在，新旧不冲突（类名不同）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 9 | `core/ecat_process_data.hpp` / `.cpp` | 映射校验（0x1C12/13 → 0x1600/1A00 逐条目累加位长度）、布局计算、两方向 64 B 缓冲、`ConsumeOutputs` / `PublishInputs`、SM 区重叠校验 | 5~7 | 不动旧 `ecat_pdo.*` |
| 10 | `core/ecat_mailbox.hpp` / `.cpp` | SM0/1 传输：静态缓冲池（位图分配，替换 `malloc`）、收发队列、repeat/toggle、`Dispatch` 回调接缝；`Start/Stop/Service/Submit` | 4、8 | 无 |

### P4 邮箱协议（步骤 11~14）

> **P4 拆成 4 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 全部可编译：这些类只被彼此引用，没有运行路径。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 11 | `core/ecat_sdo.hpp` / `.cpp` | 请求解析（expedited / normal / segmented）、下载/上传分段、toggle、abort code 全表、pending 应答所有权 | 7、10 | 无 |
| 12 | `core/ecat_sdo_info.hpp` / `.cpp` | 对象列表分片、条目描述查询、续发游标 | 7、10 | 无 |
| 13 | `core/ecat_coe.hpp` / `.cpp` | CoE 头解析与路由（SDO / SDO Info / 不支持的服务按原错误返回） | 11、12 | 无 |
| 14 | `core/ecat_sii_service.hpp` / `.cpp` | ESC EEPROM 命令处理：busy/error/CRC、读宽度 4/8、reload、延迟落盘 | 3、4 | 无 |

### P5 组合根（步骤 15~16）

> **P5 拆成 2 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 可编译，但此时新链路仍未接入任何入口。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 15 | `core/ecat_state_machine.hpp` / `.cpp` | 16 个转换分支、ESM 超时、错误确认、pending、回滚顺序、`ReportLocalError` | 7~14 | 无 |
| 16 | `core/ecat_device.hpp` / `.cpp` | 组合根：`Init/Poll/OnPdi/OnSync0/OnSync1/OnTick1ms/OnEepromCommand`；把各域按 §6.1 的顺序串起来 | 1~15 | 无 |

### P6 硬件实现（步骤 17~22）

> **P6 拆成 6 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 新 HAL 用新类名旁路存在，旧端口层继续供旧栈使用。
> **不可编译的步骤：无。** 第 22 步是第一个"新实现参与真实编译"的检查点，由用户编译。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 17 | `drivers/ethercat/hal/hpm/ecat_esc_port_hpm.hpp` / `.cpp` | 从 `esc_hpmicro.c` + `hpm_ecat_hw.c` 迁移：MMIO、DPRAM 读写、AL 事件、MDIO、事件注册与屏蔽；实现 `EcatEsc` | 16 | 不动旧 `esc_hpmicro.c`；**不重复定义同一 Zephyr 设备** |
| 18 | `drivers/ethercat/hal/hpm/ecat_sii_hpm.hpp` / `.cpp` | 从 `hpm_ecat_e2p_emulation.c` 迁移 Flash 模拟；板载 I2C 模式走已有驱动装载；实现 `EcatSii`；读宽度由调用方传入，不存全局 | 17 | 不动旧端口文件 |
| 19 | `drivers/ethercat/hal/phy/**` | 整目录从 `hal/ports/phy/` 搬到 `hal/phy/`，只适配 `EcatPhy` 契约（`MdioRead/MdioWrite` 由 `EcatEsc&` 提供），**芯片配置值一个不改** | 17 | CMake 暂不切换；旧路径保留副本直到第 27 步 |
| 20 | `drivers/ethercat/hal/hpm/ecat_board_hpm.hpp` | 板级常量（Flash 分区地址、端口数、SII 尺寸） | 17 | 无 |
| 21 | `drivers/CMakeLists.txt` | 新 HAL 源文件加入编译；旧端口层仍在；精确列文件，不 glob；两套都无启动副作用 | 17~20 | 本步后新旧同时编，符号不冲突 |
| 22 | `framework/drivers/CMakeLists.txt` | 新 core 15 个源文件加入编译；旧 SSC/APPL/旧 core 仍在 | 1~16、21 | **首个完整新实现编译检查点**，由用户编译 |

> 第 22 步之前，"新实现能编译"只是静态审查结论，不作"已通过编译"声明。

### P7 切换（步骤 23~26）

> **P7 拆成 4 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> **不可编译的步骤：23、24、25。第 26 步恢复完整可编译。**
> 这四步是同一个切换批次。若不希望经历"改到一半编不过"，把这四步作为一次授权连续执行，但仍然按步报进度，不许静默做完。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 23 | `ethercat.hpp` / `.cpp` | 改为值成员 `EcatDevice`，回调换成函数指针 + ctx，加 `State()`；删掉 `EcatStack` 引用 | 22 | `trd_test.cpp` 签名不匹配 → **编不过**，第 26 步修 |
| 24 | `drivers/CMakeLists.txt` | 停编旧端口层（`esc_hpmicro.c`、`hpm_ecat_hw.c`、`ecat_zephyr_port.c`、`hpm_ecat_e2p_emulation.c`），启用新 HAL 工厂 | 23 | 旧栈失去端口层 → 仍编不过 |
| 25 | `framework/drivers/CMakeLists.txt` | 停编 SSC 7 个 `.c`、`appl/ecat_appl_hooks.cpp`、旧 `ecat_stack.*` / `ecat_pdo.*` / `ecat_eeprom.*` | 24 | 旧栈彻底移除 → 仍编不过 |
| 26 | `project/thread/test/trd_test.cpp` | 适配新签名（`PdoHandler` + ctx）；`PdoOutputs` / `PdoInputs` 两个 32 B 结构体保持；日志每条 ≤4 个参数 | 25 | **本步后恢复完整可编译**，等用户编译+烧录 |

### P8 清旧与收尾（步骤 27~30）

> **P8 拆成 4 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。
> 前置：步骤 26 之后用户已完成基础上板验证（OP + 双向 PDO）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 27 | `ssc/` 目录 + `appl/` 目录 | 先逐项清点"旧函数 → 新方法"映射表，确认无遗漏后整目录删除 | 26 验收 | 无可编译影响（已停编） |
| 28 | `drivers/ethercat/hal/ports/` 整目录 | 删除旧 C 端口层、旧头、PHY 旧副本；保留仍被引用的第三方声明 | 27 | 无 |
| 29 | `drivers/Kconfig`、双方 `CMakeLists.txt` | 清除 SSC include 路径、旧文件名、过期 help 文本；保留 PHY/EEPROM 门禁 | 28 | 不动其他 Kconfig 和 dts |
| 30 | 本文 + `core/LICENSE-BECKHOFF.md` | 回填完成状态、接口用法、功能覆盖、未验证项；登记派生来源 | 27~29 | 不提交不上传，除非用户单独要求 |

---

## 8. 典型 old → new 对照

### 8.1 空桩不是"待实现"，是"骗调用方"

```cpp
// old（EcatDevice 里 8 个）：状态机以为自己在激活 SM，其实什么都没做
uint16_t StartInputHandler(uint16_t*) { return 0; }
uint16_t StartOutputHandler() { return 0; }
uint16_t GetDeviceID() { return 0; }

// new：把"没实现"变成"显式不支持"，由调用方决定怎么办
uint16_t EcatDevice::StartOutput() const
{
	return kAlStatusCodeNoError;   // 输出通道只有 SM2 使能，没有额外应用准备
}
```

原则：**每个返回 `NOERROR` 的钩子必须在注释里写清"为什么这里可以是空的"**，写不出来的就是没实现，不该返回成功。

### 8.2 常量表不许搬进 RAM

```cpp
// old（ecat_objects.cpp，280 行）：每个实例跑一遍 memcpy，占 RAM
void EcatObjectConfig::ResetStorage()
{
	unsigned char initial[264] = "SM output parameter\000Synchronization Type\000...\377";
	std::memcpy(a_name0x1_c32_, initial, sizeof(initial));
	EntryDescription initial2[9] = { {kDeftypeUnsigned8, 0x8, kAccessRead}, ... };
	std::memcpy(as_entry_desc0x1600_, initial2, sizeof(initial2));
}

// new：编译期常量，0 B RAM
namespace objects
{
	inline constexpr char kNameSmOutParameter[] =
		"SM output parameter\0Synchronization Type\0Cycle Time\0\0...\0\xff";

	inline constexpr EntryDescription kEntryDesc0x1600[] = {
		{ DataType::Unsigned8,  8,  Access::Read },
		{ DataType::Unsigned32, 32, Access::Read },
		...
	};

	inline constexpr ObjectEntry kApplicationObjects[] = {
		{ 0x1600, ObjectCode::Record, kEntryDesc0x1600, kName0x1600,
		  &kMapping0x1600, nullptr, {} },
		...
	};
}
```

### 8.3 读写分离的是数据和错误

```cpp
// old：数据被丢掉，返回值被当成功
uint16_t EepromReadHook(uint32_t word_addr)
{
	(void)s_hal->EepromRead(word_addr);    // 读到的字去哪了？
	return 0U;                              // 永远成功
}

// new：数据与错误分开，调用方必须处理
bool EcatSiiService::ReadWord(uint32_t address, uint16_t* value)
{
	return sii_.ReadWord(address, value);
}
```

### 8.4 邮箱不知道 CoE 存在

```cpp
// old（mailbox.c）：邮箱里直接 switch 协议号调 CoE
UINT8 MailboxServiceInd(TMBX MBXMEM *pMbx)
{
	switch (GetProtocol(pMbx)) {
	case MODE_COE:   return COE_ServiceInd((TCOEMBX*)pMbx);
	case MODE_AOE:   return AOE_ServiceInd((TMOBXMEM*)pMbx);
	...
	}
}

// new：邮箱只认一个分发回调，CoE 由组合根接上去
void EcatMailbox::Service(EcatEsc& esc)
{
	...
	const uint8_t result = (dispatcher_ != nullptr)
		? dispatcher_(dispatch_context_, &frame)
		: kMailboxErrorUnknownService;
	...
}
```

### 8.5 状态机调用别人，而不是伸手改字段

```cpp
// old（C）：状态机、SM、PDO 全是全局量，跨域随手改
nEscAddrOutputData = (UINT16)(pSyncMan->AddressLength & SM_ADDRESS_MASK);
if (pSyncMan->Settings[SM_SETTING_CONTROL_OFFSET] & SM_SETTING_MODE_ONE_BUFFER_VALUE) {
	nPdOutputBuffer = 1;
}
bEcatFirstOutputsReceived = FALSE;

// new：谁的状态谁改，状态机只发命令
uint16_t EcatStateMachine::EnterSafeOp()
{
	const uint16_t configuration = process_data_.Configure();
	if (configuration != kAlStatusCodeNoError) {
		return configuration;
	}
	const uint16_t area = process_data_.ValidateAreas(...);
	if (area != kAlStatusCodeNoError) {
		return area;
	}
	const uint16_t mailbox = mailbox_.Start(esc_);
	if (mailbox != kAlStatusCodeNoError) {
		return mailbox;
	}
	first_outputs_received_ = false;
	...
}
```

### 8.6 没有友元

```cpp
// old：人人 friend，等于没有封装
class EcatDevice final
{
	friend class EcatStateMachine;
	friend class EcatCycle;
	friend class EcatMailbox;
	... 13 个 ...
};

// new：零 friend。跨域需要的信息通过小接口给
class EcatDictionary
{
public:
	using StateQuery = AlState (*)(void* context);
	void SetStateQuery(StateQuery query, void* context);
	...
};
```

---

## 9. 验证

### 9.1 静态检查（每完成一个阶段自查一次）

- [ ] `framework/drivers/communication/ethercat/core/` 与 `interface/` 里 `grep -rn "hpm_\|HW_Esc\|DISABLE_ESC_INT\|pEsc"` 无结果。
- [ ] `drivers/ethercat/` 里 `grep -rn "EcatDevice\|EcatStateMachine\|EcatMailbox"` 无结果。
- [ ] 整个 `ethercat/` 目录 `grep -rn "friend class"` 无结果。
- [ ] `grep -rn "malloc\|free(" framework/drivers/communication/ethercat/` 无结果。
- [ ] `grep -rn "memcpy(.*initial\|ResetStorage" framework/drivers/communication/ethercat/core/` 无结果（常量表不该运行期初始化）。
- [ ] `grep -rn "return 0; }\|{ return 0; }" core/*.hpp` 的每一项都有注释说明"为什么可以为空"。
- [ ] 每个类只有一个 `.hpp` + `.cpp` 对（`ecat_application_objects.hpp` / `ecat_limits.hpp` / `ecat_registers.hpp` / `ecat_wire.hpp` / `ecat_object.hpp` / `ecat_types.hpp` 为头文件式定义）。

### 9.2 编译检查点

| 步骤 | 谁编 | 预期 |
|---|---|---|
| 22 | 用户 | 新旧两套同时编译通过，无重复符号 |
| 26 | 用户 | 只剩新实现，完整编译通过 |
| 29 | 用户 | 清旧后仍编译通过 |

### 9.3 上板验证（步骤 26 之后，用户执行）

| 场景 | 证据 |
|---|---|
| 上电 → 主站扫描 | PHY link up、从站被扫到、AL 状态从 INIT 走到 OP |
| INIT → PREOP | 邮箱通道打开，SDO 可用 |
| PREOP → SAFEOP | 输入通道打开，`PdoLayout().tx_size == 32` |
| SAFEOP → OP | 收到首帧输出后才允许；未收到时请求 OP 被拒并给出 AL 状态码 |
| 主 → 从 PDO | 8 个 UDINT 全量核对，用变化序号排除旧帧；日志每条 ≤4 参数，分两次打 |
| 从 → 主 PDO | 主站收到完整 8 项，与 `TxProvider` 提交内容一致 |
| CoE 读 | 0x1018 身份、0x1600/0x1A00 映射、0x1C12/0x1C13 分配、0x1C32/0x1C33 同步参数 |
| SDO 错误路径 | 不存在索引 / 错误长度 / 只读写入 / 状态不允许 → 原 abort code |
| SDO 分段 | 多段上传/下载、toggle 翻转、终段长度、Complete Access 起始 subindex |
| 邮箱 repeat | 主站重发请求不丢应答、不重复释放缓冲 |
| 掉线重连 | 协议不死锁；`thread show test` 的 `run` 持续增长、`task last/max` 非 0 |
| SII | 板载 EEPROM 模式加载成功；Flash 模拟模式在对应硬件单独验收 |
| DC / Sync0 / Sync1 | 有主站配置时单独验收；没有则记录"已迁移、未上板验证" |

**不能只以"主站扫到从站"或"编译通过"判定完成。** 基础 OP + PDO 通过，也不代表邮箱/SDO 功能等价。

### 9.4 资源对账

完成时必须给出：

- `sizeof(EcatDevice)`，以及它与"被删掉的 C 全局变量总和"的对比。
- 新实现的 `.text` / `.rodata` / `.bss` / `.data` 增量（对比重构前的 map 文件）。
- 确认没有引入堆使用（`grep malloc` 无结果 + 运行时 `k_heap` 统计不变）。

---

## 10. 完成定义

重构完成时应当满足：

1. **分层可见**：从 `trd_test.cpp` 到寄存器，每一跳的职责在代码里能一句话说清，不需要翻注释。
2. **依赖单向**：core 不认识任何厂商符号，HAL 不认识任何协议符号，两个方向都能用 `grep` 证明。
3. **状态唯一**：§2.3 的表每一行只有一个持有者，不存在第二份拷贝或需要手动同步的镜像。
4. **常量进 Flash**：对象表、名称串、状态转换表、寄存器偏移全部 `constexpr`，RAM 里只留真正可变的数据。
5. **无空桩**：所有"返回成功但什么都没做"的函数要么补上实现，要么变成显式的不支持。
6. **旧实现已删除**：`ssc/`、`appl/`、`hal/ports/` 不再存在，许可来源可追溯。
7. **行为等价**：§6.1 的每一条都在上板验证中确认，DC 和 SDO 这类本次没验证的项明确标注为"未验证"而不是"通过"。

---

## 11. 与旧文档的关系

- `doc/EtherCAT按USB分层风格重构规划.md`：边界是"不碰 SSC"，已被本规划取代。保留作为历史记录，不按其中的目标继续执行。
- `doc/EtherCAT从站协议栈融入规划.md`：P1~P5 的落地规划，其成果（SSC 引入、ESC 驱动、板级装配）是本次重构的**输入基线**，已经完成，不重复。
- `doc/EtherCAT首次进入OP缓慢调研.md`：诊断记录，与本规划无冲突。
- 本文取代上一版《EtherCAT 从站整体 C++ 重构规划》。上一版的问题是"分层写对了、切割方式还是按文件"，实际执行时被带回了逐文件翻译。
