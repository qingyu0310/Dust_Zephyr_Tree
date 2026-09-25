# dust create 空用户区调研与规划

> 日期：2026-09-21
> 目标：新增 `dust create <名字>` 子命令，一键在 `E:\Zephyr\projects\<名字>` 生成一个**空用户区**（零线程、零板卡）。
> 骨架来源：`E:\Zephyr\projects\template`（实盘模板，白名单拷贝 + 剥除板卡/线程）。

---

## 1. 需求

用户要建一个新用户区时，现在只能手工复制 `projects\template` 再逐个删板卡配置、删线程、改 Kconfig —— 容易漏、容易把 `build\`（几十 MB）和 `.cache\` 一起拷过去。

希望：

```text
dust create zhangsan
-> E:\Zephyr\projects\zhangsan 出现一个能直接开工的空用户区
```

"空"的定义（用户确认）：**零线程、零板卡** —— 没有任何 `TRD_*` 业务线程，没有任何 `boards\*\*\` 板卡配置目录。

---

## 2. 调研：空用户区到底需要什么

### 2.1 路径深度是硬约束

`<用户区>\project\CMakeLists.txt:12` 用相对路径定位子树：

```cmake
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr_user")
set(FW_ROOT         "${ZEPHYR_USER_DIR}/framework")
```

反推：`<用户区>\project\` 往上 3 级必须正好是 `E:\Zephyr\`。也就是

```text
E:\Zephyr\zephyr_user\...           ← 子树（深度固定）
E:\Zephyr\projects\<名字>\project\  ← 用户区必须在 projects\ 下一级
```

`SDK_GLUE_DIR` 兜底路径同款（`../../../../Zephyr_HPMicro/sdk_glue`）。

**结论**：`dust create` 的目标根只能是 `E:\Zephyr\projects\`，不能任意指定盘符/层级。多一层少一层都编译不过。

### 2.2 必须有（缺了直接炸）

对着 `template\project\CMakeLists.txt` 逐条推：

| # | 文件 | 依据 |
|---|---|---|
| 1 | `<名字>\project\CMakeLists.txt` | `dust build` 的规则 B 就靠它认"这是工作区根"（`build.bat:15`）；也是构建入口 |
| 2 | `<名字>\project\Kconfig` | 定义 `PRJ_MAIN`。没有它，`prj.conf` 里的 `CONFIG_PRJ_MAIN=y` 是给不存在的符号赋值 |
| 3 | `<名字>\project\prj.conf` | `CONFIG_PRJ_MAIN=y` 决定 framework 六层是否 `add_subdirectory`（`CMakeLists.txt:122`、`:146`） |
| 4 | `<名字>\project\thread\CMakeLists.txt` | `CMakeLists.txt:147` 的 `add_subdirectory(${APP_THREAD})` 是**无条件**执行的（只要 `PRJ_MAIN=y`），目录里没有 CMakeLists.txt 直接 CMake 报错 |

> `main()` 由 `framework/init/main.c` 提供，业务项目**不需要 src/**（`CMakeLists.txt:133` 注释）。所以空用户区零线程也能链接。

### 2.3 有更好（不影响能不能编）

| 文件/目录 | 作用 |
|---|---|
| `<名字>\algorithm\` `cmd\` `drivers\` `init\` `modules\` `topic\` | 用户层六模块（与架构层六模块同名不同路径）。`CMakeLists.txt:101/148` 用 `if(EXISTS .../CMakeLists.txt)` 保护，**空目录直接跳过**，不报错 |
| `<名字>\.clangd` | 编译数据库指向 `project/build/`，IDE 补全/跳转用 |
| `<名字>\project\boards\` | 板卡配置根目录。空着时 `file(GLOB)` 全部落空（见 §2.4），不报错 |
| `<名字>\project\.gitignore` | 挡 `build/`、`.cache/`、`.clangd` |

**六模块目录必须带 `.gitkeep`**：git 不跟踪空目录，不加占位就进不了版本库（`template` 现在就缺这个 —— 它没有 `.gitkeep` 也没被 git 管理，所以一直没暴露）。

### 2.4 没有板卡时会发生什么（关键调研结论）

`CMakeLists.txt:33-50` 的四条 glob，在 `boards\` 为空时**全部落空且不报错**：

```cmake
if(NOT DEFINED BOARD_CFG)
  	set(BOARD_CFG ${BOARD})       # 没有 -DBOARD_CFG 时退化成用板名当配置名
