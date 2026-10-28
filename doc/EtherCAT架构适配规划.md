/**
 * @file EtherCAT架构适配规划
 * @author qingyu
 * @brief 把 EtherCAT 从站适配进 framework 架构（C++ 驱动层 + 业务线程 + 板级）
 * @version 0.1
 * @date 2026-10-28
 *
 * @copyright Copyright (c) 2026
 *
 */

# EtherCAT 从站架构适配规划

> 前置：`doc/EtherCAT从站协议栈融入规划.md` 已完成 P1/P2（协议栈与 ESC 驱动已落地）。
> 本文解决的是**怎么把它套进现有架构**，不是怎么拿到协议栈。
>
> 目标形态：业务代码只写 PDO 字段映射，不碰 C 签名、不碰 `HW_*` 宏、不碰协议栈。

---

## 1. 现状盘点（实测）

### 1.1 已落地的文件

| 位置 | 内容 | 归属判断 |
|---|---|---|
| `zephyr_user/drivers/ethercat/ssc/` | 倍福 SSC 5.13，22 文件，已中性化（`esc_hw.h` 契约、`<stdint.h>`） | ⚠️ 位置待定，见 §2 |
| `sdk_glue_user/drivers/ethercat/esc_hpmicro.c` + `include/.../hpm_esc.h` | HPM ESC 设备驱动 368 + 131 行 | ✅ 正确（HPM 厂商层） |
| `sdk_glue_user/drivers/ethercat/esc_port/` | `esc_hw.h`、`hpm_ecat_hw.c/.h`、`e2p_emulation`、`ecat_zephyr_port`、`ecat_config.h`、`user_config.h` | ✅ 正确（HPM 通用端口层） |
| `sdk_glue_user/drivers/ethercat/esc_port/hpm5e31/` | `hpm5e31_phy.c/.h`、`ecat_board_cfg.h` | ⚠️ HPM5E31 专用，6E00 要另写 |
| `sdk_glue_user/dts/bindings/ethercat/hpmicro,hpm-esc.yaml` | binding 60 行 | ✅ |

### 1.2 现有架构的同类先例（照它抄）

| 层 | 先例 | 形态 |
|---|---|---|
| 驱动封装 | `framework/drivers/communication/can/can.hpp` + `can.cpp` | C++ 类，`Init(dev, cfg)` + `Send()` + `SetRxCallback(std::function)`，文件头带完整使用说明 |
| Kconfig 门禁 | `framework/drivers/Kconfig` 的 `DUST_COM_CAN` → `select CAN` | 一个 `DUST_COM_*` 一个驱动 |
| CMake 装配 | `framework/drivers/CMakeLists.txt` 的 `if(CONFIG_DUST_COM_CAN)` | `target_sources(app PRIVATE ...)` + `target_include_directories(app PRIVATE ...)` |
| 业务线程 | `project/thread/remote/trd_remote.cpp` | 框架类带 `Task()`，子树只写薄启动 |
| 注册 | `REGISTER_INIT(..., EarlyInit, High, HaltOnFail, "x_init")` + `REGISTER_THREAD(...)` | 六要素齐全 |

---

## 2. 目标分层

### 2.1 分层图

