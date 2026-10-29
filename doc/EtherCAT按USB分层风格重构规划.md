# EtherCAT 按 USB/CherryUSB 分层风格重构规划（A 计划）

> 日期：2026-10-29（同日修订：分层单元从「板卡」改为「PHY 芯片」）
> 参考模板：`framework/drivers/communication/stream/usb/`（按 CherryUSB 分层重写过的成熟模块）
> 目标：把当前 **C++ 外壳直连厂商头 + 板卡分支散落在 C 里** 的 EtherCAT 模块，重构成与 USB 一致的
> **interface / core / 顶层** 三层，且 **PHY 驱动以芯片为单位独立成文件**。
> 铁律：**行为零变化**——重构只搬代码、拆文件、加抽象层，不改初始化顺序、不改 SSC、不改寄存器操作。

---

## 0. 现状问题

### 0.1 当前分层

```text
framework/drivers/communication/ethercat/
├── ethercat.cpp      309 行   C++ 外壳（Init/Run + 12 个 APPL_* 钩子 + EEPROM 转发）
├── ethercat.hpp      127 行
└── ssc/              21 文件   倍福 SSC 5.13 原版（约 1.4 万行，第三方）

sdk_glue_user/drivers/ethercat/      （2184 行，芯片/板卡相关）
├── esc_hpmicro.c            431 行   Zephyr ESC 设备驱动
├── hpm_ecat_hw.c            573 行   HW_* 宏 + ISR + EEPROM 时序
├── hpm5e31_phy.c            392 行   PHY 复位/配置/链路轮询（三颗 PHY 混在一个文件里）
├── hpm_ecat_e2p_emulation.c 254 行   SII Flash 模拟
├── ecat_zephyr_port.c        53 行   中断回调注册
└── ecat_board_cfg.h          83 行   ★ 板卡配置文件（PHY 型号/地址/引脚全在里面）
```

### 0.2 六个具体问题

| # | 问题 | 位置 | 后果 |
|---|---|---|---|
| 1 | **C++ 外壳直连厂商头** | `ethercat.cpp:23-24` `#include "hpm_ecat_hw.h"` | 换芯片（6E00 / STM32）必须改 framework |
| 2 | **PHY 驱动按板卡分派** | `ecat_board_cfg.h:20-31` `#if CONFIG_BOARD_UHPM6E00EVK` | 新板 → 改 C 代码 |
| 3 | **三颗 PHY 挤在一个文件** | `hpm5e31_phy.c` 里 `rtl8201f_config` / `tja1101b_config` / `jl1111_config` 并列，用 `#if BOARD_ECAT_PHY_JL1111` 选 | 加一颗 PHY → 改这个文件、改宏、改条件编译 |
| 4 | **端口层反向依赖 SSC 符号** | `hpm_ecat_hw.c` 直接调 `PDI_Isr()` / `Sync0_Isr()` / 读 `EepromReadSize` | 端口层与 SSC 双向耦合 |
| 5 | **PHY 地址两处定义** | `ecat_board_cfg.h` 硬编码 + dts `phy-addr-offset` | 容易不一致 |
| 6 | **C++ 外壳 309 行混装 4 类职责** | 生命周期 + 12 个 APPL 钩子 + PDO 映射 + EEPROM 转发 | 与 USB 的 `usb.cpp` 只有 153 行纯 Stream 职责对比 |

### 0.3 USB 模块的分层（对照模板）

```text
usb/
├── usb.hpp / usb.cpp        153 + 69 行   顶层：继承 Stream，只管 Read/Send/回调分发
├── usb_rx_queue.hpp         107 行
├── interface/
│   ├── usb_hal.hpp          175 行        纯虚基类，零平台依赖
│   └── usb_types.hpp        174 行
└── core/
    ├── usb_dev_port.hpp/.cpp
    ├── usb_cdc_acm.hpp/.cpp
    ├── usb_descriptor.hpp/.cpp
    └── usb_cdc_config.hpp

HAL 实现在子树（zephyr_user/drivers/usb/hal/）：
├── hpm/usb_hal_hpm.hpp/.cpp
└── stm32/usb_hal_stm32.hpp/.cpp
```

**三个关键设计**：纯虚基类在 framework、实现在子树 drivers、`GetDefaultHal()` 工厂由实现方提供。

---

## 1. 分层原则：以 PHY 芯片为单位，板卡只在 dts 里选配置

### 1.1 一句话

> **没有「板卡配置文件」这回事。PHY 驱动定义自己有哪些配置项，板卡在 dts 的 PHY 节点里填值。**
> **C 代码里不许出现任何 `CONFIG_BOARD_*`、不许出现 `ecat_board_cfg.h`。**

### 1.2 为什么现在不对

当前有两层板卡耦合：

```c
/* 第一层：ecat_board_cfg.h —— 板卡配置文件本身 */
#if defined(CONFIG_BOARD_UHPM6E00EVK)
#define BOARD_ECAT_PHY_JL1111      (1)
#define BOARD_ECAT_PORT0_PHY_ADDR  (0U)
#define BOARD_ECAT_PORT1_PHY_ADDR  (1U)
#else
#define BOARD_ECAT_PHY_JL1111      (0)
#define BOARD_ECAT_PORT0_PHY_ADDR  (1U)
#define BOARD_ECAT_PORT1_PHY_ADDR  (0U)
#endif

/* 第二层：hpm5e31_phy.c —— 用上面那个宏再分支 */
#if BOARD_ECAT_PHY_JL1111
    s0 = jl1111_config(...);
#else
    s0 = rtl8201f_config(...);
#endif
```

**配置文件本身就是问题**：它把「哪颗 PHY、什么地址、什么模式」这些**纯数据**变成了**要维护的 C 文件**。
加一块新板 → 改 `ecat_board_cfg.h` → 再改 `hpm5e31_phy.c` 的条件编译。**两个文件都得动。**

### 1.3 改成什么样

**删掉 `ecat_board_cfg.h`。** 板卡的所有 PHY 差异都写在 dts 的 PHY 节点属性里：

```text
hal/ports/phy/
├── phy_rtl8201f.cpp      100BASE-TX，MII
├── phy_tja1101b.cpp      100BASE-T1，MII
└── phy_jl1111.cpp        100BASE-TX，MII
```

板卡 dts 直接给每颗 PHY 配参数：

