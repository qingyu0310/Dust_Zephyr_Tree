# test 线程 USB 板卡无关化规划（业务层统一 dustusb_usb0 + STM32 自研 binding）

> 2026-08-14。**背景**：主仓库 CI（build-hpm5361icb）编瘦身 project 时炸在 `project/thread/test/trd_test.cpp`——它硬编码了 STM32 节点 `DT_NODELABEL(usbotg_fs)`，但 hpm5361icb 板设备树里没有 `usbotg_fs`（只有 `usb0`）→ 宏展开失败。
>
> **根因（架构问题）**：板卡节点选择被硬编码在**业务线程层**，违反"板卡差异收在设备树 overlay、业务层板卡无关"的分层约定。自研 USB 栈（`Usb` + `UsbHal` 抽象 + 各板卡 `GetDefaultHal()`）已封装板卡差异，业务层不应再碰具体节点名。
>
> **方案（2026-08-14 二次修订，用户拍板）**：
> 1. 业务层统一 `DT_NODELABEL(dustusb_usb0)`；
> 2. 三板卡 overlay 各自提供 `dustusb_usb0:` 标签；
> 3. **STM32 也改用自研 binding `st,st-dustusb`**（对称 `hpmicro,hpm-dustusb`），自研 USB 栈彻底不依赖 Zephyr 官方 `st,stm32-otgfs` binding。

## 0. 原则

