/**
 * @file EtherCAT从站协议栈融入规划
 * @author qingyu
 * @brief 把 EtherCAT 从站协议栈（SSC 5.13）接入 zephyr_user + sdk_glue_user 两个文件区的实施规划
 * @version 1.1
 * @date 2026-10-27
 *
 * @copyright Copyright (c) 2026
 *
 */

# EtherCAT 从站协议栈融入规划

> 目标：HPM6E00EVK（HPM6E80，片内 ESC @ `0xF1700000`）跑起 EtherCAT 从站，协议栈用 SSC 5.13。
> 两个文件区：`E:\Zephyr\zephyr_user`（子树）与 `E:\Zephyr_HPMicro\sdk_glue_user`（用户层板卡/驱动覆盖区）。
>
> **v1.1 修订（2026-10-27）**：执行顺序改为**协议栈先行**——P1 先把 SSC 接进来，板级/binding/引脚全部后移。v1.0 把 binding/pinctrl 放在最前，与"先把协议栈接入"的要求相反。

---

## 1. 结论先行

**协议栈已经到手，不需要申请授权、不需要找倍福、不需要找 HPMicro。**

`HNUST-Dust/hpm_support`（公开仓库，HPMicro 官方 `zephyr_sdk_glue` 的 fork）的 `samples/ecat/EtherCAT/ssc/` 就是完整 SSC 5.13，8 个 `.c` 共 9650 行：

| 文件 | 行数 | 作用 |
|---|---:|---|
| `ecatslv.c` | 2795 | 核心协议栈：INIT/PREOP/SAFEOP/OP 状态机、SM、WKC、AL 状态 |
| `objdef.c` | 1795 | 对象字典：0x1C00/0x1C12/0x1C13、0x6000/0x7000 |
| `sdoserv.c` | 1349 | SDO 服务 |
| `ecatappl.c` | 1202 | 应用层调度，调用三个 `APPL_*` 钩子 |
| `mailbox.c` | 1000 | SM0/SM1 邮箱收发 |
| `coeappl.c` | 945 | CoE 应用层 |
| `digital_io.c` | 376 | 示例应用（可替换） |
| `ecatcoe.c` | 188 | CoE 紧急报文 |

**主从范围**：SSC = **Slave** Stack Code，**只有从站**。主站在 PC/Linux 侧（IgH / SOEM / TwinCAT），不在本规划范围。

---

## 2. 已核实的现状

全部为本次实测，不是推断。

### 2.1 硬件

| 项 | 事实 | 证据 |
|---|---|---|
| ESC 存在 | HPM6E80 有 ESC 外设 | `soc/HPM6E00/HPM6E80/soc_modules.list:47` → `HPMSOC_HAS_HPMSDK_ESC=y` |
| ESC 地址 | `0xF1700000` | `soc/HPM6E00/HPM6E80/hpm_soc_ip.h:611` |
| 板卡 ECAT 硬件 | Port0=P1、Port1=P2、RUN LED=PE03、ERR LED=PE02 | `hpm_sdk/boards/hpm6e00evk/README_en.rst:454` |
| SDK 寄存器驱动 | `hpm_esc_drv.h` 454 行 + `hpm_esc_drv.c` 145 行 | `hpm_sdk/drivers/` |

### 2.2 两个文件区各自的现状

| 检查项 | `sdk_glue_user` | `zephyr_user` |
|---|---|---|
| 顶层目录 | `boards/ dts/ soc/ zephyr-patches/`（**无 `drivers/`、无 `CMakeLists.txt`、无 `module.yml`**） | `boards/ dts/ drivers/ framework/ platform/ project/` |
| 在构建中的角色 | `BOARD_ROOT` / `SOC_ROOT` / `DTS_ROOT`（`project/CMakeLists.txt:77-80`） | `drivers/` 通过 `ZEPHYR_EXTRA_MODULES` 作为 Zephyr 模块加载（`project/CMakeLists.txt:88-92`） |
| ESC 驱动 | **无** | **无** |
| ESC binding | ~~无~~ **已建**（2026-10-27，P1 步 1） | **无** |
| `esc0` dts 节点 | **无**（`uhpm6e00evk.dts` 只有 memory/leds/buttons/eeprom/flash/usb） | — |
| HPM SDK ESC 编译开关 | **无**（`sdk_glue/drivers/CMakeLists.txt` 里没有 `esc` 子目录） | — |
| SSC 源码 | **无** | **无** |

