# EtherCAT 重构第五阶段：PHY 作为设备模块、ESC 走 HAL、板卡在 trd 组装

日期：2026-10-31（重写版，上一版整份作废）。
范围：`framework/drivers/device/ecat_phy/`、`framework/drivers/communication/ethercat/`、`drivers/ethercat/`、`drivers/CMakeLists.txt`、`drivers/Kconfig`、`project/thread/ecat/`、`sdk_glue_user/dts/`。
本文只交付规划，不实施源码改动、不编译、不烧录。

---

## 0. 上一版错在哪

上一版（同日）把 PHY 做成了**由 dts `compatible` 选中的 Zephyr 设备**，每颗芯片 `DEVICE_DT_INST_DEFINE`，`EcatEscPortHpm` 通过 `DEVICE_DT_GET(...).data` 把实例捞回来。用户的三条纠正：

> **esc 要调用底层 api 所以用 hal 去隔开，phy 是具体模块，这里写错了并且位置也他妈放错了，应该放到 drivers/modules 层，还有他妈的我说板卡组装是到 trd 里面组装，他妈的老子要的不是一个固定的板卡配置，而是无论什么板卡都可以组装**

后续再纠正 PHY 的落点：

> **phy 放到这里 framework\drivers\device**

再纠正 SII：

> **有两种介质你他妈不能做选择？分开个几把啊？你他妈就算写成一个类，他妈的就不能放在 esc 中组装？**

即：**SII 是 ESC 的功能，不是第二块硬件；两种介质在 `EcatEsc` 内部按配置选，不许拆成独立接口/独立类。**

拆成三条硬约束：

| 编号 | 纠正 | 上一版的做法 | 为什么错 |
|---|---|---|---|
| **R1** | ESC 要调用底层 API，所以用 **HAL 隔开** | `EcatEscPortHpm` 直接 `#include "hpm_esc.h"` 调 `hpm_esc_read` / `hpm_esc_write` | 协议侧的 ESC 适配与厂商 C API 之间没有隔离层，换 SDK 版本/换芯片要改同一文件 |
| **R2** | PHY 是**设备模块**，落点 **`framework/drivers/device/ecat_phy/`** | 放在 `drivers/ethercat/hal/phy/`，并且是 `DEVICE_DT_INST_DEFINE` 的 Zephyr 设备 | PHY 是器件模块，不是 ESC 的 HAL；放进 `hal/` 把"传输能力"和"驱动能力"混在一起 |
| **R3** | **板卡组装在 trd 里做**，要"任何板卡都能组装"，不是固定板卡配置 | PHY 型号由 dts `compatible` + `DT_HAS_*_ENABLED` 在**编译期**决定 | 板卡 dts 写死 `compatible = "hpmicro,ecat-phy-jl1111"` → 换个板子要改 dts + binding + Kconfig，组装逻辑散在三个地方 |

**上一版的核心错误一句话**：把"选哪个 PHY"交给了设备树和 Kconfig，等于把组装写死在板卡配置里。用户要的是**同一份代码，任何板卡在 trd 里写出自己的装配**。

---

## 1. 四层职责

```text
┌──────────────────────────────────────────────────────────────┐
│ L4  trd（板卡组装）    project/thread/ecat/trd_ecat.cpp       │
│     选 ESC HAL、选 PHY 模块、填 PhyConfig、接线、轮询链路      │
│     ← 每条板卡的差异只出现在这里                              │
└───────────────────────┬──────────────────────────────────────┘
                        │ EcatEsc& / EcatPhy&
┌───────────────────────▼──────────────────────────────────────┐
│ L3 协议核心            framework/.../communication/ethercat/   │
│     EcatDevice 只认 EcatEsc（SII 是 ESC 的功能）—— 不认识 PHY │
└───────────────────────┬──────────────────────────────────────┘
                        │ interface/ecat_esc.hpp 纯虚契约
┌───────────────────────▼──────────────────────────────────────┐
│ L2b ESC HAL            drivers/ethercat/hal/hpm/               │
│     EcatEscHpm —— 唯一允许调厂商 API 的地方（含 SII 两种介质） │
└───────────────────────┬──────────────────────────────────────┘
                        │ hpm_esc_*（厂商 C API）
┌───────────────────────▼──────────────────────────────────────┐
│ L2a 厂商驱动           sdk_glue_user/drivers/ethercat/         │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ L3b PHY 设备模块       framework/drivers/device/ecat_phy/      │
│     PhyJl1111 / PhyRtl8201f / PhyTja1101b                     │
│     只依赖 EcatEsc 契约里的 MdioRead / MdioWrite               │
│     ← 不是 Zephyr 设备，不读 dts，不 include 厂商头            │
└──────────────────────────────────────────────────────────────┘
```