```dts
&esc0 {
    phys = <&ecat_phy0 &ecat_phy1>;
};

ecat_phy0: ethercat-phy@0 {
    compatible = "hpmicro,ecat-phy-jl1111";   /* ← 选驱动 */
    mdio-address = <0>;                       /* ← 选地址 */
    phy-mode = "mii";                         /* ← 选接口 */
    autoneg;                                  /* ← 选自协商 */
    advertise = <0x0100>;                     /* ← 选通告能力（100M 全双工） */
    led-mode = <0>;                           /* ← 芯片私有项 */
};
```

**三层解耦**：

| 层 | 决定什么 | 在哪 |
|---|---|---|
| PHY 驱动 | 芯片有哪些可配项、怎么写寄存器 | `phy_xxx.cpp` |
| binding | 该芯片允许哪些属性、类型与默认值 | `hpmicro,ecat-phy-xxx.yaml` |
| 板卡 | 每颗 PHY 的属性取什么值 | 板卡 dts |

加新板 = **只写 dts**。加新 PHY 芯片 = 新增 `phy_xxx.cpp` + `yaml` + Kconfig 符号，**不动任何已有文件**。
**任何一层都不需要「板卡 config 文件」。**

### 1.4 PHY 驱动接口边界

| 该管 | 不该管 |
|---|---|
| 芯片复位时序（软复位 / 引脚） | ESC 寄存器读写 |
| 接口模式（MII / RMII / RGMII） | 中断 |
| 自协商开关与通告能力 | EEPROM |
| 链路状态读取 | AL 状态机 |
| 私有页寄存器 | PDO 映射 |
| LED 模式 | 板卡引脚分配（dts/pinctrl 管） |

---

## 2. 目标分层

```text
framework/drivers/communication/ethercat/
├── ethercat.hpp / ethercat.cpp      顶层：继承 Stream + 三个业务回调
├── interface/
│   ├── ecat_hal.hpp                 ESC 硬件抽象（纯虚，零厂商）
│   ├── ecat_phy.hpp                 PHY 驱动抽象（纯虚，零厂商）
│   └── ecat_types.hpp               EtherCAT 规范类型
├── core/
│   ├── ecat_stack.hpp/.cpp          SSC 生命周期
│   ├── ecat_pdo.hpp/.cpp            PDO 映射计算
│   └── ecat_eeprom.hpp/.cpp         SII 钩子安装
├── appl/
│   └── ecat_appl_hooks.cpp          12 个 extern "C" APPL_* 钩子（唯一出现 extern "C" 处）
└── ssc/                             倍福 SSC 5.13 原版（不动）

zephyr_user/drivers/ethercat/        （子树 drivers，Zephyr 模块式）
├── hal/hpm/
│   ├── ecat_hal_hpm.hpp/.cpp        继承 EcatHal（ESC 本体，不含 PHY 逻辑）
│   └── esc_hpmicro.c                Zephyr ESC 设备驱动
└── hal/ports/
    ├── hpm_ecat_hw.c / .h           SSC 的 HW_* 端口层
    ├── ecat_zephyr_port.c / .h      中断注册
    ├── hpm_ecat_e2p_emulation.c/.h  SII Flash 模拟
    ├── ecat_port_hw.h               ★ 端口层常量（SII 分区地址、EEPROM I2C 引脚、延时）
    └── phy/                         ★ 按 PHY 芯片分层
        ├── ecat_phy_common.hpp      clause22 寄存器（constexpr）+ EcatPhyMdio 公共工具
        ├── ecat_phy_mdio.cpp        EcatPhyMdio::SoftReset 实现（三颗共用）
        ├── ecat_phy_bind.hpp/.cpp   从 dts 取 PHY 实例（compatible → 驱动）
        ├── jl1111/                  ★ 一颗芯片一个文件夹
        │   └── phy_jl1111.hpp/.cpp
        ├── rtl8201f/
        │   └── phy_rtl8201f.hpp/.cpp
        └── tja1101b/
            └── phy_tja1101b.hpp/.cpp
```

### 2.1 依赖方向

```text
业务（trd_test.cpp）
    ↓ SetRxHandler / SetTxProvider / Run
EthercatSlave（ethercat.hpp，纯 framework）
    ↓
EcatStack（core，唯一 include ssc/*.h 的地方）
    ↓
EcatHal（interface，纯虚）      EcatPhy（interface，纯虚）
    ↑ 继承                        ↑ 继承
EcatHalHpm（子树 hal/hpm）        PhyJl1111 / PhyRtl8201f / PhyTja1101b（子树 hal/ports/phy）
    ↓ 持有 EcatPhy* 数组
```

**EcatHal 持有 PHY 列表**，`UpdateLink()` 遍历调用 `phy->LinkUp()`，`Init()` 遍历调用 `phy->Config()`。
HAL 不知道具体是哪颗 PHY，只知道 `EcatPhy*`。

---

## 3. 逐文件职责

### 3.1 `interface/ecat_types.hpp`（新建，约 80 行）

只放 EtherCAT 规范（ETG.1000）定义的类型，不依赖 SSC、不依赖厂商。

| 类型 | 内容 |
|---|---|
| `enum class EcatState : uint8_t` | `Init / PreOp / SafeOp / Op / Boot` |
| `struct PdoLayout` | `uint16_t rx_size / tx_size` |
| `struct EcatPortState` | `bool link[3]` |
| `enum class EcatIrq : uint8_t` | `Pdi / Sync0 / Sync1 / Reset` |
| `enum class EcatPhySpeed : uint8_t` | `Mbps10 / Mbps100 / Mbps1000` |

### 3.2 `interface/ecat_phy.hpp`（新建，约 110 行）

**PHY 驱动的唯一契约。零厂商符号、零板卡符号。**

配置参数由 dts 传入（`PhyConfig`），驱动只管「把这些值写进寄存器」，
**不含任何板卡相关的默认值、不含任何 `#if`**：

```cpp
/**
 * @brief PHY 配置（来自 dts 的 PHY 节点属性）
 */
struct PhyConfig
{
	uint8_t  mdio_address = 0;              // mdio-address
	uint16_t advertise    = 0x0100;         // advertise（bit8=100M FD）
	bool     autoneg      = true;           // autoneg
	uint8_t  interface    = 0;              // phy-mode: 0=MII 1=RMII
	uint8_t  led_mode     = 0;              // led-mode（芯片私有，0=默认）
};

/**
 * @brief EtherCAT PHY 驱动接口
 */
class EcatPhy
{
public:
	virtual ~EcatPhy() = default;

	virtual const char* Name() const = 0;       // 芯片名（日志用，如 "JL1111"）
	virtual uint8_t Address() const = 0;        // 本 PHY 在 ESC MDIO 上的地址

	/**
	 * @brief 按 dts 给的参数配置芯片
	 * @param cfg  dts 属性值
	 * @return true=成功
	 */
	virtual bool Config(const PhyConfig& cfg) = 0;

	virtual bool LinkUp(bool* up) = 0;          // 读链路；false 表示 MDIO 失败
};
```

