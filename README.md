# zephyr_user — 子树架构总览

> 面向 dust 战队内部成员。本架构依附于 zephyr 树，是一套**子树系统架构**，通过维护者↔使用者的
> 上下级管理关系，保证成员在共同架构上各自发展、又不会后期分叉成互不相通的多个版本。

---

## 1. 目录结构

```text
zephyr_user/
├── boards/      用户自研板卡（zephyr 树没有的板卡，如 stm32f407igh6）
├── dts/         用户自研 devicetree binding
├── doc/         架构文档
├── drivers/     子树驱动层（本仓库板卡底层驱动，Zephyr 模块式）
├── framework/   架构子模块（使用者不允许修改）
├── platform/    平台中间层支持
└── project/     维护者业务层工作区
```

---

## 2. 各目录是干什么的

### boards

放置 zephyr 树未包含的板卡定义，只放**底层设备树**。例如 `boards/st/stm32f407igh6/`
（rm_c 板的芯片型号）：板卡 dts、pinctrl、defconfig、board.cmake、烧录配置。具体内容以真实文件为准。

### dts

放置用户自研的 devicetree binding（compatible → schema 校验）。例如
`dts/bindings/mtd/winbond,w25q128.yaml`（上游 zephyr 没有的 W25Q128 SPI NOR Flash 型号）、
`dts/bindings/usb/hpmicro,hpm-dustusb.yaml`（自研 HPM USB 控制器）。

### drivers

**子树驱动层**：本仓库自己的底层驱动，按 Zephyr 官方驱动模型写（`zephyr/module.yml` + Kconfig +
devicetree 设备模型注册），与 `zephyr/drivers/` 同构。放 Zephyr 树没有的板卡专属底层控制器驱动：

- `drivers/fsmc/` — STM32 FSMC 8080 LCD 控制器（`st,fsmc-lcd`）
- `drivers/usb/hal/hpm/` — USB 板卡底层 HAL（`UsbHalHpm`，HPM EHCI）

与 `framework/drivers`（架构驱动层，C++ 外设封装）的分工：**架构 drivers 封装通用外设对象**，
**子树 drivers 放板卡专属底层驱动**。换板卡只动这里，架构层稳定。

### doc

放置架构文档：

- `zephyr_子树架构搭建指南.md` — 从零搭建纯 zephyr 子树环境
- `zephyr_HPM_搭建指南.md` — 在子树基础上添加 HPM SDK
- `zephyr_HPM_底层修改.md` — HPM 底层运行时 bug 修复（直接贴代码）
- `子树架构描述.md` — 每层是干什么的 + 配置规范 + 维护者/使用者关系

### framework

放置**架构子模块**（6 个独立 submodule：`drivers` `algorithm` `modules` `topic` `cmd` `init`）。
**使用者不允许修改**子模块内容；有 bug 或新需求，向维护者提 issue，或在自己的工作区单独维护
（见 `子树架构描述.md` §6.6）。

### platform

放置**平台中间层**支持。当前为 `platform/cmsis`：ARM CMSIS 兼容垫片（`cmsis_core.h` 遮蔽 +
`HAS_CMSIS_CORE` 符号），解决 Zephyr v4.3 与 CMSIS 新旧命名差异。

### project

**维护者业务层工作区**。维护者需要一个能查看/验证所有子树内容的工作环境，因此把它放进子树；
新成员也可先在这里过渡，熟悉架构后再转到自己的使用者工作区。

---

## 3. 为什么设计子树

形成一套完整的**上下级管理关系**：公共能力由维护者集中演进，使用者在自己工作区组合。
防止成员"刚用架构时相同、后期却各不相同"——公共模块只维护一份，产品差异留在各自工作区。

---

## 4. project 的两种身份

- **维护者工作区**：验证所有子树内容。
- **成员模板**：新成员先在 `project` 过渡，熟悉架构子模块用法，后期转向自己的使用者工作区。

---

## 5. 成员工作区必备内容

`project/` 下的内容是每个成员工作区都**必须**有的：

```text
CMakeLists.txt    ← 构建装配（引入 framework + platform/cmsis）
Kconfig           ← 业务开关（select DUST_*）
prj.conf          ← 通用配置
boards/           ← 板卡配置
thread/           ← 业务线程
```

