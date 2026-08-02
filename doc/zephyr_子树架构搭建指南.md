# Zephyr 子树架构搭建指南（不含 HPM SDK）

> 目标：在一台**什么都没有的干净 Windows 电脑**上，从零搭出本套 **zephyr 子树架构**——即
> `zephyr_user`（framework 架构层 + 业务工程）+ 官方 `zephyr` 树，并能编译出 **STM32 板卡**
> 的最小应用。本文档**不含 HPM SDK**，只讲纯 zephyr 的子树搭建。
>
> 本文档基于 2026-08-02 实际搭建完成的状态编写，**冗余优先**：每一步都贴命令、贴源码、解释为什么，
> 照着做即可成功。凡是踩过的坑都收在 [§9 常见问题](#9-常见问题排查)。
>
> 姊妹篇：《zephyr_hpm_重建指南.md》——含 HPM SDK 的完整版（HPM 平台 + sdk_glue + CherryUSB）。

---

## 目录

- [0. 本文档是什么 / 不是什么](#0-本文档是什么--不是什么)
- [1. 架构总览](#1-架构总览)
- [2. 目录规划](#2-目录规划)
- [3. 前置工具安装（从零）](#3-前置工具安装从零)
- [4. 拉取官方 zephyr](#4-拉取官方-zephyr)
- [5. 拉取 zephyr_user 子树](#5-拉取-zephyr_user-子树)
- [6. 建立用户区（应用工程）](#6-建立用户区应用工程)
- [7. 环境变量](#7-环境变量)
- [8. 编译验证](#8-编译验证)
- [9. 常见问题排查](#9-常见问题排查)

---

## 0. 本文档是什么 / 不是什么

**是什么：** 一篇能把"空电脑"变成"能编译 STM32 zephyr 应用"的完整操作手册。它讲的是**纯 zephyr 子树**
这一侧——`zephyr_user` 仓库怎么来、framework 架构层是什么、用户区应用怎么写、怎么编译。

**不是什么：**
- ❌ 不含 HPM（hpm5361icb / sdk_glue / CherryUSB / sdk_env）——那是另一篇《重建指南》的范围。
- ❌ 不含用户业务算法 / 驱动内部实现——只讲**怎么把架子搭起来、能编译**。

**本文解决的核心问题：** 别人克隆到 `zephyr_user` 和 `project` 之后，怎么配环境、怎么编译出固件。

---

## 1. 架构总览（参考）

```text
E:\Zephyr\                        ← 工作区根（workspace）
│
├── zephyr\                       ← 官方 zephyr v4.3.0（git clone，与官方仓库一致）
│     └── west.yml                ← zephyr 官方 manifest（含 hal_st / cmsis 等模块声明）
│
├── zephyr-sdk-0.16.8\            ← Zephyr SDK 工具链（GCC 12.2.0 + host tools）
│
├── zephyr_user\                  ← 用户自研子树（独立 git 仓库：Dust_Zephyr_Tree）
│     ├── framework\              ← 架构层（7 个 git 子模块）
│     │     ├── drivers\
│     │     ├── algorithm\
│     │     ├── modules\
│     │     ├── topic\
│     │     ├── cmd\
│     │     ├── init\
│     │     └── zephyr\           ←   module.yml + Kconfig（架构层统一装配）
│     ├── platform\cmsis\         ←   ARM CMSIS 兼容模块（cmsis_core.h 遮蔽）
│     ├── boards\                 ←  用户自研板卡（st/stm32f407igh6）
│     ├── dts\                    ←  用户自研 devicetree binding
│     └── project\                ←  业务工程子模块（Dust_Zephyr_Architecture_Project）
│
├── projects\                     ← 应用工程区
│     └── temp\                   ←   最小用户区（本文演示的应用，独立目录）
│
└── .venv\                        ← Python 虚拟环境（west / cmake 等）
```

**分工原则（为什么这么拆）：**
- **官方 `zephyr` 树**：官方代码，不动。STM32 的 HAL（hal_st）、CMSIS（cmsis）都作为 zephyr 的 west 模块，
  放在 `E:\Zephyr\modules\hal\...`，由 `west update` 拉取。
- **`zephyr_user` 子树**：用户自研、独立 git 管理。`framework/` 是架构层，各层是独立子模块仓库
  （`Dust_Zephyr_Architecture_*`），可单独演进版本。
- **`projects\<app>`（用户区）**：一个独立的 Zephyr 应用，通过 CMake 把 `zephyr_user` 的
  `framework` + `platform/cmsis` 装配进来，只写自己的业务线程。

**门禁体系（重点，必须理解）：**
- 架构层每个功能都有 `DUST_XXX` Kconfig 符号（如 `DUST_DEV_GPIO_OUTPUT`、`DUST_CTL_TIMER`）。
- 用户区的业务门禁（如 `TRD_GPIO`）`select` 对应的 `DUST_XXX`。
- 用户区 Kconfig 通过 `source "Kconfig.zephyr"` 接入 Zephyr；架构层 Kconfig 由
  `framework/zephyr/module.yml` 的 `kconfig: zephyr/Kconfig` 自动加载（framework 挂在
  `ZEPHYR_EXTRA_MODULES` 里）。

---

## 2. 目录规划

本文用如下目录（与团队现有机器保持一致，避免绝对路径歧义）：

```text
E:\Zephyr\zephyr                    ← 官方 v4.3.0
E:\Zephyr\zephyr-sdk-0.16.8         ← SDK
E:\Zephyr\zephyr_user               ← 用户子树
E:\Zephyr\projects\temp             ← 用户区（最小应用）
E:\Zephyr\.venv                     ← Python venv
E:\Zephyr\.west\config              ← west workspace 配置
E:\Zephyr\modules\hal\...           ← zephyr 的 west 模块（hal_st、cmsis 等，自动拉取）
```

> 也可以用其他盘/目录，但**整个文档的相对路径都基于上面这套**。若改盘符，请全局替换。

---

## 3. 前置工具安装（从零）

一台全新 Windows 电脑，按顺序装。

### 3.1 Git

```powershell
winget install Git.Git
# 装完重开终端，验证：
git --version
```

### 3.2 Python 3.12（推荐用 uv，或官网安装包）

用 `uv`（更干净、好管理）：

```powershell
winget install astral-sh.uv
uv python install 3.12
```

或者去 [python.org](https://www.python.org/downloads/) 装 Python 3.12，勾选 **Add to PATH**。

验证：

```powershell
python --version   # 期望 Python 3.12.x
```

### 3.3 CMake / Ninja / dtc

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install open-source-win.dtc   # devicetree compiler
```

验证：

```powershell
cmake --version    # 3.20+，实测 4.3.1 可用
ninja --version
dtc --version      # 1.4.6+
```

### 3.4 west（Zephyr 的构建前端）

在 Python venv 里装 west。先建 venv：

```powershell
cd E:\Zephyr
uv venv .venv --python 3.12
```

> 若不用 uv，用 `python -m venv E:\Zephyr\.venv`。

激活并装 west：

```powershell
.\.venv\Scripts\Activate.ps1
python -m pip install -U pip
python -m pip install west
west --version     # 期望 1.5.x
```

> ⚠️ 以后每次开终端都要先 `.\\.venv\\Scripts\\Activate.ps1`，并**挂载环境变量**（见 [§7](#7-环境变量)）。

### 3.5 Zephyr SDK 0.16.8

从 GitHub 下载并解压到 `E:\Zephyr\zephyr-sdk-0.16.8`：

```powershell
# 到 GitHub release 页下载：
#   https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v0.16.8
# 下载 Windows 版（例如 zephyr-sdk-0.16.8_windows-x86_64.zip）
# 解压后目录改名为 zephyr-sdk-0.16.8，放到 E:\Zephyr\
```

SDK 里含：ARM toolchain（`arm-zephyr-eabi`）、host-tools（cmake/ninja/dtc 等）。
**装完需要运行一次 SDK 的 setup 脚本（可选，若 SDK 环境变量已指向它则非必须）：**

```powershell
cd E:\Zephyr\zephyr-sdk-0.16.8
.\setup.cmd
```

> 本文用 `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` + `ZEPHYR_SDK_INSTALL_DIR` 指向 SDK，
> 不依赖 setup 把工具链写进 PATH（见 [§7](#7-环境变量)）。

---

## 4. 拉取官方 zephyr

### 4.1 克隆 zephyr v4.3.0

```powershell
cd E:\Zephyr
git clone --depth 1 --branch v4.3.0 https://github.com/zephyrproject-rtos/zephyr.git zephyr
```

### 4.2 建 west workspace

west 需要一个 workspace 配置，指明 manifest 在 zephyr 树里：

```powershell
mkdir -p E:\Zephyr\.west
# 写配置（注意用 LF 换行，路径用正斜杠）
@"
[manifest]
path = zephyr
file = west.yml
"@ | Set-Content -Path E:\Zephyr\.west\config -Encoding ASCII
```

### 4.3 拉 zephyr 的依赖模块（含 STM32 HAL、CMSIS）

```powershell
cd E:\Zephyr\zephyr
python -m pip install -r scripts/requirements.txt
west zephyr-export
west update
```

> `west update` 会按 `west.yml` 把模块拉进 `E:\Zephyr\modules\...`。
> 编译 **STM32 板必须**有 `hal_st`（stm32f4xx.h）和 `cmsis`（core_cm4.h）两个模块。
> 若只装了一部分（比如 `west update hal_st`），补拉：
>
> ```powershell
> west update hal_st cmsis
> ```

---

## 5. 拉取 zephyr_user 子树

### 5.1 配置 SSH（访问私有仓库）

`zephyr_user` 和架构子模块在 `qingyu0310` 的 GitHub。**推荐用自定义 SSH host**（走 443，
用 qingyu0310 的私钥）。在 `C:\Users\<你>\.ssh\config` 追加：

```text
Host github-qingyu0310
    HostName ssh.github.com
    User git
    Port 443
    IdentityFile C:/Users/<你>/.ssh/id_ed25519_qingyu0310
    IdentitiesOnly yes

Host github.com
    HostName ssh.github.com
    User git
    Port 443
    IdentityFile C:/Users/<你>/.ssh/id_ed25519_qingyu0310
    IdentitiesOnly yes
```

> 没有 qingyu0310 的私钥，找团队管理员要。也可用 HTTPS（下文仓库地址换成
> `https://github.com/qingyu0310/xxx.git`）但拉私有库需要 PAT。

### 5.2 克隆 zephyr_user

```powershell
cd E:\Zephyr
git clone git@github-qingyu0310:qingyu0310/Dust_Zephyr_Tree.git zephyr_user
```

### 5.3 拉 framework 架构子模块 + 业务工程子模块

```powershell
cd E:\Zephyr\zephyr_user
git submodule update --init --recursive
```

这会拉齐 `.gitmodules` 里声明的 7 个子模块（framework 的 6 层 + project 业务工程）：
`drivers` `algorithm` `modules` `topic` `cmd` `init` `project`。

> `framework/init` 的 url 是 `git@github.com:...`（标准 SSH），依赖 [§5.1](#51-配置-ssh访问私有仓库)
> 里 `Host github.com` 那段（否则报 qingyu0620 无权限，见 [FAQ §9.6](#96-ssh-认证成-qingyu0620)）。

### 5.4 确认子树完整

```powershell
ls E:\Zephyr\zephyr_user\framework     # 应看到 algorithm cmd drivers init modules topic zephyr
ls E:\Zephyr\zephyr_user\platform\cmsis  # 应看到 Kconfig cmsis_core.h zephyr
git -C E:\Zephyr\zephyr_user submodule status   # 每层都指向具体 commit（无 `-` 前缀）
```

---

## 6. 建立用户区（应用工程）

用户区 = 一个独立 Zephyr 应用，放在 `E:\Zephyr\projects\<app>`，通过 CMake 引用
`zephyr_user` 的架构层。它只写自己的业务线程，不碰架构层代码。

> **不需要自己搭**：`E:\Zephyr\projects\temp` 已是一个最小用户区示例（空转 GPIO 线程），
> 结构完整、可直接编译。直接用它对编译环境做验证：

```powershell
cd E:\Zephyr\projects\temp
west build -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c
```

> 若已配置 `dust`（见 [§8.1](#81-将-dust-命令添加进环境)），可用等价命令 `dust build board_rm_c`。

**用户区结构（以 `temp` 为准）：**

```text
E:\Zephyr\projects\temp\
├── CMakeLists.txt                  ← 顶层构建（通用）
├── Kconfig                         ← 业务门禁（PRJ_MAIN / TRD_XXX）
├── prj.conf                        ← 通用配置
├── boards\st\board_rm_c\           ← 板卡专属（board.cmake / conf / overlay 占位）
│     ├── board.cmake
│     ├── openocd.cfg
│     ├── stm32f407igh6.conf
│     └── stm32f407igh6.overlay     ← 必须有（dust 靠它反推 BOARD），内容可空
└── thread\                         ← 业务线程
      ├── CMakeLists.txt
      ├── thread.hpp                ← Thread 模板
      └── gpio\trd_gpio.cpp
```

> 新用户区复制 `temp` 结构改即可；架构层（framework + platform/cmsis）由 CMakeLists
> 自动装配，业务线程在 `thread\` 里写并通过 `REGISTER_THREAD` 注册。

## 7. 环境变量

**挂载到 venv 的 Activate.ps1**，激活即自动配置。在 `E:\Zephyr\.venv\Scripts\Activate.ps1`
**末尾追加**以下 5 行（必须纯 ASCII，否则 PowerShell 5.1 按 GBK 读会乱码）：

```powershell
# ZEPHYR_USER subtree env (SELF-MAINTAINED)
$env:ZEPHYR_BASE                = "E:\Zephyr\zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR     = "E:\Zephyr\zephyr-sdk-0.16.8"
$env:ZEPHYR_TOOLCHAIN_VARIANT   = "zephyr"
```

> - 本机不需要 HPM 的 `SDK_GLUE_DIR` / `ZEPHYR_SDK_GLUE_MODULE_DIR`（那是《重建指南》的）。
> - ⚠️ **重建 venv 会清掉 Activate.ps1**，重建后需重新追加这 3 行。

激活：

```powershell
cd E:\Zephyr
.\.venv\Scripts\Activate.ps1
echo $env:ZEPHYR_BASE    # 应输出 E:\Zephyr\zephyr
```

---

## 8. 编译验证

在**用户区**目录，用 west 直接编译（`BOARD` 用 STM32 板名，`BOARD_CFG` 指向板卡配置目录）：

```powershell
cd E:\Zephyr\projects\temp
# 首次或改 CMakeLists 后建议 -p 干净编译
west build -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c
```

> - `-b stm32f407igh6`：Zephyr 板名（板定义在 `zephyr_user/boards/st/stm32f407igh6`）。
> - `-DBOARD_CFG=board_rm_c`：让 `E:\Zephyr\projects\temp\CMakeLists.txt` 的 glob 匹配到
>   `boards/st/board_rm_c/` 下的 overlay / conf / board.cmake。

**成功标志：**

```text
Memory region         Used Size  Region Size  %age    Used
           FLASH:        ...       ...         ...    ...
[150/150] Linking ...   (无 FAILED)
```

产物在 `E:\Zephyr\projects\temp\build\zephyr\zephyr.elf`。

### 8.1 将 `dust` 命令添加进环境

`dust` 是团队对 `west build` 的封装，让你不必记板名映射，直接用 `dust build <板卡配置名>`。

**它是什么：** 两个脚本在 `zephyr_user\framework\cmd\build\`：
- `dust.cmd`：子命令分发（`dust build <board>` → 调 `build.bat`）。
- `build.bat`：核心逻辑——遍历当前项目 `boards\*\<name>\*.overlay` 反推出 BOARD 名，
  然后执行 `west build -b <BOARD> <args> -- -DBOARD_CFG=<name>`。

**安装（把脚本目录加进 PATH）：**

```powershell
[Environment]::SetEnvironmentVariable(
  "Path",
  $env:Path + ";E:\Zephyr\zephyr_user\framework\cmd\build",
  "User"
)
```

> 之后开新终端生效（当前终端需重开，或用 `refreshenv`）。也可把上面这行追加到
> `Activate.ps1` 末尾（同 [§7](#7-环境变量) 的挂载方式），每次激活自动带。

**用法（在项目根目录）：**

```powershell
cd E:\Zephyr\projects\temp
dust build board_rm_c        # 等价于 west build -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c
```

> `build.bat` 要求**当前目录是项目根**（有 `CMakeLists.txt`），且必须在激活了 venv
> （west 可用）的终端里运行。未找到匹配的板卡配置时，退化为 `west build -b <name>`。

---

## 9. 常见问题排查

> 全部来自 2026-08-02 实际搭建时踩过的坑，按"现象 → 原因 → 解决"。

### 9.1 板子缓存冲突：`build directory targets board X, but board Y was specified`

- **现象**：换板编译时，`west build` 报 build 目录已属于另一块板。
- **原因**：build 目录缓存了上一次的 board。
- **解决**：删 build 目录或用 `-p`：
  ```powershell
  rm -rf E:\Zephyr\projects\temp\build
  ```

### 9.2 `cmsis_core.h: No such file or directory`

- **现象**：`asm_inline_gcc.h:24: fatal error: cmsis_core.h: No such file`。
- **原因**：`cmsis_core.h` 在 `zephyr_user/platform/cmsis/`，没进 include 路径。这是 **ARM 板特有**
  （RISC-V 板不走 `asm_inline_gcc.h`，所以一直没暴露）。
- **解决**：板卡 `E:\Zephyr\projects\temp\boards\st\board_rm_c\board.cmake` 的
  `BOARD_GLOBAL_INCLUDES` 加上 `${ZEPHYR_USER_DIR}/platform/cmsis`（temp 里现成文件已配好，
  照着抄），根 CMakeLists 的 `foreach` 会自动加。

### 9.3 `stm32f4xx.h: No such file or directory`

- **现象**：`stm32f4xx.h`（ST 设备头）找不到。
- **原因**：缺 zephyr 的 `hal_st` 模块。
- **解决**：拉 ST HAL：
  ```powershell
  cd E:\Zephyr && west update hal_st
  ```

### 9.4 `core_cm4.h: No such file or directory`

- **现象**：`stm32f407xx.h:166: fatal error: core_cm4.h`。
- **原因**：缺 zephyr 的 `cmsis` 模块（`core_cm4.h` 在 `modules/hal/cmsis/CMSIS/Core/Include`）。
- **解决**：
  1. 拉模块：`cd E:\Zephyr && west update cmsis`
  2. 板卡 `E:\Zephyr\projects\temp\boards\st\board_rm_c\board.cmake` 的 `BOARD_GLOBAL_INCLUDES`
     加 `${CMAKE_SOURCE_DIR}/../../modules/hal/cmsis/CMSIS/Core/Include`（temp 里现成文件已配好）。

### 9.5 `HAS_CMSIS_CORE ... is assigned in a configuration file, but is not directly user-configurable`

- **现象**：conf 里写 `CONFIG_HAS_CMSIS_CORE=y` 报"no prompt"。
- **原因**：`HAS_CMSIS_CORE` 原本是无 prompt 的符号，只能被 select，不能直接写 conf。
- **解决**：给 `zephyr_user/platform/cmsis/Kconfig` 里的 `HAS_CMSIS_CORE` 加 prompt：
  ```kconfig
  config HAS_CMSIS_CORE
      bool "Enable ARM CMSIS core header"
  ```
  之后 conf 里就能写了。

### 9.6 SSH 认证成 qingyu0620（不是 qingyu0310）

- **现象**：push/拉私有库报 `Permission denied`。
- **原因**：`git@github.com:` 默认走 `id_ed25519`（qingyu0620），无 qingyu0310 权限。
- **解决**：在 `~/.ssh/config` 加 `Host github-qingyu0310`（走 443、用 qingyu0310 私钥），仓库地址用
  `git@github-qingyu0310:qingyu0310/xxx.git`；并加 `Host github.com` 段指向同一把 qingyu0310 私钥
  （[§5.1](#51-配置-ssh访问私有仓库)）。

### 9.7 `No SOURCES given to Zephyr library: ...sdk_glue__drivers__can`

- **现象**：编译时 warning `No SOURCES given to Zephyr library .../sdk_glue__drivers__can`。
- **原因**：应用加载了 HPM 的 sdk_glue（`ZEPHYR_EXTRA_MODULES` 里加了它），但 STM32 板
  `CONFIG_CAN_HPMICRO`/`MCAN_HPMICRO` 都是关的，can 目录建了个空库。
- **解决**：**STM32 用户区根本别加 sdk_glue**（参考 `E:\Zephyr\projects\temp\CMakeLists.txt`，
  里面没有 sdk_glue）。

### 9.8 `HPM53_SINGLE_PRECISION_FPU undefined` / 其他 HPM 符号

- **现象**：Kconfig 阶段报 HPM 相关 undefined。
- **原因**：应用里 HPM 的 soc/Kconfig 没被加载（因为你根本没拉 HPM 侧）。
- **解决**：STM32 用户区忽略即可；只有编译 HPM 板才需要《重建指南》里的 sdk_glue 链路。

### 9.9 `CMakeCache directory ... is different`

- **现象**：build 缓存目录路径不匹配。
- **原因**：build 目录从别处拷贝/移动过。
- **解决**：`rm -rf build` 重来。

### 9.10 west 不认 `build` / `zephyr-export` 命令

- **现象**：`west: unknown command "build"`。
- **原因**：E 盘不是 west workspace（缺 `.west/config`）。
- **解决**：按 [§4.2](#42-建-west-workspace) 建 `.west/config`，再 `west zephyr-export`。

---

_搭建指南（纯 zephyr 子树版）更新至 2026-08-02。_