> **不含** `Read/Write` 的寄存器访问——由 `EcatHal::MdioRead/MdioWrite` 提供，
> PHY 驱动在构造时拿到 `EcatHal&`，`Config()/LinkUp()` 内部通过它访问芯片。
>
> **不含**板卡默认值——`advertise`/`autoneg`/`interface`/`led_mode` 全部由 dts 决定，
> 驱动只把它们翻译成芯片寄存器。

### 3.3 `interface/ecat_hal.hpp`（新建，约 130 行）

对应 `usb_hal.hpp`。**不 include 任何 `hpm_*` 头。**

```cpp
class EcatHal
{
public:
    using EventCallback = void (*)(void* context, EcatIrq irq);

    struct Config {
        uint8_t port_count = 2;
    };

    virtual ~EcatHal() = default;

    // 生命周期
    virtual bool Init(const Config& cfg, EventCallback cb, void* ctx) = 0;
    virtual bool Start() = 0;
    virtual void Stop()  = 0;

    // ESC 寄存器/DPRAM
    virtual void Read(uint16_t addr, void* buf, uint16_t len) = 0;
    virtual void Write(uint16_t addr, const void* buf, uint16_t len) = 0;
    virtual uint16_t AlEvent() = 0;

    // MDIO（PHY 驱动通过它访问芯片）
    virtual bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* v)  = 0;
    virtual bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t v)  = 0;

    // 链路（软件模式写 GPR_CFG2）
    virtual void SetLink(uint8_t port, bool up) = 0;

    // 遍历所有 PHY 的 LinkUp() 并写 ESC
    virtual void UpdateLink() = 0;

    // EEPROM（SII）
    virtual uint16_t EepromRead(uint32_t word_addr) = 0;
    virtual void     EepromWrite(uint32_t word_addr, uint16_t data) = 0;
    virtual uint32_t EepromReload() = 0;

    // 时间基准（SSC 的 HW_GetTimer）
    virtual uint32_t TimerGet() = 0;
    virtual void     TimerClear() = 0;

    // 临界区
    virtual void EnterCritical() = 0;
    virtual void LeaveCritical() = 0;
};

EcatHal& GetDefaultEcatHal();
```

### 3.4 `core/ecat_stack.hpp/.cpp`（新建，约 90 + 230 行）

对应 `usb_dev_port`。**framework 里唯一 include `ssc/*.h` 的文件**（`appl/` 除外）。

| 职责 | 内容 |
|---|---|
| `Init()` | 照参考 11 步：`hal.Init` → `MainInit` → 重装 EEPROM 钩子 → `APPL_GenerateMapping` → `hal.UpdateLink` → `bRunApplication=TRUE` → `hal.Start` |
| `Run()` | `while (bRunApplication) { MainLoop(); hal.UpdateLink() 每 100ms; k_usleep(100); }` |
| `Stop()` | `bRunApplication = FALSE` + `hal.Stop()` |
| ISR 分发 | `OnIrq(EcatIrq)` → `PDI_Isr()` / `Sync0_Isr()` / `Sync1_Isr()` |
| 业务回调 | `CycleHandler` / `RxHandler` / `TxProvider` |

### 3.5 `core/ecat_pdo.hpp/.cpp`（新建，约 40 + 120 行）

从 `ethercat.cpp:250-309` 的 `APPL_GenerateMapping` 拆出。纯计算。

### 3.6 `core/ecat_eeprom.hpp/.cpp`（新建，约 40 + 80 行）

4 个 `pAPPL_EEPROM_*` 钩子的安装与转发。

### 3.7 `appl/ecat_appl_hooks.cpp`（新建，约 90 行）

12 个 `extern "C"` APPL_* 全搬，全部转发 `EcatStack::Instance()`。

### 3.8 `ethercat.hpp/.cpp`（重写，约 100 + 70 行）

瘦身到与 `usb.hpp/cpp` 同级：继承 `Stream`，只暴露 `Init / Run / Stop / Set*Handler`。

### 3.9 `hal/ports/phy/`（新建，一颗芯片一个文件夹）

公共部分放 `phy/` 根下，每颗芯片的驱动放各自的子文件夹：

| 文件 | 内容 | 约行数 |
|---|---|---|
| `phy/ecat_phy_common.hpp` | clause22 寄存器（constexpr）+ `EcatPhyMdio` 公共访问工具 | 161 |
| `phy/ecat_phy_mdio.cpp` | `EcatPhyMdio::SoftReset` 实现（三颗芯片共用） | 50 |
| `phy/ecat_phy_bind.hpp/.cpp` | 从 dts `phys` 属性构造 `EcatPhy*` 数组（compatible → 驱动） | 36 + 129 |
| `phy/jl1111/phy_jl1111.hpp/.cpp` | JL1111：复位、LED 模式、MII、ANAR 100/FD、关广播定址、BMSR 链路 | 42 + 176 |
| `phy/rtl8201f/phy_rtl8201f.hpp/.cpp` | RTL8201F：软复位、page7 MII、EEE 关、page4、ANAR 100/FD、BMSR 链路 | 40 + 168 |
| `phy/tja1101b/phy_tja1101b.hpp/.cpp` | TJA1101B：软复位、默认 MII/AN、BMSR 链路 | 40 + 118 |

**每颗 PHY 的 `LinkUp()` 只读自己芯片的寄存器，没有任何跨芯片分支。**
**加一颗新芯片 = 新建 `phy/<芯片名>/` 文件夹，不动已有文件。**

### 3.10 `hal/ports/ecat_board_cfg.h` → **删除**，改名 `ecat_port_hw.h`

**不再有「板卡配置文件」。** 原文件里唯一不是 PHY 相关的三样东西挪进 `ecat_port_hw.h`：

| 项 | 内容 |
|---|---|
| SII 分区地址 | `DT_REG_ADDR(DT_NODELABEL(ethercat_eeprom_partition))`（本来就从 dts 读） |
| EEPROM I2C 引脚 | `init_esc_eeprom_pin()` / `init_esc_eeprom_as_i2c_pin()`（ESC 的 I2C 通道，与 PHY 无关） |
| `board_delay_ms()` | busy-wait 延时 |