**为什么 PHY 在 framework 而不在子树 drivers**：

`framework/drivers/device/` 下已有 `gpio/`、`pwm/`——它们都是"把 Zephyr 设备包成 C++ 类"的器件模块。PHY 属于同一类：一颗芯片的寄存器序列，与板卡无关。放在这里，所有用户区共享同一份 PHY 驱动。

**为什么协议核心不认识 PHY**：

`EcatDevice` 的职责是 AL 状态机、邮箱、过程数据；链路状态是**板卡侧的事实**，由 trd 读 PHY 再通过 `EcatEsc::SetLinkState` 写回 ESC。`EcatDevice` 只认 `EcatEsc` 一个契约（SII 也在里面）。

**依赖方向**：

- `framework/drivers/device/ecat_phy/` → `framework/.../ethercat/interface/ecat_esc.hpp`（只用 MDIO 两个方法）
- `drivers/ethercat/hal/hpm/` → `framework/.../interface/ecat_esc.hpp` + 厂商头
- `project/thread/ecat/trd_ecat.cpp` → 以上全部 + `framework/.../ethercat/ethercat.hpp`
- **PHY 模块与 ESC HAL 之间零代码依赖**：PHY 拿的是 `EcatEsc&` 抽象，不认识 `EcatEscHpm`。

---

## 2. 目录结构

### 2.1 现状

```text
framework/
└── drivers/
    ├── communication/
    │   └── ethercat/
    │       ├── interface/
    │       │   ├── ecat_esc.hpp
    │       │   ├── ecat_phy.hpp          ← 要搬走（R2）
    │       │   ├── ecat_sii.hpp
    │       │   └── ecat_types.hpp
    │       └── core/ ...
    └── device/
        ├── gpio/{input,output}.{hpp,cpp}
        └── pwm/pwm.{hpp,cpp}

drivers/
└── ethercat/
    └── hal/
        ├── hpm/
        │   ├── ecat_board_hpm.hpp
        │   ├── ecat_esc_port_hpm.{hpp,cpp}      ← ESC 适配，直接调 hpm_esc_*
        │   ├── ecat_log.hpp
        │   └── ecat_sii_hpm.{hpp,cpp}
        └── phy/                                  ← 位置错（R2）
            ├── ecat_phy_common.hpp
            ├── ecat_phy_mdio.cpp
            ├── jl1111/phy_jl1111.{hpp,cpp}       ← 是 DEVICE_DT_INST_DEFINE（R3）
            ├── rtl8201f/phy_rtl8201f.{hpp,cpp}
            └── tja1101b/phy_tja1101b.{hpp,cpp}
```

### 2.2 目标

```text
framework/
└── drivers/
    ├── communication/
    │   └── ethercat/
    │       ├── interface/
    │       │   ├── ecat_esc.hpp              ← SII 方法并入这里
    │       │   └── ecat_types.hpp            ← ecat_phy.hpp 移走 / ecat_sii.hpp 删除
    │       └── core/ ...
    └── device/
        ├── gpio/{input,output}.{hpp,cpp}
        ├── pwm/pwm.{hpp,cpp}
        └── ecat_phy/                          ← 新（R2）
            ├── ecat_phy.hpp                   ← 从 interface/ 搬来（抽象）
            ├── ecat_phy_common.hpp            ← 从 drivers/.../hal/phy/ 搬来
            ├── ecat_phy_mdio.cpp              ← 公共 MDIO 工具，改成只用 EcatEsc
            ├── jl1111/phy_jl1111.{hpp,cpp}
            ├── rtl8201f/phy_rtl8201f.{hpp,cpp}
            └── tja1101b/phy_tja1101b.{hpp,cpp}

drivers/
└── ethercat/
    └── hal/
        └── hpm/
            └── ecat_hal_hpm.{hpp,cpp}          ← 一个平台的 EtherCAT HAL
```

**要点**：

