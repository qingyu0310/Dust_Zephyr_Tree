# zephyr_user 子树架构总览

> 更新日期：2026-08-21
> 适用路径：`E:\Zephyr\zephyr_user`

`zephyr_user` 是一棵面向 Zephyr 的用户自研子树。它把公共能力拆成 `framework/`，把板卡专属底层驱动留在 `drivers/`，把板卡和设备树放在 `boards/` / `dts/`，再用 `project/` 把这些能力装配进具体应用。

## 目录

```text
zephyr_user/
├── boards/      用户自研板卡定义
├── dts/         用户自研 devicetree binding
├── doc/         架构、搭建、调研文档
├── drivers/     子树板卡底层驱动与 HAL
├── framework/   架构层子模块 + Zephyr 模块入口
├── platform/    平台兼容层
└── project/     当前维护者验证工程
```

## 各目录职责

### `boards/`

放 Zephyr 树里没有的板卡定义。当前主要是 `boards/st/stm32f407igh6/`，包含 `dts`、`pinctrl`、`defconfig`、`board.cmake`、`board.yml` 和 `README.md`。

### `dts/`

放自定义 binding。当前能看到的例子包括：

- `dts/bindings/mtd/winbond,w25q128.yaml`
- `dts/bindings/fsmc/st,fsmc-lcd.yaml`
- `dts/bindings/usb/hpmicro,hpm-dustusb.yaml`
- `dts/bindings/usb/st,st-dustusb.yaml`
- `dts/bindings/gpio/heart-beat.yaml`

### `drivers/`

这是子树的板卡底层驱动层，按 Zephyr 模块方式装配：

- `drivers/fsmc/` - STM32 FSMC LCD 底层驱动
- `drivers/usb/hal/hpm/` - HPM EHCI USB HAL
- `drivers/usb/hal/stm32/` - STM32 OTG FS USB HAL

对应 Kconfig 里现在有 `DUST_USB_DEVICE_HAL_HPM` 和 `DUST_USB_DEVICE_HAL_STM32`。`framework/drivers` 负责通用外设封装，`drivers/` 负责板卡专属底层实现。

### `framework/`

这是公共架构层。`framework/drivers`、`framework/algorithm`、`framework/modules`、`framework/topic`、`framework/cmd`、`framework/init` 是 6 个独立 submodule；`framework/zephyr/` 是 Zephyr 模块入口，用来自动加载这些层的 Kconfig。

当前各层大致对应：

- `drivers` - 通用外设封装和通信流
- `algorithm` - 纯计算、滤波、辨识、缓冲
- `modules` - 设备能力组合层
- `topic` - 跨线程数据契约
- `cmd` - shell / buzzer / flash / fatal 等横切能力
- `init` - 启动、注册表、阶段调度

### `platform/`

当前是 `platform/cmsis/`，主要提供 `HAS_CMSIS_CORE` 和 `cmsis_core.h` 兼容垫片，解决 ARM Cortex-M 下的 CMSIS 头与 Zephyr 空桩差异。

### `project/`

当前这是维护者的验证工程，不承载业务大线程。`project/CMakeLists.txt` 负责把 `framework/`、`platform/cmsis/` 和 `drivers/` 挂进 Zephyr 构建；`project/Kconfig` 决定当前验证哪些架构能力；`project/thread/` 里只保留当前需要的线程样例。

现在能看到的线程样例有：

- `project/thread/gpio/trd_gpio.cpp` - GPIO 心跳
- `project/thread/test/trd_test.cpp` - USB CDC ACM 回环测试

## 装配链路

`project/CMakeLists.txt` 的实际装配顺序是：

```text
BOARD / DTS / BOARD_ROOT
    -> ZEPHYR_EXTRA_MODULES: framework + platform/cmsis + drivers
    -> add_subdirectory(framework 各层)
    -> add_subdirectory(project/thread)
```

`project/Kconfig` 里当前常见的门禁是：

```kconfig
config PRJ_MAIN
    bool "Main business project"
    default n

config TRD_GPIO
    bool "GPIO thread task"
    select DUST_DEV_GPIO_OUTPUT
    select DUST_CTL_TIMER

config TRD_TEST
    bool "User test add config"
    default y
    select DUST_COM_USB
```

其中 `TRD_TEST` 对应的测试线程当前是 USB CDC ACM 回环。

## 板卡配置

`project/boards/` 下面当前有两套板卡配置：

- `project/boards/hpm/hpm5361icb/`
- `project/boards/st/puzhong/`

它们都用 `board.cmake` / `*.conf` / `*.overlay` 组合板卡差异。当前例子里：

- HPM 板卡启用 `CONFIG_DUST_USB_DEVICE_HAL_HPM=y`
- STM32 板卡启用 `CONFIG_DUST_USB_DEVICE_HAL_STM32=y`

当前常见的设备树别名也比较克制，只保留真正被用到的语义名，例如：

```dts
aliases {
    shell-uart  = &uart3;
    dustusb_usb0 = &usb0;
};
```

## 编译方式

`framework/cmd/build/dust.cmd` 包了一层 `dust build <name>`。当前脚本会在 `project/boards/*/<name>/` 里找 `*.overlay`，找到后自动把 `BOARD_CFG` 传给 `west build`。

常见用法：

```powershell
dust build hpm5361icb
dust build puzhong
```

直接用 `west` 也可以：

```powershell
west build -b hpm5361icb -- -DBOARD_CFG=hpm5361icb
west build -b stm32f407igh6 -- -DBOARD_CFG=puzhong
```

## 推荐阅读顺序

1. `README.md`
2. `子树架构介绍.md`
3. `framework/init` -> `framework/cmd` -> `framework/topic` -> `framework/algorithm` -> `framework/drivers` -> `framework/modules`
4. `project/CMakeLists.txt`、`project/Kconfig`、`project/thread/*`