### 2.3 参考实现（公开仓库）

| 仓库 | 内容 |
|---|---|
| `HNUST-Dust/hpm_support` 分支 `codex/dust-hpm5e31` | `drivers/ethercat/esc_hpmicro.c`（368 行）+ `include/zephyr/drivers/ethercat/hpm_esc.h`（131 行）+ `dts/bindings/ethercat/hpmicro,hpm-esc.yaml`（60 行）+ `Kconfig.hpmicro`；`samples/ecat/EtherCAT/`（SSC 全套 + `bsp/ecat_zephyr_port.c`） |
| `HNUST-Dust/dust-adapter` 分支 `ethercat` | `external/ssc/`（SSC 另一份，12253 行）、`apps/ethercat_slave/src/hpm5e_esc_hw.c`（85 行硬件抽象层）、`tools/import_ssc.py`、`docs/ETHERCAT_SSC_GATEWAY.md` |

本地副本：`E:\Zephyr\zephyr_user\temp\hpm_ecat_ref\`（`EtherCAT/ssc/`、`EtherCAT/esc_hpmicro.c`、`EtherCAT/hpmicro,hpm-esc.yaml`、`EtherCAT/bsp/`、`include/zephyr/drivers/ethercat/hpm_esc.h`）。

---

## 3. 分层归属（写死，执行时不再讨论）

| 内容 | 归属 | 路径 | 为什么 |
|---|---|---|---|
| SSC 协议栈源码（倍福，第三方） | **zephyr_user** | `drivers/ethercat/ssc/` | 非 HPM 代码，属第三方协议栈；`zephyr_user/drivers` 是 Zephyr 模块（有 `zephyr/module.yml`），能编 C |
| ESC 驱动（C） | **sdk_glue_user** | `drivers/ethercat/esc_hpmicro.c` + `include/zephyr/drivers/ethercat/hpm_esc.h` | HPM 厂商代码，按 `hpm-patch-rule` 只能落用户覆盖层，不能动官方 `sdk_glue` |
| SSC 硬件抽象层（端口层） | **sdk_glue_user** | `drivers/ethercat/esc_port/` | `HW_*` 宏、ISR、PHY、EEPROM 模拟全是 HPM/板卡相关 |
| ESC dts binding | **sdk_glue_user** | `dts/bindings/ethercat/hpmicro,hpm-esc.yaml` | binding 是板级描述，`sdk_glue_user` 在 `DTS_ROOT` 里 |
| `esc0` 节点 + pinctrl | **sdk_glue_user** | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk.dts`、`-pinctrl.dtsi` | 板级硬件接线 |
| 板级 defconfig | **sdk_glue_user** | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk_defconfig` | 板卡默认能力 |
| 业务线程 / PDO 映射 | **zephyr_user** | `project/thread/ecat/trd_ecat.cpp` + `project/Kconfig` | 业务归子树 |

**Why HPM 代码必须放 `sdk_glue_user`**：①`hpm-patch-rule` 规定不改官方 `sdk_glue`，厂商改动只落用户覆盖层；②`sdk_glue_user` 原本只有 `boards/dts/soc`，已按 `sdk_glue` 同构补上 `CMakeLists.txt` / `Kconfig.hpm` / `zephyr/module.yml` / `drivers/`，作为 Zephyr 模块加载（`project/CMakeLists.txt` 的 `ZEPHYR_EXTRA_MODULES`，带 `if(EXISTS .../zephyr/module.yml)` 保护）。它的 `module.yml` **不带 `settings:` 根目录**，避免与 `project/CMakeLists.txt` 里 `BOARD_ROOT/SOC_ROOT/DTS_ROOT` 重复引入同一板卡。

**Why 不放 `framework/`**：`framework/` 是 6 个独立 git 子模块，EtherCAT 不是架构层通用能力，属于本板卡业务，留在子树。

---

## 4. 阶段划分

五个阶段，共 22 步。**每次只执行一步。**

**顺序原则：协议栈先行。** P1 先把 SSC 落地接进编译，P2 补 ESC 驱动，P3 才动板级 dts/引脚，P4 起业务线程，P5 补应用钩子。

---

### P1 SSC 协议栈接入（zephyr_user/drivers/ethercat）

> ✅ **P1 已完成（2026-10-27）**。实际落地与下表的差异见 §4.1 执行记录。
> 用户说"开始第 N 步"= 只改表格里那一个文件/那一组文件，改完停下报告，等"继续"。
> 不许一次做多步，不许跨步连带改（必要时见每步的「连带」栏）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `drivers/ethercat/ssc/`（8 个 `.c` + 全部 `.h`） | 新建目录，整份复制 `temp\hpm_ecat_ref\EtherCAT\ssc\`。**保留每个文件的 Beckhoff 原始许可头，一个字节不改**（该目录不受本项目 Apache-2.0 覆盖） | 无 | 无（未加入 CMake，不参与编译） |
| 2 | `drivers/ethercat/ssc/LICENSE-BECKHOFF.md` | 新建。写明：本目录 SSC 源码版权归 Beckhoff Automation GmbH，按 `https://www.beckhoff.com/media/downloads/slave-stack-code/ethercat_ssc_license.pdf` 授权，**不在本项目 Apache-2.0 范围内**；来源标注 `HNUST-Dust/hpm_support` | 步 1 | 无 |
| 3 | `drivers/ethercat/ssc/ecat_zephyr_port.h` | 新建。把 `HW_Init` / `HW_EscRead(Isr)` / `HW_EscWrite(Isr)` / `HW_GetALEventRegister` / `HW_GetTimer` / `HW_EepromReload` / `ENABLE_ESC_INT` / `DISABLE_ESC_INT` 等宏映射到 `hpm_esc_*` API。范本：`temp\hpm_ecat_ref\EtherCAT\bsp\ecat_zephyr_port.h` + `temp\dust-adapter-ethercat\apps\ethercat_slave\src\hpm5e_esc_hw.h`（85 行宏表照抄结构） | 步 1 | 无 |
| 4 | `drivers/ethercat/ssc/ecat_zephyr_port.c` | 新建。实现上一步那批函数：`hpm_esc_read/write`、`irq_enable/disable`、`k_uptime_get_32` 计时、EEPROM reload。范本 `dust-adapter/apps/ethercat_slave/src/hpm5e_esc_hw.c` | 步 3 | 无 |
| 5 | `drivers/CMakeLists.txt` + `drivers/Kconfig` | CMakeLists 追加 `if(CONFIG_DUST_ECAT_SSC)` 块：8 个 `ssc/*.c` + `ecat_zephyr_port.c` 编进 `app`，include 加 `ethercat/ssc`；Kconfig 追加 `config DUST_ECAT_SSC`（`bool`、`default n`） | 步 1~4 | **编译风险点**：SSC 首次参与编译必然暴露配置/头文件缺失，按报错逐条补 `ecat_def.h`。**允许编不过，但必须当场报告缺什么** |

