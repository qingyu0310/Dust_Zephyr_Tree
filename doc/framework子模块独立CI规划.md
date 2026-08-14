# CI/CD 统一规划（framework 子模块独立 CI + project 多板卡 CI）

> 2026-08-10。给 framework 六个独立仓库子模块各配一份"最小编译 CI"——子模块 push 立即验证编译，不等主仓库 PR。借鉴 XRobot 模块级独立 CI（每个模块仓库自带 GitHub Actions 拉 libxr 编译）的思路，落到 Zephyr 模块环境。
>
> 方案核心（层 1）：一个**全框架聚合骨架工程**（native_sim 上编译无业务 thread 的框架合体），六子模块共用；子模块 CI 拉全框架 + 当前 commit 覆盖 → native_sim 编译。编译错误天然定位到炸的文件。
>
> 2026-08-11 更新：**明确 native_sim 定位**——唯一正当场景是无板卡的 CI 环境 PC 仿真编译（本规划即其用途）；日常开发有板卡直接真机（dust build 不经过 native_sim）；与 Webots 仿真无关（已否决）。术语澄清：免 zephyr-sdk ≠ 免 Zephyr 源码（board 在 Zephyr 树内）；ninja 是底层构建工具、native_sim 是编译目标（绕不开的是 Zephyr 构建链）；非"只编单文件"而是完整 Zephyr 应用（产出 zephyr.exe）。详见 §1.4。
>
> 2026-08-14 更新：**升级为 CI/CD 统一规划**——project 瘦身版由新仓库 `Dust_Zephyr_Architecture_Project` 接管（见 [qingyu接管project仓库规划](qingyu接管project仓库规划.md)），支持 3 板卡（hpm5361icb + board_rm_c/stm32f407igh6 + puzhong/stm32f4_disco），**不能只与 hpm5361 耦合，每次提交须编译所有板卡**。新增**层 2：project 仓库多板卡真板 CI**（§3）。统一三层架构：
> - 层 1：framework 子模块 native_sim CI（§2，原规划主体）
> - 层 2：project 仓库多板卡真板 CI（§3，本次新增，核心）
> - 层 3：主仓库 PR 门禁 `build-hpm5361icb`（§4，保留）
>
> 详见 §0 / §1.5 / §3 / §4。

> **2026-08-14 重写修订记录**（对照代码逐条核证后修正，本次重写落地以下 6 处）：
> 1. **骨架 ci/CMakeLists.txt 缺头文件路径**：`init/Init_entry.cpp` 无条件 include `buzzer.hpp`（cmd/buzzer）+ `log.hpp`（cmd/shell）+ `System_startup.h`（init），骨架必须补 `target_include_directories(app PRIVATE .../init .../cmd)`，否则阶段 1 native_sim 编译必炸。→ 任务 1.1 已补。
> 2. **prj.conf 符号名错**：`CONFIG_DUST_SHELL` 不存在，真实符号是 `CONFIG_DUST_CMD_SHELL`（framework/cmd/Kconfig:1）；`CONFIG_DUST_TPC_*_TO` 是通配符（.conf 文件不支持 `*`），真实符号是 `DUST_TPC_REMOTE_TO` / `DUST_TPC_IMU_TO` / `DUST_TPC_TO_CAN_TX`（framework/topic/Kconfig）。→ 任务 1.3 已改。
> 3. **"shell 不依赖具体硬件"是错的**：`DUST_CMD_SHELL` `select DUST_COM_UART_DMA`，`shell.cpp:150` 硬编码 `DEVICE_DT_GET(DT_ALIAS(shell_uart))`。**native_sim 板卡只有 chosen `zephyr,shell-uart`（native_sim.dts:19），没有 `shell-uart` alias（native_sim.dts:31-37）** → 骨架开 shell 编译炸。骨架 prj.conf **不开** shell/log/var/buzzer/flash（native_sim 无对应设备树 alias/设备）。init 层宏是空实现（log.hpp #else 空宏、buzzer.hpp #else 空/while(1)），不开这些 CONFIG 也能编。→ 任务 1.3 注释与符号已改。
> 4. **阶段顺序矛盾**：原阶段 2 workflow 从主仓库 sparse 拉 `ci/` 目录，但 `ci/` 原定阶段 3.3 才提交主仓库 → 阶段 2 试点时 `ci/` 不存在。→ 主仓库提交 `ci/` 提前到**阶段 1 末尾**。
> 5. **层 2 CI `Pin current project` 的 `${{ github.sha }}` 在 PR 事件是 merge SHA**：project 仓库 `git fetch origin <merge-sha>` 拉不到 → PR 触发失败。→ 改用 `${{ github.event.pull_request.head.sha || github.sha }}`。
> 6. **层 2 CI checkout 主仓库 submodules recursive 的前置**：主仓库 project 子模块指针目前**未更新**（qingyu 接管规划的"后续动作"），主仓库 gitlink 指向的旧 commit 在新仓库（孤儿分支瘦身版）**不存在** → `submodules recursive` 拉 project 子模块会失败。→ 层 2 CI 落地**前置：先更新主仓库 project 指针**（见 §3.3 / 阶段 4）。
>
> 用户拍板（2026-08-14）：**层 1 子模块 CI 统一 clone 全部六子模块**（当前子模块 Pin 当前 SHA，其余用 master）——骨架 add_subdirectory / Kconfig rsource 零容错，最简可靠。→ 阶段 2/3 workflow 已按此改。
> 7. **补充上传分支规矩（§4.0）**：上传侧分支规矩（主仓库必须 PR / 子模块直推 master / 旧分支残留坑 / workflow 触发分支）补入文档——CI 只管"push 后自动编译"，分支上传是上传动作侧的规矩，两套都要落地。
> 8. **native_sim 只支持 Linux（§1.4，2026-08-14 实测）**：Windows 编不了（POSIX arch 平台检查报错）。本地验证改为"配置阶段通过即可，真编交 CI"（用户拍板）；任务 1.2 module.yml 的 `kconfig` 值相对模块根 = `zephyr/Kconfig`（写 `Kconfig` 报 "does not point to a valid Kconfig file"，已修）。

## 0. 背景与目标

### 现状（问题）