- `framework/drivers/device/ecat_phy/` 自包含：抽象接口 + 公共寄存器 + 三颗芯片，与 `device/gpio/`、`device/pwm/` 同构。
- `drivers/ethercat/hal/hpm/` **一个平台一个文件**，与 `drivers/usb/hal/hpm/usb_hal_hpm.{hpp,cpp}` 完全同构：

  | 现文件名 | 处置 |
  |---|---|
  | `ecat_esc_port_hpm.{hpp,cpp}` | 并入 `ecat_hal_hpm.{hpp,cpp}`，类改名 `EcatEscPortHpm` → `EcatEscHpm` |
  | `ecat_sii_hpm.{hpp,cpp}` | 并入 `ecat_hal_hpm.{hpp,cpp}`，`EcatSiiHpm` 类删除，SII 方法成为 `EcatEscHpm` 的成员 |
  | `ecat_board_hpm.hpp` | **删除**。里面的寄存器偏移与 `framework/.../spec/ecat_registers.hpp` 逐个重复（`0x0220` / `0x0120` / `0x0130` / `0x0134` / `0x0500` / `0x0504` / `0x0508` / `0x0800`），HAL 实现 `EcatEsc` 契约本就依赖协议层，直接用 `ecat::reg::*`；`kHpmMaxPorts` 与 `kSiiReadWidth*` 并进 `ecat_hal_hpm.hpp` |
  | `ecat_log.hpp` | 并入 `ecat_hal_hpm.hpp`（`ECAT_LOG` 宏 + `extern "C" ecat_port_log` 声明），`ecat_port_log` 的定义也放 `ecat_hal_hpm.cpp` |

- **`ecat_sii.hpp` 删除**：SII 是 ESC 对 EEPROM 的访问，不是独立硬件。两种介质（Flash 模拟 / 板载 I2C EEPROM）做成 `EcatEsc` 的配置项，在同一个 HAL 实现里 `#if`/运行期选择，不再单独一个接口、单独一个类、单独一个 trd 成员。
- PHY 模块**不再有 `DEVICE_DT_INST_DEFINE`**，也不再 include `<zephyr/devicetree.h>`。
- **PHY 不再用 `ECAT_LOG`**：`ecat_log.hpp` 是子树 HAL 的日志桥，PHY 搬进 framework 后 include 不到，改用 framework 的 `DUST_LOG_INF`。

---

## 3. 接口变化

### 3.1 `EcatPhy`（搬到 `framework/drivers/device/ecat_phy/ecat_phy.hpp`）

现状（`communication/ethercat/interface/ecat_phy.hpp`）：

```cpp
class EcatPhy
{
public:
	virtual const char* Name() const = 0;
	virtual uint8_t Address() const = 0;
	virtual bool Init(const struct device* mdio_dev, const PhyConfig& cfg) = 0;   // ← 依赖 Zephyr 设备
	virtual bool LinkUp(bool* up) = 0;
};
```

改成：

```cpp
class EcatEsc;

class EcatPhy
{
public:
	virtual ~EcatPhy() = default;

	virtual const char* Name() const = 0;                          // 芯片名（日志用）
	virtual uint8_t     Address() const = 0;                       // MDIO 地址（来自 PhyConfig）

	virtual bool Init(EcatEsc& esc, const PhyConfig& cfg) = 0;     // 通过 ESC 的 MDIO 通道配置芯片
	virtual bool LinkUp(EcatEsc& esc, bool* up) = 0;               // 通过 ESC 的 MDIO 通道读 BMSR
};
```

`PhyConfig` 结构不变，留在同一个头里。

**为什么把 `EcatEsc&` 作为参数传而不是成员持有**：

- PHY 是值对象，trd 里 `static PhyJl1111 phy0_;` 即可，不引入指针生命周期问题；
- `EcatEsc` 的契约里已有 `MdioRead(uint8_t phy, uint8_t reg, uint16_t* value)` / `MdioWrite(...)`，PHY 只用这两个；
- PHY 模块因此**不依赖厂商头、不依赖 dts、不依赖 Zephyr 设备**。

### 3.2 `EcatEsc`（`communication/ethercat/interface/ecat_esc.hpp`）

三处调整：

1. **删掉 `PollLink()`**。链路轮询移到 trd，ESC 不该认识 PHY。
2. **加上 `SetLinkState(uint8_t port, bool up)`**。这是把 PHY 读到的状态写回 ESC 的唯一入口，厂商侧对应 `hpm_esc_set_link_state`。
3. **并入 SII**（原 `EcatSii` 的五个方法），介质模式作为 `Config` 的字段：