```
┌─ 业务层 ────────────────────────────────────────────────┐
│ project/thread/ecat/trd_ecat.cpp                        │
│   - 薄启动（REGISTER_INIT / REGISTER_THREAD）           │
│   - 只写 PDO 字段映射：底盘状态 ↔ SM3，指令 ↔ SM2       │
└────────────────┬────────────────────────────────────────┘
                 │ 注册 std::function（不写 C 签名）
┌────────────────▼────────────────────────────────────────┐
│ framework/drivers/communication/ethercat/               │
│   ethercat.hpp / ethercat.cpp                           │
│   class EthercatSlave {                                 │
│     Init(); Run();                                      │
│     SetRxHandler(fn);      // 主站→从站（SM2 到达）      │
│     SetTxProvider(fn);     // 从站→主站（SM3 发出前）    │
│     SetCycleHandler(fn);   // 每周期业务                │
│   }                                                     │
│   ethercat.cpp 里同时实现 3 个 extern "C" APPL_* 钩子，  │
│   只做转发 → 业务永远看不到 C 签名                      │
│   ssc/  ← 倍福 SSC 5.13（第三方，独立许可）             │
└────────────────┬────────────────────────────────────────┘
                 │ esc_hw.h 契约（中性名）
┌────────────────▼────────────────────────────────────────┐
│ sdk_glue_user/drivers/ethercat/                         │
│   esc_hpmicro.c + hpm_esc.h   ← ESC 设备驱动            │
│   esc_port/                   ← HW_* 宏 + ISR + SII     │
│   esc_port/hpm6e00evk/        ← 板卡 PHY/地址（新写）   │
└─────────────────────────────────────────────────────────┘
```

### 2.2 三条硬规则

1. **反向依赖禁止**：`framework/` 不知道 `sdk_glue_user/` 的存在。两者只通过 `esc_hw.h` + `hpm_esc.h` 这两个**头文件名**耦合，链接期合拢。
2. **业务不碰 C**：`APPL_*` 三个钩子只在 `framework/.../ethercat.cpp` 里出现一次，业务侧只注册 `std::function`。
3. **芯片代码只在 `sdk_glue_user`**：`hpm-patch-rule` 规定不动官方 `sdk_glue`，厂商代码一律落用户覆盖层。

### 2.3 关于 SSC 位置的待拍板

| 方案 | 位置 | 优点 | 缺点 |
|---|---|---|---|
| **A（推荐）** | `framework/drivers/communication/ethercat/ssc/` | 与 C++ 封装同层，include 路径自然，不跨子模块 | 第三方代码进架构层子模块，需在 framework README 声明许可 |
| B | 留在 `zephyr_user/drivers/ethercat/ssc/` | 架构层保持纯净 | framework 的 `.cpp` 要 include 到 `zephyr_user/drivers/...`，跨模块路径耦合 |

> 本文按 **A** 写步骤；若拍板 B，P1 整段作废，改为在 P2 的 CMake 里加 include 路径。

---

## 3. 阶段划分

六个阶段，共 19 步。**每次只执行一步。**

> 用户说"开始第 N 步" = 只改表格里那一个文件（或明确列出的那一组），改完停下报告，等"继续"。

---

### P1 SSC 迁入 framework（5 步）

> **P1 拆成 5 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `framework/drivers/communication/ethercat/ssc/` | 从 `zephyr_user/drivers/ethercat/ssc/` 整目录搬入（`git mv` 语义，保留 `.c`/`.h` 全部 22 文件 + `LICENSE-BECKHOFF.md`） | 无 | **编不过**：`zephyr_user/drivers/CMakeLists.txt` 里 8 个 `ssc/*.c` 路径失效 |
| 2 | `zephyr_user/drivers/CMakeLists.txt` | 删除 `if(CONFIG_DUST_ECAT_SSC)` 整块（SSC 不再由该模块编译） | 步 1 | 恢复可编 |
| 3 | `zephyr_user/drivers/Kconfig` | 删除 `config DUST_ECAT_SSC`（迁到 framework 的 Kconfig） | 步 2 | **编不过**：任何 `select DUST_ECAT_SSC` 会报未定义（当前无引用者，实际可编） |
| 4 | `zephyr_user/drivers/ethercat/` | 删除空目录 | 步 3 | 无 |
| 5 | `framework/drivers/communication/ethercat/ssc/LICENSE-BECKHOFF.md` 头部补一句 | 追加"本目录随 framework 子模块分发，版权与许可按 Beckhoff 协议，不受本项目 Apache-2.0 覆盖" | 步 1 | 无 |

**P1 结束状态**：SSC 已在 framework 内，尚未参与编译（等 P2）。工程**可编过**。