- framework 六个子模块（drivers/algorithm/modules/topic/cmd/init）是**独立 GitHub 仓库**（`Dust_Zephyr_Architecture_*`），但**没有任何自己的 CI**。
- 编译验证只在主仓库 `zephyr_user` 的 PR 门禁里做——`.github/workflows/ci-build.yml` 的 `build-hpm5361icb` job（hpm5361icb 板 + HPM SDK_GLUE 环境）。
- 后果：子模块作者 push 后，**要等主仓库下一次 PR 才知道编不编译得过**。编译炸了也不知道是哪个子模块改的。
- **project 瘦身版（2026-08-14 新仓库接管）支持 3 板卡**——`hpm5361icb`（HPM，riscv64）+ `board_rm_c/stm32f407igh6`（STM32 自定义板，arm）+ `puzhong/stm32f4_disco`（STM32 官方板，arm）。**现有 CI 只编 hpm5361icb**（只装 riscv64 工具链），project 改动若只影响 st 板卡，CI 发现不了 = project 与 hpm5361 单一耦合。

### 目标

**统一三层 CI/CD**（framework 子模块 → project → 主仓库门禁）：

- **层 1（framework 子模块）**：每个 framework 子模块仓库自带一份 GitHub Actions：**push / PR / 每月定时**，native_sim 编译框架聚合骨架。编译通过 = 该子模块改动没炸。粒度靠编译错误定位（错误天然指向文件）。
- **层 2（project 仓库）**：`Dust_Zephyr_Architecture_Project` 自带多板卡 CI——**每次提交（push/PR）编译所有板卡**（hpm5361icb + stm32 ×2），project 不单耦合 hpm5361。
- **层 3（主仓库门禁）**：主仓库 PR 门禁保留 `build-hpm5361icb`，验证主仓库自身改动 + 子模块指针；不重复跑 project 的 3 板卡矩阵（project 内容已在层 2 验过）。

### 借鉴来源

XRobot 的模块级独立 CI（`xrobot_create_mod` 生成的 `build.yml`）：模块仓库自带 workflow，CI 里拉 libxr 依赖 + 生成 main + `cmake && make` 编译验证。详见调研文档第 7.1 节。

## 1. 现状盘点（证据）

### 1.1 主仓库 CI（复用环境基准）

`.github/workflows/ci-build.yml`：

| 项 | 值 |
| --- | --- |
| 触发 | push(master) / pull_request / workflow_dispatch |
| 板卡 | hpm5361icb |
| 环境 | ubuntu + Zephyr SDK 0.16.8（riscv64）+ west workspace + HPM SDK_GLUE（sdk_glue + sdk_glue_user + CherryUSB + apply-patches） |
| 构建 | `west build -p always -b hpm5361icb`（在 `zephyr_user/project`） |
| 依赖仓库 | zephyr@v4.3.0、`qingyu0310/Dust_Zephyr_HPMicro_Tree`（sdk_glue_user） |

主仓库 CI **重**（HPM SDK + patch），子模块 CI 应**轻**——native_sim 不需要 HPM SDK / zephyr-sdk（用宿主编译器）。

**局限（2026-08-14）**：只装 `riscv64-zephyr-elf` 工具链（`setup.sh -t riscv64-zephyr-elf`，ci-build.yml:58），**只编 hpm5361icb**；project 的 st 板卡（arm 工具链）完全未覆盖。

### 1.2 framework 六子模块

`.gitmodules` 记录的仓库：

```text
framework/drivers    → qingyu0310/Dust_Zephyr_Architecture_Drivers
framework/algorithm  → qingyu0310/Dust_Zephyr_Architecture_Algorithm
framework/modules    → qingyu0310/Dust_Zephyr_Architecture_Modules
framework/topic      → qingyu0310/Dust_Zephyr_Architecture_Topic
framework/cmd        → qingyu0310/Dust_Zephyr_Architecture_Cmd
framework/init       → qingyu0310/Dust_Zephyr_Architecture_Init
```

### 1.3 子模块编译期依赖（决定骨架要拉哪些）

grep 实证（`framework/` 下）：