原文件里这些**全部删除**（搬去 dts）：

| 删掉的符号 | 现在从哪来 |
|---|---|
| `BOARD_ECAT_PHY_JL1111` | 不再需要（dts 的 `compatible` 决定驱动） |
| `BOARD_ECAT_PORT0_PHY_ADDR` | dts 的 `mdio-address` |
| `BOARD_ECAT_PORT1_PHY_ADDR` | dts 的 `mdio-address` |
| `BOARD_ECAT_SUPPORT_PORT1` | dts 的 `phys` 长度 |
| `BOARD_ECAT_PHY_ADDR_OFFSET` | dts 的 `phy-addr-offset`（已有） |
| `BOARD_FLASH_BASE_ADDRESS` | 无引用，直接删 |

### 3.11 `hal/hpm/ecat_hal_hpm.hpp/.cpp`（新建，约 60 + 200 行）

继承 `EcatHal`，持有 `EcatPhy*` 数组（由 `ecat_phy_bind` 从 dts 构造）。
**PHY 配置参数也是 `ecat_phy_bind` 从 dts 读出来、逐颗喂给驱动的**：

```cpp
bool EcatHalHpm::Init(const Config& cfg, EventCallback cb, void* ctx)
{
    ecat_irq_init();
    if (ecat_hardware_init(kEscDev) != 0) return false;   // 时钟/引脚/EEPROM/ESC 本体

    // dts 的 phys 属性 → 驱动实例 + 各自的 PhyConfig
    phy_count_ = ecat_phy_bind(phys_, phy_cfgs_, kMaxPorts);
    for (uint8_t i = 0; i < phy_count_; i++) {
        if (!phys_[i]->Config(phy_cfgs_[i])) { /* 记录但不中断 */ }
    }
    return true;
}

void EcatHalHpm::UpdateLink()
{
    for (uint8_t i = 0; i < phy_count_; i++) {
        bool up = false;
        if (!phys_[i]->LinkUp(&up)) up = false;
        SetLink(i, up);
    }
}
```

---

## 4. dts / Kconfig / 构建链

### 4.1 新增 binding：`sdk_glue_user/dts/bindings/ethercat/hpmicro,ecat-phy.yaml`

**PHY 的配置项全部在这里声明**——binding 就是「芯片可配项」的契约，板卡只填值：

```yaml
description: EtherCAT PHY attached to the HPMicro ESC MDIO bus
compatible: "hpmicro,ecat-phy"
include: base.yaml

properties:
  mdio-address:
    type: int
    required: true
    description: PHY address on the ESC MDIO bus

  phy-mode:
    type: string
    default: "mii"
    enum: ["mii", "rmii", "rgmii"]
    description: Host interface mode between ESC and this PHY

  autoneg:
    type: boolean
    description: Enable auto-negotiation (default off = forced speed/duplex)

  advertise:
    type: int
    default: 0x0100
    description: |
      Clause-22 ANAR advertisement bits.
      bit8 = 100BASE-TX full duplex, bit7 = 100BASE-TX half,
      bit6 = 10BASE-T full, bit5 = 10BASE-T half, bit0 = 802.3 selector.

  speed:
    type: string
    default: "100"
    enum: ["10", "100", "1000"]
    description: Forced speed when autoneg is disabled

  duplex:
    type: string
    default: "full"
    enum: ["half", "full"]
    description: Forced duplex when autoneg is disabled

  led-mode:
    type: int
    default: 0
    description: Chip-private LED mode value (0 = driver default)
```

三颗芯片各自继承（**只在需要追加私有属性时才写新 yaml**）：

| binding 文件 | compatible | 额外私有属性 | Kconfig 符号 |
|---|---|---|---|
| `hpmicro,ecat-phy-jl1111.yaml` | `hpmicro,ecat-phy-jl1111` | 无（继承全部） | `DUST_ECAT_PHY_JL1111` |
| `hpmicro,ecat-phy-rtl8201f.yaml` | `hpmicro,ecat-phy-rtl8201f` | `eee-enable`（默认 false） | `DUST_ECAT_PHY_RTL8201F` |
| `hpmicro,ecat-phy-tja1101b.yaml` | `hpmicro,ecat-phy-tja1101b` | `master-slave`（默认 "auto"） | `DUST_ECAT_PHY_TJA1101B` |

### 4.2 `esc0` binding 增加 `phys`

```yaml
  phys:
    type: phandles
    description: PHYs attached to ESC port 0..n, in port order.
```

### 4.3 板卡 dts 写法（板卡唯一的 PHY 配置入口）

**hpm6e00evk（两颗 JL1111，PORTA@0 / PORTB@1）**：

```dts
&esc0 {
    phys = <&ecat_phy0 &ecat_phy1>;
    /* 其余不变 */
};

ecat_phy0: ethercat-phy@0 {
    compatible = "hpmicro,ecat-phy-jl1111";
    mdio-address = <0>;
    phy-mode = "mii";
    autoneg;
    advertise = <0x0100>;          /* 只通告 100M 全双工 */
    led-mode = <0>;
};

ecat_phy1: ethercat-phy@1 {
    compatible = "hpmicro,ecat-phy-jl1111";
    mdio-address = <1>;
    phy-mode = "mii";
    autoneg;
    advertise = <0x0100>;
    led-mode = <0>;
};
```

**hpm5e31gn1（RTL8201F@1 + TJA1101B@0）**：

```dts
&esc0 {
    phys = <&ecat_phy0 &ecat_phy1>;
};

ecat_phy0: ethercat-phy@1 {
    compatible = "hpmicro,ecat-phy-rtl8201f";
    mdio-address = <1>;
    phy-mode = "mii";
    autoneg;
    advertise = <0x0100>;          /* 只通告 100M 全双工 */
    eee-enable;                    /* RTL8201F 私有：关 EEE 保 MII 时钟连续 → 不写 */
};

ecat_phy1: ethercat-phy@0 {
    compatible = "hpmicro,ecat-phy-tja1101b";
    mdio-address = <0>;
    phy-mode = "mii";
    autoneg;
    master-slave = "auto";         /* TJA1101B 私有：主从自协商 */
};
```

> **注意 hpm5e31gn1 上 `eee-enable` 不要写**——现有代码是**关掉** EEE 以保证 MII 时钟连续，
> binding 里该属性默认 false（不写即关闭），所以 dts 里不出现即可。

### 4.4 Kconfig

**framework 侧 `framework/drivers/communication/ethercat/Kconfig`（新建）**：