```cpp
class EcatEsc
{
public:
	/**
	 * @brief SII 介质模式
	 */
	enum class SiiMode : uint8_t
	{
		Emulated,	// Flash 分区模拟，应用接管读写
		OnBoard,	// 板上 I2C EEPROM，ESC 自己走 I2C 主控
	};

	struct Config
	{
		uint8_t port_count = 2;						// 使能端口数（1~3）
		SiiMode sii_mode   = SiiMode::OnBoard;		// SII 介质模式
	};

	...

	virtual void SetLinkState(uint8_t port, bool up) = 0;	// 把端口链路状态写进 ESC

	virtual SiiMode GetSiiMode() const = 0;											// 当前介质模式
	virtual bool ReadSiiWord(uint32_t word_address, uint8_t* data, uint16_t length) = 0;	// 读一段 SII
	virtual bool WriteSiiWord(uint32_t word_address, const uint8_t* data, uint16_t length) = 0;	// 写一段 SII
	virtual bool ReloadSii() = 0;													// 重载 SII
	virtual void StoreSii() = 0;													// 落盘
};
```

**为什么不再单独 `EcatSii`**：

- SII 的访问寄存器（0x0500 / 0x0504 / 0x0508）在 ESC 里，读介质也由 ESC 的 I2C 主控完成——它是 ESC 的功能，不是第二块硬件。
- 两种介质不是两个接口，是同一个接口的两种实现分支：`Config::sii_mode` 决定 `EcatEscHpm` 内部走 Flash 模拟还是板载 EEPROM（厂商侧本来就用 `CONFIG_HPM_ESC_EEPROM_EMULATION` 分叉）。
- trd 里不再有 `sii_` 成员，只组装 `esc_` 一个对象；`EcatDevice`、`EcatSiiService` 都只持有 `EcatEsc&`。

### 3.3 `EcatDevice`（`communication/ethercat/core/ecat_device.*`）

`Init(EcatEsc& esc, EcatSii& sii)` → `Init(EcatEsc& esc)`。

`EcatSiiService` 的 `Bind(esc, sii)` 改成 `Bind(esc)`，内部只走 `EcatEsc` 的 SII 方法。

同时删 `Poll()` 里的链路轮询：

```cpp
// 现状
if (now >= next_link_poll_) {
	esc_->PollLink();
	next_link_poll_ = now + kLinkPollMs;
}
```

删掉这一段，`next_link_poll_` 成员一并删除。`EcatDevice` 从此不认识 PHY。

### 3.4 `EthercatSlave`（`communication/ethercat/ethercat.{hpp,cpp}`）

现状无参 `Init()`，内部取全局默认实例。改成注入：

```cpp
class EthercatSlave final
{
public:
	bool Init(EcatEsc& esc);
	...
};
```

`ethercat.cpp` 里删掉对 `GetDefaultEcatEsc()` / `GetDefaultEcatSii()` 的调用。**这两个全局工厂函数随之删除**。

---

## 4. trd 组装形态

新建 `project/thread/ecat/`（与 `gpio` / `imu` / `remote` / `test` 同级）：

```text
project/thread/ecat/
├── CMakeLists.txt
└── trd_ecat.cpp
```

`trd_ecat.cpp`：

```cpp
/**
 * @file trd_ecat.cpp
 * @author qingyu
 * @brief EtherCAT 从站薄启动：板卡组装 + 链路轮询 + 单拍推进
 * @version 0.1
 * @date 2026-10-31
 *
 * @copyright Copyright (c) 2026
 */

#include "thread.hpp"
#include "ethercat.hpp"
#include "ecat_hal_hpm.hpp"
#include "ecat_phy.hpp"
#include "phy_jl1111.hpp"

namespace {

// 本板卡的装配：ESC HAL + 两颗 JL1111。换板卡只改这一段。
ecat::EcatEscHpm  esc_ {};
ecat::PhyJl1111   phy0_ {};
ecat::PhyJl1111   phy1_ {};

ecat::EcatPhy* const kPhys[] = { &phy0_, &phy1_ };

constexpr uint8_t  kPortCount = 2;
constexpr uint32_t kLinkPollMs = 100;

ecat::EthercatSlave slave_ {};
int64_t next_link_poll_ = 0;

/**
 * @brief 组装本板卡的 EtherCAT 硬件
 * @return true=成功
 */
bool Assemble()
{
	ecat::EcatEsc::Config esc_cfg {};

	esc_cfg.port_count = kPortCount;
	esc_cfg.sii_mode   = ecat::EcatEsc::SiiMode::OnBoard;

	if (!esc_.Open(esc_cfg)) {
		return false;
	}

	ecat::PhyConfig phy_cfg {};

	phy_cfg.mdio_address = 0;
	phy_cfg.advertise    = 0x0100;
	phy_cfg.autoneg      = true;
	phy_cfg.interface    = 0;

	if (!phy0_.Init(esc_, phy_cfg)) {
		return false;
	}

	phy_cfg.mdio_address = 1;

	if (!phy1_.Init(esc_, phy_cfg)) {
		return false;
	}
	return true;
}

/**
 * @brief 轮询 PHY 链路并写回 ESC
 */
void PollLink()
{
	for (uint8_t port = 0; port < kPortCount; ++port) {
		bool up = false;

		if (kPhys[port] != nullptr && kPhys[port]->LinkUp(esc_, &up)) {
			esc_.SetLinkState(port, up);
		} else {
			esc_.SetLinkState(port, false);
		}
	}
}

}

bool thread_init()
{
	if (!Assemble()) {
		return false;
	}
	return slave_.Init(esc_);
}

static TaskResult Task(void* context)
{
	(void)context;

	const int64_t now = k_uptime_get();

	if (now >= next_link_poll_) {
		PollLink();
		next_link_poll_ = now + kLinkPollMs;
	}

	slave_.Poll();          // 单拍，立刻返回
	return TaskResult::Continue;
}

REGISTER_INIT  (thread_init,  EarlyInit,  High, HaltOnFail, "ecat_init");
REGISTER_THREAD(thread_start, EarlyThread, "ecat_start");
```