---

### P2 framework C++ 封装（5 步）

> **P2 拆成 5 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `framework/drivers/communication/ethercat/ethercat.hpp` | 新建。声明 `class EthercatSlave`：`Init()`、`Run()`、`SetRxHandler(std::function<void(RxPdo&)>)`、`SetTxProvider(std::function<void(TxPdo&)>)`、`SetCycleHandler(std::function<void()>)`。文件头照 `can.hpp` 写完整使用说明（dts 片段 / Kconfig / 初始化 / 用法示例）。**不声明任何 `APPL_*`、不 include 任何 `hpm_*`** | P1 完成 | 无（未加入 CMake） |
| 2 | `framework/drivers/communication/ethercat/ethercat.cpp` | 新建。实现 `EthercatSlave`（单例 `Instance()`）+ **3 个 `extern "C"` 钩子**（`APPL_OutputMapping` / `APPL_Application` / `APPL_InputMapping`）只做转发到已注册的 `std::function` + 初始化序列（取 `esc0` 设备、注册 IRQ、`MainInit()`）；`Run()` 内 `while(true) MainLoop()` | 步 1 | 无（未加入 CMake） |
| 3 | `framework/drivers/Kconfig` | 追加 `config DUST_COM_ETHERCAT`：`default n`、`bool "EtherCAT slave (Beckhoff SSC 5.13)"`、`help` 说明"协议栈为第三方、许可见 ssc/LICENSE-BECKHOFF.md，端口层由各芯片提供"。**不 select 任何厂商符号** | 无 | 无 |
| 4 | `framework/drivers/CMakeLists.txt` | 追加 `if(CONFIG_DUST_COM_ETHERCAT)` 块：编 `communication/ethercat/ethercat.cpp` + 8 个 `communication/ethercat/ssc/*.c`；include 加 `communication/ethercat`、`communication/ethercat/ssc` | 步 2、3 | **编不过**：SSC 要 `esc_hw.h`，此时 HPM 侧没有对应 include（P4 前） |
| 5 | `framework/README.md` / `drivers/README.md` | 补 EtherCAT 一节：分层、许可、如何接新芯片（只写新的 `esc_hw.h` + 端口层） | 步 4 | 无 |

**P2 结束状态**：framework 侧完整，但 `CONFIG_DUST_COM_ETHERCAT` 还开不起来（缺 HPM 端口层对接 + dts 节点）。

---

### P3 板级 dts（sdk_glue_user，4 步）

