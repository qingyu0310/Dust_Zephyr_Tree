# USB 板卡底层下沉规划（UsbHalHpm → 子树 drivers）

> 2026-08-09。把 USB 的**板卡底层**（HPM 芯片 HAL `UsbHalHpm` + binding）从架构层 `framework/drivers/` 下沉到**子树 drivers** `zephyr_user/drivers/`。**中间层（`UsbHal` 接口 + `UsbDevPort`/`UsbCdcAcm` 协议栈 + `Usb` Stream 适配）留在架构层不动。**

## 0. 背景与目标

### 现状分层（参考 CherryUSB 三层）

```text
framework/drivers/communication/stream/usb/
├── usb.cpp / usb.hpp              Usb — 顶层 Stream 适配（业务侧接口）        ← 中间层，留
├── usb_rx_queue.hpp               接收队列                                     ← 中间层，留
├── interface/usb_hal.hpp          UsbHal 纯虚接口（芯片无关）                  ← 中间层，留
├── interface/usb_types.hpp        类型定义                                     ← 中间层，留
├── core/                          UsbDevPort / UsbCdcAcm / descriptor 协议栈    ← 中间层，留
├── hal/hpm/usb_hal_hpm.{hpp,cpp}  UsbHalHpm — HPM 芯片底层（寄存器/PHY/DMA/QHD·qTD/IRQ） ← 板卡底层，下沉
├── dts/bindings/usb/hpmicro,hpm-qingyuusb.yaml  板卡节点 binding               ← 板卡底层，下沉
└── Kconfig                        DUST_USB_DEVICE_STACK / HAL_HPM / CDC_ACM
```

### 为什么下沉（决策依据）

- `UsbHalHpm` 是**芯片/板卡专属**（HPM5361 EHCI），与子树 drivers 里的 `fsmc` 同性质——板卡底层驱动归子树。
- framework 是**只读 git 子模块**，多板卡 HAL（未来 `UsbHalStm32`）都塞架构层会导致：架构层膨胀、改板卡 HAL 要动只读子模块 + 跨仓库提交。
- 下沉后**架构层稳定**：换板卡只动子树 drivers，中间层一行不改。符合"主干相同分支不同"。

### 边界（本次不动）

`UsbHal` 接口、`UsbDevPort`/`UsbCdcAcm`/descriptor 协议栈、`usb_rx_queue.hpp`、`usb.cpp`、`interface/`——全部留在 framework。

## 1. 现状盘点（证据，迁移前核对）

| 项 | 当前位置 | 证据 |
| --- | --- | --- |
| HAL 头/源 | `framework/drivers/communication/stream/usb/hal/hpm/usb_hal_hpm.{hpp,cpp}` | 文件 |
| binding | `framework/drivers/communication/stream/usb/dts/bindings/usb/hpmicro,hpm-qingyuusb.yaml` | 文件 |
| HAL Kconfig | `framework/drivers/communication/stream/usb/Kconfig` 的 `DUST_USB_DEVICE_HAL_HPM` 段 | Kconfig:9-11 |
| HAL 编译块 | `framework/drivers/CMakeLists.txt` `if(CONFIG_DUST_USB_DEVICE_HAL_HPM)` | CMakeLists:53-63 |
| 栈 Kconfig 入口 | `framework/zephyr/module.yml` `kconfig: zephyr/Kconfig` → `rsource "../drivers/communication/stream/usb/Kconfig"` | module.yml / zephyr/Kconfig:10 |
| binding 挂载 | `framework/zephyr/module.yml` `settings.dts_root: drivers/communication/stream/usb` | module.yml |
| 栈对 HAL 依赖 | `DUST_USB_DEVICE_STACK` → `select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED` | usb/Kconfig:6 |
| 顶层开关 | `framework/drivers/Kconfig` `DUST_COM_USB` → `select DUST_USB_DEVICE_STACK` | drivers/Kconfig:69-73 |
| HAL 源码 include 依赖 | `#include "usb_hal.hpp"`（interface）、`#include "log.hpp"`（`framework/cmd/shell/`）、HPM SDK 头 | usb_hal_hpm.cpp:11-23 |
| `GetDefaultHal()` 定义 | `usb_hal_hpm.cpp`（下沉后由子树 drivers 提供），声明在 `interface/usb_hal.hpp:175` | usb.cpp:34 调用 |
| 板卡 overlay 使用 | `project/boards/hpm/hpm5361icb/hpm5361icb.overlay:93-94` `qingyuusb_usb0: &usb0 { compatible = "hpmicro,hpm-qingyuusb"; }` | overlay |
| 业务调用方 | `project/thread/pc/trd_pc.cpp:44-45` 用 `DT_NODELABEL(qingyuusb_usb0)` | trd_pc.cpp |

