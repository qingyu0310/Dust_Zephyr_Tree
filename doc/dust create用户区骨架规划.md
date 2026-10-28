# dust create 用户区骨架规划

> 日期：2026-10-28
> 目标：新增 `dust create template` 子命令，在**当前用户区**铺开一套可编译的基础骨架（六模块 + project 链路），链路文件强制覆盖、业务文件不动。
> 参考实现：`E:\Zephyr\projects\gongxun`（已验证可编译的用户区）。
> **取代**：`doc/dust create空用户区规划.md`（那份是"新建 `projects\<名字>` 空目录"的语义，本次改为"在当前用户区铺骨架"，作废不再执行）。

---

## 1. 需求与拍板结论

### 1.1 一句话需求

在一个已经存在的用户区根目录下执行

```text
dust create template
```

就把**六模块（algorithm / cmd / drivers / init / modules / topic）+ project/ 构建链路**铺出来，铺完能直接 `dust build <board>` 编过；以后加用户层模块只需往对应目录里填内容，不用再改链路。

### 1.2 用户拍板的 6 条（写死，执行时不许改）

| # | 问题 | 结论 |
|---|---|---|
| 1 | `template` 指什么 | **固定模板名**。命令形式 `dust create template`，没有别的模板名 |
| 2 | 目标位置 | **当前用户区根**，不新建 `template` 文件夹（`dust create template` 在 `projects\gongxun` 下执行就往 gongxun 里铺） |
| 3 | 板卡配置 | **先只建文件夹** —— `project\boards\` 空着 + `.gitkeep`，不造 vendor/cfg 目录 |
| 4 | 六模块建到什么程度 | **空文件也能编译，且加用户区模块方便** —— 每个模块带 `CMakeLists.txt` + `Kconfig` + `zephyr/module.yml` 三件套（内容是注释 + 示例，不是空文件） |
| 5 | 已有文件怎么办 | **链路文件强制覆盖**；业务文件（`thread\*\*.cpp`、`boards\` 下的实际板卡配置）保留不动 |
| 6 | 骨架怎么存 | **`framework\cmd\create\skeleton\` 整棵树**，脚本用 `xcopy` 整棵拷（不用 `>>echo` 逐行拼，避免中文编码问题） |

### 1.3 `project/CMakeLists.txt` 取哪份

**gongxun 的用户区路径 + 主仓库的 `if(EXISTS)` 保护**。

- 路径用 gongxun 版（`../../../zephyr_user`、`../../../../Zephyr_HPMicro/sdk_glue`）—— 因为产物在 `projects\<名字>\project\`，深度和 gongxun 一样；
- `SDK_GLUE_DIR` / `SDK_GLUE_USER_DIR` 保留主仓库版的 `if(EXISTS)` 保护 —— gongxun 版是无条件 `list(APPEND ZEPHYR_EXTRA_MODULES "${SDK_GLUE_DIR}")`，在没有 HPM 树的机器上（ST 板）会报 `is not a valid zephyr module`（`doc/zephyr_子树架构搭建指南.md` §9.7 的成因之一）。

---

## 2. 调研：为什么是这套文件

### 2.1 用户区根必须有哪些东西（对照 `gongxun\project\CMakeLists.txt` 逐条推）

| # | 文件 | 缺了会怎样 | 依据 |
|---|---|---|---|
| 1 | `project\CMakeLists.txt` | `dust build` 认不出这是工作区根（`build.bat:15` 的规则 B） | 构建入口 |
| 2 | `project\Kconfig` | `prj.conf` 里 `CONFIG_PRJ_MAIN=y` 赋给不存在的符号 | 定义 `PRJ_MAIN` |
| 3 | `project\prj.conf` | `PRJ_MAIN` 不为 y → framework 七层全部 `add_subdirectory` 跳过 → 没有 `main()` | `CMakeLists.txt:122`、`:146` |
| 4 | `project\thread\CMakeLists.txt` | `add_subdirectory(${APP_THREAD})` 是**无条件**执行的（`CMakeLists.txt:147`），目录里没 CMakeLists.txt 直接 CMake 报错 | 线程装配表 |
| 5 | `project\boards\`（目录） | `file(GLOB)` 三条全部落空**且不报错**（`CMakeLists.txt:33-50`），所以缺了也能编，但 `dust build` 会退化成"直接传 Zephyr 板名" | 板卡配置根 |

### 2.2 六个模块目录要什么才"空也能编、加模块方便"

`project\CMakeLists.txt` 对六模块做**两件事**：

```cmake
# ① 当 Zephyr 模块加载（module.yml 自动拉 Kconfig）
foreach(_app IN ITEMS ${APP_ALGORITHM} ${APP_CMD} ${APP_DRIVERS} ${APP_INIT} ${APP_MODULES} ${APP_TOPIC})
  if(EXISTS "${_app}/CMakeLists.txt")
    list(APPEND ZEPHYR_EXTRA_MODULES "${_app}")
  endif()
endforeach()

# ② 装配进 app target
foreach(_app IN ITEMS ...)
  if(EXISTS "${_app}/CMakeLists.txt")
    get_filename_component(_name "${_app}" NAME)
    add_subdirectory(${_app} ${CMAKE_CURRENT_BINARY_DIR}/${_name})
  endif()