```kconfig
config DUST_ECAT_DEVICE_STACK
    bool
    select DUST_ECAT_DEVICE_APPL
    select DUST_ECAT_HAL_HPM if DT_HAS_HPMICRO_HPM_ESC_ENABLED

config DUST_ECAT_DEVICE_APPL
    bool
    default n
```

**子树 `zephyr_user/drivers/Kconfig` 追加**：

```kconfig
config DUST_ECAT_HAL_HPM
    bool "HPMicro ESC EtherCAT HAL"
    depends on CLOCK_CONTROL && GPIO && PINCTRL
    select DUST_ECAT_PHY_JL1111    if DT_HAS_HPMICRO_ECAT_PHY_JL1111_ENABLED
    select DUST_ECAT_PHY_RTL8201F  if DT_HAS_HPMICRO_ECAT_PHY_RTL8201F_ENABLED
    select DUST_ECAT_PHY_TJA1101B  if DT_HAS_HPMICRO_ECAT_PHY_TJA1101B_ENABLED

config DUST_ECAT_PHY_JL1111
    bool

config DUST_ECAT_PHY_RTL8201F
    bool

config DUST_ECAT_PHY_TJA1101B
    bool
```

> **PHY 驱动由 dts 的 compatible 决定是否参与编译**——没有板卡判断，也没有手改 defconfig。

### 4.5 `zephyr_user/drivers/CMakeLists.txt` 追加

```cmake
# EtherCAT 板卡底层 HAL（HPM ESC）+ 按芯片分层的 PHY 驱动
if(CONFIG_DUST_ECAT_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/ecat_hal_hpm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/esc_hpmicro.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/hpm_ecat_hw.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/ecat_zephyr_port.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/hpm_ecat_e2p_emulation.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/ecat_phy_bind.cpp
        ${HPM_SDK_DIR}/drivers/src/hpm_esc_drv.c
        ${HPM_SDK_DIR}/components/eeprom_emulation/eeprom_emulation.c
        ${HPM_SDK_DIR}/utils/hpm_crc32.c)

    if(CONFIG_DUST_ECAT_PHY_JL1111)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/phy_jl1111.cpp)
    endif()
    if(CONFIG_DUST_ECAT_PHY_RTL8201F)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/phy_rtl8201f.cpp)
    endif()
    if(CONFIG_DUST_ECAT_PHY_TJA1101B)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/phy_tja1101b.cpp)
    endif()

    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy
        ${FW_ROOT}/drivers/communication/ethercat/interface
        ${FW_ROOT}/drivers/communication/ethercat/ssc
        ${HPM_SDK_DIR}/components/eeprom_emulation
        ${HPM_SDK_DIR}/components/eeprom_emulation/port
        ${HPM_SDK_DIR}/utils)
endif()
```

### 4.6 `sdk_glue_user/drivers/ethercat/` 整体删除

内容搬进 `zephyr_user/drivers/ethercat/hal/`；`sdk_glue_user/drivers/Kconfig` 的 `rsource "ethercat/Kconfig.hpmicro"` 一并删。

> **理由**：USB 的 HAL 就在子树 drivers，不在 sdk_glue_user。sdk_glue_user 只该放 board/soc/dts。

---

## 5. 分阶段执行

> **每阶段开头一张五列步表；用户说「开始第 N 步」= 只改表格里那一步对应的文件，改完停下报告，等「继续」。**
> **不许一次做多步，不许跨步连带改（必要时见「连带」栏）。**

### P1 — interface 层（4 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1-1 | `framework/.../ethercat/interface/ecat_types.hpp` | 新建（§6.1） | 无 | 无（未接 CMake） |
| 1-2 | `framework/.../ethercat/interface/ecat_phy.hpp` | 新建（§6.2） | 1-1 | 无 |
| 1-3 | `framework/.../ethercat/interface/ecat_hal.hpp` | 新建（§6.3） | 1-2 | 无 |
| 1-4 | `zephyr_user/drivers/ethercat/hal/ports/phy/ecat_phy_common.hpp` | 新建 clause22 寄存器（constexpr）+ `EcatPhyMdio` 公共工具 | 无 | 无 |
| 1-5 | `hal/ports/phy/ecat_phy_mdio.cpp` | `EcatPhyMdio::SoftReset` 实现 | 1-4 | 无 |

### P2 — PHY 驱动（按芯片，4 步）

> 每颗芯片一个文件夹：`phy/<芯片名>/phy_<芯片名>.hpp/.cpp`。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 2-1 | `hal/ports/phy/jl1111/phy_jl1111.hpp/.cpp` | 从 `hpm5e31_phy.c` 搬 JL1111 逻辑 | P1 | 无（未接 CMake） |
| 2-2 | `hal/ports/phy/rtl8201f/phy_rtl8201f.hpp/.cpp` | 从 `hpm5e31_phy.c` 搬 RTL8201F 逻辑 | 2-1 | 无 |
| 2-3 | `hal/ports/phy/tja1101b/phy_tja1101b.hpp/.cpp` | 从 `hpm5e31_phy.c` 搬 TJA1101B 逻辑 | 2-2 | 无 |
| 2-4 | `hal/ports/phy/ecat_phy_bind.hpp/.cpp` | dts `phys` → `EcatPhy*` 数组 | 2-3 | 无 |

### P3 — HAL 实现（3 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 3-1 | `sdk_glue_user/drivers/ethercat/*` → `zephyr_user/drivers/ethercat/hal/ports/` | 整目录 `git mv`（除 `hpm5e31_phy.*`，它已被 P2 取代） | P2 | `sdk_glue_user` 的 CMake 此时失效（P7-4 修） |
| 3-2 | `hal/hpm/ecat_hal_hpm.hpp` | 新建声明（§6.4） | 3-1 | 无 |
| 3-3 | `hal/hpm/ecat_hal_hpm.cpp` | 新建实现（§6.5） | 3-2 | 无 |

### P4 — core 层（4 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 4-1 | `framework/.../ethercat/core/ecat_pdo.hpp/.cpp` | 搬 `APPL_GenerateMapping` | P3 | 无 |
| 4-2 | `framework/.../ethercat/core/ecat_eeprom.hpp/.cpp` | 搬 4 个 `pAPPL_EEPROM_*` 安装 | 4-1 | 无 |
| 4-3 | `framework/.../ethercat/core/ecat_stack.hpp` | 新建声明（§6.6） | 4-2 | 无 |
| 4-4 | `framework/.../ethercat/core/ecat_stack.cpp` | 新建实现（§6.7，11 步照参考） | 4-3 | 无 |