| 子模块 | 编译期依赖 | 证据 |
| --- | --- | --- |
| algorithm | **无**（仅 C/C++ 标准库 + Zephyr 内核计时 ExecTimer） | algorithm/README.md:6,338 |
| topic | Zephyr IPC（zbus/k_msgq） | — |
| init | 无（main 入口 + 注册宏，始终编译；**无条件 include cmd 的 buzzer.hpp/log.hpp**） | framework/zephyr/Kconfig:11 注释；[init/Init_entry.cpp:14-15](framework/init/Init_entry.cpp#L14-L15) |
| cmd | drivers（shell 用 `Stream`）、Zephyr 内核（log 的 k_sem） | cmd 依赖 log.hpp → drivers/stream |
| drivers | cmd（`log.hpp`，全部 communication/device 驱动引用） | `grep -rln cmd/shell drivers/*.cpp` 实证 |
| modules | drivers + algorithm + topic | 电机/IMU 调用驱动接口 |

注意两点（2026-08-14 修订）：
- cmd ↔ drivers **互相依赖**（cmd 的 shell 用 drivers 的 Stream，drivers 的 log.hpp 在 cmd）。所以骨架不能只编单个子模块，必须带依赖。
- **init 无条件 include cmd 的 buzzer.hpp / log.hpp**（Init_entry.cpp:14-15），但两个头文件内部有 `#ifdef CONFIG_DUST_CMD_BUZZER` / `#ifdef CONFIG_DUST_CMD_SHELL_LOG` 保护，**CONFIG 关时宏为空/空实现**（log.hpp #else 空宏、buzzer.hpp #else EXEC_BUZZER_SHORT() 空 + EXEC_BUZZER_ERR while(1)）。所以 init 层**不需要**骨架开 shell/log/buzzer 就能编——只要求 cmd 的 include 路径可达（见 §4 阶段 1 任务 1.1）。

### 1.4 native_sim 现状

**定位（2026-08-11 明确）**：native_sim 的唯一正当场景是**无板卡的 CI 环境 PC 仿真编译**（即本规划用途）。维护者日常开发验证**不用 native_sim**——有板卡（hpm5361icb/STM32）直接真机跑更真实，`dust build` 编真板与 native_sim 无关；native_sim 模拟环境跑完还得真机再验，是重复。**与 Webots 仿真无关**（Webots 方案已否决，见 [Webots仿真接入规划](Webots仿真接入规划.md)：Zephyr 固件无法作为 Webots 控制器，需 OS 抽象=架构归零）。native_sim 就是 Zephyr 本尊在 PC 上编译运行，**不引入任何 OS 抽象**，架构原封不动。

**术语澄清**：
- **免 zephyr-sdk ≠ 免 Zephyr 源码**——native_sim 免的是交叉编译工具链（zephyr-sdk）与板卡 SDK（HPM SDK），但**必须 git clone Zephyr 源码**（`zephyr/boards/native/native_sim/` 就在 Zephyr 树里，对应 §2.3 构建布局的 `zephyr/` 目录）。
- **ninja 是构建工具，native_sim 是编译目标**——`west build -b native_sim` 底层就是 cmake（配置）→ ninja（编译）。绕不开的是 Zephyr 构建链本身（Kconfig 裁剪 / DTS 展开 / 链接段收集 / 内核符号），裸编译 framework 过不去。
- **不是"只编单文件"**——native_sim 把真板卡定义换成一块模拟板（用自带 `native_sim.dts`），其余流程与真板编译一模一样：编译全部源码 + Zephyr 内核，走完整 Kconfig/DTS/链接段，产出 `zephyr.exe`。CI 只编"无业务 thread 的框架合体"，是因为骨架工程本身不含业务 thread。

- 官方 Zephyr 树已有 `boards/native/native_sim`（`E:\Zephyr\zephyr\boards\native\native_sim\`），含 `native_sim.dts`/`native_sim_defconfig`。
- 用户仓库**无任何 native_sim 配置**（无 boards/native、无 native_sim overlay/conf）。
- 用户全仓 `native_sim` 只在调研文档/ build 产物里出现，**无落地**。
- **native_sim 用宿主编译器（gcc/clang），免 zephyr-sdk（交叉工具链）与 HPM SDK（板卡 SDK），但仍需拉 Zephyr 源码** —— CI 环境相比真板编译轻（省 SDK 安装，Zephyr 源码照拉）。
- **⚠️ native_sim 只能在 Linux 上编译（2026-08-14 实测）**：POSIX arch 在 Windows/macOS 上直接报 `The POSIX architecture only works on Linux`（arch/posix/CMakeLists.txt:4）。**本地 Windows 验证只能走到配置阶段**（Kconfig 合并 / dts 生成 / host 工具链找到全部正常，终点 = POSIX 平台检查）；native_sim 真实编译只能在 ubuntu（CI）上做。用户拍板（2026-08-14）：**配置阶段通过即可，真编交 CI**。

**native_sim 板卡 UART/设备树事实（2026-08-14 补充，决定骨架 CONFIG 边界）**：
- `native_sim.dts` 有 `uart0`（compatible `zephyr,native-pty-uart`）+ chosen `zephyr,console = &uart0` / `zephyr,shell-uart = &uart0`（native_sim.dts:17-29）。
- **但 aliases 段只有 eeprom-0/i2c-0/spi-0/led0/rtc，没有 `shell-uart`**（native_sim.dts:31-37）。
- 自研 shell 用 `DT_ALIAS(shell_uart)`（shell.cpp:150，不是 `DT_CHOSEN`）→ **native_sim 上开 `CONFIG_DUST_CMD_SHELL` 编译错**（`DT_ALIAS(shell_uart)` 无定义）。
- native_sim 的 uart0 是 pty uart，**不是 DMA uart**——`DUST_COM_UART_DMA`（UartDma 自研驱动）没有对应设备。
- **结论**：骨架 prj.conf 不开 shell/log/var/buzzer/flash，也不开任何 drivers 硬件类 CONFIG（CAN/UART DMA/USB/PWM/SPI/GPIO Output）。

### 1.5 project 板卡结构（2026-08-14，决定层 2 矩阵）

`zephyr_user/project/boards/`（瘦身版，新仓库 `Dust_Zephyr_Architecture_Project` 接管）：

| 板卡目录 | 实际板名 | 工具链 | 构建依赖 | 构建命令 |
| --- | --- | --- | --- | --- |
| `boards/hpm/hpm5361icb/` | `hpm5361icb` | riscv64-zephyr-elf | HPM SDK_GLUE + CherryUSB + apply-patches | `west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb` |
| `boards/st/board_rm_c/` | `stm32f407igh6` | arm-zephyr-eabi | `zephyr_user/platform/cmsis`（board.cmake 的 BOARD_GLOBAL_INCLUDES）+ BXCAN/BMI088 | `west build -p always -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c` |
| `boards/st/puzhong/` | `stm32f4_disco` | arm-zephyr-eabi | hal/stm32（west 模块）+ DMA_STM32 | `west build -p always -b stm32f4_disco -- -DBOARD_CFG=puzhong` |

- **CMakeLists 板卡匹配机制**：`file(GLOB ... boards/*/${BOARD_CFG}/${BOARD}.overlay)`——`BOARD_CFG` 是板卡目录名（hpm5361icb/board_rm_c/puzhong），`BOARD` 是 Zephyr 实际板名。CI 必须传 `-DBOARD_CFG`（见 [project/CMakeLists.txt:27-31](project/CMakeLists.txt#L27-L31)）。
- **project 不是自包含构建单元**：project/CMakeLists 引用 `${ZEPHYR_USER_DIR}`（= zephyr_user）下的 framework 六子模块 + `platform/cmsis`——**层 2 CI 必须 checkout 主仓库 `Dust_Zephyr_Tree`（submodules recursive 拉 framework）+ 覆盖当前 project commit**。
- **stm32f407igh6 是自定义板**（`board_rm_c`，真实比赛板）：board.cmake 的 BOARD_GLOBAL_INCLUDES 引用 `${ZEPHYR_USER_DIR}/platform/cmsis` 与兄弟目录 `modules/hal/cmsis/CMSIS/Core/Include`；stm32f4_disco 是 Zephyr 官方板（overlay/conf 在 puzhong 目录覆盖）。

## 2. 层 1 目标结构（framework 子模块 native_sim CI）

### 2.1 骨架工程（放主仓库 `zephyr_user/ci/`，一份维护）

```text
zephyr_user/ci/
├── CMakeLists.txt          # 全框架聚合（无业务 thread）
├── prj.conf                # 框架各层 CONFIG 按需打开（只开 native_sim 能编的纯逻辑层）
├── zephyr/
│   ├── module.yml          # Zephyr 模块挂载（kconfig 聚合）
│   └── Kconfig             # rsource ../framework/各子模块 Kconfig
└── README.md               # 骨架用法说明
```

### 2.2 各子模块仓库（一份 workflow，照抄）

```text
Dust_Zephyr_Architecture_Algorithm/
└── .github/workflows/ci.yml   # 六子模块同一模板（改 checkout 路径）
```

### 2.3 构建布局（CI 组装）

```text
workspace/
├── zephyr/           # git clone zephyr@v4.3.0
├── framework/        # git clone 六子模块（master），当前子模块 checkout 当前 SHA
│   ├── drivers/
│   ├── algorithm/
│   ├── modules/
│   ├── topic/
│   ├── cmd/
│   └── init/
├── ci/               # sparse checkout 主仓库的 ci/ 目录
└── build/            # west build 输出
```

**拉取策略（2026-08-14 用户拍板）**：每个子模块 CI **统一 clone 全部六子模块**（当前子模块 checkout 当前 SHA，其余 clone master）。骨架 `add_subdirectory` / `ci/zephyr/Kconfig` 的 rsource **零容错**——目录必须全在。algorithm 虽零编译依赖，也 clone 其余五个（纯源码 clone，native_sim 免 SDK，成本可接受；换取骨架最简单可靠）。

## 3. 层 2：project 仓库多板卡 CI（2026-08-14 新增，核心）

### 3.1 目标与板卡矩阵

**目标**：`Dust_Zephyr_Architecture_Project` 仓库自带 CI——**每次提交（push/PR）编译所有板卡**，project 不再单耦合 hpm5361。

**矩阵**（新增板卡 = 加一行）：

| board | cfg（BOARD_CFG） | sdk_target | need_hpm | 依赖 |
| --- | --- | --- | --- | --- |
| hpm5361icb | hpm5361icb | riscv64-zephyr-elf | true | HPM SDK_GLUE |
| stm32f407igh6 | board_rm_c | arm-zephyr-eabi | false | platform/cmsis |
| stm32f4_disco | puzhong | arm-zephyr-eabi | false | hal/stm32 |

### 3.2 workflow 设计

**文件**：`Dust_Zephyr_Architecture_Project/.github/workflows/ci.yml`（old：无 → new）：

```yaml
name: Project Multi-Board CI

on:
  push:
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build:
    name: build-${{ matrix.board }}
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false          # 一块板失败不取消其他
      matrix:
        include:
          - board: hpm5361icb
            cfg: hpm5361icb
            sdk_target: riscv64-zephyr-elf
            need_hpm: true
          - board: stm32f407igh6
            cfg: board_rm_c
            sdk_target: arm-zephyr-eabi
            need_hpm: false
          - board: stm32f4_disco
            cfg: puzhong
            sdk_target: arm-zephyr-eabi
            need_hpm: false
    env:
      ZEPHYR_BASE: ${{ github.workspace }}/zephyr
      ZEPHYR_SDK_INSTALL_DIR: /opt/zephyr-sdk-0.16.8
      ZEPHYR_TOOLCHAIN_VARIANT: zephyr
      SDK_GLUE_DIR: ${{ github.workspace }}/hpm/zephyr_sdk_glue
      SDK_GLUE_USER_DIR: ${{ github.workspace }}/hpm/sdk_glue_user
    steps:
      # 主仓库（framework 六子模块 + platform/cmsis + project 子模块）
      # ⚠️ 前置：主仓库 project 子模块指针必须先更新到新仓库瘦身 commit，否则 recursive 拉不到（见 §3.3）
      - uses: actions/checkout@v4
        with:
          repository: qingyu0310/Dust_Zephyr_Tree
          path: zephyr_user
          submodules: recursive

      # 覆盖 project 到当前提交（防主仓库指针旧）
      # PR 事件下 github.sha 是 merge SHA（project 仓库 fetch 不到），改用 head.sha
      - name: Pin current project
        run: |
          cd ${{ github.workspace }}/zephyr_user/project
          git fetch --depth 1 origin ${{ github.event.pull_request.head.sha || github.sha }}
          git checkout ${{ github.event.pull_request.head.sha || github.sha }}

      # 系统依赖
      - name: Install system deps
        run: |
          sudo apt-get update
          sudo apt-get install --no-install-recommends -y \
            git cmake ninja-build gperf ccache dfu-util device-tree-compiler \
            python3-dev python3-pip python3-setuptools python3-tk xz-utils \
            file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1

      # HPM 树（提供 sdk_glue_user + apply-patches，仅 hpm 板卡）
      - name: Checkout HPM tree
        if: matrix.need_hpm
        uses: actions/checkout@v4
        with:
          repository: qingyu0310/Dust_Zephyr_HPMicro_Tree
          path: hpm/sdk_glue_user

      # Zephyr SDK（按板卡工具链，缓存 key 区分）
      - name: Cache Zephyr SDK
        id: cache-sdk
        uses: actions/cache@v4
        with:
          path: /opt/zephyr-sdk-0.16.8
          key: zephyr-sdk-0.16.8-${{ matrix.sdk_target }}

      - name: Install Zephyr SDK
        if: steps.cache-sdk.outputs.cache-hit != 'true'
        run: |
          curl -sL https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz | \
            sudo tar -xJ -C /opt
          sudo /opt/zephyr-sdk-0.16.8/setup.sh -t ${{ matrix.sdk_target }} -c

      # west + zephyr 树
      - name: Setup west workspace
        run: |
          python3 -m pip install west
          git clone --depth 1 --branch v4.3.0 https://github.com/zephyrproject-rtos/zephyr.git ${{ github.workspace }}/zephyr
          mkdir -p ${{ github.workspace }}/.west
          printf '[manifest]\npath = zephyr\nfile = west.yml\n' > ${{ github.workspace }}/.west/config
          cd ${{ github.workspace }}/zephyr && python3 -m pip install -r scripts/requirements.txt
          west update

      # HPM SDK（仅 hpm 板卡）
      - name: Clone HPM official repos
        if: matrix.need_hpm
        run: |
          git clone https://github.com/hpmicro/zephyr_sdk_glue.git ${{ github.workspace }}/hpm/zephyr_sdk_glue
          git clone --depth 1 --filter=blob:none --sparse --branch v1.11.0 https://github.com/hpmicro/sdk_env.git ${{ github.workspace }}/hpm/sdk_env
          git -C ${{ github.workspace }}/hpm/sdk_env sparse-checkout set hpm_sdk
          git clone https://github.com/cherry-embedded/CherryUSB.git ${{ github.workspace }}/hpm/modules/lib/CherryUSB

      - name: Apply HPM patches
        if: matrix.need_hpm
        run: |
          bash ${{ github.workspace }}/hpm/sdk_glue_user/apply-patches.sh \
            --zephyr ${{ github.workspace }}/zephyr \
            --sdk-env ${{ github.workspace }}/hpm/sdk_env \
            --sdk-glue ${{ github.workspace }}/hpm/zephyr_sdk_glue \
            --cherryusb ${{ github.workspace }}/hpm/modules/lib/CherryUSB

      # 构建
      - name: Build ${{ matrix.board }}
        run: |
          cd ${{ github.workspace }}/zephyr_user/project
          west build -p always -b ${{ matrix.board }} -- -DBOARD_CFG=${{ matrix.cfg }}

      # 产物
      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware-${{ matrix.board }}
          path: |
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.elf
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.map
          if-no-files-found: error
```

### 3.3 环境组装与关键点

- **checkout 主仓库而非自包含**：project 依赖 zephyr_user 的 framework 六子模块 + `platform/cmsis`（§1.5），CI 组装 = checkout 主仓库（submodules recursive 拉 framework，project 子模块拉的是主仓库记录指针）+ `Pin current project` 覆盖当前 commit（与层 1 子模块 CI 的 "Pin current submodule" 同思路）。
- **⚠️ 硬前置：主仓库 project 子模块指针必须已更新到新仓库瘦身 commit**。当前（2026-08-14）主仓库 .gitmodules 的 project URL 已指向新仓库 `Dust_Zephyr_Architecture_Project`，但**指针还是旧 commit**（qingyu 接管规划的"后续动作"，未做）。新仓库是孤儿分支瘦身版（main=4c675b3，只有 1 个提交）——**主仓库 gitlink 指向的旧 commit 在新仓库不存在** → `submodules: recursive` 拉 project 子模块直接失败。**必须先完成主仓库 project 指针更新（PR + auto-merge），层 2 CI 才能落地**。
- **HPM 步骤全部 `if: matrix.need_hpm` 门禁**：只有 hpm5361icb job 走 HPM 树 + HPM SDK + patches，st job 跳过——**st 板卡不装 HPM SDK，轻**。
- **工具链按矩阵**：hpm 装 riscv64，st 装 arm（`setup.sh -t ${{ matrix.sdk_target }}`），SDK 缓存 key 区分避免互相覆盖。
- **`-DBOARD_CFG` 必须传**：匹配板卡目录（hpm5361icb/board_rm_c/puzhong），漏传则 glob 找不到 overlay。
- **framework 用主仓库指针版本**：project CI 用主仓库 .gitmodules 记录的 framework 指针；framework 自身改动由层 1 native_sim CI 验证，主仓库指针更新后 project CI 自然跟随。

### 3.4 验证

- [ ] project 仓库 push 触发 3 个 job，hpm5361icb/stm32f407igh6/stm32f4_disco 全编译绿
- [ ] st 板卡 job 不装 HPM SDK（日志无 sdk_env/CherryUSB 步骤）
- [ ] 只改 st 板卡相关代码 → CI 能暴露（此前只编 hpm5361 发现不了）
- [ ] project 仓库 PR（非 push）触发时 `Pin current project` 成功（head.sha 生效）

## 4. 分阶段执行

### 4.0 上传分支规矩（所有阶段通用，2026-08-14 补充）

> CI 只管"push 后自动编译"；**push 到哪个分支、用不用 PR、有没有踩旧分支坑**是上传动作侧的规矩，与 CI 配套执行。来源：CI/CD 记忆 + git 记忆（旧分支残留坑）。

**各仓库 push 方式（与记忆一致）**：

| 仓库 | push 方式 | 说明 |
| --- | --- | --- |
| 主仓库 zephyr_user | **必须 PR**（GH013）+ 每个 PR 点 Enable auto-merge | master 直推被 GitHub 服务端拒绝；CI 通过后不会自动合并，必须 PR 上点 auto-merge |
| framework 六子模块 / project / HPM 树 | **直推 master，无门禁** | 不用 PR；子模块 push 即触发各自 CI（framework→层 1，project→层 2） |

**旧分支残留坑（最容易踩，务必先防）**：
- 合并删分支的旧分支名**不能继续用**——在残留本地分支上干活再 push，输出 `[new branch]` 重建同名分支 → 与 master 冲突，被迫 reset/force push 救场。
- **主动预防**：在旧分支干活/上传前，先 `git fetch origin && git log --oneline origin/master..HEAD` 验证——若本分支提交已带 PR 编号（#N）或 origin/master 已包含本分支内容，立即 `git checkout master && git pull` 从最新 master 开新分支。**不要等 push 报 `[new branch]` 才反应**。

**workflow 触发分支**：
- 主仓库 ci-build.yml：push 限 `branches: [master]`（主仓库只经 PR 合流到 master）。
- 层 1 / 层 2 workflow（§2.2 / §3.2）：`on: push` **不限分支**——子模块直推 master + 临时分支 push 都提前编译验证，pull_request 事件也触发（PR 验证）。不限比限 master 更稳，不用改。

**上传前 HPM 树对齐（硬规定，记忆）**：上传任何仓库前先看 `Dust_Zephyr_HPMicro_Tree`（`E:\Zephyr_HPMicro\sdk_glue_user`）有无未推送改动；有改动先推 HPM 树，否则主仓库/子模块 CI 编译过不了（缺底层 pinctrl/dtsi）。

---

### 阶段 1：骨架工程（主仓库 `zephyr_user/ci/`）+ 提交主仓库

**目标**：native_sim 上能编译"框架聚合（无业务 thread）"的最小工程，并**提交主仓库**（阶段 2 试点的 workflow 要 sparse 拉 `ci/`，必须先入库）。

**任务 1.1 — `ci/CMakeLists.txt`**（old：无 → new；**含修订 1：补 target_include_directories**）：

```cmake
cmake_minimum_required(VERSION 3.20.0)

# 全框架聚合骨架：无业务 thread，native_sim 编译验证 framework 合体
set(FRAMEWORK "${CMAKE_CURRENT_SOURCE_DIR}/../framework")   # 指向 CI 组装的 framework/

# 各子模块作为 Zephyr 模块挂载（ci/zephyr/module.yml 已聚合 Kconfig）
set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}" CACHE STRING "" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ci_framework)

# 头文件路径：init/Init_entry.cpp 无条件 include cmd 的 buzzer.hpp/log.hpp + init 的 System_startup.h
# （必加，否则阶段 1 native_sim 编译必炸；与 project/CMakeLists.txt:121-124 同构）
target_include_directories(app PRIVATE
    ${FRAMEWORK}/init
    ${FRAMEWORK}/cmd
)

# 装配 framework 各层（与 project/CMakeLists.txt:110-115 同构，但无 project/thread）
# ⚠️ 六目录必须全在（子模块 CI 统一 clone 全部六子模块，见 §2.3）
add_subdirectory(${FRAMEWORK}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
add_subdirectory(${FRAMEWORK}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
add_subdirectory(${FRAMEWORK}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
add_subdirectory(${FRAMEWORK}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
add_subdirectory(${FRAMEWORK}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
add_subdirectory(${FRAMEWORK}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)
```

**任务 1.2 — `ci/zephyr/module.yml` + `ci/zephyr/Kconfig`**：

`ci/zephyr/module.yml`（**`kconfig` 值相对模块根 `ci/`，须写 `zephyr/Kconfig`，与 framework/zephyr/module.yml 同构；写 `Kconfig` 会报 "does not point to a valid Kconfig file"**）：
```yaml
name: dust_framework_ci
build:
  kconfig: zephyr/Kconfig
```

`ci/zephyr/Kconfig`（rsource 相对本文件目录，指向 CI 组装的 framework/；**与 framework/zephyr/Kconfig 同构**）：
```kconfig
# framework 聚合 Kconfig（CI 骨架用）
# 与 framework/zephyr/Kconfig 同构；rsource 相对 ci/zephyr/
rsource "../../framework/drivers/Kconfig"
rsource "../../framework/algorithm/Kconfig"
rsource "../../framework/modules/Kconfig"
rsource "../../framework/topic/Kconfig"
rsource "../../framework/cmd/Kconfig"
rsource "../../framework/drivers/communication/stream/usb/Kconfig"
# init 无独立 Kconfig 且始终编译，不 rsource
```

**任务 1.3 — `ci/prj.conf`**（**含修订 2/3：符号名改对 + 不开 shell/log/buzzer**）：

只开 native_sim 能编的**纯逻辑层** CONFIG。**不开** shell/log/var/buzzer/flash（自研 shell 依赖 `DT_ALIAS(shell_uart)`，native_sim 无此 alias，见 §1.4）；**不开** drivers 硬件类（CAN/UART DMA/USB/PWM/SPI/GPIO Output）。
```kconfig
# 纯逻辑层全开（验证语法/模板/头文件）
CONFIG_DUST_BUF_BIPBUF=y
CONFIG_DUST_BUF_RINGBUF=y
CONFIG_DUST_CTL_PID=y
CONFIG_DUST_CTL_TIMER=y
CONFIG_DUST_CTL_EXECTIMER=y
CONFIG_DUST_FLT_HPF=y
CONFIG_DUST_FLT_KALMAN=y
CONFIG_DUST_FLT_KALMAN_EKF=y
CONFIG_DUST_FLT_LPF=y
CONFIG_DUST_FLT_QUATERNION=y
CONFIG_DUST_ID_RLS=y
CONFIG_DUST_ID_MOTOR_PLANT=y
CONFIG_DUST_MATH_EIGEN=y
CONFIG_DUST_MOD_CTL_POWER=y
# topic 数据通道（zbus/msgq，native_sim 支持）
CONFIG_DUST_TPC_REMOTE_TO=y
CONFIG_DUST_TPC_IMU_TO=y
CONFIG_DUST_TPC_TO_CAN_TX=y
# ⚠️ 不开 shell/log/var/buzzer/flash：
#   DUST_CMD_SHELL select DUST_COM_UART_DMA，shell.cpp:150 用 DT_ALIAS(shell_uart)
#   native_sim 无 shell-uart alias → 开必炸；init 层宏为空实现，不开也能编
```
> 执行时按 native_sim 编译报错逐条裁剪/补全——**prj.conf 的目标是"本机可编的全部框架 CONFIG"**，具体集合在首次编译后定稿，本规划不臆测每个符号。**注意：`.conf` 文件不支持通配符**（原草案 `CONFIG_DUST_TPC_*_TO=y` 无效），一律写全符号名。

**任务 1.4 — `ci/README.md`**：骨架用法说明（怎么组装 framework/、怎么 `west build -b native_sim`、CONFIG 边界）。

**任务 1.5 — 提交 `ci/` 到主仓库**（old：无 → new）：`ci/` 目录 commit，走主仓库 PR + auto-merge（GH013 门禁，见 CI/CD 记忆）。**提交后才进阶段 2**（workflow 要 sparse 拉 `ci/`）。

**产出**：`zephyr_user/ci/{CMakeLists.txt, prj.conf, zephyr/module.yml, zephyr/Kconfig, README.md}`，主仓库 master 已含 `ci/`。

**验证**：本地 `west build -b native_sim ci` 走到**配置阶段通过**（Kconfig 合并 / dts 生成 / host 工具链找到；**Windows 终点 = POSIX arch 平台检查，非错误**，见 §1.4）；native_sim **真实编译由阶段 2 CI（ubuntu）首次验证**（用户 2026-08-14 拍板"配置阶段通过即可，真编交 CI"）；主仓库 PR 合并后 master 有 `ci/`。

---

### 阶段 2：试点子模块 CI（algorithm——零编译依赖）

**目标**：algorithm 仓库带一份 `ci.yml`，push 即 native_sim 编译骨架。

**任务 2.1 — `Dust_Zephyr_Architecture_Algorithm/.github/workflows/ci.yml`**（old：无 → new；**含修订 4/修订"用户拍板"：统一 clone 全部六子模块，当前子模块 Pin 当前 SHA**）：

```yaml
name: Framework Submodule CI

on:
  push:
  pull_request:
  schedule:
    - cron: '0 3 1 * *'   # 每月定时回归，防长期不动的子模块悄悄失效

jobs:
  build-native-sim:
    name: build-native-sim
    runs-on: ubuntu-latest
    env:
      ZEPHYR_BASE: ${{ github.workspace }}/zephyr
      ZEPHYR_TOOLCHAIN_VARIANT: host   # native_sim 用宿主编译器，免 zephyr-sdk
    steps:
      # 当前子模块（放到框架聚合位）
      - uses: actions/checkout@v4
        with:
          path: framework/algorithm

      # 其余五个子模块（统一 clone 全部六子模块，当前子模块在下一步 Pin 当前 SHA）
      - name: Clone sibling modules
        run: |
          for m in drivers modules topic cmd init; do
            git clone --depth 1 https://github.com/qingyu0310/Dust_Zephyr_Architecture_${m^}.git \
              ${{ github.workspace }}/framework/$m
          done

      # zephyr 树
      - name: Clone zephyr
        run: |
          git clone --depth 1 --branch v4.3.0 https://github.com/zephyrproject-rtos/zephyr.git ${{ github.workspace }}/zephyr
          python3 -m pip install west
          cd ${{ github.workspace }}/zephyr && python3 -m pip install -r scripts/requirements.txt

      # 骨架工程（sparse 从主仓库拉 ci/，前置：阶段 1.5 已提交主仓库）
      - name: Checkout CI skeleton
        run: |
          git clone --depth 1 --filter=blob:none --sparse https://github.com/qingyu0310/Dust_Zephyr_Tree.git ${{ github.workspace }}/ci_main
          cd ${{ github.workspace }}/ci_main && git sparse-checkout set ci
          mv ci ${{ github.workspace }}/ci

      # 当前子模块覆盖到当前 commit（防 master 指针漂移）
      # PR 事件下 github.sha 是 merge SHA，用 head.sha
      - name: Pin current submodule
        run: |
          cd ${{ github.workspace }}/framework/algorithm
          git fetch --depth 1 origin ${{ github.event.pull_request.head.sha || github.sha }}
          git checkout ${{ github.event.pull_request.head.sha || github.sha }}

      # 构建
      - name: Build native_sim
        run: |
          cd ${{ github.workspace }}/ci
          west build -p always -b native_sim -d ${{ github.workspace }}/build
```

> **仓库 URL 注意**：`Dust_Zephyr_Architecture_${m^}` 的 `${m^}` 是 bash 首字母大写（drivers→Drivers，modules→Modules，topic→Topic，cmd→Cmd，init→Init）。若 GitHub 仓库名大小写与 `Dust_Zephyr_Architecture_*` 实际不一致，改为手写五个 `git clone` 行，不依赖 bash 大小写转换。

**任务 2.2 — 验证 workflow**：push 到 algorithm 仓库，CI 绿。若 native_sim 编译缺符号，回阶段 1 调 `ci/prj.conf`（裁剪/补 CONFIG）。

**产出**：algorithm 仓库 `.github/workflows/ci.yml`。

**验证**：push 触发，native_sim 编译通过（编译是用户动作，此处为用户验证项）。

---

### 阶段 3：推广到六子模块

**目标**：六子模块全部带 `ci.yml`（同一模板，只改 checkout 路径）。

**任务 3.1 — 模板复制**（每子模块 workflow 的差异仅 2 处）：
- `actions/checkout` 的 `path`：`framework/algorithm` → `framework/<本模块>`
- `Pin current submodule` 的 `cd` 路径：`framework/<本模块>`

**注意**：**不再需要原"依赖 clone 段映射"**——阶段 2 已统一 clone 全部六子模块，cmd↔drivers 互相依赖、modules 依赖 drivers+algorithm+topic 等全部由"clone 全部"覆盖，无需按依赖挑（§2.3 用户拍板）。

**任务 3.2 — 各子模块 CI 的 prj.conf 侧重**：骨架 `ci/prj.conf` 是共享的（主仓库一份），六子模块共用。需要差异的 CONFIG（如 modules 开电机/IMU 相关）在阶段 1 定稿的 prj.conf 里统一全开（能 native_sim 编的）。modules 的电机/IMU 驱动依赖硬件设备，native_sim 编不了，**阶段 1 定稿时不加**。

**产出**：六子模块各带 `ci.yml`；主仓库含 `ci/` 骨架。

**验证**：六子模块各自 push 触发 CI 全绿。

---

### 阶段 4：project 仓库多板卡 CI（层 2，2026-08-14）

**前置（已完成）**：新仓库 `Dust_Zephyr_Architecture_Project` 已接管当前 project（瘦身版，3 板卡），见 [qingyu接管project仓库规划](qingyu接管project仓库规划.md)。

**前置（2026-08-14 新增，必须做）**：**主仓库 project 子模块指针更新到新仓库瘦身 commit**（qingyu 接管规划的"后续动作"）——当前主仓库 gitlink 指向旧 commit，新仓库（孤儿分支瘦身版）无此 commit，`submodules recursive` 拉 project 会失败（§3.3）。做完再进本阶段。

**目标**：project 仓库自带多板卡 CI——每次提交编译所有板卡。

**任务 4.1 — `Dust_Zephyr_Architecture_Project/.github/workflows/ci.yml`**：按 §3.2 完整 workflow 落地（矩阵 3 板卡 + need_hpm 门禁 + head.sha 修正）。

**任务 4.2 — 验证**：push 触发 3 job（hpm5361icb / stm32f407igh6 / stm32f4_disco），全绿；PR 触发 Pin 成功。

**产出**：project 仓库 `.github/workflows/ci.yml`。

**验证**：3 板卡全编译绿；st job 无 HPM SDK 步骤；只改 st 相关代码 CI 能暴露；PR 事件 Pin 成功。

---

### 阶段 5（可选）：主仓库 CI 联动优化

**目标**：主仓库 CI 不必每次重复验证已由子模块 CI 覆盖的部分。

**任务**：`ci-build.yml` 的 `build-hpm5361icb` 增加"子模块指针未变则跳过"优化（如 `git diff` 检测 `.gitmodules` 指向的子模块 commit）。**本轮不做**，留待六子模块 CI 稳定后评估。

**产出**：无（本轮）。

**验证**：无。

## 5. 验证标准（汇总）

- [ ] `zephyr_user/ci/` 骨架工程 native_sim 编译通过（无业务 thread）
- [ ] 主仓库 master 已含 `ci/` 骨架（阶段 1.5 PR 合并）
- [ ] algorithm 子模块 push 触发 CI 且编译绿
- [ ] 六子模块各带 `ci.yml`，各自 push 全绿
- [ ] 编译错误能定位到具体子模块文件（验证"粒度"目标达成）
- [ ] 主仓库 `build-hpm5361icb` 仍绿（骨架不影响主流程）
- [ ] 主仓库 project 指针已更新到新仓库瘦身 commit
- [ ] project 仓库 push 触发 3 板卡矩阵全绿（hpm5361icb + stm32 ×2）
- [ ] st 板卡改动在 project CI 能暴露（此前只编 hpm5361 发现不了）
- [ ] project 仓库 PR 触发时 `Pin current project` 成功（head.sha）

## 6. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| native_sim 上 drivers 硬件 CONFIG 编不过 | CAN/UART DMA/USB 依赖具体设备，native_sim 无 | 骨架 prj.conf 不开硬件类 CONFIG；真板编译仍由主仓库 `build-hpm5361icb` 兜底 |
| sparse checkout 主仓库拉 ci/ | git 技巧，runner 需支持 `--filter` | 备选：ci/ 独立小仓库或每子模块自带 ci/ |
| 子模块 checkout 当前 commit | 若主仓库指针未更新，submodule 拉旧 | CI 显式 `git fetch + checkout ${GITHUB_SHA}` 覆盖 |
| `ZEPHYR_TOOLCHAIN_VARIANT: host` | native_sim 默认 host 工具链，不需 SDK | 确认 Zephyr v4.3.0 支持该 env（官方 native_sim 默认即 host） |
| 依赖子模块 master 漂移 | 依赖子模块 master 改动导致本子模块编译失败 | 这正是想暴露的问题；若需稳定可改用主仓库子模块指针 |
| cmd↔drivers 互相依赖 | 骨架必须拉对方 | **已由"统一 clone 全部六子模块"覆盖**（§2.3 用户拍板），无需按依赖挑 |
| project CI 拉主仓库旧 project 指针 | submodules recursive 拉主仓库记录的 project 旧 commit | `Pin current project` 显式 fetch + checkout `${GITHUB_SHA}` 覆盖 |
| **主仓库 project 指针未更新**（2026-08-14 新增） | 主仓库 gitlink 指向旧 commit，新仓库（孤儿分支瘦身版）无此 commit → `submodules recursive` 拉 project 失败 | **层 2 CI 硬前置：先更新主仓库 project 指针（阶段 4 前置）**，否则 CI 起不来 |
| **PR 事件下 `github.sha` 是 merge SHA**（2026-08-14 新增） | project/子模块仓库 `git fetch origin <merge-sha>` 拉不到 → Pin 步骤失败 | 改用 `${{ github.event.pull_request.head.sha \|\| github.sha }}` |
| **骨架缺 include 路径**（2026-08-14 新增） | init/Init_entry.cpp 无条件 include cmd 的 buzzer.hpp/log.hpp + init 的 System_startup.h | 骨架 CMakeLists 补 `target_include_directories(app PRIVATE .../init .../cmd)`（任务 1.1） |
| **native_sim 开自研 shell 编译炸**（2026-08-14 新增） | shell.cpp:150 用 `DT_ALIAS(shell_uart)`，native_sim 无此 alias（只有 chosen） | 骨架 prj.conf 不开 DUST_CMD_SHELL/LOG/VAR/BUZZER/FLASH（任务 1.3） |
| **prj.conf 通配符无效**（2026-08-14 新增） | `.conf` 文件不支持 `*` 通配符，`CONFIG_DUST_TPC_*_TO=y` 无效 | 一律写全符号名（任务 1.3） |
| st 板卡依赖 `platform/cmsis` | board_rm_c 的 BOARD_GLOBAL_INCLUDES 引用 zephyr_user/platform/cmsis + modules/hal/cmsis | 层 2 CI checkout 主仓库全量（framework + platform/cmsis），不做 sparse |
| HPM SDK 环境重 | hpm5361icb 需 sdk_glue + sdk_env + CherryUSB + patches（几分钟） | `need_hpm` 矩阵门禁：仅 hpm job 装；st job 轻量；SDK 缓存复用 |
| project CI 用主仓库 framework 指针 | project 编译用主仓库记录的 framework 版本，非 framework 最新 master | 预期行为：framework 改动走层 1 native_sim CI + 主仓库指针更新后 project CI 自然跟随 |

## 7. 执行清单（逐条勾）

- [ ] **上传前分支检查（§4.0，每次 push 前）**：子模块/主仓库确认 push 分支——子模块直推 master、主仓库走 PR + auto-merge；旧分支先 `git fetch origin && git log origin/master..HEAD` 对比，不在残留旧分支上干活；上传前查 HPM 树对齐
- [ ] 阶段 1：主仓库建 `zephyr_user/ci/`（CMakeLists.txt 含 include 路径 / prj.conf 符号改对 / zephyr/module.yml / zephyr/Kconfig / README.md）
- [ ] 阶段 1：本地组装 framework/ 六子模块，用户 native_sim 编译骨架通过
- [ ] 阶段 1：按编译报错定稿 prj.conf CONFIG 集合
- [ ] 阶段 1.5：主仓库提交 `ci/` 骨架（走 PR + auto-merge），master 已含 `ci/`
- [ ] 阶段 2：algorithm 仓库加 `ci.yml`（统一 clone 全部六子模块 + head.sha），用户 push 验证 CI 绿
- [ ] 阶段 3：其余五子模块加 `ci.yml`（同模板，改 checkout 路径）
- [ ] 阶段 4 前置：主仓库更新 project 子模块指针到新仓库瘦身 commit（PR + auto-merge）
- [ ] 阶段 4：project 仓库加 `ci.yml`（§3.2 多板卡 workflow），用户 push 验证 3 板卡全绿
- [ ] 阶段 5（可选）：主仓库 CI 子模块指针未变跳过优化
- [ ] 用户验证：六子模块 push 全绿 + project 3 板卡全绿 + 主仓库 build-hpm5361icb 仍绿