> **P3 拆成 4 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk-pinctrl.dtsi` | 新增 `pinmux_esc` 组。引脚按 `hpm_sdk/boards/hpm6e00evk/pinmux.c:695` `init_esc_pins()` 逐条搬：PA09 REFCK、PA30 MDIO、PA31 MDC、PA10(复位 GPIO)、PA15 CTR_3(LINK0)、PA11 CTR_0(LINK1)、PE02 CTR_6(ERR)、PE03 CTR_1(RUN)、PE06 EVTO_0(SYNC0)、PORTA 13 根(PA16~PA29)、PORTB 13 根(PB12~PB23) | 无 | 无（组未被引用） |
| 2 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk.dts` | 新增 `esc0: ethercat@f1700000`：`compatible = "hpmicro,hpm-esc"`、`reg = <0xf1700000 0x10000>`、`interrupts`（4 路 PDI/SYNC0/SYNC1/RESET，**中断号按 HPM6E00 SOC dtsi 核，不许照抄 5E31 的 61/62/63/64**）、`interrupt-names`、`clocks`、`pinctrl-0 = <&pinmux_esc>`、`phy-reset-gpios`、`phy-addr-offset`、`link0/1-ctrl-index`、`run-error-leds` + `aliases { ethercat0 = &esc0; }` | 步 1 | **编不过**：驱动 `esc_hpmicro.c` 未必匹配 6E00 的寄存器差异，P4 处理 |
| 3 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk.yaml` | `supported:` 加 `- ethercat` | 无 | 无 |
| 4 | `boards/hpmicro/uhpm6e00evk/uhpm6e00evk_defconfig` | 加 `CONFIG_HPM_ESC=y`（ESC 驱动） | P2 步 3（`DUST_COM_ETHERCAT` 已定义） | 无 |

---

### P4 HPM 端口层对齐 6E00（3 步）

> **P4 拆成 3 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `sdk_glue_user/drivers/ethercat/CMakeLists.txt` | 把 `CONFIG_DUST_ECAT_SSC` 改成 `CONFIG_DUST_COM_ETHERCAT`（跟随 framework 的门禁名）；include 路径里 `ZEPHYR_USER_DIR/drivers/ethercat/ssc` 改成 `FW_ROOT/drivers/communication/ethercat/ssc` | P2 步 4 | 无 |
| 2 | `sdk_glue_user/drivers/ethercat/esc_port/hpm6e00evk/` | 新建目录，放 `hpm6e00evk_phy.c/.h`（PHY 初始化，型号按 HPM6E00EVK 原理图）+ `ecat_board_cfg.h`（`BOARD_ECAT_PHY_ADDR_OFFSET` / `PORT0_PHY_ADDR` / `PORT1_PHY_ADDR` / NMII link ctrl index）。**HPM5E31 那套原样保留不动** | P3 步 2 | **编不过**：`esc_port/hpm_ecat_hw.h` 仍 include `hpm5e31_phy.h` |
| 3 | `sdk_glue_user/drivers/ethercat/esc_port/hpm_ecat_hw.h` + `CMakeLists.txt` | 把 `#include "hpm5e31_phy.h"` 改为中性契约 `#include "esc_phy.h"`；`hpm5e31/` 和 `hpm6e00evk/` 各提供一个 `esc_phy.h`；CMake 按板卡门禁（`CONFIG_BOARD_*` 或 Kconfig）二选一加入 include 路径与源码 | 步 2 | 恢复可编 |

---

### P5 业务线程（4 步）

> **P5 拆成 4 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | `project/Kconfig` | 追加 `config TRD_ECAT`：`bool`、`default n`、`select DUST_COM_ETHERCAT`、`select HPM_ESC`。命名照 `TRD_GPIO` / `TRD_TEST` | P4 步 3 | 无 |
| 2 | `project/thread/CMakeLists.txt` | 追加 `if(CONFIG_TRD_ECAT)` → `add_subdirectory(ecat)` | 步 1 | 无 |
| 3 | `project/thread/ecat/CMakeLists.txt` | 新建，`target_sources(APP_THREAD PRIVATE trd_ecat.cpp)` | 步 2 | 无 |
| 4 | `project/thread/ecat/trd_ecat.cpp` | 新建薄启动（照 `trd_remote.cpp`）：`thread_init()` 里 `EthercatSlave::Instance().Init()` + 注册 `SetRxHandler` / `SetTxProvider` / `SetCycleHandler`（**只写 PDO 字段映射**）；`thread_start()` 起线程 `Run()`；`REGISTER_INIT` + `REGISTER_THREAD` 六要素齐全 | 步 3 | 无 |

---

### P6 对象字典与验收（3 步）

> **P6 拆成 3 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---:|---|---|---|---|
| 1 | 对象字典 / PDO 映射定义 | 沿用 SSC 的 `objdef.c` + `digital_io` 示例对象，先跑通通信；业务对象待 §4 拍板后另起一步 | P5 完成 | 无 |
| 2 | 板级/项目 conf | 打开 `CONFIG_TRD_ECAT=y`，首次真实构建 | 步 1 | 首次全链路编译，预期逐条补错 |
| 3 | 上板验收 | `ethercat slaves` 看到从站 → PREOP → SAFEOP → OP；`ethercat pdos -p 0` 读到映射；WKC 稳定 | 步 2 | 无 |