endforeach()
```

两处都以 **`CMakeLists.txt` 是否存在**为门禁。所以：

- **有 `CMakeLists.txt`（哪怕只有注释）** → 被当模块加载 + 被 `add_subdirectory`；
- `module.yml` 里 `build: kconfig: Kconfig` → Zephyr 会去读同目录 `Kconfig`，**该文件必须存在**（内容可以是纯注释）；
- 于是"三件套齐全但只有注释"= 完全合法的空模块，编得过，而且以后往里加内容不用碰 `project\CMakeLists.txt`。

`gongxun` 现在只有 4 个模块有内容（algorithm / drivers / modules / topic），`cmd` 和 `init` 是空目录 —— 它们靠"没有 CMakeLists.txt 所以被跳过"蒙过去。骨架里给全部 6 个都配三件套，就是为了**加模块时不用再改链路**。

### 2.3 `module.yml` 的 name 必须全局唯一

Zephyr 的 `zephyr_module.py` 会校验模块名重复。框架层已占 `user_framework`（`framework/zephyr/module.yml`），所以骨架里用 `user_algorithm` / `user_cmd` / `user_drivers` / `user_init` / `user_modules` / `user_topic`。

> 不同用户区之间不会冲突 —— 一次构建只加载一个用户区。

### 2.4 `module.yml` 只写 `kconfig` 不写 `cmake`

`zephyr_module.py:236` 只在存在 `zephyr/CMakeLists.txt`（或显式 `build.cmake`）时才把模块加进 CMake 构建树。骨架**故意不写 `cmake:`** —— CMake 装配统一由 `project\CMakeLists.txt` 的 `add_subdirectory` 负责，与框架层 `user_framework` 的写法一致。

### 2.5 为什么 `xcopy` 整棵拷，而不是 `>>echo` 逐行拼

| 方案 | 中文注释 | 加文件 | 可读性 |
|---|---|---|---|
| `>>"%DEST%\project\Kconfig" echo ...` | `chcp 65001` 下仍可能按 ANSI 落盘，IDE 打开是乱码 | 改 bat，每加一个文件加十几行 `echo` | 差 |
| `xcopy skeleton\* 目标\` | 文件本身是 UTF-8，`xcopy` 是字节拷贝，不动编码 | 往 skeleton 里放文件即可，脚本一行不改 | 好 |

### 2.6 覆盖语义天然正确

`xcopy /E /I /Y /H` 只覆盖**skeleton 里存在的文件**。所以：

- `project\CMakeLists.txt`、`project\Kconfig`、`project\prj.conf`、`project\thread\CMakeLists.txt`、六模块三件套 → **被骨架版覆盖**（= 用户要的"链路文件强制覆盖"）；
- `project\thread\motor\trd_motor.cpp`、`project\boards\st\gongxun\*`、六模块里的业务源码 → **skeleton 里没有，一个字节都不动**。

不需要写"判断是否覆盖"的逻辑。

---

## 3. 骨架树最终形态

```text
<用户区根>\
├── .clangd                              ← 编译数据库指向 project\build\
├── algorithm\
│   ├── CMakeLists.txt                   ← 注释 + 示例
│   ├── Kconfig                          ← 注释 + 示例
│   └── zephyr\module.yml                ← name: user_algorithm
├── cmd\                                 ← 同上三件套（user_cmd）
├── drivers\                             ← 同上（user_drivers）
├── init\                                ← 同上（user_init）
├── modules\                             ← 同上（user_modules）
├── topic\                               ← 同上（user_topic）
└── project\
    ├── CMakeLists.txt                   ← 用户区路径 + SDK_GLUE 存在性保护
    ├── Kconfig                          ← PRJ_MAIN + USE_CMD_*（default n）
    ├── prj.conf                         ← 与主仓库 project\prj.conf 一致
    ├── .gitignore                       ← .cache/ + build/
    ├── boards\
    │   └── .gitkeep                     ← 空（板卡后面自己加）
    └── thread\
        └── CMakeLists.txt               ← 空装配表（注释示例）