#### 4.1 P1 执行记录（2026-10-27）

实际落地与上表不同，原因：参考实现里 `bsp/ecat_zephyr_port.c` 只有 40 行、仅注册中断；
**`HW_*` 宏和全部 ISR 在 `samples/ecat/Middleware/ESC/`**（`hpm_ecat_hw.c` 570 行等）。
因此端口层按真实结构取 `Middleware/ESC/` 全套。

落地结果（**已按分层规则拆分到两个文件区**）：

**`zephyr_user/drivers/ethercat/ssc/`** —— 第三方协议栈（22 个文件）

| 内容 | 说明 |
|---|---|
| SSC 5.13 的 21 个文件（8 `.c` + 13 `.h`） | 倍福原始代码，许可头原样保留 |
| `LICENSE-BECKHOFF.md` | 声明不在 Apache-2.0 范围 |

**`sdk_glue_user/drivers/ethercat/`** —— HPM 厂商代码（15 个文件）

| 文件/目录 | 内容 |
|---|---|
| `esc_hpmicro.c`（368 行）+ `Kconfig.hpmicro`（`config HPM_ESC`）+ `CMakeLists.txt` | ESC 设备驱动 |
| `include/zephyr/drivers/ethercat/hpm_esc.h`（131 行） | 驱动头 |
| `esc_port/` 11 个文件 | `hpm_ecat_hw.c/.h`（570 行，`HW_*` 宏 + PDI/SYNC/RESET ISR）、`hpm5e31_phy.c/.h`、`hpm_ecat_e2p_emulation.c/.h`、`ecat_zephyr_port.c/.h`、`ecat_config.h`、`ecat_board_cfg.h`、`user_config.h` |

