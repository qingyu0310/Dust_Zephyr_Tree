# 共享契约层（contract）

> 版本：0.1
> 日期：2026-11-12

## 这是什么

**底层实现必须满足的形状。** 一组纯虚基类 + 纯数据类型，零 Zephyr 依赖、零厂商依赖、零状态。

它不服务于上层业务，而是**约束底层实现的接口**——比如 `UsbHal` 规定"HPM 和 STM32 的 USB HAL 必须提供哪些方法"，`EcatEsc` 规定"HPM ESC 的 HAL 必须提供哪些能力"。

## 为什么单独列一层

契约在物理上属于**底层与上层的共同边界**，放哪一方都会造成另一方向上看：

- 放在上层的 `interface/` 目录 → 底层 `include` 它就是"底层依赖上层"
- 放在底层的 `drivers/` 目录 → 上层 `include` 它就是"上层依赖底层实现"

所以它独立成 `contract/`，**是唯一允许跨层双向引用的东西**。

## 清单（6 个文件，831 行）

| 文件 | 行数 | 内容 | 当前位置（P2 步 14 迁入本目录） |
|---|---|---|---|
| `usb_hal.hpp` | 175 | USB 硬件抽象（纯虚基类 `UsbHal`） | `framework/drivers/communication/stream/usb/interface/` |
| `usb_types.hpp` | 174 | USB 数据类型（事件、端点、配置） | 同上 |
| `ecat_esc.hpp` | 188 | ESC 能力契约（纯虚基类 `EcatEsc`） | `framework/drivers/communication/ethercat/interface/` |
| `ecat_types.hpp` | 188 | EtherCAT 数据类型 | 同上 |
| `ecat_profile.hpp` | 71 | PDO 布局描述 | 同上 |
| `esf_dump.hpp` | 35 | 异常栈帧转储接口 | `framework/cmd/fatal/` |

## 判定标准（进这个目录必须同时满足）

1. **纯虚基类或纯数据类型**——没有实现、没有状态、没有静态成员
2. **零厂商依赖**——不出现 `hpm_` / `stm32_` / `HAL_` 等前缀
3. **被两个方向引用**——底层实现它，上层（或中间层协议栈）使用它

> 注：`esf_dump.hpp` 依赖 `<zephyr/kernel.h>`（要用 `struct arch_esf` 的完整定义），
> 它是唯一带 Zephyr 依赖的契约头。这不算违规——契约层约束的是"不绑厂商"，
> 绑 Zephyr 内核类型是这套架构本身的既有前提。

## 依赖规则

```text
底层实现（zephyr/drivers/）──实现──→ contract/
中间层协议栈（midlayer/）  ──使用──→ contract/
```

**contract 不依赖任何人。** 它不 include 上层、不 include 底层实现、不 include Zephyr。

## 验收命令

```bash
# ① contract 里不得出现厂商符号
grep -rn "hpm_\|HAL_PCD\|stm32_ll_\|STM32_\|HPM_" contract/
# 期望：为空

# ② 契约清单与实际文件一致
ls contract/
```