### P5 — appl 钩子（1 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 5-1 | `framework/.../ethercat/appl/ecat_appl_hooks.cpp` | 12 个 `extern "C"` 全搬（§6.8） | 4-4 | 无 |

### P6 — 顶层瘦身（2 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 6-1 | `framework/.../ethercat/ethercat.hpp` | 重写（§6.9） | 5-1 | `ethercat.cpp` 此时编不过（6-2 修） |
| 6-2 | `framework/.../ethercat/ethercat.cpp` | 重写（§6.10）+ 连带改 `trd_test.cpp` | 6-1 | 业务侧类名/单例用法 |

### P7 — 构建链与 dts（7 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 7-1 | `framework/.../ethercat/Kconfig` | 新建（§4.4） | P6 | 无 |
| 7-2 | `framework/drivers/Kconfig` | `DUST_COM_ETHERCAT` 加 `select DUST_ECAT_DEVICE_STACK` | 7-1 | 无 |
| 7-3 | `zephyr_user/drivers/Kconfig` | 加 `DUST_ECAT_HAL_HPM` + 三个 PHY 符号（§4.4） | 7-2 | 无 |
| 7-4 | `zephyr_user/drivers/CMakeLists.txt` | 加 HAL + PHY 段（§4.5） | 7-3 | **本步之后才能编过** |
| 7-5 | `sdk_glue_user/dts/bindings/ethercat/` | 新增 `hpmicro,ecat-phy.yaml` + 三个芯片 yaml；`hpmicro,hpm-esc.yaml` 加 `phys`（§4.1、§4.2） | 7-4 | 无 |
| 7-6 | 三块板 dts | 加 PHY 节点 + `phys` 属性（§4.3） | 7-5 | 无 |
| 7-7 | `hal/ports/ecat_board_cfg.h` → `ecat_port_hw.h` | 删 `#if CONFIG_BOARD_*` 与全部 `BOARD_ECAT_PHY_*`；只留 SII 分区 / EEPROM I2C 引脚 / 延时（§3.10） | 7-6 | 引用处改名 |

### P8 — 清理与验证（2 步）

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 8-1 | `sdk_glue_user/drivers/ethercat/` | 整目录删除；`sdk_glue_user/drivers/Kconfig` 删 `rsource` 行 | 7-6 编过 | 无 |
| 8-2 | `framework/.../ethercat/CMakeLists.txt` | 源码列表改成新结构 | 8-1 | 无 |

---

## 6. 关键文件内容

### 6.1 `interface/ecat_types.hpp`

```cpp
/**
 * @file ecat_types.hpp
 * @author qingyu
 * @brief EtherCAT 基础类型 — 不依赖 SSC、不依赖任何厂商头
 * @version 0.1
 * @date 2026-10-29
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

/**
 * @brief EtherCAT 应用层状态机（AL State）
 */
enum class EcatState : uint8_t
{
	Init   = 0x01,                          // 初始化
	PreOp  = 0x02,                          // 预运行（邮箱可用）
	SafeOp = 0x04,                          // 安全运行（输入有效）
	Op     = 0x08,                          // 运行（输入输出都有效）
	Boot   = 0x03,                          // 引导（固件升级）
};

/**
 * @brief EtherCAT 中断源
 */
enum class EcatIrq : uint8_t
{
	Pdi   = 0,                              // PDI 中断（主中断）
	Sync0 = 1,                              // 分布式时钟 SYNC0
	Sync1 = 2,                              // 分布式时钟 SYNC1
	Reset = 3,                              // ESC 复位
};

/**
 * @brief PHY 速率
 */
enum class EcatPhySpeed : uint8_t
{
	Mbps10   = 0,                           // 10 Mbit/s
	Mbps100  = 1,                           // 100 Mbit/s
	Mbps1000 = 2,                           // 1000 Mbit/s
};

/**
 * @brief 过程数据布局（SM2 / SM3 字节数）
 */
struct PdoLayout
{
	uint16_t rx_size = 0;                   // 主站→从站（SM2 / RxPDO）
	uint16_t tx_size = 0;                   // 从站→主站（SM3 / TxPDO）
};

/**
 * @brief 端口链路状态
 */
struct EcatPortState
{
	bool link[3] = { false, false, false }; // 各端口链路
};
```

### 6.2 `interface/ecat_phy.hpp`

```cpp
/**
 * @file ecat_phy.hpp
 * @author qingyu
 * @brief EtherCAT PHY 驱动抽象 — 以 PHY 芯片为单位，不依赖任何平台
 * @version 0.1
 * @date 2026-10-29
 *
 * 每颗 PHY 芯片实现一个子类：PhyJl1111 / PhyRtl8201f / PhyTja1101b。
 * 板卡在 dts 里用 compatible 选驱动，C 代码里没有板卡分支。
 *
 * 职责边界：
 *   - 必须处理：芯片复位、接口模式、自协商、链路读取、私有页寄存器
 *   - 不得处理：ESC 寄存器、中断、EEPROM、AL 状态机、引脚复用
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_types.hpp"

class EcatHal;

/**
 * @brief EtherCAT PHY 驱动接口
 */
class EcatPhy
{
public:
	virtual ~EcatPhy() = default;

	/**
	 * @brief 芯片名（日志用）
	 */
	virtual const char* Name() const = 0;

	/**
	 * @brief 本 PHY 在 ESC MDIO 总线上的地址
	 */
	virtual uint8_t Address() const = 0;

	/**
	 * @brief 配置芯片：复位 → 接口模式 → 自协商能力 → 启协商
	 * @return true=成功
	 */
	virtual bool Config() = 0;

	/**
	 * @brief 读链路状态
	 * @param up  输出：链路是否 up
	 * @return true=读取成功（false 表示 MDIO 失败，调用方按 down 处理）
	 */
	virtual bool LinkUp(bool* up) = 0;
};
```

### 6.3 `interface/ecat_hal.hpp`

见 §3.3 的完整声明。要点：**不 include 任何 `hpm_*`**，PHY 通过 `MdioRead/MdioWrite` 访问。

### 6.4 `hal/hpm/ecat_hal_hpm.hpp`