**include 可行性**：子树 drivers 的源文件经 `target_sources(app)` 并入 app target，app 已含 `target_include_directories(app PRIVATE ${FW_ROOT}/init ${FW_ROOT}/cmd)`（project/CMakeLists.txt:121-124），故 `log.hpp` 无需显式加路径；`usb_hal.hpp` 需在子树 drivers 的 include 里显式加 interface 目录；`FW_ROOT`/`SDK_GLUE_DIR` 变量在 project/CMakeLists.txt 定义，add_subdirectory 子目录可见。

## 2. 目标结构

```text
zephyr_user/drivers/
├── Kconfig                      # 新增 DUST_USB_DEVICE_HAL_HPM 定义
├── CMakeLists.txt               # 新增 if(CONFIG_DUST_USB_DEVICE_HAL_HPM) 编译块
├── fsmc/
│   └── fsmc.cpp                 # 已有
├── usb/hal/hpm/
│   ├── usb_hal_hpm.hpp          # 从 framework 移入（内容不变）
│   └── usb_hal_hpm.cpp          # 从 framework 移入（内容不变）
└── zephyr/module.yml            # 不变（kconfig: Kconfig 即可，binding 不走模块 dts）

zephyr_user/dts/bindings/usb/
└── hpmicro,hpm-qingyuusb.yaml   # 从 framework 移入（与 fsmc binding 同位，走主仓库 DTS_ROOT）
```

## 3. 分阶段执行

### 阶段 1：HAL 文件下沉

**目标**：`usb_hal_hpm.{hpp,cpp}` 从 framework 移到 `zephyr_user/drivers/usb/hal/hpm/`。

**任务**：
1. 新建目录 `zephyr_user/drivers/usb/hal/hpm/`。
2. 复制 `framework/drivers/communication/stream/usb/hal/hpm/usb_hal_hpm.hpp` → `zephyr_user/drivers/usb/hal/hpm/usb_hal_hpm.hpp`，**内容逐字节一致**（移动不改值）。
3. 复制 `.../usb_hal_hpm.cpp` → `zephyr_user/drivers/usb/hal/hpm/usb_hal_hpm.cpp`，内容不变（include 靠路径解析，不改源码）。
4. 在 framework/drivers 子模块 `git rm -r framework/drivers/communication/stream/usb/hal`。

**产出**：`zephyr_user/drivers/usb/hal/hpm/{usb_hal_hpm.hpp,usb_hal_hpm.cpp}`；framework 无 `hal/` 目录。

**验证**：`cat` 对比两边 usb_hal_hpm.cpp 一致（`diff <(git show HEAD:...) 新文件` 或手工核对）。

---

### 阶段 2：Kconfig 下沉

**目标**：`DUST_USB_DEVICE_HAL_HPM` 定义移到子树 drivers/Kconfig；framework 只保留"栈对 HAL 的依赖 select"。

**任务 2.1 — framework `communication/stream/usb/Kconfig`（old → new）**：

old（现第 1-11 行）：
```kconfig
# USB 设备栈：中间层（栈内核） + 各芯片 HAL（参考 CherryUSB 分层）
config DUST_USB_DEVICE_STACK
    bool
    select DUST_USB_DEVICE_CDC_ACM
    # 各芯片 HAL 用 DTS 条件 select
    select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED
    # select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_..._ENABLED   # 预留多芯片

config DUST_USB_DEVICE_HAL_HPM
    bool "HPMicro EHCI USB HAL"
    default n
```

new：
```kconfig
# USB 设备栈：中间层（栈内核）+ 各芯片 HAL（参考 CherryUSB 分层）
# HAL 定义已下沉到 zephyr_user/drivers/Kconfig（板卡底层归子树，2026-08-09）
config DUST_USB_DEVICE_STACK
    bool
    select DUST_USB_DEVICE_CDC_ACM
    # 各芯片 HAL 用 DTS 条件 select（DUST_USB_DEVICE_HAL_HPM 定义在 zephyr_user/drivers/Kconfig）
    select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED
    # select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_..._ENABLED   # 预留多芯片
```
即：**删除 `DUST_USB_DEVICE_HAL_HPM` 定义块（原 9-11 行），保留 `DUST_USB_DEVICE_STACK` 及其 `select DUST_USB_DEVICE_HAL_HPM if ...` 行**。