### 配置规范（通用 vs 板卡）

- **`CMakeLists.txt` 和 `prj.conf`**：放置不同板卡的**通用配置**（C++ 环境、调度、总门禁）。
- **板卡特殊配置**：放在 `boards/` 里，不写进根 prj.conf。

### project/boards

项目所需不同板卡的设备树内容，包含：

- `board.cmake`：链接板卡所需的特殊文件——如 st 系列链接 cmsis 相关头文件，hpm 系列链接 sdk。
- `xx.conf`：配置板卡特殊外设或模块——如不同板卡的外设 `conf=y`、挂载不同的 IMU 等模块。
- `overlay`：板卡设备树（引脚、alias、chosen）。

### project/thread

管理用户业务层，文件内容**只有线程**，用于编写业务线程逻辑。通过 `Kconfig + CMake` 拉取编译对应
线程源文件，提供快速增删线程的作用。

- `thread.hpp`：轻量级线程创建类，服务线程创建；每个线程有固定创建格式，详见文件。
- 线程通过设置对应线程的 Kconfig，然后在下面 `select` 对应子模块，即可拉取所需架构子模块。
- 目前架构子模块基本涵盖所需内容；**未涵盖**时，向维护者提需求，或自己仿照子模块编写习惯在
  自己工作区维护，稳定后提交给维护者，经审查后统一上传。
- `project/thread` 还拥有 **test 线程**，通过 `TRD_TEST` 快速搭建测试或临场 demo：

```kconfig
config TRD_TEST
    bool "User test add config"
    default n
    select DUST_xx
    help
      Temporary user test switch.
```

---

## 6. 编译方式

维护者提供一个 bat 脚本：`framework\cmd\build\build.bat`。它根据编译命令所在目录，寻找对应板卡名，
并做了战队编译适配：

```powershell
dust build hpm5361icb          # 普通编译
dust build hpm5361icb -p       # 全编译（pristine）
```

`hpm5361icb` 指 `project\boards\hpm\hpm5361icb\hpm5361icb.overlay`。脚本会顺着文件目录循环查找
是否有符合该名字的设备树进行编译，因此这是**支持所有板卡**的架构，理论上可实现统一逻辑在不同板卡运行。

### 板卡设备树节点命名

不同板卡的设备树节点命名有讲究，都用**通用名**，减少对物理外设的耦合。以
`project\boards\hpm\hpm5361icb\hpm5361icb.overlay` 为例：

```dts
aliases {
    user-can1   = &mcan0;
    remote-uart = &uart4;
    imu-spi     = &icm42688p;
    imu-pwm     = &imu_pwm;
    buzzer-pwm  = &buzzer_pwm;
    flash-spi   = &w25q128;
    pc-usb      = &dustusb_usb0;
};
```

---

## 7. 如何添加自己的工作区

最好在**与子树同级**下创建一个用户工作区总文件夹，再放置自己的业务逻辑。例如：

```text
Zephyr\projects\hpm5361\
Zephyr\projects\temp\
```

使用者内容最小只需要包含 `project/` 下的所有内容；板卡和线程可按需增删。需要自己的独立内容时，
可在自己工作区内创建文件夹，通过 Kconfig 和 CMakeLists 添加并编译。

详细搭建步骤见 [doc/zephyr_子树架构搭建指南.md](doc/zephyr_子树架构搭建指南.md)。

---

## 推荐阅读顺序

**第一阶段 · 读架构文档**（先理解每层是干什么的）：

1. `README.md` — 子树总览
2. `子树架构描述.md` — 每层职责、配置规范、维护者/使用者关系
3. `framework/init` → `framework/cmd` → `framework/topic` → `framework/algorithm` → `framework/drivers` → `framework/modules`
   各自的 `ARCHITECTURE.md` 和 `README.md`

**第二阶段 · 读源码**（再理解具体实现/类）：

1. `project` 源码 — 业务装配入口
2. `framework/init` → `framework/cmd` → `framework/topic` → `framework/algorithm` → `framework/drivers` → `framework/modules` 源码