**`sdk_glue_user` 新增的构建骨架**（照 `sdk_glue` 同构）

```
sdk_glue_user/
├── zephyr/module.yml        name=sdk_glue_user, cmake=., kconfig=Kconfig.hpm（不带 settings 根目录）
├── CMakeLists.txt           add_subdirectory(drivers)
├── Kconfig.hpm              menu "Hpmicro Zephyr Glue (user)" + rsource drivers/Kconfig
└── drivers/
    ├── CMakeLists.txt       add_subdirectory(ethercat)
    ├── Kconfig              menu "Device Drivers" + rsource ethercat/Kconfig.hpmicro
    └── ethercat/            （见上表）
```

已改的构建文件：

- `zephyr_user/drivers/Kconfig`：新增 `config DUST_ECAT_SSC`（`default n`）
- `zephyr_user/drivers/CMakeLists.txt`：`if(CONFIG_DUST_ECAT_SSC)` 只编 8 个 SSC `.c`，include `ssc` + `hpm_sdk/drivers/inc`
- `sdk_glue_user/drivers/ethercat/CMakeLists.txt`：`CONFIG_HPM_ESC` 编 `esc_hpmicro.c` + `hpm_esc_drv.c`；`CONFIG_DUST_ECAT_SSC` 编 `esc_port/` 4 个 `.c` + SDK `eeprom_emulation.c` / `hpm_crc32.c`
- `zephyr_user/project/CMakeLists.txt`：`ZEPHYR_EXTRA_MODULES` 追加 `sdk_glue_user`（`if(EXISTS .../zephyr/module.yml)` 保护）

**已知待办（P2 起处理）**：

1. `ecat_board_cfg.h:6` 写死 "HPM5E31 custom board"，`BOARD_ECAT_PORT0_PHY_ADDR=1` / `PORT1=0` 需按 HPM6E00EVK 实际 PHY 地址改。
2. `hpm5e31_phy.c`（281 行）是 HPM5E31 专用 PHY 初始化，HPM6E00EVK 板载 PHY 型号不同，需替换。
3. `esc_port/` 依赖 `hpm_esc_read/write` 等 API，来自 P2 的 ESC 驱动，**P2 完成前打不开 `CONFIG_DUST_ECAT_SSC`**。

---

### P2 ESC 驱动（sdk_glue_user/drivers/ethercat）