**换板卡示例**（HPM5E31：RTL8201F + TJA1101B）只改装配段：

```cpp
ecat::EcatEscHpm  esc_ {};
ecat::PhyRtl8201f phy0_ {};
ecat::PhyTja1101b phy1_ {};
```

**没有任何一处需要改 dts、binding 或 Kconfig 的 PHY 符号**——这就是 R3 要的"任何板卡都能组装"。

---

## 5. dts 与 binding 的处置

### 5.1 dts

删掉 `uhpm6e00evk.dts` 里的两个 PHY 节点（第 63~82 行整块），`esc0` 只保留 ESC 本体属性：

```dts
esc0: ethercat@f1700000 {
	compatible = "hpmicro,hpm-esc";
	reg = <0xf1700000 0x10000>;
	interrupt-parent = <&plic>;
	interrupts = <133 2>, <134 3>, <135 3>, <136 2>;
	interrupt-names = "pdi", "sync0", "sync1", "reset";
	clocks = <&clk CLOCK_ESC0 8 1>;
	pinctrl-0 = <&pinmux_esc0 &pinmux_ecat_io>;
	pinctrl-names = "default";
	phy-reset-gpios = <&gpioa 10 GPIO_ACTIVE_LOW>;
	phy-addr-offset = <0>;
	link0-ctrl-index = <3>;
	link1-ctrl-index = <0>;
	software-link0;
	software-link1;
	port1-enabled;
	status = "okay";
};
```

**为什么 PHY 节点要删**：MDIO 地址、接口模式、通告能力现在写在 trd 的 `PhyConfig` 里。dts 留一份会和 trd 冲突，两处不同步时很难查。

### 5.2 binding

删除 `sdk_glue_user/dts/bindings/ethercat/` 下四个文件：

- `hpmicro,ecat-phy.yaml`
- `hpmicro,ecat-phy-jl1111.yaml`
- `hpmicro,ecat-phy-rtl8201f.yaml`
- `hpmicro,ecat-phy-tja1101b.yaml`

---

## 6. Kconfig 与 CMake

### 6.1 `framework/drivers/Kconfig`

加三个设备符号（与 `DUST_DEV_GPIO_OUTPUT` / `DUST_DEV_PWM` 同级）：

```kconfig
config DUST_DEV_ECAT_PHY_JL1111
    default n
    bool "JL1111 EtherCAT PHY"
    help
        JL1111 100BASE-TX PHY（HPM6E00EVK 的 PORTA/PORTB）。
        由业务线程（trd）组装时选用，不按 dts 自动选中。

config DUST_DEV_ECAT_PHY_RTL8201F
    default n
    bool "RTL8201F EtherCAT PHY"
    help
        RTL8201F 100BASE-TX PHY（HPM5E31 自定义板的 PORTA）。

config DUST_DEV_ECAT_PHY_TJA1101B
    default n
    bool "TJA1101B EtherCAT PHY"
    help
        TJA1101B 100BASE-T1 PHY（HPM5E31 自定义板的 PORTB）。
```

### 6.2 `framework/drivers/CMakeLists.txt`

在 `DUST_COM_ETHERCAT` 块之后加：