endif()

file(GLOB OVERLAY_FILES  .../boards/*/${BOARD_CFG}/${BOARD}.overlay)   # 空 → 不设 DTC_OVERLAY_FILE
file(GLOB PRJ_CONF_FILES .../boards/*/${BOARD_CFG}/${BOARD}.conf)      # 空 → 不设 EXTRA_CONF_FILE
file(GLOB BOARD_CMAKE    .../boards/*/${BOARD_CFG}/board.cmake)        # 空 → 不 include
foreach(_dir IN LISTS BOARD_GLOBAL_INCLUDES)                           # 未定义 → 空循环
```

所以空用户区**能配置、能进 CMake**，但**真编到哪块板、编不编得过，取决于板**：

| 板 | 空用户区能否编过 | 原因 |
|---|---|---|
| HPM（`hpm5361icb`） | 基本能编 | `template\boards\hpm\hpm5361icb\board.cmake` 只加了一条 USB QTD 宏 + runner 参数，不是编译必需 |
| STM32（`stm32f407igh6` / `stm32f4_disco`） | **编不过** | `board.cmake` 提供的 `BOARD_GLOBAL_INCLUDES` 里有 `${ZEPHYR_USER_DIR}/platform/cmsis` —— 缺了就 `cmsis_core.h: No such file or directory`（搭建指南 FAQ §9.2） |

**结论：空用户区是"结构完整但还烧不了"的状态。要真编译，至少补一个 `boards\<vendor>\<配置名>\board.cmake`。**

`dust build` 在空用户区的行为也要说清楚：扫描 `boards\*\<名字>\*.overlay` 找不到 → **退化成 `west build -b <名字>`**（`build.bat:37-41`），也就是说这时候要直接把 **Zephyr 板名**当参数传：

```powershell
dust build stm32f407igh6      # 而不是 board_rm_c
```

### 2.5 结论：空用户区文件清单

```text
E:\Zephyr\projects\<名字>\
├── .clangd                      ← 拷 template 根
├── algorithm\.gitkeep           ← 空目录（用户层六模块）
├── cmd\.gitkeep
├── drivers\.gitkeep
├── init\.gitkeep
├── modules\.gitkeep
├── topic\.gitkeep
└── project\
    ├── CMakeLists.txt           ← 拷 template project\
    ├── Kconfig                  ← 重写（剥 TRD_*，USE_CMD_* 默认关）
    ├── prj.conf                 ← 拷 template project\
    ├── .gitignore               ← 新建（template 没有，照 qingyu 补）
    ├── boards\.gitkeep          ← 空目录（零板卡）
    └── thread\
        └── CMakeLists.txt       ← 重写（空装配表）
```

共 **8 个目录 + 11 个文件**，不含任何 `boards\<vendor>\<cfg>\`、不含任何 `trd_*.cpp`。

---

## 3. 命令设计

### 3.1 命令形式

```text
dust create <名字>
```

- 参数只有一个：新用户区的名字。
- 目标固定 `<Zephyr>\projects\<名字>`（§2.1 的深度约束）。
- **不提供** `-t <模板名>`、不提供 `--board` 之类的选项 —— 模板写死 `template`，板卡是后面手动加的。

### 3.2 模板来源与剥除

| 动作 | 对象 |
|---|---|
| **拷** | `.clangd`、`project\CMakeLists.txt`、`project\prj.conf` |
| **重写** | `project\Kconfig`、`project\thread\CMakeLists.txt`、`project\.gitignore` |
| **建空** | 六个模块目录 + `.gitkeep`、`project\boards\` + `.gitkeep` |
| **不拷** | `project\build\`、`project\.cache\`（产物/缓存）、`boards\hpm|st\*\`（板卡配置）、`thread\gpio\` `thread\test\`（业务线程） |

`template` 以后往 `boards\` 里加板卡、往 `thread\` 里加线程，**都不会影响 `dust create` 的产物** —— 因为是白名单拷贝，不是整树复制。

### 3.3 源路径与目标路径的推导

脚本放在 `framework\cmd\create\create.bat`，用自身位置推导，不写死盘符：

```text
%~dp0                     = E:\Zephyr\zephyr_user\framework\cmd\create\
..\..\..\..\              = E:\Zephyr\
<上面>\projects\template  = E:\Zephyr\projects\template      ← 模板
<上面>\projects\<名字>    = E:\Zephyr\projects\<名字>        ← 目标
```

与 `project\CMakeLists.txt` 的相对路径假设完全一致（也是"用户区在 `Zephyr\projects\` 下一级"）。

---

## 4. 逐文件改动（old → new）

### 4.1 新增 `framework/cmd/create/create.bat`（整文件，照抄）

```bat
@echo off
chcp 65001 >nul
setlocal

rem dust create — 从 template 骨架生成一个空用户区（零线程、零板卡）
rem   dust create <name>  ->  <Zephyr>\projects\<name>

rem 推导仓库根（本脚本在 <Zephyr>\zephyr_user\framework\cmd\create\ 下）
pushd "%~dp0..\..\..\.."
set "ZEPHYR_ROOT=%CD%"
popd
set "PROJECTS_ROOT=%ZEPHYR_ROOT%\projects"
set "TPL_ROOT=%PROJECTS_ROOT%\template"

set "NAME=%~1"
if "%NAME%"=="" (
    echo Usage: dust create ^<name^>
    exit /b 1
)

set "DEST=%PROJECTS_ROOT%\%NAME%"
if exist "%DEST%" (
    echo [ERROR] target already exists: %DEST%
    exit /b 1
)
if not exist "%TPL_ROOT%\project\CMakeLists.txt" (
    echo [ERROR] template not found: %TPL_ROOT%
    exit /b 1
)

rem 用户区根：六个空模块目录（用户层）+ .gitkeep（git 不跟踪空目录）
mkdir "%DEST%" 2>nul
for %%m in (algorithm cmd drivers init modules topic) do (
    mkdir "%DEST%\%%m" 2>nul
    type nul > "%DEST%\%%m\.gitkeep"
)

rem 用户区根：clangd 配置（编译数据库指向 project\build\）
copy /y "%TPL_ROOT%\.clangd" "%DEST%\.clangd" >nul

rem project\：构建入口
mkdir "%DEST%\project" 2>nul
copy /y "%TPL_ROOT%\project\CMakeLists.txt" "%DEST%\project\CMakeLists.txt" >nul
copy /y "%TPL_ROOT%\project\prj.conf"       "%DEST%\project\prj.conf"       >nul

rem project\boards\：留空（板卡未定），占位保证进版本库
mkdir "%DEST%\project\boards" 2>nul
type nul > "%DEST%\project\boards\.gitkeep"

rem project\thread\：空装配表
mkdir "%DEST%\project\thread" 2>nul

rem project\.gitignore
> "%DEST%\project\.gitignore" echo .cache/
>>"%DEST%\project\.gitignore" echo build/
>>"%DEST%\project\.gitignore" echo .clangd

rem project\Kconfig：只留业务门禁 PRJ_MAIN；USE_CMD_* 默认关（开了就要外设 overlay）
> "%DEST%\project\Kconfig" echo # 用户区业务门禁 Kconfig
>>"%DEST%\project\Kconfig" echo # 业务门禁符号（PRJ_MAIN / TRD_XXX / USE_XXX）保持原名，不加 DUST_ 前缀
>>"%DEST%\project\Kconfig" echo # 业务门禁 select 的架构符号统一 DUST_ 前缀
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo config PRJ_MAIN
>>"%DEST%\project\Kconfig" echo     bool "Main business project"
>>"%DEST%\project\Kconfig" echo     default n
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo if PRJ_MAIN
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo config USE_CMD_SHELL
>>"%DEST%\project\Kconfig" echo     bool "Shell debug console (thread + var/log)"
>>"%DEST%\project\Kconfig" echo     default n
>>"%DEST%\project\Kconfig" echo     select DUST_CMD_SHELL_LOG
>>"%DEST%\project\Kconfig" echo     select DUST_CMD_SHELL_VAR
>>"%DEST%\project\Kconfig" echo     help
>>"%DEST%\project\Kconfig" echo       Shell debug console - UART thread + var/log sub-commands.
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo config USE_CMD_BUZZER
>>"%DEST%\project\Kconfig" echo     bool "Buzzer control"
>>"%DEST%\project\Kconfig" echo     default n
>>"%DEST%\project\Kconfig" echo     select DUST_CMD_BUZZER
>>"%DEST%\project\Kconfig" echo     help
>>"%DEST%\project\Kconfig" echo       Buzzer control - PWM beep via cmd/buzzer.
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo config USE_CMD_FLASH
>>"%DEST%\project\Kconfig" echo     bool "SPI Flash"
>>"%DEST%\project\Kconfig" echo     default n
>>"%DEST%\project\Kconfig" echo     select DUST_CMD_FLASH
>>"%DEST%\project\Kconfig" echo     help
>>"%DEST%\project\Kconfig" echo       SPI NOR Flash read/write/erase via cmd/flash.
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo endif # PRJ_MAIN
>>"%DEST%\project\Kconfig" echo.
>>"%DEST%\project\Kconfig" echo source "Kconfig.zephyr"

rem project\thread\CMakeLists.txt：空装配表
> "%DEST%\project\thread\CMakeLists.txt" echo # 业务线程装配 — 门禁 TRD_XXX 保持原名（不加 DUST_ 前缀）
>>"%DEST%\project\thread\CMakeLists.txt" echo # 新增线程时在此追加：
>>"%DEST%\project\thread\CMakeLists.txt" echo # if(CONFIG_TRD_XXX)
>>"%DEST%\project\thread\CMakeLists.txt" echo #     target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx/trd_xxx.cpp)
>>"%DEST%\project\thread\CMakeLists.txt" echo #     target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/xxx)
>>"%DEST%\project\thread\CMakeLists.txt" echo # endif()

echo [dust] created user area: %DEST%
exit /b 0
```

### 4.2 修改 `framework/cmd/build/dust.cmd`

**old（当前全文）：**

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

**new（全文替换，两处不同：加 create 分支 + usage 补一行）：**

```bat
@echo off
setlocal
rem dust CLI — 子命令分发
rem   dust build <board> [extra west build args]
rem   dust flash [extra west flash args]
rem   dust create <name>
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
echo        dust create ^<name^>
exit /b 1
```

> `create` 是脚本目录（`framework\cmd\create\`），与 `build\` 平级；`cmd\` 下其余目录（shell/buzzer/fatal/flash/linker）是**运行时 C++ 模块**，不放脚本。

### 4.3 文档同步（阶段 3）

| 文件 | 改动 |
|---|---|
| `framework/cmd/README.md` | §build/ 章节后新增 `create/` 章节：命令、骨架清单、与 template 的关系 |
| `doc/zephyr_子树架构搭建指南.md` | §6「建立用户区」（`搭建指南:296-330`）现在还在讲 `E:\Zephyr\projects\temp`（**该目录已不存在**），改成讲 `dust create` + 产物结构 |
| `快速上手.md` | 起新用户区处补 `dust create <名字>` |

---

## 5. 分阶段执行

### 阶段 1：create.bat 落地

- **目标**：`dust create <名字>` 能在 `projects\` 下生成 §2.5 那棵树。
- **具体干什么**：新建 `framework/cmd/create/create.bat`，内容 = §4.1 全文（逐字照抄）。
- **产出**：`create.bat`（新文件）。
- **验证**：§7 验证清单 1~8 条。

### 阶段 2：dust.cmd 分发

- **目标**：`dust create` 能调到脚本，`dust` 裸敲能列全三个子命令。
- **具体干什么**：`framework/cmd/build/dust.cmd` 按 §4.2 的 new 全文替换。
- **产出**：`dust.cmd`（改）。
- **验证**：§7 验证清单 9~10 条。

### 阶段 3：生成物可编译性验证（静态）

- **目标**：确认空用户区的 `CMakeLists.txt` 能吃下"零板卡"这个状态。
- **具体干什么**：`dust create _probe` 造一个，然后
  1. `dust build hpm5361icb`（HPM 板，预期编过）
  2. `dust build stm32f407igh6`（ST 板，**预期失败于 `cmsis_core.h`** —— 这是 §2.4 写明的预期，不是 bug）
  3. 验完删掉 `_probe`
- **产出**：实测结论回填 §2.4 的表格。
- **验证**：HPM 编过 + ST 按预期卡在 cmsis + 补一个 `boards\st\<配置名>\board.cmake` 后 ST 也编过。
- **注意**：**编译是用户的动作，AI 不代跑**（`verbatim-execution` 铁律第 8 条）。

### 阶段 4：真板烧录（用户执行）

空用户区要烧录必须先补板卡配置。

---

## 6. 关键细节

### 6.1 为什么是"白名单拷贝"而不是"整树复制再删"

`template\project\` 下面有 `build\`（几十 MB 编译产物）和 `.cache\clangd\`。整树复制再删：

- 白拷几十 MB 再删掉，慢且没必要；
- 万一删漏了，新用户区带着旧 `build\CMakeCache.txt` —— 里面写死了 `template` 的源路径，**新用户区一编译就报 cache 冲突**（2026-08-13 踩过：从 hpm5361 复制的用户区 build/ 残留 CMakeCache 指向旧路径）。

白名单拷贝从根上避免这两件事。

### 6.2 为什么 `USE_CMD_*` 保留但全改 `default n`

用户要的"空"是**零线程、零板卡**，`USE_CMD_*` 既不是线程也不是板卡，留在 Kconfig 里等于给用户区留了"cmd 层怎么开"的样板。

但**必须把 `default y` 改成 `default n`**：`USE_CMD_SHELL` 默认开 → `select DUST_CMD_SHELL_LOG/VAR` → 编译 `shell.cpp` → 它要用 `DT_ALIAS(shell_uart)` → 空用户区没有 overlay → **编译直接报错**。

### 6.3 为什么不给 `.ps1` 版

`build.ps1` / `flash.ps1` 是给已有 `build.ps1` 对齐用的；`dust create` 没有历史包袱，真实入口就是 `dust.cmd` → `create.bat` 一条路。再写一份 `.ps1` 只会多一处要同步维护的重复逻辑。要用 `create.ps1` 随时加。

### 6.4 注释里的 `template` 字样不做替换

拷过来的 `project\CMakeLists.txt` 里有几行注释写着 `# template/（用户区根）`、`# template/algorithm` 之类。**不替换**，原因：

1. 要在 `.bat` 里做字符串替换得借 PowerShell，且中文注释过 `Set-Content` 有编码风险（PowerShell 5.1 默认 ANSI）；
2. 这几处**只是注释**，`PROJECT_ROOT` 等变量全是 `${CMAKE_CURRENT_SOURCE_DIR}` 相对推导，功能上零影响。

真嫌碍眼，改一行注释比让脚本变脆划算。

### 6.5 目标已存在就报错，绝不覆盖

```bat
if exist "%DEST%" (
    echo [ERROR] target already exists: %DEST%
    exit /b 1
)
```

用户区里有别人的代码，覆盖是不可逆事故。宁可让用户自己删了再建。

### 6.6 不做 `git init`

用户没要求。要不要把新用户区变成 git 仓库由用户自己 `git init`。六个模块目录和 `boards\` 里的 `.gitkeep` 是为**将来**用户 `git init` 准备的（git 不跟踪空目录）。

---

## 7. 验证清单

| # | 操作 | 期望 |
|---|---|---|
| 1 | `dust create`（无参） | 打印 `Usage: dust create <name>`，返回非 0 |
| 2 | `dust create zhangsan` | 打印 `[dust] created user area: E:\Zephyr\projects\zhangsan`，返回 0 |
| 3 | 看 `projects\zhangsan\` | 六个模块目录 + `.clangd`，每个模块目录里有 `.gitkeep` |
| 4 | 看 `projects\zhangsan\project\` | `CMakeLists.txt` / `Kconfig` / `prj.conf` / `.gitignore` / `boards\.gitkeep` / `thread\CMakeLists.txt` |
| 5 | **确认没有** `project\build\`、`project\.cache\` | 两个目录都不存在 |
| 6 | **确认没有** `boards\hpm\*`、`boards\st\*`、`thread\gpio\`、`thread\test\` | 全部不存在 |
| 7 | 再敲一次 `dust create zhangsan` | 报 `[ERROR] target already exists`，返回非 0，**不覆盖** |
| 8 | `cat project\Kconfig` / `thread\CMakeLists.txt` | 中文注释不乱码；Kconfig 里没有 `TRD_GPIO` / `TRD_TEST`；`USE_CMD_*` 全是 `default n` |
| 9 | `dust`（裸敲） | 打印三行 usage（build / flash / create），返回非 0 |
| 10 | 在 `projects\zhangsan` 下 `dust flash` | 定位成功（进到 `zhangsan\project`），报 `no build directory found`（预期，因为还没 build） |
| 11 | 在 `projects\zhangsan` 下 `dust build hpm5361icb` | 编译通过（HPM 板不依赖板卡配置里的 include） |
| 12 | 在 `projects\zhangsan` 下 `dust build stm32f407igh6` | **预期失败于 `cmsis_core.h: No such file or directory`** —— §2.4 的结论，不是 bug |
| 13 | 照 `template\project\boards\st\board_rm_c\` 给 zhangsan 补一个 `boards\st\<配置名>\board.cmake` 后重编 | ST 板编过 → 证明"空用户区 + 一个 board.cmake"就够 |

---

## 8. 不做的事

1. **不做多模板**（`-t <模板名>`）—— 模板写死 `template`。
2. **不做 `git init`**、不建远端仓库。
3. **不自动加板卡配置** —— 空用户区就是空的，板卡用户自己挑。
4. **不问"要不要带 gpio 心跳线程"** —— 用户已确认零线程。
5. **不动 `template` 本身**（不删它的板卡/线程、不给它加 `.gitkeep`）—— 它是"满"的实盘模板，`dust create` 只从它白名单取件。
6. **不动 `dust build` / `dust flash` 的任何现有行为**。

---

## 9. 结论

`dust create <名字>` = **白名单拷贝 `projects\template` 的骨架文件 + 剥掉全部板卡与线程**。

调研的核心结论有两条，都写进脚本设计了：

1. **路径深度是硬约束** —— 用户区必须落在 `E:\Zephyr\projects\` 下一级，脚本只能建在那里。
2. **空用户区结构完整但还烧不了** —— `boards\` 空着时四条 glob 全落空、不报错，HPM 板能编，STM32 板会卡在 `cmsis_core.h`；要真用，补一个 `board.cmake` 就够。