```cpp
/**
 * @file ecat_hal_hpm.hpp
 * @author qingyu
 * @brief HPMicro ESC 硬件抽象层
 * @version 0.1
 * @date 2026-10-29
 *
 * 包装子树 hal/ports/ 下的 C 端口层，持有由 dts 决定的 EcatPhy* 数组。
 * 不含任何 PHY 芯片逻辑（那在 hal/ports/phy/ 里按芯片分文件）。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_hal.hpp"
#include "ecat_phy.hpp"

/**
 * @brief HPMicro EtherCAT 硬件抽象层
 */
class EcatHalHpm final : public EcatHal
{
public:
	EcatHalHpm() = default;

	bool Init(const Config& cfg, EventCallback callback, void* context) override;
	bool Start() override;
	void Stop() override;

	void Read(uint16_t addr, void* buf, uint16_t len) override;
	void Write(uint16_t addr, const void* buf, uint16_t len) override;
	uint16_t AlEvent() override;

	bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* value) override;
	bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t value) override;

	void SetLink(uint8_t port, bool up) override;
	void UpdateLink() override;

	uint16_t EepromRead(uint32_t word_addr) override;
	void     EepromWrite(uint32_t word_addr, uint16_t data) override;
	uint32_t EepromReload() override;

	uint32_t TimerGet() override;
	void     TimerClear() override;

	void EnterCritical() override;
	void LeaveCritical() override;

private:
	static constexpr uint8_t kMaxPorts = 3;

	EcatPhy*  phys_[kMaxPorts] = { nullptr, nullptr, nullptr };
	PhyConfig phy_cfgs_[kMaxPorts] {};      // 从 dts 读出的每颗 PHY 参数
	uint8_t   phy_count_ = 0;
	bool      ready_ = false;
};
```

### 6.5 `hal/hpm/ecat_hal_hpm.cpp`（骨架）

```cpp
bool EcatHalHpm::Init(const Config& cfg, EventCallback callback, void* context)
{
	(void)cfg;
	if (ready_) {
		return true;
	}

	ecat_irq_init();                                  // 端口层注册 PDI/SYNC/RESET
	if (ecat_hardware_init(kEscDev) != 0) {           // 时钟/引脚/EEPROM/ESC 本体
		return false;
	}

	phy_count_ = ecat_phy_bind(phys_, kMaxPorts);     // dts 的 phys 属性 → 驱动实例
	for (uint8_t i = 0; i < phy_count_; i++) {
		if (!phys_[i]->Config()) {
			DUST_LOG_WRN("ecat phy %s@%u config failed",
				     phys_[i]->Name(), phys_[i]->Address());
		}
	}

	ready_ = true;
	return true;
}

void EcatHalHpm::UpdateLink()
{
	for (uint8_t i = 0; i < phy_count_; i++) {
		bool up = false;
		if (!phys_[i]->LinkUp(&up)) {
			up = false;
		}
		SetLink(i, up);
	}
}
```

> `ecat_irq_init()` / `ecat_hardware_init()` 内部不再调 `ecat_phy_config()`——
> PHY 配置改由本文件遍历 `phys_[]` 完成（P3-1 搬目录时同步删掉 `hpm_ecat_hw.c` 里的 PHY 调用）。

### 6.6 `core/ecat_stack.hpp`

```cpp
class EcatStack final
{
public:
	using CycleHandler = std::function<void()>;
	using RxHandler    = std::function<void(const uint8_t*, size_t)>;
	using TxProvider   = std::function<void(uint8_t*, size_t)>;

	static EcatStack& Instance();

	EcatStack(const EcatStack&) = delete;
	EcatStack& operator=(const EcatStack&) = delete;

	bool Init();
	void Run();
	void Stop();

	void SetCycleHandler(CycleHandler handler);
	void SetRxHandler(RxHandler handler);
	void SetTxProvider(TxProvider handler);

	PdoLayout GetPdoLayout() const { return layout_; }

	// 仅供 APPL_* 钩子调用
	void OnOutputMapping(uint16_t* pData);
	void OnInputMapping(uint16_t* pData);
	void OnApplication();

private:
	EcatStack() = default;
	~EcatStack() = default;

	static void OnIrq(void* ctx, EcatIrq irq);
	void        DispatchIrq(EcatIrq irq);

	EcatHal&     hal_ = GetDefaultEcatHal();
	PdoLayout    layout_ {};
	CycleHandler cycle_handler_ {};
	RxHandler    rx_handler_ {};
	TxProvider   tx_handler_ {};
	bool         initialised_ = false;
};
```

### 6.7 `core/ecat_stack.cpp`（初始化顺序，逐字照参考）

```cpp
bool EcatStack::Init()
{
	if (initialised_) {
		return true;
	}

	EcatHal::Config cfg {};
	if (!hal_.Init(cfg, OnIrq, this)) {
		return false;
	}
	if (MainInit() != 0U) {
		return false;
	}
	EcatEeprom::Install(hal_);                 // MainInit 会清钩子，必须在其后
	layout_ = EcatPdo::Generate();
	if (layout_.rx_size == 0 && layout_.tx_size == 0) {
		return false;
	}
	hal_.UpdateLink();
	bRunApplication = TRUE;
	hal_.Start();

	initialised_ = true;
	return true;
}
```

### 6.8 `appl/ecat_appl_hooks.cpp`

12 个 `extern "C"` 全部转发 `EcatStack::Instance()`；`ecat_port_log` 也放这里。
**全工程唯一出现 `extern "C"` APPL_* 的文件。**

### 6.9 / 6.10 `ethercat.hpp/.cpp`

```cpp
namespace ecat {

class EthercatSlave final : public Stream
{
public:
	bool Init();
	bool Run();
	void Stop();

	PdoLayout GetPdoLayout() const { return stack_.GetPdoLayout(); }

	void SetCycleHandler(EcatStack::CycleHandler h) { stack_.SetCycleHandler(std::move(h)); }
	void SetRxHandler(EcatStack::RxHandler h)       { stack_.SetRxHandler(std::move(h)); }
	void SetTxProvider(EcatStack::TxProvider h)     { stack_.SetTxProvider(std::move(h)); }

	uint16_t Read(uint8_t*, uint16_t) override { return 0; }
	bool     Send(const uint8_t*, uint32_t) override { return false; }

private:
	EcatStack& stack_ = EcatStack::Instance();
};

} // namespace ecat
```

> `trd_test.cpp` 从 `EthercatSlave::Instance()` 改成 `ecat::EthercatSlave slave;`（值语义），
> 在 `thread_init()` 里构造、`Task()` 里调 `slave.Run()`。

---

## 7. 验证清单

### 7.1 静态检查（每阶段后）