---

## 4. 待拍板（执行前必须定）

1. **SSC 放 framework（方案 A）还是留 zephyr_user（方案 B）** —— 本文按 A 写。
2. **Kconfig 命名**：`DUST_COM_ETHERCAT`（照 `DUST_COM_CAN` / `DUST_COM_SPI`）是否认可？
3. **PDO 布局**：自定，还是照 `dust-adapter` 的 DTC1（SM2=72B / SM3=96B）？
4. **对象字典范围**：先 `digital_io` 示例，还是直接上业务对象（底盘状态 / 电机反馈 / IMU）？
5. **主站**：IgH（Linux 内核模块）/ SOEM（用户态）/ TwinCAT（仅调试）？
6. **SII 存储**：Flash 模拟 EEPROM（当前 `ESC_EEPROM_EMULATION=1`，2KB）还是独立 Flash 分区？
7. **DC（分布式时钟）**：首版 FreeRun 不做 DC，是否认可？
8. **EtherCAT 中断是否走 ISR 直接处理**：还是像 `Can` 那样中断里只投递、业务线程处理？

---

## 5. 编译状态总表

| 阶段 | 结束时 | 说明 |
|---|---|---|
| P1 步 1 | ❌ 编不过 | ssc 路径失效 |
| P1 步 2~5 | ✅ 能 | SSC 未参与编译 |
| P2 步 1~3 | ✅ 能 | 未加入 CMake |
| P2 步 4~5 | ❌ 编不过 | SSC 缺 `esc_hw.h` |
| P3 步 1~3 | ❌ 编不过 | 承上 |
| P3 步 4 | ❌ 编不过 | 仍需 P4 |
| P4 步 1~2 | ❌ 编不过 | `hpm5e31_phy.h` 硬引用 |
| P4 步 3 | ✅ 能 | 链路接通，可编译 |
| P5 步 1~3 | ✅ 能 | |
| P5 步 4 | ✅ 能 | 钩子已由 framework 提供 |
| P6 | ⚠️ 视配置 | 首次全链路构建 |

---

## 6. 与已有文件的关系

| 已有 | 处理方式 |
|---|---|
| `zephyr_user/drivers/ethercat/ssc/` | P1 迁走，目录删除 |
| `zephyr_user/drivers/Kconfig` 的 `DUST_ECAT_SSC` | P1 步 3 删除 |
| `zephyr_user/drivers/CMakeLists.txt` 的 EtherCAT 块 | P1 步 2 删除 |
| `sdk_glue_user/drivers/ethercat/esc_hpmicro.c` / `hpm_esc.h` / `esc_port/` | 保留，P4 调整 include 与门禁名 |
| `sdk_glue_user/drivers/ethercat/esc_port/hpm5e31/` | **原样保留不动**，6E00 另起目录 |
| `sdk_glue_user/dts/bindings/ethercat/hpmicro,hpm-esc.yaml` | 保留 |
| `project/CMakeLists.txt` 的 `sdk_glue_user` 模块挂载 | 保留 |

---

## 7. 参考文件

| 用途 | 路径 |
|---|---|
| 驱动封装范本 | `framework/drivers/communication/can/can.hpp` / `can.cpp` |
| Kconfig / CMake 范本 | `framework/drivers/Kconfig`、`framework/drivers/CMakeLists.txt` |
| 薄启动线程范本 | `project/thread/remote/trd_remote.cpp` |
| SSC 源码 | `zephyr_user/drivers/ethercat/ssc/` |
| HPM 端口层范本 | `sdk_glue_user/drivers/ethercat/esc_port/` |
| 6E00EVK ESC 引脚全表 | `sdk_env/hpm_sdk/boards/hpm6e00evk/pinmux.c:695` `init_esc_pins()` |
| binding 参考拓扑 | `sdk_glue_user/drivers/ethercat/../README.md`（`esc0: ethercat@f1700000` 示例） |