```cmake
# EtherCAT PHY 设备模块（与 gpio/pwm 同层）
# 编译哪些芯片由业务门禁决定，不按 dts 自动选。
if(CONFIG_DUST_DEV_ECAT_PHY_JL1111)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/ecat_phy_mdio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/jl1111/phy_jl1111.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/ethercat/interface)
endif()

if(CONFIG_DUST_DEV_ECAT_PHY_RTL8201F)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/ecat_phy_mdio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/rtl8201f/phy_rtl8201f.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/ethercat/interface)
endif()

if(CONFIG_DUST_DEV_ECAT_PHY_TJA1101B)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/ecat_phy_mdio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy/tja1101b/phy_tja1101b.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/device/ecat_phy
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/ethercat/interface)
endif()
```

> `ecat_phy_mdio.cpp` 是三颗共用的公共实现，多个门禁同时开会重复 `target_sources` 同一文件。CMake 对 `app` target 重复加入同一源文件会报错，因此在最终实现时可以把它提到一个无条件块里（只要任一 PHY 符号开启就编一次），或用 `set` 去重。**这一步实现时必须实测。**

### 6.3 `drivers/CMakeLists.txt`

删掉 `DUST_ECAT_HAL_HPM` 块里三颗 PHY 的 `target_sources` 与 `if(CONFIG_DUST_ECAT_PHY_*)` 分支，只留 ESC HAL 与 SII：

```cmake
if(CONFIG_DUST_ECAT_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/ecat_hal_hpm.cpp
        ${HPM_SDK_DIR}/drivers/src/hpm_esc_drv.c
        ${HPM_SDK_DIR}/utils/hpm_crc32.c)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm
        ${FW_ROOT}/drivers/communication/ethercat/interface
        ${FW_ROOT}/drivers/communication/ethercat/core
        ${FW_ROOT}/drivers/device/ecat_phy
        ${SDK_GLUE_USER_DIR}/drivers/ethercat
        ${SDK_GLUE_USER_DIR}/drivers/ethercat/include
        ${HPM_SDK_DIR}/components/eeprom_emulation
        ${HPM_SDK_DIR}/components/eeprom_emulation/port
        ${HPM_SDK_DIR}/utils)
endif()
```

### 6.4 `drivers/Kconfig`

删掉 `DUST_ECAT_PHY_JL1111` / `_RTL8201F` / `_TJA1101B`，删掉 `DUST_ECAT_HAL_HPM` 里的三条 `select ... if DT_HAS_HPMICRO_ECAT_PHY_*_ENABLED`。

```kconfig
config DUST_ECAT_HAL_HPM
    bool "HPMicro ESC EtherCAT HAL"
    default n
    depends on CLOCK_CONTROL && GPIO && PINCTRL
    select HPM_ESC
    help
      HPMicro 片内 ESC 的 HAL 实现：EcatEsc 契约的厂商落地（SII 两种介质也在这里）。
      只提供底层 API 的隔离层，不含 PHY——PHY 是 framework 设备模块，
      由业务线程（trd）按板卡组装。
```

### 6.5 `project/Kconfig`

加业务门禁：

```kconfig
config TRD_ECAT
    bool "EtherCAT slave thread"
    default n
    select DUST_COM_ETHERCAT
    select DUST_ECAT_HAL_HPM
    select DUST_DEV_ECAT_PHY_JL1111
    help
      EtherCAT 从站业务线程。PHY 型号在 trd_ecat.cpp 里组装，
      这里 select 的是本板卡用到的模块，换板卡时同步改这几个 select。
```

`DUST_COM_ETHERCAT` 是协议核心的门禁（`framework/drivers/CMakeLists.txt` 里已有），确认它存在且已 `select` 必要符号。

---

## 7. 执行步骤