**任务 2.2 — 子树 drivers/Kconfig（old → new）**：

old：
```kconfig
config DUST_DEV_FSMC
    bool "STM32 FSMC LCD controller (HX8357DN 8080)"
    default n
    help
      FSMC 底层设备驱动：读 lcd_fsmc 节点时序/背光属性，配置 Bank 寄存器。

config DUST_DEV_FSMC_INIT_PRIORITY
    int "FSMC init priority"
    default 40
    help
      FSMC 设备初始化优先级（POST_KERNEL 阶段，需早于 LCD 业务线程）。
```

new（文件末尾追加）：
```kconfig

# USB 板卡底层 HAL（2026-08-09 从 framework/drivers/communication/stream/usb 下沉）
config DUST_USB_DEVICE_HAL_HPM
    bool "HPMicro EHCI USB HAL"
    default n
```

**产出**：framework usb/Kconfig 无 HAL 定义块；`zephyr_user/drivers/Kconfig` 有 `DUST_USB_DEVICE_HAL_HPM`。

**验证**：Kconfig 树合并后 `select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED` 能解析到子树定义（编译期确认，无 "unknown symbol"）。

---

### 阶段 3：CMake 下沉

**目标**：HAL 编译块从 framework 移到子树 drivers。

**任务 3.1 — framework/drivers/CMakeLists.txt：删除整块**（现 53-63 行）：
```cmake
if(CONFIG_DUST_USB_DEVICE_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/stream/usb/hal/hpm/usb_hal_hpm.cpp
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device/hpm_usb_device.c
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/hpm_usb_drv.c)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/stream/usb/hal/hpm
        ${CMAKE_CURRENT_SOURCE_DIR}/communication/stream/usb/interface
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/inc)
endif()
```

**任务 3.2 — 子树 drivers/CMakeLists.txt：追加编译块**（old 现只有 FSMC 块）：
```cmake
# USB 板卡底层 HAL（HPM EHCI，2026-08-09 下沉自 framework）
if(CONFIG_DUST_USB_DEVICE_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm/usb_hal_hpm.cpp
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device/hpm_usb_device.c
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/hpm_usb_drv.c)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm
        ${FW_ROOT}/drivers/communication/stream/usb/interface
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/inc)
endif()
```
说明：
- include 里 interface 路径改为 `FW_ROOT/drivers/communication/stream/usb/interface`（framework 中间层接口）。
- `log.hpp` 靠 app target 已有的 `${FW_ROOT}/cmd` include，**不需要**显式加。
- `FW_ROOT`、`SDK_GLUE_DIR` 在 project/CMakeLists.txt 定义，add_subdirectory 子树 drivers 时子目录可见。

**产出**：framework 不再编译 HAL；子树 drivers 编译 HAL + SDK 源。

**验证**：编译后 `usb_hal_hpm.cpp` 参与构建；无 include 找不到、无重复符号（`GetDefaultHal` 只定义一次）。

---

### 阶段 4：binding + module.yml 下沉

**目标**：binding 移到主仓库 dts/bindings（与 fsmc binding 同位），framework module.yml 移除 dts_root。

**任务 4.1 — 移动 binding**：
- 复制 `framework/drivers/communication/stream/usb/dts/bindings/usb/hpmicro,hpm-qingyuusb.yaml` → `zephyr_user/dts/bindings/usb/hpmicro,hpm-qingyuusb.yaml`（内容不变）。
- 在 framework/drivers 子模块 `git rm -r framework/drivers/communication/stream/usb/dts`。
- 主仓库 `zephyr_user/dts/bindings/usb/` 会经 project/CMakeLists.txt `DTS_ROOT`（第 73-74 行：`${ZEPHYR_USER_DIR}`、`${ZEPHYR_USER_DIR}/dts`）被 devicetree 解析到，`DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED` 宏继续生成。

**任务 4.2 — framework `zephyr/module.yml`（old → new）**：

old：
```yaml
name: user_framework
build:
  kconfig: zephyr/Kconfig
  settings:
    dts_root: drivers/communication/stream/usb
```
new：
```yaml
name: user_framework
build:
  kconfig: zephyr/Kconfig
```
即：**删除 `settings.dts_root` 段**（binding 已移出 framework）。

**产出**：binding 在 `zephyr_user/dts/bindings/usb/`；framework module.yml 无 dts_root；framework 无 `dts/` 目录。