```

共 **6 个目录 + 24 个文件**（其中 18 个是六模块三件套）。

---

## 4. 分阶段执行

> **每阶段开头一张五列步表；用户说"开始第 N 步" = 只写表格里那一步对应的文件，写完停下报告，等"继续"。**
> **不许一次做多步，不许跨步连带改。**
> 全部 18 步**没有一步会让工程编不过**（都是新增文件；P2/P3 改的是 `.bat`，不参与 Zephyr 构建）。

### P1 骨架树落地（13 步）

> **P1 拆成 13 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只写表格里那一步的文件，改完停下报告，等"继续"。
> 不许一次做多步，不许跨步连带改。
> 本阶段全程不编译（骨架目录 `framework\cmd\create\skeleton\` 不在任何构建路径里）。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `framework/cmd/create/skeleton/project/CMakeLists.txt` | 新建（§5.1 全文） | 无 | 无 |
| 2 | `framework/cmd/create/skeleton/project/Kconfig` | 新建（§5.2 全文） | 无 | 无 |
| 3 | `framework/cmd/create/skeleton/project/prj.conf` | 新建（§5.3 全文） | 无 | 无 |
| 4 | `framework/cmd/create/skeleton/project/.gitignore` | 新建（§5.4 全文） | 无 | 无 |
| 5 | `framework/cmd/create/skeleton/project/boards/.gitkeep` | 新建（空文件） | 无 | 无 |
| 6 | `framework/cmd/create/skeleton/project/thread/CMakeLists.txt` | 新建（§5.5 全文） | 无 | 无 |
| 7 | `framework/cmd/create/skeleton/.clangd` | 新建（§5.6 全文） | 无 | 无 |
| 8 | `skeleton/algorithm/{CMakeLists.txt,Kconfig,zephyr/module.yml}` | 新建 3 文件（§5.7） | 无 | 无 |
| 9 | `skeleton/cmd/{同}` | 新建 3 文件（§5.7，把 `<algorithm>` 换成 `cmd`） | 步 8 | 无 |
| 10 | `skeleton/drivers/{同}` | 新建 3 文件（§5.7，换成 `drivers`） | 步 9 | 无 |
| 11 | `skeleton/init/{同}` | 新建 3 文件（§5.7，换成 `init`） | 步 10 | 无 |
| 12 | `skeleton/modules/{同}` | 新建 3 文件（§5.7，换成 `modules`） | 步 11 | 无 |
| 13 | `skeleton/topic/{同}` | 新建 3 文件（§5.7，换成 `topic`） | 步 12 | 无 |

> 步 8~13 是同一套模板换模块名。**每步只写一个模块目录的三个文件**，写完停下。

### P2 脚本落地（1 步）

> **P2 只有 1 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `framework/cmd/create/create.bat` | 新建（§5.8 全文） | P1 全部完成 | 无 |

### P3 命令分发（1 步）

> **P3 只有 1 步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `framework/cmd/build/dust.cmd` | 改（§5.9 old→new 全文替换） | P2 步 1 | 无 |

### P4 文档同步（3 步）

> **P4 拆成 3 步，每次只执行一步。**

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|---|---|---|---|---|
| 1 | `framework/cmd/README.md` | 改（§5.10） | P3 完成 | 无 |
| 2 | `doc/zephyr_子树架构搭建指南.md` | 改（§5.11，§6「建立用户区」整节重写） | 无 | 无 |
| 3 | `快速上手.md` | 改（§5.12，§3「拉代码」后补一节） | 无 | 无 |

---

## 5. 逐文件 old → new 全文

### 5.1 `skeleton/project/CMakeLists.txt`（新建）

来源：`E:\Zephyr\projects\gongxun\project\CMakeLists.txt`，**只改 SDK_GLUE 段**（加 `if(EXISTS)` 保护）。逐字全文：

```cmake
cmake_minimum_required(VERSION 3.20.0)

# ============================================================================
#
# 路径规划
#
# ============================================================================

# 当前项目（<用户区>/project）
set(PROJECT_ROOT 	"${CMAKE_CURRENT_SOURCE_DIR}/..")              # <用户区>/
# 子树（zephyr_user，架构层）
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr_user")
set(FW_ROOT         "${ZEPHYR_USER_DIR}/framework")
# 用户工作区内部目录（用户层，framework 六模块平铺用户区根）
set(APP_ALGORITHM   "${PROJECT_ROOT}/algorithm")                   # <用户区>/algorithm
set(APP_CMD         "${PROJECT_ROOT}/cmd")                         # <用户区>/cmd
set(APP_DRIVERS     "${PROJECT_ROOT}/drivers")                     # <用户区>/drivers
set(APP_INIT        "${PROJECT_ROOT}/init")                        # <用户区>/init
set(APP_MODULES     "${PROJECT_ROOT}/modules")                     # <用户区>/modules
set(APP_TOPIC       "${PROJECT_ROOT}/topic")                       # <用户区>/topic
set(APP_THREAD      "${CMAKE_CURRENT_SOURCE_DIR}/thread")          # <用户区>/project/thread/

# 业务门禁符号
set(CONFIG_SYM PRJ_MAIN)

# ============================================================================
#
# Zephyr 树自带
#
# ============================================================================

# 板级 overlay / conf / board.cmake
if(NOT DEFINED BOARD_CFG)
  	set(BOARD_CFG ${BOARD})
endif()

file(GLOB OVERLAY_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.overlay)
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

# Zephyr SDK
if(DEFINED ENV{ZEPHYR_SDK_INSTALL_DIR})
  	set(ZEPHYR_SDK_INSTALL_DIR "$ENV{ZEPHYR_SDK_INSTALL_DIR}" CACHE PATH "" FORCE)