> **本阶段拆成 10 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件（或一组必须同时改的文件），改完停下报告，等"继续"。
> **第 1 步是只读核查，不改代码。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | 无（只读） | 核查现有调用点：`PollLink()` / `GetDefaultEcatEsc()` / `GetDefaultEcatSii()` / `DEVICE_DT_GET(ecat_phy*)` / `LinkUp(`。**产出：调用点清单** | 无 | 不改任何文件 |
| 2 | `framework/drivers/device/ecat_phy/`（新建） | 把 `communication/ethercat/interface/ecat_phy.hpp` 整份搬进 `device/ecat_phy/ecat_phy.hpp`；`Init`/`LinkUp` 参数改 `EcatEsc&`；`#include "interface/ecat_esc.hpp"` | 1 | 旧 include 失效，三颗 PHY 编不过 |
| 3 | `framework/drivers/communication/ethercat/interface/ecat_esc.hpp` | 删 `PollLink()`；加 `SetLinkState(uint8_t, bool)`；**并入 SII 五个方法 + `SiiMode` + `Config::sii_mode`** | 2 | `EcatEscPortHpm` / `EcatDevice` 编不过 |
| 4 | `framework/drivers/communication/ethercat/core/ecat_device.{hpp,cpp}` + `core/mailbox/ecat_sii_service.{hpp,cpp}` + 删 `interface/ecat_sii.hpp` | `EcatDevice::Init(EcatEsc&)`；`EcatSiiService::Bind(EcatEsc&)`；删 `Poll()` 里的 `esc_->PollLink()` 与 `next_link_poll_`；删 `ecat_sii.hpp` | 3 | 无 |
| 5 | `framework/drivers/communication/ethercat/ethercat.{hpp,cpp}` | `EthercatSlave::Init(EcatEsc&)`；删 `GetDefaultEcatEsc/GetDefaultEcatSii` 调用 | 4 | trd（尚不存在） |
| 6 | `drivers/ethercat/hal/hpm/ecat_esc_port_hpm.{hpp,cpp}` + `ecat_sii_hpm.{hpp,cpp}` + `ecat_board_hpm.hpp` + `ecat_log.hpp` → `ecat_hal_hpm.{hpp,cpp}` | 整目录合并成一个文件：类改名 `EcatEscPortHpm` → `EcatEscHpm`；删 `CollectPhys` / `phys_[]` / `PollLink()` / `GetDefaultEcatPort` / `GetDefaultEcatEsc` / `GetDefaultEcatSii`；删 `ecat_board_hpm.hpp` 的重复寄存器常量改用 `ecat::reg::*`；实现 `SetLinkState`；`ecat_port_log` 定义搬进来 | 3 | 无 |
| 7 | `framework/drivers/device/ecat_phy/`（搬迁） | 把 `drivers/ethercat/hal/phy/` 下 `ecat_phy_common.hpp`、`ecat_phy_mdio.cpp`、三颗芯片整目录搬进 `device/ecat_phy/`；三颗 PHY 删 `DEVICE_DT_INST_DEFINE` 与 dts 宏，改 `Init(EcatEsc&, const PhyConfig&)`；`ecat_phy_mdio.cpp` 改用 `EcatEsc::MdioRead/MdioWrite`，`ECAT_LOG` 改 `DUST_LOG_INF` | 2 | 无 |
| 8 | `framework/drivers/CMakeLists.txt`、`framework/drivers/Kconfig`、`drivers/CMakeLists.txt`、`drivers/Kconfig` | 按 §6 改；加 `DUST_DEV_ECAT_PHY_*`，删 `DUST_ECAT_PHY_*` | 7 | 编译期才知道有没有漏 |
| 9 | `sdk_glue_user/dts/...` + binding | 删两个 PHY 节点、删四个 binding 文件 | 7 | 无 |
| 10 | `project/thread/ecat/`（新建）+ `project/Kconfig` + `project/thread/CMakeLists.txt` | 新建 `CMakeLists.txt` + `trd_ecat.cpp`；加 `TRD_ECAT`；加 `if(CONFIG_TRD_ECAT)` 块 | 5~9 | **本步之后恢复可编译**，等用户编译 |

**编译状态**：第 2 步起 `drivers/` 侧 PHY 编不过，直到第 7 步搬完恢复；第 10 步补上 trd 后整体可编（不含厂商头路径问题）。

---

## 8. 验收

| 步 | 验收 |
|---|---|
| 1 | 调用点清单列出全部 `PollLink` / `GetDefault` / `DEVICE_DT_GET(ecat_phy` / `LinkUp` 位置 |
| 2 | `grep -rn "struct device" framework/drivers/device/ecat_phy/ecat_phy.hpp` 为空 |
| 3 | `grep -n "PollLink" framework/.../interface/ecat_esc.hpp` 为空；`grep -n "SetLinkState\|ReadSiiWord\|SiiMode" ...` 有输出 |
| 4 | `grep -n "PollLink\|next_link_poll" framework/.../core/ecat_device.*` 为空；`ls framework/.../interface/ecat_sii.hpp` 不存在；`grep -rn "EcatSii\\b" framework/.../core/` 为空 |
| 5 | `grep -rn "GetDefaultEcat" framework/` 为空；`grep -rn "EcatSii" framework/.../ethercat.{hpp,cpp}` 为空 |
| 6 | `ls drivers/ethercat/hal/hpm/` 只剩 `ecat_hal_hpm.{hpp,cpp}`；`grep -rn "CollectPhys\|GetDefaultEcat\|ecat_board_hpm\|ecat_log.hpp" drivers/ethercat/hal/hpm/` 为空 |
| 7 | `grep -rn "DEVICE_DT_INST_DEFINE\|DT_INST_REG_ADDR\|DT_INST_PARENT" framework/drivers/device/ecat_phy/` 为空；`grep -rn "hpm_esc_\|ECAT_LOG" framework/drivers/device/ecat_phy/` 为空 |
| 8 | `grep -rn "DUST_ECAT_PHY" drivers/Kconfig` 为空；`grep -rn "DUST_DEV_ECAT_PHY" framework/drivers/Kconfig` 有输出 |
| 9 | `grep -rn "ecat-phy" sdk_glue_user/dts/` 为空 |
| 10 | `grep -n "EcatEscHpm\|PhyJl1111" project/thread/ecat/trd_ecat.cpp` 有输出；用户编译通过 |