**验证**：`DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED` 仍生成（`find build -name "devicetree_generated.h"` 里 grep）。

---

### 阶段 5：子模块提交 + 主仓库指针 + 验证

**目标**：framework/drivers 子模块改动落地，主仓库子模块指针更新，全链路验证。

**任务 5.1 — framework/drivers 子模块提交**（`e:\Zephyr\zephyr_user\framework\drivers`）：
- `git add -A`，commit message（按用户指示写，默认参考）：`refactor: USB HAL 下沉至 zephyr_user/drivers（板卡底层归子树）`
- 直推 master（framework 子模块无门禁，见 CI/CD 记忆）。

**任务 5.2 — 主仓库更新子模块指针 + 新增文件**：
- 主仓库 add 新文件（`drivers/usb/hal/hpm/*`、`dts/bindings/usb/*`、`drivers/Kconfig`、`drivers/CMakeLists.txt`）
- 更新 `framework/drivers` 子模块指针。
- 主仓库走 PR（`upload-usb-hal-subsink` 类分支）——主仓库必须 PR + auto-merge（GH013 Ruleset），见 CI/CD 记忆。

**任务 5.3 — 用户编译验证**（编译为用户专属动作，AI 不编译）：
- 编译 hpm5361icb，确认 `CONFIG_DUST_USB_DEVICE_HAL_HPM=y`（`DUST_COM_USB` 打开时被栈 select）。
- 烧录后 PC 串口/USB 枚举识别 CDC 设备，trd_pc 通信正常。

**产出**：两仓库改动落地；主仓库 PR 合并后子模块指针/文件一致。

**验证**：编译通过 + USB 枚举/收发正常。

## 4. 验证标准（汇总）

- [ ] hpm5361icb 编译通过（无 unknown symbol、无重复符号、无 include 找不到）
- [ ] `CONFIG_DUST_USB_DEVICE_HAL_HPM` 生效（CONFIG_DUST_COM_USB 打开时 =y）
- [ ] `DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED` 仍生成
- [ ] framework/drivers 子模块无 `hal/`、`dts/` 残留，无 `usb_hal_hpm` 引用
- [ ] PC 枚举 USB CDC 成功，trd_pc 收发正常

## 5. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| `select DUST_USB_DEVICE_HAL_HPM` 跨模块 | framework 模块 select 子树模块定义的符号 | Kconfig 树合并后正常；编译验证 |
| binding 位置变化丢 `DT_HAS_` 宏 | 放错目录 devicetree 找不到 binding | 放 `zephyr_user/dts/bindings/usb/`（DTS_ROOT 已覆盖）；按验证标准 2 查宏 |
| `FW_ROOT`/`SDK_GLUE_DIR` 变量可见性 | 子树 drivers 的 CMakeLists 需要这两个变量 | project/CMakeLists 已定义，add_subdirectory 继承；编译验证 |
| 使用者工作区 | ai_imu/detected 等复制了 framework 子模块副本 | 本轮只动主仓库；使用者工作区后续各自 `git submodule update` |

## 6. 执行清单（逐条勾）

- [ ] 阶段 1：建 `zephyr_user/drivers/usb/hal/hpm/`，移入 usb_hal_hpm.{hpp,cpp}（内容不变）
- [ ] 阶段 1：framework 子模块 `git rm -r communication/stream/usb/hal`
- [ ] 阶段 2：framework usb/Kconfig 删 `DUST_USB_DEVICE_HAL_HPM` 定义块（保留 select 行）
- [ ] 阶段 2：`zephyr_user/drivers/Kconfig` 追加 `DUST_USB_DEVICE_HAL_HPM` 定义
- [ ] 阶段 3：framework/drivers/CMakeLists.txt 删 `if(CONFIG_DUST_USB_DEVICE_HAL_HPM)` 块
- [ ] 阶段 3：`zephyr_user/drivers/CMakeLists.txt` 追加 HAL 编译块（含 interface include + SDK 源）
- [ ] 阶段 4：binding 移到 `zephyr_user/dts/bindings/usb/`；framework 删 `communication/stream/usb/dts`
- [ ] 阶段 4：framework `zephyr/module.yml` 删 `settings.dts_root`
- [ ] 阶段 5：framework/drivers 子模块提交并直推 master
- [ ] 阶段 5：主仓库 add 新文件 + 更新子模块指针，走 PR + auto-merge
- [ ] 阶段 5：用户编译 hpm5361icb + USB 枚举验证