else()
  	set(ZEPHYR_SDK_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr-sdk-0.16.8" CACHE PATH "")
endif()

# ============================================================================
#
# 子树模块（zephyr_user：framework / cmsis / zephyr_user/drivers）
#
# ============================================================================

# SDK glue（HPM 平台可选）
if(DEFINED ENV{SDK_GLUE_DIR})
  	set(SDK_GLUE_DIR "$ENV{SDK_GLUE_DIR}")
else()
  	set(SDK_GLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Zephyr_HPMicro/sdk_glue")
endif()
if(DEFINED ENV{SDK_GLUE_USER_DIR})
  	set(SDK_GLUE_USER_DIR "$ENV{SDK_GLUE_USER_DIR}")
else()
  	set(SDK_GLUE_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../Zephyr_HPMicro/sdk_glue_user")
endif()

# BOARD_ROOT / SOC_ROOT / DTS_ROOT 指向 zephyr_user
list(APPEND BOARD_ROOT "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}")
list(APPEND DTS_ROOT   "${ZEPHYR_USER_DIR}/dts")
if(EXISTS "${SDK_GLUE_DIR}")
  	# 用户层与官方并存：同名板卡在用户层加 u 前缀（如 uhpm6e00evk），两边都能构建
  	list(APPEND BOARD_ROOT "${SDK_GLUE_USER_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}/dts")
  	list(APPEND BOARD_ROOT "${SDK_GLUE_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}/dts")
endif()

# 外来模块加载（module.yml 自动加载 Kconfig）
set(ZEPHYR_EXTRA_MODULES
  "${FW_ROOT}"
  "${ZEPHYR_USER_DIR}/platform/cmsis"
  "${ZEPHYR_USER_DIR}/drivers"
)
# sdk_glue 仅存在时加入——st 板卡无 HPM 树
if(EXISTS "${SDK_GLUE_DIR}")
  list(PREPEND ZEPHYR_EXTRA_MODULES "${SDK_GLUE_DIR}")
endif()
# 用户层 HPM 覆盖模块（只带 drivers，不重复 soc/boards/dts）
if(EXISTS "${SDK_GLUE_USER_DIR}/zephyr/module.yml")
  list(APPEND ZEPHYR_EXTRA_MODULES "${SDK_GLUE_USER_DIR}")
endif()

# 用户工作区模块（用户层六模块；目录空时跳过，填内容后自动加载）
foreach(_app IN ITEMS ${APP_ALGORITHM} ${APP_CMD} ${APP_DRIVERS} ${APP_INIT} ${APP_MODULES} ${APP_TOPIC})
  if(EXISTS "${_app}/CMakeLists.txt")
    list(APPEND ZEPHYR_EXTRA_MODULES "${_app}")
  endif()
endforeach()

# ============================================================================
#
# 接入 Zephyr 构建系统（必须在全部模块加载之后、任何 app 装配之前）
#
# ============================================================================

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_project)

# ============================================================================
#
# 装配子树架构（framework 各层 + zephyr_user/drivers）
#
# ============================================================================

if(CONFIG_${CONFIG_SYM})
  	add_subdirectory(${FW_ROOT}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
  	add_subdirectory(${FW_ROOT}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
  	add_subdirectory(${FW_ROOT}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
  	add_subdirectory(${FW_ROOT}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
  	add_subdirectory(${FW_ROOT}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
  	add_subdirectory(${FW_ROOT}/shell     ${CMAKE_CURRENT_BINARY_DIR}/framework/shell)
  	add_subdirectory(${FW_ROOT}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)

  	add_subdirectory(${ZEPHYR_USER_DIR}/drivers ${CMAKE_CURRENT_BINARY_DIR}/zephyr_user/drivers)
endif()

# 业务源码与系统入口（main() 由架构层 init 提供，业务项目不再有 src/）
target_include_directories(app PRIVATE
  	${FW_ROOT}/init
  	${FW_ROOT}/cmd
)

# 板卡专属全局 include（由 boards/<board>/board.cmake 的 BOARD_GLOBAL_INCLUDES 提供）
foreach(_dir IN LISTS BOARD_GLOBAL_INCLUDES)
  	zephyr_include_directories(${_dir})
endforeach()
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)

# 装配用户工作区（用户层六模块 + thread；目录空时跳过）
if(CONFIG_${CONFIG_SYM})
  	add_subdirectory(${APP_THREAD})
  	foreach(_app IN ITEMS ${APP_ALGORITHM} ${APP_CMD} ${APP_DRIVERS} ${APP_INIT} ${APP_MODULES} ${APP_TOPIC})
    	if(EXISTS "${_app}/CMakeLists.txt")
      		get_filename_component(_name "${_app}" NAME)
      		add_subdirectory(${_app} ${CMAKE_CURRENT_BINARY_DIR}/${_name})
    	endif()
  	endforeach()
endif()
```

> **与 gongxun 版的唯一差异**：`ZEPHYR_EXTRA_MODULES` 不再无条件含 `${SDK_GLUE_DIR}`，改为 `if(EXISTS)` 前置追加。其余逐字照抄。

### 5.2 `skeleton/project/Kconfig`（新建）

```kconfig
# 用户区业务门禁 Kconfig
# 业务门禁符号（PRJ_MAIN / TRD_XXX / USE_XXX）保持原名，不加 DUST_ 前缀
# 业务门禁 select 的架构符号统一 DUST_ 前缀

config PRJ_MAIN
    bool "Main business project"
    default n

if PRJ_MAIN

# ============================================================================

# =================       功能配置区 — 各模块开关       ========================

config USE_CMD_SHELL
    bool "Shell debug console (thread + var/log)"
    default n
    select DUST_CMD_SHELL_LOG
    select DUST_CMD_SHELL_VAR
    help
      Shell debug console - UART thread + var/log sub-commands.
      打开需要 overlay 里配 shell-uart 别名，否则 shell.cpp 编译报错。

config USE_CMD_BUZZER
    bool "Buzzer control"
    default n
    select DUST_CMD_BUZZER
    help
      Buzzer control - PWM beep via cmd/buzzer.

config USE_CMD_FLASH
    bool "SPI Flash"
    default n
    select DUST_CMD_FLASH
    help
      SPI NOR Flash read/write/erase via cmd/flash.

# ============================================================================

# =================               TRD_XXX                 ====================
# 新增业务线程时照下面这块追加，并在 project/thread/CMakeLists.txt 加对应 if 块
#
# config TRD_XXX
#     bool "XXX thread"
#     default n
#     select DUST_DEV_XXX
#     help
#       XXX thread description.

# ============================================================================

endif # PRJ_MAIN

source "Kconfig.zephyr"
```

> **`USE_CMD_*` 全部 `default n`**：`USE_CMD_SHELL` 默认开会 `select DUST_CMD_SHELL_LOG/VAR` → 编译 `shell.cpp` → 它要 `DT_ALIAS(shell_uart)` → 骨架没有 overlay → **编译直接报错**。

### 5.3 `skeleton/project/prj.conf`（新建）

```conf
# Common settings for all projects
CONFIG_CPP=y
CONFIG_REQUIRES_FULL_LIBCPP=y
CONFIG_STD_CPP17=y
CONFIG_STATIC_INIT_GNU=y
CONFIG_THREAD_NAME=y
CONFIG_SYS_CLOCK_TICKS_PER_SEC=2000
CONFIG_PICOLIBC_IO_FLOAT=y

# 项目级基础设施
CONFIG_DYNAMIC_INTERRUPTS=y
CONFIG_HW_STACK_PROTECTION=y
CONFIG_MAIN_STACK_SIZE=3072
CONFIG_HEAP_MEM_POOL_SIZE=2048
CONFIG_INIT_STACKS=y
CONFIG_THREAD_STACK_INFO=y

# 全局 -O2 优化，替代默认 -Os
CONFIG_SPEED_OPTIMIZATIONS=y

CONFIG_PRJ_MAIN=y
```

> 与 `zephyr_user/project/prj.conf` 逐字一致（不带 `CONFIG_FPU` —— 那是板级/芯片级的东西，放 `boards\<vendor>\<cfg>\<board>.conf`）。

### 5.4 `skeleton/project/.gitignore`（新建）

```gitignore
.cache/
build/
```

### 5.5 `skeleton/project/thread/CMakeLists.txt`（新建）

```cmake
# 业务线程装配 — 门禁 TRD_XXX 保持原名（不加 DUST_ 前缀）
#
# 新增一个线程时：
#   1. 在 project/Kconfig 加 config TRD_XXX
#   2. 在下面照这块追加
#
# if(CONFIG_TRD_XXX)
# 	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx/trd_xxx.cpp)
# 	target_include_directories(app PRIVATE
# 		${CMAKE_CURRENT_SOURCE_DIR}
# 		${CMAKE_CURRENT_SOURCE_DIR}/xxx
# 	)
# endif()
```

### 5.6 `skeleton/.clangd`（新建）

来源：`E:\Zephyr\projects\gongxun\.clangd`，逐字照抄（把首行注释里的 `ai_imu` 改成"用户区"）：

```yaml
# 用户区根 clangd 配置
# CMake 在 project/ 下，编译数据库 project/build/（compile_commands.json）相对本文件指向
# framework/ platform/ boards/ dts/ 等子树也能用这套编译数据库
CompileFlags:
  CompilationDatabase: project/build/
  Add:
    - -ferror-limit=0
    - -Wno-unused-variable
    - "-Wno-#pragma-messages"
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/arm-zephyr-eabi/lib/gcc/arm-zephyr-eabi/12.2.0/include
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/arm-zephyr-eabi/lib/gcc/arm-zephyr-eabi/12.2.0/include-fixed
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/arm-zephyr-eabi/arm-zephyr-eabi/include/c++/12.2.0/arm-zephyr-eabi
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/riscv64-zephyr-elf/include/c++/12.2.0
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/lib/gcc/riscv64-zephyr-elf/12.2.0/include
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/lib/gcc/riscv64-zephyr-elf/12.2.0/include-fixed
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/riscv64-zephyr-elf/include
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/riscv64-zephyr-elf/include/c++/12.2.0/riscv64-zephyr-elf
    - -isystem
    - E:/Zephyr/zephyr-sdk-0.16.8/riscv64-zephyr-elf/picolibc/include
  Remove:
    - -mabi=*
    - -march=*
    - -mcpu=*
    - -mfp16-format=*
    - --specs=*
    - -fno-printf-return-value
    - --param=*
    - -fmacro-prefix-map=*
    - -fno-reorder-functions
    - -ftls-model=*
    - -mno-relax

Diagnostics:
  Suppress:
    - drv_unknown_argument
    - unknown_warning_option
    - unused_variable
    - no_member
    - "#pragma_messages"
    - anon_bitfield_qualifiers
    - -Wint-to-pointer-cast
```

### 5.7 六模块三件套（步 8~13，每步一个模块目录）

下面以 **`algorithm`** 为样板。其余五个把 `<algorithm>` → 模块名、`<ALGORITHM>` → 大写模块名、`DUST_CHK_XXX` → 对应前缀（见 §5.7.4 对照表）即可。

#### 5.7.1 `skeleton/algorithm/CMakeLists.txt`

```cmake
# 用户层 algorithm 装配 — 门禁 DUST_CHK_XXX
# 本文件由 project/CMakeLists.txt 的 add_subdirectory(${APP_ALGORITHM}) 引入
#
# 新增一个算法时照这块追加：
#
# if(CONFIG_DUST_CHK_XXX)
# 	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/check/xxx/xxx.cpp)
# 	target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/check/xxx)
# endif()
```

#### 5.7.2 `skeleton/algorithm/Kconfig`

```kconfig
# 用户层 algorithm 模块 — 门禁 DUST_CHK_XXX（跟架构层 algorithm 同风格）
# Kconfig 由 zephyr/module.yml 自动加载（build.kconfig: Kconfig），无需 rsource。
#
# 新增一个算法时照这块追加：
#
# config DUST_CHK_XXX
#     bool "XXX checksum"
#     default n
#     help
#       XXX 说明。
```

#### 5.7.3 `skeleton/algorithm/zephyr/module.yml`

```yaml
name: user_algorithm
build:
  kconfig: Kconfig
```

#### 5.7.4 六个模块的替换对照表

| 模块 | CMake 注释里的 `APP_*` | Kconfig 门禁前缀 | `module.yml` 的 name |
|---|---|---|---|
| `algorithm` | `APP_ALGORITHM` | `DUST_CHK_XXX` | `user_algorithm` |
| `cmd` | `APP_CMD` | `DUST_CMD_XXX` | `user_cmd` |
| `drivers` | `APP_DRIVERS` | `DUST_DEV_XXX` | `user_drivers` |
| `init` | `APP_INIT` | `DUST_INIT_XXX` | `user_init` |
| `modules` | `APP_MODULES` | `DUST_MOD_DEV_XXX` | `user_modules` |
| `topic` | `APP_TOPIC` | `DUST_TPC_XXX` | `user_topic` |

> 前缀照架构层现成的（`framework/*/Kconfig`），**不许自创**（见记忆 `kconfig-build-chain` 的 `no-invent-symbol-prefix`）。
> `init` 层目前框架里没有 `DUST_INIT_*` 门禁符号，骨架里写成 `DUST_INIT_XXX` 只作示例占位，真加时按实际需要命名。

### 5.8 `framework/cmd/create/create.bat`（新建）

```bat
@echo off
chcp 65001 >nul
setlocal

rem dust create — 在当前用户区铺开基础骨架（六模块 + project 链路）
rem   dust create template
rem   模板名固定 template；目标 = 当前用户区根，不新建目录

if /i not "%~1"=="template" (
    echo Usage: dust create template
    exit /b 1
)

set "SKELETON=%~dp0skeleton"
if not exist "%SKELETON%\project\CMakeLists.txt" (
    echo [ERROR] skeleton not found: %SKELETON%
    exit /b 1
)

rem 定位用户区根（与 build.bat 同款规则）
set "USER_ROOT="
if exist "%CD%\project\CMakeLists.txt" (
    set "USER_ROOT=%CD%"
) else if exist "%CD%\CMakeLists.txt" (
    for %%d in ("%CD%\..") do set "USER_ROOT=%%~fd"
)

if "%USER_ROOT%"=="" (
    echo [ERROR] dust create could not locate a user area root from:
    echo         current dir: %CD%
    echo         expected one of:
    echo         - %CD%\project\CMakeLists.txt
    echo         - %CD%\CMakeLists.txt
    exit /b 1
)

rem 整棵拷贝：skeleton 里的链路文件覆盖同名文件；
rem 业务文件（thread\*\*.cpp、boards\ 下的板卡配置）skeleton 里没有，不会被碰
xcopy /E /I /Y /H "%SKELETON%\*" "%USER_ROOT%\" >nul
if errorlevel 1 (
    echo [ERROR] copy failed: %SKELETON% -^> %USER_ROOT%
    exit /b 1
)

echo [dust] skeleton created in %USER_ROOT%
exit /b 0
```

### 5.9 `framework/cmd/build/dust.cmd`（改）

**old（当前全文，34 行）：**

```bat
@echo off
setlocal
rem dust CLI — 子命令分发
rem   dust build <board> [extra west build args]
rem   dust flash [extra west flash args]
if /i "%~1"=="build" (
    call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
if /i "%~1"=="flash" (
    call "%~dp0flash.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
echo Usage: dust build ^<board^> [extra west build args]
echo        dust flash [extra west flash args]
exit /b 1
```

**new（全文替换，两处不同：加 `create` 分支 + usage 补一行）：**

```bat
@echo off
setlocal
rem dust CLI — 子命令分发
rem   dust build <board> [extra west build args]
rem   dust flash [extra west flash args]
rem   dust create template
if /i "%~1"=="build" (
    call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
if /i "%~1"=="flash" (
    call "%~dp0flash.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
if /i "%~1"=="create" (
    call "%~dp0..\create\create.bat" %2
    exit /b %errorlevel%
)
echo Usage: dust build ^<board^> [extra west build args]
echo        dust flash [extra west flash args]
echo        dust create template
exit /b 1
```

> `create` 是脚本目录（`framework\cmd\create\`），与 `build\` 平级；`cmd\` 下其余目录（buzzer/fatal/flash/linker/shell）是**运行时 C++ 模块**，不放脚本。

### 5.10 `framework/cmd/README.md`（改）

在目录树里加一行 `create/`，并在 `## build/ — 编译与烧录脚本` 章节之后新增 `## create/ — 用户区骨架生成` 章节：

```markdown
## create/ — 用户区骨架生成

在**当前用户区根**铺开一套可编译的基础骨架（六模块 + project 链路）。

```text
dust create template
```

- 模板名固定 `template`，没有别的模板；
- 目标 = 当前目录所属的用户区根（`<cwd>\project\CMakeLists.txt` 或 `<cwd>\CMakeLists.txt` 两种入口都认）；
- **链路文件强制覆盖**（`project\CMakeLists.txt`、`project\Kconfig`、`project\prj.conf`、`project\thread\CMakeLists.txt`、六模块三件套）；
- **业务文件不动**（`thread\*\*.cpp`、`boards\` 下的板卡配置）—— `xcopy` 只覆盖 skeleton 里存在的文件。

骨架源：`create/skeleton/`，加骨架内容直接往这个目录放文件，脚本不用改。
```

同时把目录树那段的

```text
├── build/             ← 编译脚本
```

改成

```text
├── build/             ← 编译与烧录脚本
├── create/            ← 用户区骨架生成（脚本 + skeleton/）
```

### 5.11 `doc/zephyr_子树架构搭建指南.md`（改，§6 整节重写）

当前 §6（`搭建指南:296-330`）讲的是手工复制 `E:\Zephyr\projects\temp`（**该目录已不存在**）。整节替换为：

```markdown
## 6. 建立用户区（应用工程）

用户区放在 `E:\Zephyr\projects\<名字>\`，深度固定（`project\CMakeLists.txt` 用 `../../../zephyr_user` 反推，多一层少一层都编不过）。

### 6.1 一键生成骨架

```powershell
cd E:\Zephyr\projects\zhangsan
dust create template
```

产物：

```text
zhangsan\
├── .clangd
├── algorithm\  cmd\  drivers\  init\  modules\  topic\   ← 用户层六模块（三件套齐，只有注释）
└── project\
    ├── CMakeLists.txt
    ├── Kconfig
    ├── prj.conf
    ├── .gitignore
    ├── boards\            ← 空，板卡自己加
    └── thread\
        └── CMakeLists.txt
```

### 6.2 加板卡

骨架的 `project\boards\` 是空的。加一块板：

```text
project\boards\<vendor>\<配置名>\
├── <Zephyr板名>.overlay
├── <Zephyr板名>.conf
└── board.cmake
```

然后 `dust build <配置名>`。

### 6.3 加业务线程

1. `project\thread\<名字>\trd_<名字>.cpp`
2. `project\thread\CMakeLists.txt` 加 `if(CONFIG_TRD_<名字>)` 块
3. `project\Kconfig` 加 `config TRD_<名字>`
```

### 5.12 `快速上手.md`（改，§3「拉代码」之后补一节）

在 `## 4. 编译现有工程` 之前插入：

```markdown
## 3.5 新建一个用户区

```powershell
mkdir E:\Zephyr\projects\zhangsan
cd E:\Zephyr\projects\zhangsan
dust create template
```

一键铺出六模块 + `project/` 构建链路，铺完直接 `dust build <板卡>` 就能编。
以后加用户层模块只需往对应目录里填内容，不用再改 `project\CMakeLists.txt`。
```

---

## 6. 参数与常量总表

| 项 | 值 | 出处 |
|---|---|---|
| 命令 | `dust create template` | 拍板 1 |
| 模板名 | 只有 `template` | 拍板 1 |
| 骨架源目录 | `framework\cmd\create\skeleton\` | 拍板 6 |
| 目标目录 | 当前用户区根（`<cwd>\project\CMakeLists.txt` 或 `<cwd>\CMakeLists.txt` 反推） | 拍板 2 |
| 拷贝方式 | `xcopy /E /I /Y /H` | 拍板 5 |
| 板卡目录 | 只建 `project\boards\` + `.gitkeep` | 拍板 3 |
| 六模块 | 三件套（CMakeLists + Kconfig + zephyr/module.yml），内容为注释 + 示例 | 拍板 4 |
| 模块名 | `user_algorithm` / `user_cmd` / `user_drivers` / `user_init` / `user_modules` / `user_topic` | §2.3 |
| `USE_CMD_*` | 全部 `default n` | §5.2 注 |
| `project\CMakeLists.txt` | gongxun 路径 + SDK_GLUE `if(EXISTS)` 保护 | 拍板 6 / §1.3 |

---

## 7. 验证清单

### 7.1 骨架完整性（P1 完成后，静态检查）

1. `framework\cmd\create\skeleton\` 下有 6 个模块目录，每个目录 3 个文件（`CMakeLists.txt` / `Kconfig` / `zephyr\module.yml`）。
2. `skeleton\project\` 下有 `CMakeLists.txt` / `Kconfig` / `prj.conf` / `.gitignore` / `boards\.gitkeep` / `thread\CMakeLists.txt`。
3. `skeleton\.clangd` 存在，`CompilationDatabase: project/build/` 一行在。
4. 六个 `module.yml` 的 `name` 互不相同，且都不等于 `user_framework`。

### 7.2 命令行为（P2+P3 完成后）

5. `dust` 裸敲 → usage 三行（build / flash / create）。
6. `dust create` 不带参数 → `Usage: dust create template`，退出码 1。
7. `dust create foo` → 同上（模板名不是 template），退出码 1。
8. 在一个**空目录**（无 `project\CMakeLists.txt`）执行 `dust create template` → 报 `could not locate a user area root`，退出码 1，**不产生任何文件**。
9. 在 `E:\Zephyr\projects\gongxun` 执行 `dust create template` → 输出 `[dust] skeleton created in E:\Zephyr\projects\gongxun`。
   - 验证 `gongxun\cmd\CMakeLists.txt` / `gongxun\cmd\Kconfig` / `gongxun\cmd\zephyr\module.yml` 出现；
   - 验证 `gongxun\project\CMakeLists.txt` 被覆盖成骨架版（`grep SDK_GLUE_DIR` 能看到 `if(EXISTS)`）；
   - **验证 `gongxun\project\thread\motor\trd_motor.cpp` 还在、内容没变**（`git status` 或文件时间戳）。

### 7.3 可编译性（用户执行，AI 不代跑）

10. 在铺完骨架的用户区执行 `dust build <该用户区已有板卡>` → 编译通过。
11. 在一个**只铺了骨架、没有任何板卡**的目录执行 `dust build hpm5361icb`（直接传 Zephyr 板名）→ 预期：能过配置阶段；如果卡在 `cmsis_core.h` 那是 ST 板缺 `BOARD_GLOBAL_INCLUDES` 的已知现象（§2.1 第 5 条），不是骨架的问题。
12. 六模块目录里**只有注释**时，构建日志里不出现 `No SOURCES given to Zephyr library`（因为没写 `cmake:`，模块不进 CMake 树）。

---

## 8. 执行清单

```text
P1 骨架树（13 步，每步一个文件/一个模块目录）
[ ] 步 1  skeleton/project/CMakeLists.txt
[ ] 步 2  skeleton/project/Kconfig
[ ] 步 3  skeleton/project/prj.conf
[ ] 步 4  skeleton/project/.gitignore
[ ] 步 5  skeleton/project/boards/.gitkeep
[ ] 步 6  skeleton/project/thread/CMakeLists.txt
[ ] 步 7  skeleton/.clangd
[ ] 步 8  skeleton/algorithm/{CMakeLists.txt,Kconfig,zephyr/module.yml}
[ ] 步 9  skeleton/cmd/{同}
[ ] 步 10 skeleton/drivers/{同}
[ ] 步 11 skeleton/init/{同}
[ ] 步 12 skeleton/modules/{同}
[ ] 步 13 skeleton/topic/{同}

P2 脚本（1 步）
[ ] 步 1  framework/cmd/create/create.bat

P3 分发（1 步）
[ ] 步 1  framework/cmd/build/dust.cmd

P4 文档（3 步）
[ ] 步 1  framework/cmd/README.md
[ ] 步 2  doc/zephyr_子树架构搭建指南.md §6
[ ] 步 3  快速上手.md §3.5

收尾
[ ] §7.1 / §7.2 静态与命令验证
[ ] §7.3 编译验证（用户执行）
[ ] doc/dust create空用户区规划.md 顶部加"已被本规划取代"说明
```

---

## 9. 关键细节与坑

### 9.1 `xcopy` 要带 `/H`，否则漏掉 `.clangd` / `.gitignore`

Windows 的 `xcopy` 默认跳过**带隐藏属性**的文件。`.clangd` / `.gitignore` 在 Windows 上通常不带隐藏属性，但用户手工改过属性时会被跳过 —— 加 `/H` 一并拷，没有副作用。

### 9.2 `xcopy` 的 `/I` 只影响目录判定，不影响覆盖

`/I` 是"目标不存在且要拷多个文件时，假定目标是目录"。骨架里有 `boards\` 空目录（靠 `.gitkeep` 保住），`/E` 连空目录一起拷。

### 9.3 为什么必须 `/Y`（而不是 `/R`）

`/Y` = 抑制"覆盖已有文件？"提示。**不带 `/Y` 会卡在交互提示上**，脚本在 CI/批处理里会挂住。`/R` 是覆盖只读文件，骨架不涉及，不加。

### 9.4 `create.bat` 里定位用户区根的两条规则和 `build.bat` 一致

```bat
if exist "%CD%\project\CMakeLists.txt"  → USER_ROOT=%CD%
else if exist "%CD%\CMakeLists.txt"     → USER_ROOT=%CD%\..
```

这样在用户区根（`projects\gongxun`）和 `project\` 里面都能执行，行为一致。

### 9.5 骨架里的注释不写死用户区名字

`project\CMakeLists.txt` 的注释里写 `<用户区>/`、`<用户区>/algorithm`，不写 `template/`。原因：产物可能落在任何用户区，写死一个名字会误导。gongxun 那份写的是 `template/`（历史遗留），骨架不继承这个坑。

### 9.6 六模块的 `module.yml` 名字不能重复，也不能和框架撞

已占：`user_framework`（`framework/zephyr/module.yml`）。骨架用 `user_<模块名>`，与框架不撞、彼此不撞（§2.3）。

### 9.7 旧规划 `doc/dust create空用户区规划.md` 的处理

那份的语义是"**新建** `projects\<名字>` 空目录（零线程零板卡）"，本次改成"**在当前用户区铺骨架**"，两者不兼容。执行本规划收尾时，在旧文档顶部加：

```markdown
> ⚠️ **2026-10-28 已被 `doc/dust create用户区骨架规划.md` 取代**：语义从"新建空用户区目录"改为"在当前用户区铺开骨架（六模块三件套 + project 链路）"。本文档不再执行。
```