---

## 9. 不许做的事

1. **不许让 PHY 模块 include 任何厂商头**（`hpm_esc.h` / `hpm_common.h`）——它只认 `EcatEsc` 的 MDIO 两个方法。
2. **不许让 PHY 模块 include `<zephyr/devicetree.h>`**，不许 `DEVICE_DT_INST_DEFINE`。
3. **不许让 PHY 模块判断 `CONFIG_*` 板卡宏**。
4. **不许在 `drivers/ethercat/hal/` 里出现 PHY 型号名**，也不许在 `framework/.../ethercat/core/` 里出现 `EcatPhy`。
4.1 **不许再把 SII 拆成独立接口/独立类**——它是 ESC 的功能，两种介质用 `Config::sii_mode` 在 `EcatEscHpm` 内部选。
5. **不许把 PHY 的 MDIO 地址 / 通告能力写进 dts 或 binding**——那是 trd 里 `PhyConfig` 的事。
6. **不许保留 `GetDefaultEcatEsc` / `GetDefaultEcatSii` 这类"全局默认板卡"工厂**。
7. **不许再写 bind / registry / pool / 模板展开读取属性**。
8. **不许改厂商驱动不进 patch**（`esc_hpmicro.c` 在 `sdk_glue_user/drivers/`，改动同步 `zephyr-patches/`）。
9. **不许在函数体内用 `/* */`**，一律 `//`。
10. **不许拆行**：算式、函数调用、参数列表一律一行写完。

---

## 10. 与前面几个阶段的关系

| 阶段 | 文档 | 内容 | 状态 |
|---|---|---|---|
| 一 | `EtherCAT从站整体C++重构规划.md` | 协议核心 C++ 化（30 步） | 已执行 |
| 二 | `EtherCAT重构第二阶段-HAL清理规划.md` | HAL 清理与编译修正（7 步） | 已执行 |
| 三 | `EtherCAT重构第三阶段-HAL分层规划.md` | 厂商 C 代码迁回 `sdk_glue_user`（5 步） | 已执行 |
| 四 | `EtherCAT重构第四阶段-PHY设备模型规划.md` | PHY 设备模型 | **作废** |
| 五 | 本文 | PHY 设备模块 + ESC HAL + trd 组装（11 步） | 待执行 |

**第四阶段文档处置**：保持作废，顶部说明改为指向本文。其 §0 的三次错误复盘、§7.6 的三颗 PHY 差异对照表仍有参考价值。

---

## 11. 本次重写的教训

上一版和第四阶段犯的是同一个错：**没有先问"为什么拆"**。

- 第四阶段答"为了复用"，所以做成 Zephyr 设备；
- 上一版答"为了 dts 选型"，所以做成 `compatible` 驱动。

两个答案都只回答了"怎么选"，没回答"谁来组装"。用户要的答案只有一个：

> **组装是板卡的事，板卡由 trd 表达；framework 只提供可组装的零件。**

所以：

- `framework/drivers/device/ecat_phy/` 提供"PHY 这个零件"（与 gpio/pwm 同层）；
- `drivers/ethercat/hal/` 提供"ESC 这个零件"（HAL 隔离底层 API）；
- `trd_ecat.cpp` 决定"这块板子用哪个零件、怎么接"。

**判据**：任何新增板卡，改动应该只出现在 `trd_*.cpp` 与 `project/Kconfig` 的 select 行里。如果还要改 `drivers/`、`framework/drivers/device/ecat_phy/` 或 dts 的 PHY 节点，说明抽象没做对。

**SII 的教训（同日再纠正）**：上一版把"两种介质"错当成"两个接口"。**同一硬件的不同实现方式不是新的抽象层**——它只是一个配置分支。判据：这个"东西"有没有自己独立的寄存器/独立的物理链路？没有，就属于宿主，做成宿主的一个字段。