| # | 命令 | 期望 |
|---|---|---|
| 1 | `grep -rn "hpm_\|CONFIG_BOARD_" framework/drivers/communication/ethercat/*.cpp framework/drivers/communication/ethercat/core/ framework/drivers/communication/ethercat/interface/` | 输出为空 |
| 2 | `grep -rn "extern \"C\"" framework/drivers/communication/ethercat/` | 只有 `appl/ecat_appl_hooks.cpp` |
| 3 | `grep -rn "CONFIG_BOARD_\|BOARD_ECAT_PHY_\|BOARD_ECAT_PORT" zephyr_user/drivers/ethercat/` | 输出为空（**PHY 分层核心验收项**） |
| 4 | `ls zephyr_user/drivers/ethercat/hal/ports/ecat_board_cfg.h` | 文件不存在（**「没有板卡 config」验收项**） |
| 5 | `grep -rn "jl1111\|rtl8201f\|tja1101" zephyr_user/drivers/ethercat/hal/ports/ --include=*.c --include=*.h` | 只在 `phy/` 目录命中（端口层不知道芯片型号） |
| 6 | `grep -rn "PDI_Isr\|Sync0_Isr\|Sync1_Isr" zephyr_user/drivers/ethercat/hal/ports/` | 输出为空 |
| 7 | `grep -c "" framework/.../ethercat/ethercat.cpp` | ≤ 130（现在 309） |

### 7.2 编译验证（P7-7 之后）

8. `dust build uhpm6e00evk -p` 通过。
9. `dust build hpm5e31gn1 -p` 通过（同一套代码，只换 dts）。
10. **换板验证**：把两块板 dts 里的 `phys` 节点对调（6e00evk 用 rtl8201f+tja1101b），
    C 代码一行不改，重新编译仍通过——证明 PHY 分层真的成立。
11. 日志顺序：`ecat hal ready` → `ESC Port0: JL1111 configured` → `ecat stack ready rx=.. tx=..`。

### 7.3 行为等价（P8 之后）

12. 网线插上 → `ESC Port0 link up`。
13. 主站扫到从站（`ethercat slaves` 能看到）。
14. 主站写 RxPDO → `trd_test.cpp` 的 `ecat rx N bytes` 正常。
15. 主站读 TxPDO → 读到 `data[i] = i` 递增序列。

---

## 8. 风险与不做的事

| 风险 | 应对 |
|---|---|
| **SSC 的 `HW_*` 是宏**，虚函数无法直接替换 | `hal/ports/hpm_ecat_hw.h` 保留宏定义，宏体一行改成调 `GetDefaultEcatHal().Read/Write` |
| **`ecat_phy_bind` 要按 compatible 选驱动** | 用 `DT_FOREACH_CHILD_STATUS_OKAY` + `IS_ENABLED(DT_HAS_...)` 生成静态表，零运行期字符串比较 |
| **PHY 配置项从 dts 读到 C** | `ecat_phy_bind` 用 `DT_PROP` 逐项取出填 `PhyConfig`；`phy-mode` 字符串用 `DT_ENUM_IDX` 转 int |
| **PHY 数量/顺序** | `phys` 数组下标即端口号；`port1-enabled` 与 `phys` 长度必须一致 |
| **单例改值语义** | 连带改 `trd_test.cpp`（P6-2 那步） |
| **PHY 配置失败要不要中断** | 不中断（现在也是这样：Port1 失败只告警），只在日志标出 |
| **`ecat_board_cfg.h` 删了会不会漏东西** | 它只剩 SII 分区 / EEPROM I2C 引脚 / 延时三样，全部搬进 `ecat_port_hw.h`；PHY 相关全部进 dts |

**不做的事**：

- 不改 SSC 任何一个文件
- 不改初始化顺序（11 步逐字照抄）
- 不改 PDO 大小（保持 64 字节）
- 不改链路处理方式（`software-link0` + `UpdateLink()` 每 100ms）
- 不引入动态内存（`EcatPhy*` 指向静态对象）

---

## 9. 执行清单

```text
P1 interface 层（4 步）
[ ] 1-1 interface/ecat_types.hpp
[ ] 1-2 interface/ecat_phy.hpp
[ ] 1-3 interface/ecat_hal.hpp
[ ] 1-4 hal/ports/phy/ecat_phy_common.hpp

P2 PHY 驱动（按芯片，4 步）
[ ] 2-1 phy/phy_jl1111.hpp/.cpp
[ ] 2-2 phy/phy_rtl8201f.hpp/.cpp
[ ] 2-3 phy/phy_tja1101b.hpp/.cpp
[ ] 2-4 phy/ecat_phy_bind.hpp/.cpp

P3 HAL 实现（3 步）
[ ] 3-1 hal/ports/ 整目录 git mv（除 hpm5e31_phy.*）
[ ] 3-2 hal/hpm/ecat_hal_hpm.hpp
[ ] 3-3 hal/hpm/ecat_hal_hpm.cpp

P4 core 层（4 步）
[ ] 4-1 core/ecat_pdo.hpp/.cpp
[ ] 4-2 core/ecat_eeprom.hpp/.cpp
[ ] 4-3 core/ecat_stack.hpp
[ ] 4-4 core/ecat_stack.cpp

P5 appl 钩子（1 步）
[ ] 5-1 appl/ecat_appl_hooks.cpp

P6 顶层瘦身（2 步）
[ ] 6-1 ethercat.hpp 重写
[ ] 6-2 ethercat.cpp 重写 + trd_test.cpp 连带改

P7 构建链与 dts（7 步）
[ ] 7-1 framework/.../ethercat/Kconfig
[ ] 7-2 framework/drivers/Kconfig 加 select
[ ] 7-3 zephyr_user/drivers/Kconfig 加 HAL + 三个 PHY 符号
[ ] 7-4 zephyr_user/drivers/CMakeLists.txt 加 HAL + PHY 段
[ ] 7-5 sdk_glue_user/dts/bindings/ethercat/ 新增基础 yaml + 三个芯片 yaml + esc 加 phys
[ ] 7-6 三块板 dts 加 PHY 节点 + phys 属性
[ ] 7-7 ecat_board_cfg.h → ecat_port_hw.h（删掉全部板卡分支）

P8 清理与验证（2 步）
[ ] 8-1 删 sdk_glue_user/drivers/ethercat/
[ ] 8-2 framework/.../ethercat/CMakeLists.txt 改源码列表

收尾
[ ] §7.1 静态检查 7 条（第 3/4/5 条是 PHY 分层核心验收）
[ ] §7.2 两块板编译 + 换板验证（C 代码零改动）
[ ] §7.3 行为等价
```