1. **业务层（thread/*）一律不写板卡专属节点名**——USB 初始化统一 `DT_NODELABEL(dustusb_usb0)`。
2. **板卡差异收在 overlay 层**：每块板 overlay 定义 `dustusb_usb0:` 标签指向本板 USB 控制器（HPM→`&usb0`，STM32→`&usbotg_fs`）。
3. **两板都用 dustusb 系列 binding**：HPM=`hpmicro,hpm-dustusb`（已有），STM32=`st,st-dustusb`（新建）——自研栈不依赖官方 USB binding。
4. **HAL 选择仍按 DTS compatible 宏分流**：`DT_HAS_HPMICRO_HPM_DUSTUSB_ENABLED`→`_HAL_HPM`，`DT_HAS_ST_ST_DUSTUSB_ENABLED`→`_HAL_STM32`。HAL 编译块与节点标签两层正交。

## 1. 现状盘点（2026-08-14 实测）

| 文件 | 现状 | 说明 |
| --- | --- | --- |
| 主干 [hpm5361icb.overlay](project/boards/hpm/hpm5361icb/hpm5361icb.overlay) | 瘦身版，无 usb 节点 | 需补 `dustusb_usb0: &usb0` |
| 主干 [trd_test.cpp](project/thread/test/trd_test.cpp) | 硬编码 `DT_NODELABEL(usbotg_fs)` | 需统一 `dustusb_usb0` |
| 范本 [temp/hpm5361icb.overlay](temp/project/boards/hpm/hpm5361icb/hpm5361icb.overlay) | `dustusb_usb0: &usb0 { compatible="hpmicro,hpm-dustusb"; clk-name=<CLOCK_USB0>; status="okay"; }` | HPM 正确范本 |
| 范本 [temp/trd_pc.cpp](temp/project/thread/pc/trd_pc.cpp) | `DT_REG_ADDR(DT_NODELABEL(dustusb_usb0))` + `DT_IRQN(...)` | 业务层正确范本 |
| [stm32f4_disco.overlay](project/boards/st/puzhong/stm32f4_disco.overlay) | `&usbotg_fs { compatible="st,stm32-otgfs"; ... }`（Zephyr 官方 binding） | 需改自研 `st,st-dustusb` |
| [st,stm32-otgfs.yaml](e:/Zephyr/zephyr/dts/bindings/usb/st,stm32-otgfs.yaml) | 官方 binding，属性全必填 | 仿此建 `st,st-dustusb.yaml` |
| framework [usb/Kconfig:8](framework/drivers/communication/stream/usb/Kconfig#L8) | `select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_OTGFS_ENABLED` | 需改 `DT_HAS_ST_ST_DUSTUSB_ENABLED` |
| [usb_hal_stm32.cpp:23-24](drivers/usb/hal/stm32/usb_hal_stm32.cpp#L23-L24) | `PINCTRL_DT_DEFINE(DT_NODELABEL(usbotg_fs))` | **不用改**（节点标签没变，pinctrl 由 overlay 提供） |

**为什么不能改 HPM binding / 能改 STM32 binding**：
- framework usb/Kconfig 的 HAL select 按 compatible 宏分流。STM32 节点若标 `hpmicro,hpm-dustusb` → `DT_HAS_HPMICRO_HPM_DUSTUSB_ENABLED` 生成 → 错误触发 `_HAL_HPM` → 编 UsbHalHpm（依赖 HPM SDK）→ STM32 板炸。
- 标 `st,st-dustusb`（自研 STM32 binding）→ `DT_HAS_ST_ST_DUSTUSB_ENABLED` 生成 → `_HAL_STM32` → 正确编 UsbHalStm32。

## 2. 阶段 1：主干 hpm5361icb.overlay 补 dustusb_usb0 节点

**目标**：hpm 板设备树有 `dustusb_usb0` 标签，`DT_HAS_HPMICRO_HPM_DUSTUSB_ENABLED` 宏生成（HAL select 前置）。

**old**（[hpm5361icb.overlay](project/boards/hpm/hpm5361icb/hpm5361icb.overlay)，文件尾 `&mtimer` 块后无 usb）：
```dts
&mtimer {
    compatible = "riscv,machine-timer", "andestech,machine-timer";
    reg-names = "mtime", "mtimecmp";
};
```

**new**（文件尾追加，照抄 temp 范本）：
```dts
&mtimer {
    compatible = "riscv,machine-timer", "andestech,machine-timer";
    reg-names = "mtime", "mtimecmp";
};

dustusb_usb0: &usb0 {
    compatible = "hpmicro,hpm-dustusb";
    clk-name = <CLOCK_USB0>;
    status = "okay";
};
```

> `CLOCK_USB0` 需 `#include <dt-bindings/clock/hpm5361-clocks.h>`。检查瘦身版 overlay 文件头，缺则补。

**产出**：`project/boards/hpm/hpm5361icb/hpm5361icb.overlay` 有 `dustusb_usb0` 节点。

**验证**：hpm5361icb 编译时 `DT_NODELABEL(dustusb_usb0)` 能解析；`CONFIG_DUST_USB_DEVICE_HAL_HPM=y`（[hpm5361icb.conf:12](project/boards/hpm/hpm5361icb/hpm5361icb.conf#L12)）。

## 3. 阶段 2：trd_test.cpp 业务层统一 dustusb_usb0

**目标**：业务层不硬编码板卡节点。

**old**（[trd_test.cpp:41-42](project/thread/test/trd_test.cpp#L41-L42)）：
```cpp
    cfg.reg_base     = DT_REG_ADDR(DT_NODELABEL(usbotg_fs));
    cfg.irq_num      = DT_IRQN(DT_NODELABEL(usbotg_fs));
```

**new**（照抄 temp/trd_pc.cpp:44-45）：
```cpp
    cfg.reg_base     = DT_REG_ADDR(DT_NODELABEL(dustusb_usb0));
    cfg.irq_num      = DT_IRQN(DT_NODELABEL(dustusb_usb0));
```

**产出**：`project/thread/test/trd_test.cpp` 不再出现 `usbotg_fs`。

**验证**：grep `trd_test.cpp` 无 `usbotg_fs`；hpm5361icb 编译通过。

## 4. 阶段 3：STM32 自研 binding `st,st-dustusb`

**目标**：STM32 板卡用自研 dustusb binding，与 HPM 对称；业务层 `dustusb_usb0` 在两板都解析到正确 USB 控制器。

### 4.1 新建 `zephyr_user/dts/bindings/usb/st,st-dustusb.yaml`

**old**：无。

**new**（仿官方 [st,stm32-otgfs.yaml](e:/Zephyr/zephyr/dts/bindings/usb/st,stm32-otgfs.yaml)，**属性必须全覆盖官方节点**，compatible 改 `st,st-dustusb`）：

```yaml
# Copyright (c) 2026, qingyu
# SPDX-License-Identifier: Apache-2.0

description: STM32 OTGFS controller (self-implemented stack)

compatible: "st,st-dustusb"

include: [usb-ep.yaml, pinctrl-device.yaml]

properties:
  reg:
    required: true

  interrupts:
    required: true

  pinctrl-0:
    required: true

  pinctrl-names:
    required: true

  ram-size:
    type: int
    required: true
    description: |
      Size of USB dedicated RAM. STM32 SOC's reference
      manual defines a shared FIFO size.

  phys:
    type: phandle
    description: PHY provider specifier

  clocks:
    required: true
```

> **属性必须全覆盖官方 `usbotg_fs` 节点**（reg/interrupts/num-bidir-endpoints/ram-size/maximum-speed/phys/clocks + pinctrl）。`num-bidir-endpoints`/`maximum-speed` 由 include 的 `usb-ep.yaml`/`usb-controller.yaml` 提供；若 binding 缺官方节点已有属性 → DT 校验报 unknown property。

### 4.2 stm32f4_disco.overlay 改 compatible + 加标签

**old**（[stm32f4_disco.overlay:28-32](project/boards/st/puzhong/stm32f4_disco.overlay#L28-L32)）：
```dts
&usbotg_fs {
	pinctrl-0 = <&usb_otg_fs_dm_pa11 &usb_otg_fs_dp_pa12>;
	pinctrl-names = "default";
	status = "okay";
};
```

**new**（加 `dustusb_usb0:` 标签 + compatible 覆盖为自研 binding）：
```dts
dustusb_usb0: &usbotg_fs {
	compatible = "st,st-dustusb";
	pinctrl-0 = <&usb_otg_fs_dm_pa11 &usb_otg_fs_dp_pa12>;
	pinctrl-names = "default";
	status = "okay";
};
```

> 节点标签 `usbotg_fs` 不变（官方 dtsi 定义），reg/interrupts/clocks 等属性保留官方值——`DT_REG_ADDR`/`DT_IRQN` 读到正确地址/中断号给 UsbHalStm32。

### 4.3 framework usb/Kconfig 改 select 条件

**old**（[usb/Kconfig:8](framework/drivers/communication/stream/usb/Kconfig#L8)）：
```kconfig
    select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_OTGFS_ENABLED
```

**new**：
```kconfig
    select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_ST_DUSTUSB_ENABLED
```

**产出**：`zephyr_user/dts/bindings/usb/st,st-dustusb.yaml`；[stm32f4_disco.overlay](project/boards/st/puzhong/stm32f4_disco.overlay) 用自研 binding；framework usb/Kconfig select 条件更新。

**验证**：
- `DT_HAS_ST_STM32_OTGFS_ENABLED` 不再生成（compatible 被覆盖）——grep framework 全库确认无其他引用（实测仅 usb/Kconfig:8 一处）。
- stm32f4_disco 编译时 `CONFIG_DUST_USB_DEVICE_HAL_STM32=y`、`DT_NODELABEL(dustusb_usb0)` 解析成功。

## 5. 阶段 4：验证（三板卡编译）

- [ ] 用户 `west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb` 通过（trd_test 不再炸）
- [ ] 用户 `west build -p always -b stm32f4_disco -- -DBOARD_CFG=puzhong` 通过（st,st-dustusb binding 生效）
- [ ] 层 2 project 多板卡 CI 三板卡全绿（board_rm_c 无 USB，跳过不影响）

## 6. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| **compatible 误标 HPM binding** | STM32 节点若标 `hpmicro,hpm-dustusb` → 触发 `_HAL_HPM` → 编 UsbHalHpm 缺 HPM SDK 炸 | 标 `st,st-dustusb`（自研 STM32 binding），Kconfig 按 `DT_HAS_ST_ST_DUSTUSB_ENABLED` 分流 |
| **binding 属性不全** | 官方 `usbotg_fs` 节点属性多（reg/interrupts/num-bidir-endpoints/ram-size/maximum-speed/phys/clocks），新 binding 若缺 → DT 校验 unknown property | 4.1 属性全覆盖（仿官方 yaml），include usb-ep.yaml/pinctrl-device.yaml |
| **hpm overlay 缺 include** | `CLOCK_USB0` 需要 `hpm5361-clocks.h` | 阶段 1 检查/补文件头 include |
| **`DT_HAS_ST_STM32_OTGFS_ENABLED` 残留引用** | 改 compatible 后官方宏不再生成，若有其他代码依赖 → 编译错 | 4.3 后 grep 全库确认无其他引用 |
| **board_rm_c 无 USB** | 该板 overlay 无 usbotg_fs，不需要 dustusb 标签/binding | 本规划不动它；确认层 2 编它时 trd_test 是否含 USB（若含需板卡侧处理） |

## 7. 执行清单（逐条勾）

- [ ] 阶段 1：主干 [hpm5361icb.overlay](project/boards/hpm/hpm5361icb/hpm5361icb.overlay) 追加 `dustusb_usb0: &usb0` 节点 + 补 `hpm5361-clocks.h` include（照抄 temp 范本）
- [ ] 阶段 2：[trd_test.cpp:41-42](project/thread/test/trd_test.cpp#L41-L42) `usbotg_fs` → `dustusb_usb0`（照抄 temp/trd_pc.cpp）
- [ ] 阶段 3.1：新建 `zephyr_user/dts/bindings/usb/st,st-dustusb.yaml`（仿官方，属性全覆盖）
- [ ] 阶段 3.2：[stm32f4_disco.overlay:28](project/boards/st/puzhong/stm32f4_disco.overlay#L28) `&usbotg_fs` → `dustusb_usb0: &usbotg_fs` + compatible 覆盖 `st,st-dustusb`
- [ ] 阶段 3.3：framework [usb/Kconfig:8](framework/drivers/communication/stream/usb/Kconfig#L8) select 条件改 `DT_HAS_ST_ST_DUSTUSB_ENABLED`；grep 全库确认 `DT_HAS_ST_STM32_OTGFS_ENABLED` 无残留引用
- [ ] 阶段 4：用户编译 hpm5361icb + stm32f4_disco 验证
- [ ] 提交主仓库走 PR + auto-merge（与 CI 规划阶段 1.5 流程一致，HPM 树对齐先查）