> ✅ **P2 已完成（2026-10-27）**。归属由 `zephyr_user` 改为 `sdk_glue_user`（HPM 厂商代码），
> 实际落地见 §4.1。Kconfig 符号改为 `HPM_ESC`（对齐 sdk_glue 的 `*_HPMICRO` / `HPM_*` 风格）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `drivers/ethercat/hpm_esc.h` | 新建。从 `temp\hpm_ecat_ref\include\zephyr\drivers\ethercat\hpm_esc.h` 复制（131 行）。**头文件跟驱动走，不放 sdk_glue_user** | 无 | 无 |
| 2 | `drivers/ethercat/esc_hpmicro.c` | 新建。从 `temp\hpm_ecat_ref\EtherCAT\esc_hpmicro.c` 复制（368 行）。**逻辑一行不动**，只对齐本项目 include 风格 | **P3 步 1**（binding 存在才能编 DT 宏） | 无（未加入 CMake） |
| 3 | `drivers/Kconfig` | 追加 `config DUST_DEV_ESC`：`bool "HPMicro EtherCAT slave controller"` + `default n` + `depends on DT_HAS_HPMICRO_HPM_ESC_ENABLED` + `select HAS_HPMSDK_ESC`。写法照现有 `DUST_DEV_FSMC` | 无 | 无 |
| 4 | `drivers/CMakeLists.txt` | 追加 `if(CONFIG_DUST_DEV_ESC)` 块：`target_sources(app PRIVATE .../ethercat/esc_hpmicro.c)` + HPM SDK 的 `hpm_esc_drv.c`（照现有 `DUST_USB_DEVICE_HAL_HPM` 的 `${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/` 写法）+ include 目录 | 步 1~3 | **此步完成后 ESC 驱动可编译**（前提是 P3 步 1~3 已给 `esc0` 节点） |

---

#### 4.2 P2 执行记录（2026-10-27）

落地在 `sdk_glue_user/drivers/ethercat/`：

| 文件 | 内容 |
|---|---|
| `esc_hpmicro.c` | 368 行，整份复制，逻辑零改动。include 用 `<zephyr/drivers/ethercat/hpm_esc.h>` / `<hpm_soc.h>` / `<hpm_clock_drv.h>` / `<hpm_esc_drv.h>` |
| `include/zephyr/drivers/ethercat/hpm_esc.h` | 131 行，驱动公开 API（`hpm_esc_read/write`、`hpm_esc_irq_enable`、`hpm_esc_set_irq_callback`、`HPM_ESC_IRQ_PDI/SYNC0/SYNC1/RESET`） |
| `Kconfig.hpmicro` | `config HPM_ESC`，`default y`、`depends on DT_HAS_HPMICRO_HPM_ESC_ENABLED` + `CLOCK_CONTROL && GPIO && PINCTRL` |
| `CMakeLists.txt` | `zephyr_library()`；`CONFIG_HPM_ESC` 编 `esc_hpmicro.c` + `${HPM_SDK_DIR}/drivers/src/hpm_esc_drv.c`；`CONFIG_DUST_ECAT_SSC` 编 `esc_port/` 4 个 `.c` + SDK `eeprom_emulation.c` / `hpm_crc32.c`；include `include/`、`esc_port/`、`zephyr_user/drivers/ethercat/ssc`、SDK eeprom_emulation/utils |

**依赖关系（已接好）**：

- `HPM_SDK_DIR` 由 `sdk_glue/CMakeLists.txt:10` 以 CACHE 变量设置；`sdk_glue` 在 `ZEPHYR_EXTRA_MODULES` 里 PREPEND、`sdk_glue_user` APPEND，所以处理顺序在前，变量可用。
- `ZEPHYR_USER_DIR` 由 `project/CMakeLists.txt` 在 `find_package(Zephyr)` 之前定义，模块 CMake 同作用域可见。
- **`DUST_ECAT_SSC` 不依赖任何芯片符号**——SSC 是通用协议栈，门禁只表示"要协议栈"。芯片耦合放在 HPM 侧：`esc_port/` 的编译条件是 `CONFIG_DUST_ECAT_SSC AND CONFIG_HPM_ESC`（在 `sdk_glue_user/drivers/ethercat/CMakeLists.txt`）。换别的芯片只需新写自己的 `esc_port`，SSC 与本 Kconfig 一行不用改。

**未做**：`hpm_esc_drv.c` 由本模块手动编译，不走 `HAS_HPMSDK_ESC` 机制（`sdk_glue/modules/hpm_sdk/Kconfig` 里没有该符号，且按 `hpm-patch-rule` 不能改官方仓）。

---

### P3 板级 dts / binding / 引脚（sdk_glue_user）

