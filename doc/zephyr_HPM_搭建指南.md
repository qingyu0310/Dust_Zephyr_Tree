# Zephyr HPM SDK 搭建指南（续篇）

> 承接《[zephyr_子树架构搭建指南.md](./zephyr_子树架构搭建指南.md)》。前一篇把一台干净电脑搭成了**纯 zephyr 子树**
> 环境（STM32 可编译）。本篇在此基础上**添加 HPM SDK**，目标：`hpm5361icb` 板能编译出可烧录固件。
>
> **本文档的核心组织原则**：HPM 搭建分两层——先把**编译必需链**做完、让 `west build -b hpm5361icb` 跑绿；
> **底层的运行时 bug 修复放到最后**。因为**编译不检查底层逻辑是否正确**——底层驱动/中断/内存换算的 bug
> 只在固件跑起来才暴露，完全不阻塞编译链接出镜像。
>
> 姊妹篇：《zephyr_hpm_重建指南.md》——含 HPM 全量 patch 的完整重建过程（含底层 bug 修复细节）。

---

## 目录

- [1. 本篇定位与原则](#1-本篇定位与原则)
- [2. 新增的仓库与目录](#2-新增的仓库与目录)
- [3. 拉取 HPM 官方资料与用户小树](#3-拉取-hpm-官方资料与用户小树)
- [4. 装配 HPM SoC（编译必需）](#4-装配-hpm-soc编译必需)
- [5. 应用工程接入 HPM（编译必需）](#5-应用工程接入-hpm编译必需)
- [6. 编译 hpm5361icb](#6-编译-hpm5361icb)
- [7. 底层 bug 修复（放到最后做）](#7-底层-bug-修复放到最后做)
- [8. 常见问题排查](#8-常见问题排查)

---

## 1. 本篇定位与原则

**接续点**：前一篇已经搭好的环境——
- `E:\Zephyr\zephyr`（官方 v4.3.0，west workspace 已建）
- `E:\Zephyr\zephyr-sdk-0.16.8`（SDK）
- `E:\Zephyr\zephyr_user`（用户子树，framework + platform/cmsis + project）
- `E:\Zephyr\projects\temp`（STM32 用户区，已验证编译）
- `E:\Zephyr\.venv`（west 环境，已挂 ZEPHYR_BASE / SDK 变量）

**本篇要加的东西**（HPM 平台专属）：

```text
E:\Zephyr_HPMicro\                  ← HPM 侧根
├── sdk_glue\                       ← HPM 官方 zephyr 适配层（soc/boards/drivers）
├── sdk_env\                        ← HPM SDK（hpm_sdk 底层库 + tools/openocd.exe）
└── sdk_glue_user\                  ← 用户 HPM 小树（HPM5300 SoC + hpm5361icb 板卡）
```

**两条线要分清：**

| 类别 | 说明 | 什么时候做 |
| --- | --- | --- |
| **编译必需链** | 不做 `west build` 根本过不去（Kconfig 报错、找不到板、编译错） | 第 4、5 章，先做 |
| **底层运行时 bug** | 编译能过、烧录也能烧，但外设跑起来行为不对（中断风暴、DMA 地址错） | 第 7 章，**最后**做 |

> 一句话：**先把固件编译链接出来，底层 bug 之后逐项补。** 编译不需要关心底层有没有 bug。

---

## 2. 新增的仓库与目录

前一篇的 E:\Zephyr 结构保持不动，HPM 的东西全放 `E:\Zephyr_HPMicro\`：

```text
E:\Zephyr_HPMicro\
├── sdk_glue\            ← 官方 hpmicro/zephyr_sdk_glue @1756cb8
│     ├── soc\           ← HPM soc 支持（装配点，见第 4 章）
│     ├── boards\        ← HPM 板卡（官方，hpm5361icb 在用户小树）
│     ├── drivers\       ← HPM 外设驱动
│     └── zephyr\module.yml
├── sdk_env\             ← 官方 hpmicro/sdk_env v1.11.0
│     ├── hpm_sdk\       ← HPM 底层 SDK（启动/链接/驱动库）
│     └── tools\         ← openocd.exe（烧录用）
└── sdk_glue_user\       ← 用户小树（独立 git：Dust_Zephyr_HPMicro_Tree）
      ├── soc\hpmicro\HPM5300\    ← HPM5300 SoC 实现（Kconfig/CMake/soc.c/linker.ld）
      ├── boards\hpmicro\hpm5361icb\   ← hpm5361icb 板卡全套
      └── boards\openocd\         ← cmsis-dap 探针配置
```

---

## 3. 拉取 HPM 官方资料与用户小树

```powershell
cd E:\Zephyr_HPMicro

# HPM 官方 zephyr 适配层
git clone https://github.com/hpmicro/zephyr_sdk_glue sdk_glue
git -C sdk_glue checkout 1756cb8

# HPM SDK（底层库 + 烧录工具）
git clone --depth 1 --branch v1.11.0 https://github.com/hpmicro/sdk_env sdk_env

# 用户 HPM 小树（SSH，见上一篇 §5.1 的 SSH 配置）
git clone git@github-qingyu0310:qingyu0310/Dust_Zephyr_HPMicro_Tree.git sdk_glue_user
```

> `sdk_glue_user` 里已经带了 hpm5361icb 板卡、HPM5300 SoC 小树、PLLv2 binding、cmsis-dap 配置，
> 克隆即用，**不需要** apply 板卡 patch。

---

## 4. 装配 HPM SoC（编译必需）

Zephyr 的 SoC 通过 vendor 级 `sdk_glue/soc/hpmicro/Kconfig` 的 `rsource` 加载各 family，**只认 sdk_glue
内部的目录**。用户 HPM5300 在 `sdk_glue_user` 小树里，必须显式把它 source 进来。共 **4 处 sdk_glue 修改
+ 1 处小树内修改 + 1 处 dtsi**。

### 4.1 `sdk_glue/soc/hpmicro/Kconfig`

在 `rsource "${ZEPHYR_SDK_GLUE_MODULE_DIR}/soc/hpmicro/*/Kconfig"` 之后追加：

```kconfig
# SELF-MAINTAINED — 用户自研 HPM5300 SoC 支持（sdk_glue_user 小树）
rsource "${ZEPHYR_SDK_GLUE_MODULE_DIR}/../sdk_glue_user/soc/hpmicro/HPM5300/Kconfig"
```

### 4.2 `sdk_glue/soc/hpmicro/Kconfig.soc`

在 `source "${ZEPHYR_SDK_GLUE_MODULE_DIR}/soc/hpmicro/*/Kconfig.soc"` 之后追加：

```kconfig
# SELF-MAINTAINED — 用户自研 HPM5300 SoC 支持（sdk_glue_user 小树）
source "${ZEPHYR_SDK_GLUE_MODULE_DIR}/../sdk_glue_user/soc/hpmicro/HPM5300/Kconfig.soc"
```

### 4.3 `sdk_glue/soc/hpmicro/Kconfig.defconfig`

在 `rsource "${ZEPHYR_SDK_GLUE_MODULE_DIR}/soc/hpmicro/*/Kconfig.defconfig.series"` 之后追加：

```kconfig
# SELF-MAINTAINED — 用户自研 HPM5300 SoC 支持（sdk_glue_user 小树）
rsource "${ZEPHYR_SDK_GLUE_MODULE_DIR}/../sdk_glue_user/soc/hpmicro/HPM5300/Kconfig.defconfig.series"
```

### 4.4 `sdk_glue/soc/hpmicro/CMakeLists.txt`

把 `add_subdirectory(${SOC_SERIES})` 改为条件分支（官方树没有 HPM5300 目录，必须指向小树，
且 out-of-tree 要显式 binary dir）：

```cmake
add_subdirectory(common)
if(SOC_SERIES STREQUAL "HPM5300")
	# SELF-MAINTAINED — 用户自研 HPM5300 SoC 支持（sdk_glue_user 小树）
	add_subdirectory(${ZEPHYR_SDK_GLUE_MODULE_DIR}/../sdk_glue_user/soc/hpmicro/HPM5300 ${CMAKE_CURRENT_BINARY_DIR}/HPM5300)
else()
	add_subdirectory(${SOC_SERIES})
endif()
```

### 4.5 `sdk_glue_user/soc/hpmicro/HPM5300/Kconfig.defconfig.series`（小树内）

这个文件最后一行 `source` 原本指向 sdk_glue 内部，**必须改为指向小树自身**（否则 source not found，
且 24MHz 时钟默认值进不了 Kconfig）：

```kconfig
source "${ZEPHYR_SDK_GLUE_MODULE_DIR}/../sdk_glue_user/soc/hpmicro/HPM5300/Kconfig.defconfig.HPM53*"
```

### 4.6 `sdk_glue/dts/riscv/hpmicro/hpm53xx.dtsi`

这个文件需要是**全量外设展开版**：osc24/32、pll0/pll1、gptmr、uart0-4、i2c、spi、mcan0-3、pwm、
hdma（dmav2）、gpio、xpi0、usb0，修正 `interrupts-extended` 中断号，clk 节点改
`compatible = "hpmicro,hpm-clock"` + `#clock-cells = <3>`。

> **文件来源（重要）**：官方 sdk_glue 仓库里的 `hpm53xx.dtsi` 是**原版**，缺全量外设节点；
> 全量展开版是**团队对官方文件的修改**，《重建指南》没有贴全文。**需要从团队已有环境复制**这个
> 文件覆盖（本机路径 `E:\Zephyr_HPMicro\sdk_glue\dts\riscv\hpmicro\hpm53xx.dtsi`），或找团队
> 要这份文件。没有它，`hpm5361icb.dts` include 时缺节点，dtc 直接解析失败。

**为什么这 4.1–4.6 是编译必需**：少任何一处都会在 Kconfig/CMake/dtc 阶段直接报错——
`HPM53_SINGLE_PRECISION_FPU undefined`、`add_subdirectory(HPM5300) not an existing directory`、
`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` 空、`source ... not found`。

---

## 5. 应用工程接入 HPM（编译必需）

### 5.1 应用 CMakeLists 挂 HPM 路径

用户区 = 一个独立 Zephyr 应用（结构见上一篇 §6 的 temp 示例）。HPM 用户区就是在 temp 结构基础上，
CMakeLists 换成下面这份（已含 `FW_ROOT`/`ZEPHYR_USER_DIR` 定义、HPM 搜索路径、板级 glob、架构层装配）。
**整份照抄**到 `E:\Zephyr\projects\<app>\CMakeLists.txt` 即可（参考已搭好的 `E:\Zephyr\projects\hpm5361\`）：

```cmake
cmake_minimum_required(VERSION 3.20.0)

# ---- (0) 框架与 zephyr_user 路径 -----------------------------------------
set(FW_ROOT         "${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr_user/framework")
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr_user")

# ---- (1) 业务门禁符号 -----------------------------------------------------
set(CONFIG_SYM PRJ_MAIN)

# ---- (2) SDK glue（HPM 平台）-----------------------------------------------
if(DEFINED ENV{SDK_GLUE_DIR})
  set(SDK_GLUE_DIR "$ENV{SDK_GLUE_DIR}")
else()
  set(SDK_GLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Zephyr_HPMicro/sdk_glue")
endif()
set(SDK_GLUE_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Zephyr_HPMicro/sdk_glue_user")

# ---- (3) BOARD_ROOT / SOC_ROOT / DTS_ROOT -----------------------------------
list(APPEND BOARD_ROOT "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}/dts")
if(EXISTS "${SDK_GLUE_DIR}")
  list(APPEND BOARD_ROOT "${SDK_GLUE_DIR}")
  list(APPEND SOC_ROOT   "${SDK_GLUE_DIR}")
  list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}")
  list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}/dts")
  list(APPEND BOARD_ROOT "${SDK_GLUE_USER_DIR}")
  list(APPEND SOC_ROOT   "${SDK_GLUE_USER_DIR}")
  list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}")
  list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}/dts")
endif()

# ---- (4) ZEPHYR_EXTRA_MODULES ----------------------------------------------
set(ZEPHYR_EXTRA_MODULES
  "${SDK_GLUE_DIR}"
  "${FW_ROOT}"
  "${ZEPHYR_USER_DIR}/platform/cmsis"
)
set(ZEPHYR_SDK_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr-sdk-0.16.8" CACHE PATH "")

# ---- (5) 板级 overlay / conf / board.cmake ---------------------------------
if(NOT DEFINED BOARD_CFG)
  set(BOARD_CFG ${BOARD})
endif()
file(GLOB OVERLAY_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.overlay)
if(OVERLAY_FILES)
  list(POP_FRONT OVERLAY_FILES DTC_OVERLAY_FILE)
endif()
file(GLOB PRJ_CONF_FILES ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.conf)
if(PRJ_CONF_FILES)
  list(POP_FRONT PRJ_CONF_FILES EXTRA_CONF_FILE)
endif()
file(GLOB BOARD_CMAKE   ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/board.cmake)
if(BOARD_CMAKE)
  include(${BOARD_CMAKE})
endif()

# ---- (6) 接入 Zephyr 构建系统 ---------------------------------------------
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hpm5361)

# ---- (7) 业务源码与系统入口 ------------------------------------------------
target_include_directories(app PRIVATE
  ${FW_ROOT}/init
  ${FW_ROOT}/cmd
)
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)

# ---- (8) 装配架构层各层（门禁打开时 add_subdirectory）----------------------
if(CONFIG_${CONFIG_SYM})
  add_subdirectory(${FW_ROOT}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
  add_subdirectory(${FW_ROOT}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
  add_subdirectory(${FW_ROOT}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
  add_subdirectory(${FW_ROOT}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
  add_subdirectory(${FW_ROOT}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
  add_subdirectory(${FW_ROOT}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)
  add_subdirectory(thread)
endif()
```

> `zephyr_user/project/CMakeLists.txt` 已经配好这套（可直接照抄）。注意 `sdk_glue_user` 不是 west
> 模块，只能靠 `BOARD_ROOT/SOC_ROOT/DTS_ROOT` 手动挂。
>
> **用户区其余文件从哪来**：
> - `boards\hpm\hpm5361icb\`（board.cmake / hpm5361icb.conf / hpm5361icb.overlay）——从
>   `zephyr_user\project\boards\hpm\hpm5361icb\` **复制**；
> - `thread\thread.hpp`——从 `zephyr_user\project\thread\` 复制；
> - `Kconfig` / `prj.conf` / `thread\` 业务线程——结构参考上一篇 §6 的 temp 示例
>   （hpm5361 最小工作区就是这么搭的，见 `E:\Zephyr\projects\hpm5361\`）。

### 5.2 环境变量

在 `E:\Zephyr\.venv\Scripts\Activate.ps1` 末尾追加（必须纯 ASCII）：

```powershell
$env:ZEPHYR_BASE                = "E:\Zephyr\zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR     = "E:\Zephyr\zephyr-sdk-0.16.8"
$env:ZEPHYR_TOOLCHAIN_VARIANT   = "zephyr"
$env:SDK_GLUE_DIR               = "E:\Zephyr_HPMicro\sdk_glue"
$env:ZEPHYR_SDK_GLUE_MODULE_DIR = "E:\Zephyr_HPMicro\sdk_glue"
```

> `ZEPHYR_SDK_GLUE_MODULE_DIR` 是第 4 章所有 `rsource` 里 `${ZEPHYR_SDK_GLUE_MODULE_DIR}` 的取值来源，
> **必须设**，否则 soc Kconfig 的 source 找不到。

### 5.3 应用级修复（HPM 相关）

> ⚠️ 这几条是《重建指南》旧结构（`modules/usb`）时代的，**当前架构已不需要**：
> - USB 协议栈已归位 `framework/drivers/communication/stream/usb/`（不再是 `modules/usb`），
>   当前架构里没有 `zephyr_user/modules/usb/CMakeLists.txt` 这个文件；
> - `UsbRxQueue` 用的 `BipBuffer` 由架构层 `DUST_BUF_BIPBUF` 门禁提供 include（algorithm 层），
>   usb 模块的 CMakeLists 不需要手动加 bipbuf 路径；
> - 业务代码已统一用 `UsbHal::Config`（如 `project/thread/pc/trd_pc.cpp:46`），没有旧 API 残留。
>
> 若编译**完整 `project`**（含 USB 线程）时链接报 `GetDefaultHal() undefined`，确认
> `hpm5361icb.conf` 里 `CONFIG_DUST_USB_DEVICE_HAL_HPM=y`。**最小工作区（hpm5361）不涉及这些**。

---

## 6. 编译 hpm5361icb

```powershell
cd E:\Zephyr\projects\hpm5361
dust build hpm5361icb
```

**成功标志**：ROM ~10.59% / RAM ~36%，无 FAILED。产物在 `build/zephyr/zephyr.elf`。

> 到这里，**编译必需链已经全部做完，能编译出可烧录的固件**。下面第 7 章的底层 bug 修复
> 不影响这一步——固件已经编出来了，只是某些外设跑起来行为不对。

---

## 7. 底层 bug 修复（已 patch 化，一键应用）

**现状（2026-08-03）**：下表全部底层修改已 **patch 化**，归档在
`Dust_Zephyr_HPMicro_Tree/zephyr-patches/`（10 个 patch，覆盖 zephyr/sdk_env/sdk_glue/cherryusb
共 37 处修改），由 `apply-patches.sh` 一键应用。**不再需要手动改官方文件。**
CI 已自动完成 clone 官方 → apply patch → 编译验证（见《zephyr_CI_CD_规划.md》M0/M1）。

| # | 修改 | 位置 | 影响（为什么） |
| --- | --- | --- | --- |
| 1 | intc_plic 补丁 | `zephyr/drivers/interrupt_controller/intc_plic.c` | RISC-V PLIC 伪中断未 acknowledge → 中断风暴/卡死，USB/GPIO 中断丢失 |
| 2 | hpm_misc 补丁 | `sdk_env/hpm_sdk/soc/HPM5300/HPM5361/hpm_misc.h` | HPM5361 缺 DLM/ILM↔system 地址换算 → DMA/USB 缓冲访问 core-local 内存地址错 |
| 3 | uart_hpmicro RX idle | `sdk_glue/drivers/serial/uart_hpmicro.c` | HPM5361 需硬件 RX idle + async 自动拉起 IRQ/DMA，否则 UART+DMA 收发不稳定 |
| 4 | 外设驱动运行时修正 | `sdk_glue/drivers/{can,gpio,pwm,spi,flash,...}` | 各外设使能时才暴露的行为修复 |
| 5 | CherryUSB array-size | `modules/lib/CherryUSB/osal/usb_osal_zephyr.c` | ARRAY_SIZE 宏冲突（-Werror 下编译失败） |

**如何使用**：
1. clone 官方 4 仓库：zephyr v4.3.0 / `sdk_env v1.11.0`（sparse 只拉 hpm_sdk）/ sdk_glue / CherryUSB
2. 跑 `apply-patches.sh --zephyr <z> --sdk-env <e> --sdk-glue <g> --cherryusb <c>`
3. `west build -b hpm5361icb` 编译验证
4. 烧录验证各外设行为

**注意**：下表修改里有部分是**编译必需**的（如 sdk_glue 的 udc v4.3 API 迁移——旧签名
`udc_ep_set_busy(dev,...)` 在 v4.3 已不存在），patch 必须在编译前 apply；其余是运行时修复。
CI 每次先 apply 再编译，两者都覆盖。

> 病因细节与改前/改后代码见《zephyr_HPM_底层修改.md》（已 patch 化，无需手动改，供理解为什么）。

---

## 8. 常见问题排查

### 8.1 `HPM53_SINGLE_PRECISION_FPU undefined`
- **原因**：小树 HPM5300 的 `Kconfig` 没被加载（第 4.1 没做）。
- **解决**：补 4.1。

### 8.2 `add_subdirectory(HPM5300) not an existing directory`
- **原因**：sdk_glue 的 `soc/hpmicro/CMakeLists.txt` 还在 `add_subdirectory(${SOC_SERIES})`。
- **解决**：按 4.4 改条件分支并显式 binary dir。

### 8.3 `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` 空（`#if == 0` 无左操作数）
- **原因**：小树 `Kconfig.defconfig.series` 没被加载。
- **解决**：补 4.3 + 4.5（source 指向小树自身）。

### 8.4 找不到板：`hpm5361icb` 不认识
- **原因**：应用 CMakeLists 没挂 `BOARD_ROOT/SOC_ROOT/DTS_ROOT` 到 sdk_glue / sdk_glue_user。
- **解决**：按 5.1。

### 8.5 `BOARD_ROOT element without a 'boards' subdirectory`
- **原因**：`sdk_glue_user` 路径基于 D 盘残留的环境变量。
- **解决**：CMakeLists 用 `CMAKE_CURRENT_SOURCE_DIR` 相对路径（5.1 已给），并把环境变量设成 E 盘。

### 8.6 烧录 openocd 找不到 `cmsis_dap.cfg` / `interface/cmsis-dap.cfg`
- **原因**：`interface/cmsis-dap.cfg` 在 sdk_glue_user 小树，openocd 搜索路径没加。
- **解决**：`hpm5361icb` 的 board.cmake 里 `--openocd-search` 加 `sdk_glue_user/boards/openocd`，
  openocd.exe 用 `sdk_env/tools/openocd/openocd.exe`（不能是 `openocd_backup`）。

### 8.7 SSH 认证成 qingyu0620
- **原因**：`git@github.com:` 默认走 qingyu0620 私钥。
- **解决**：用 `git@github-qingyu0310:...`（见上一篇 §5.1）。

---

_续篇（HPM SDK 版）更新至 2026-08-02。先编译通，底层 bug 后置。_