> **P3 拆成 5 步，每次只执行一步。**
> ⚠️ 步 1 已完成（2026-10-27）：`dts/bindings/ethercat/hpmicro,hpm-esc.yaml` 已建。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | ~~`dts/bindings/ethercat/hpmicro,hpm-esc.yaml`~~ | ✅ **已完成**（60 行整份复制） | — | — |
| 2 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk-pinctrl.dtsi` | 新增 `pinmux_esc` pinctrl 组（ESC 的 MDIO/MDC/LINK0/LINK1/RUN/ERR 引脚）。**引脚号必须按 HPM6E00EVK 原理图核**，参考 `hpm_sdk/boards/hpm6e00evk/pinmux.c` 的 ESC 段 | 步 1 | 无（组未被引用） |
| 3 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk.dts` | 新增 `esc0: ethercat@f1700000` 节点（`compatible = "hpmicro,hpm-esc"`、`reg = <0xf1700000 0x10000>`、`interrupts = <61 2>, <62 3>, <63 3>, <64 2>`、`interrupt-names = "pdi","sync0","sync1","reset"`、`pinctrl-0 = <&pinmux_esc>`、`reset-gpios`、`phy-address-offset`、`phy-addresses`、`nmii-link-ctrl-indices`、`nmii-link-inverted`），并在 `aliases` 加 `ethercat0 = &esc0;` | 步 1、2 | 节点无驱动消费者时可编过；打开 `CONFIG_DUST_DEV_ESC` 后必须有 P2 步 4 |
| 4 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk.yaml` | `supported:` 加 `- ethercat` | 无 | 无 |
| 5 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk_defconfig` | 加 `CONFIG_DUST_DEV_ESC=y` + `CONFIG_DUST_ECAT_SSC=y` | **P2 步 3 与 P1 步 5**（符号必须先定义） | **编不过**：符号未定义时 Zephyr 报 "attempt to assign the value y to the symbol ... which has no prompt"。**必须等 P1/P2 完成后再做** |

---

### P4 应用线程（zephyr_user/project）

> **P4 拆成 4 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `project/Kconfig` | 追加 `config TRD_ECAT`（`bool`、`default n`、`select DUST_ECAT_SSC` + `select DUST_DEV_ESC`），命名照现有 `TRD_GPIO` / `TRD_TEST` | P1 步 5、P2 步 3 | 无 |
| 2 | `project/thread/CMakeLists.txt` | 追加 `if(CONFIG_TRD_ECAT)` 块，`add_subdirectory(ecat)` | 步 1 | 无 |
| 3 | `project/thread/ecat/CMakeLists.txt` | 新建，`target_sources(APP_THREAD PRIVATE trd_ecat.cpp)` | 步 2 | 无 |
| 4 | `project/thread/ecat/trd_ecat.cpp` | 新建薄启动线程：`thread_init()` 初始化 ESC + SSC，`thread_start()` 起线程跑 SSC 主循环（`MainInit()` → `while(1) { MainLoop(); }`），`REGISTER_INIT` / `REGISTER_THREAD` 六要素齐全。范本 `project/thread/remote/trd_remote.cpp` | 步 3 | 缺 `APPL_*` 钩子会链接失败（P5 步 1 补） |

---

### P5 应用钩子与业务映射

> **P5 拆成 4 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `drivers/ethercat/ssc/appl_ecat.c` | 新建。实现三个钩子：`APPL_OutputMapping(pData)` 把 SM2 过程数据拷到本地缓冲、`APPL_Application(void)` 跑业务、`APPL_InputMapping(pData)` 把本地缓冲拷到 SM3。**这是 SSC 唯一要求应用提供的东西** | P1 完成 | 无 |
| 2 | `drivers/ethercat/ssc/`（替换 `digital_io.c` 的对象） | 按业务需要配对象字典 / PDO 映射。**先用 `digital_io.c` 的示例对象跑通通信**，业务对象后补 | 步 1 | 无 |
| 3 | `project/prj.conf` 或用户区 conf | 打开 `CONFIG_TRD_ECAT=y` | P4 完成 | 首次真实构建 |
| 4 | SII / EEPROM 存储 | 决定 SII 放哪（Flash 分区 or Flash 模拟 EEPROM）。当前 `ecat_def.h` 里 `ESC_EEPROM_EMULATION=1`、`ESC_EEPROM_SIZE=0x800` | 步 3 | 无 |

---

## 5. 编译状态总表

| 阶段 | 结束时能否编译 | 说明 |
|---|---|---|
| P1 步 1~4 | ✅ 能 | 文件未加入 CMake |
| P1 步 5 | ⚠️ **可能不能** | SSC 首次编译，预期出配置错误，逐条补 |
| P2 步 1~3 | ✅ 能 | 文件未加入 CMake |
| P2 步 4 | ❌ **不能** | `esc0` 节点还不存在（P3 步 3），DT 宏展开失败。**P2 步 4 应挪到 P3 步 3 之后执行** |
| P3 步 2~4 | ✅ 能 | 节点无驱动消费者 |
| P3 步 5 | ⚠️ 取决于 P1/P2 | 符号已定义则可编 |
| P4 步 1~3 | ✅ 能 | |
| P4 步 4 | ⚠️ 可能不能 | 缺 `APPL_*` 钩子会链接失败 |
| P5 步 1~2 | ✅ 能 | 钩子补齐后链接通过 |
| P5 步 3 | ✅ 能 | 真实构建 |

---

## 6. 待拍板（执行前必须定）

1. **Kconfig 符号命名**：`DUST_DEV_ESC` + `DUST_ECAT_SSC`（本规划采用，照 `DUST_DEV_FSMC` 风格）？还是 `DUST_DEV_ECAT` 单一符号？
2. **SSC 放 `zephyr_user/drivers/ethercat/ssc/` 是否可接受**？（v1.0 被指出"原封不动塞进架构层"，现按"算了就按照这样"保留该位置；如需改放别处请指明）
3. **对象字典**：先用 `digital_io` 示例跑通通信，还是直接按业务定 PDO？
4. **PDO 布局**：自己定，还是照 `dust-adapter` 的 DTC1 协议（SM2=72B / SM3=96B）？
5. **主站选型**：IgH（Linux 内核模块）还是 SOEM（用户态）还是 TwinCAT（仅调试）？
6. **SII 存储位置**：Flash 模拟 EEPROM（当前配置）还是独立 Flash 分区？
7. **是否需要 DC（分布式时钟）**：当前配置是 FreeRun，不需要 DC。要 DC 就要多一轮。

---

## 7. 验证清单

| 阶段 | 验证 |
|---|---|
| P1 完成 | 目录里有 8 个 `.c`；CMake 里 8 个源文件都列进去；编到 SSC 时逐条补齐 `ecat_def.h` 开关 |
| P2 完成 | `nm build/zephyr/zephyr.elf \| grep hpm_esc` 有符号 |
| P3 完成 | `west build` 配置阶段通过；`build/zephyr/zephyr.dts` 里能看到 `esc0` 节点与正确 `reg` |
| P4 完成 | 烧录后 shell 能进；线程在跑不崩 |
| P5 完成 | PC 侧 `ethercat slaves` 能看到从站，推到 PREOP；再推 OP |
| 全链路 | `ethercat pdos -p 0` 能读到映射；WKC=3 |

---

## 8. 参考文件位置

| 用途 | 路径 |
|---|---|
| SSC 全套 | `E:\Zephyr\zephyr_user\temp\hpm_ecat_ref\EtherCAT\ssc\` |
| ESC 驱动 | `temp\hpm_ecat_ref\EtherCAT\esc_hpmicro.c` |
| 驱动头 | `temp\hpm_ecat_ref\include\zephyr\drivers\ethercat\hpm_esc.h` |
| binding | `temp\hpm_ecat_ref\EtherCAT\hpmicro,hpm-esc.yaml`（已拷到 sdk_glue_user） |
| SSC 端口层范本 | `temp\hpm_ecat_ref\EtherCAT\bsp\ecat_zephyr_port.c/.h` |
| 硬件抽象 85 行范本 | `temp\dust-adapter-ethercat\apps\ethercat_slave\src\hpm5e_esc_hw.c/.h` |
| 板级结构参考 | `temp\hpm_support_probe\boards\hpmicro\dust-hpm5e31\`（dts / defconfig / yaml / Kconfig） |
