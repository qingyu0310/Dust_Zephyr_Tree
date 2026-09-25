# dust flash 入口规划

> 日期：2026-09-21
> 目标：新增 `dust flash` 子命令——把 `west flash` 从"必须先手动 `cd` 到项目 `project/` 目录"改成"可在工作区根直接执行"。
> 语义：`dust flash <参数...>` ≡ `cd <解析到的项目根> && west flash <参数...>`，**效果与 west flash 完全一致，只多一步自动进入项目目录**。

---

## 1. 背景与问题

### 1.1 现状：`dust build` 已经会自动进 project，`west flash` 不会

`dust build` 的工作区自动定位在 2026-08-25 已完成（见 `doc/dust构建入口工作区自动定位规划.md`），当前入口链路：

```text
dust build
-> framework\cmd\build\dust.cmd      子命令分发（只认 build）
-> framework\cmd\build\build.bat     定位项目根 +  west build
```

`build.bat` 里的定位逻辑（`framework/cmd/build/build.bat:12-28`）：

```bat
set "TARGET_DIR="
if exist "%CD%\CMakeLists.txt" (
    set "TARGET_DIR=%CD%"
) else if exist "%CD%\project\CMakeLists.txt" (
    set "TARGET_DIR=%CD%\project"
)
...
pushd "%TARGET_DIR%"
```

也就是说，在 `E:\Zephyr\zephyr_user` 直接敲 `dust build hpm5361icb`，脚本会自动 `pushd` 到
`E:\Zephyr\zephyr_user\project` 再编译，产物落在 `E:\Zephyr\zephyr_user\project\build`。

### 1.2 问题：`west flash` 在同一个目录敲就失败

用户在 `E:\Zephyr\zephyr_user` 直接敲 `west flash`：

```text
E:\Zephyr\zephyr_user> west flash
FATAL ERROR: no build directory found
```

**根因**：`west flash` 不带 `-d` 时，默认在**当前工作目录**下找 `build\`。
而产物在 `E:\Zephyr\zephyr_user\project\build`，不是 `E:\Zephyr\zephyr_user\build`。

所以真正缺的不是"另一套 flash 参数"，而是：

**`dust flash` 缺少"我当前在哪个工作区层级"的判断逻辑——和 build 当初的问题一模一样。**

### 1.3 为什么不能直接在 dust.cmd 里写死 `cd project`

那会把"自动进入 `project`"做成维护者工作区专属特判，用户工作区（`E:\Zephyr\projects\<user>`）吃不到同样能力。
`dust build` 当初就是踩这个坑改过来的，`flash` 必须一次走对。

---

## 2. 目标与边界

### 2.1 要达成的

1. 新增 `dust flash [west flash 的全部参数]` 子命令。
2. 项目根解析规则**与 `dust build` 完全一致**——复用同一套判定，不新增第二套语义。
3. 参数**原样透传**给 `west flash`：`--runner`、`-d/--build-dir`、`-p`、`--erase` 等全部生效。
4. 支持三种启动位置（与 build 相同）：

| 启动位置 | 例子 | 期望 |
|---|---|---|
| 维护者工作区根 | `E:\Zephyr\zephyr_user` | 自动进入 `zephyr_user\project` |
| 任意工作区根 | `E:\Zephyr\projects\qingyu` | 自动进入该工作区自己的 `project\` |
| 任意项目根 | `E:\Zephyr\zephyr_user\project` 等 | 留在当前目录 |

### 2.2 不做的

1. 不引入板卡名位置参数（`dust flash hpm5361icb` 这种）——`west flash` 从 build 目录读 BOARD 与 runner，**不需要** `-b`。
2. 不在 `dust.cmd` 里写路径判断逻辑（入口壳只负责分发，否则 `dust flash` 与直接调 `flash.bat` 行为会分叉）。
3. 不做"flash 前自动重新编译"。
4. 不改 `dust build` 的任何现有行为、不碰 `west` 配置。

---

## 3. 改后的入口链路

```text
dust flash
-> framework\cmd\build\dust.cmd      新增 flash 分发分支
-> framework\cmd\build\flash.bat     新增：定位项目根 → pushd → west flash
```

与 build 完全同构：

| | 入口壳 | 干活的脚本 | 定位逻辑 |
|---|---|---|---|
| build | `dust.cmd`（build 分支） | `build.bat` | `<cwd>` → `<cwd>\project` → 报错 |
| **flash** | `dust.cmd`（**flash 分支**） | **`flash.bat`** | **同上，逐字复用** |

---

## 4. 逐文件改动（old → new）

### 4.1 新增 `framework/cmd/build/flash.bat`（整文件）

**写死内容，直接照抄：**

```bat
@echo off
chcp 65001 >nul

rem 固定 ZEPHYR_BASE 与 SDK_GLUE_DIR（覆盖环境变量缺失/脏值）
set "ZEPHYR_BASE=E:\Zephyr\zephyr"
set "SDK_GLUE_DIR=E:\Zephyr_HPMicro\sdk_glue"

set "TARGET_DIR="
if exist "%CD%\CMakeLists.txt" (
    set "TARGET_DIR=%CD%"
) else if exist "%CD%\project\CMakeLists.txt" (
    set "TARGET_DIR=%CD%\project"
)

if "%TARGET_DIR%"=="" (
    echo [ERROR] dust flash could not locate a project root from:
    echo         current dir: %CD%
    echo         expected one of:
    echo         - %CD%\CMakeLists.txt
    echo         - %CD%\project\CMakeLists.txt
    exit /b 1
)

pushd "%TARGET_DIR%"

west flash %*

set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
```

与 `build.bat` 的差别只有三处，其余逐字相同：

1. **没有 `set NAME=%1`**——flash 没有板卡位置参数。
2. **没有 `enabledelayedexpansion`**——flash 没有 `for /d` 循环、不需要 `!VAR!`，去掉可避免参数里的 `!` 被吃掉。
3. **`west flash %*`** 而不是 `west build -b ... %2 %3 ... %9`——全部参数原样透传。

### 4.2 新增 `framework/cmd/build/flash.ps1`（整文件）

与 `build.ps1` 对齐（`build.ps1` 是同一批脚本里的 PowerShell 版）：

```powershell
param(
    [string]$Opts = ""
)

# 固定 SDK_GLUE_DIR（覆盖系统可能存在的脏环境变量）
$env:SDK_GLUE_DIR = "E:\Zephyr_HPMicro\sdk_glue"

# 定位目标项目：当前目录已是项目根→当前目录；工作区根→其下 project；否则报错
$__cwd = (Get-Location).Path
$targetDir = $null
if (Test-Path (Join-Path $__cwd "CMakeLists.txt")) {
    $targetDir = $__cwd
} elseif (Test-Path (Join-Path $__cwd "project\CMakeLists.txt")) {
    $targetDir = Join-Path $__cwd "project"
}

if ($null -eq $targetDir) {
    Write-Error @"
dust flash could not locate a project root from:
  current dir: $__cwd
  expected one of:
  - $__cwd\CMakeLists.txt
  - $__cwd\project\CMakeLists.txt
"@
    exit 1
}

Set-Location $targetDir

west flash $Opts
```

> 说明：`dust flash` 的真实入口是 `.bat`（`dust.cmd` 只调 bat）。`.ps1` 是**与 `build.ps1` 保持齐**，供习惯 PowerShell 的场景直接调。

### 4.3 修改 `framework/cmd/build/dust.cmd`

**old（当前全文，`framework/cmd/build/dust.cmd:1-9`）：**

```bat
@echo off
setlocal
rem dust CLI — 子命令分发：dust build <board> [extra west build args]
if /i "%~1"=="build" (
    call "%~dp0build.bat" %2 %3 %4 %5 %6 %7 %8 %9
    exit /b %errorlevel%
)
echo Usage: dust build ^<board^> [extra west build args]
exit /b 1
```

**new（全文替换）：**

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

`%2 %3 %4 %5 %6 %7 %8 %9` 的写法与 build 分支逐字一致（`%1` 已被 `dust.cmd` 自己消费掉，所以从这里开始传）。

### 4.4 文档同步（阶段 3，见 §6）

| 文件 | 改动 |
|---|---|
| `framework/cmd/README.md` | §build/ 章节（`README.md:180-203`）改成"编译与烧录脚本"，补 `flash.bat` / `flash.ps1` 说明与用法示例 |
| `doc/zephyr_子树架构搭建指南.md` | §8.1（`搭建指南:381-412`）里 `dust` 的定义补上 flash；"它是什么"从两个脚本改成三个脚本；用法示例补 `dust flash` |
| `快速上手.md` | `快速上手.md:82-88` 与 `:168` 附近，在 `dust build` 之后补 `dust flash` |

---

## 5. 参数透传语义（必须逐条对上）

`dust flash` 后写什么，就原样送给 `west flash`，**不增不减不改**：

| 用户敲的 | 实际执行（在解析到的项目根下） |
|---|---|
| `dust flash` | `west flash` |
| `dust flash --runner openocd` | `west flash --runner openocd` |
| `dust flash -d build2` | `west flash -d build2` |
| `dust flash --erase` | `west flash --erase` |

**关键**：`-d/--build-dir` 是**相对 pushd 之后的目录**解析的，
所以 `dust flash -d build2` 找的是 `<项目根>\build2`——与"手动 `cd` 进 `project` 再 `west flash -d build2`"完全一致。这就是"效果跟 west flash 一样"的定义。

---

## 6. 分阶段执行方案

> 每个阶段按"目标 → 具体干什么 → 产出 → 验证"推进，验证通过再进下一阶段。

### 阶段 1：flash.bat 落地 + dust.cmd 分发

- **目标**：`dust flash` 在三种启动位置都能解析到正确的项目根并执行 `west flash`。
- **具体干什么**：
  1. 新建 `framework/cmd/build/flash.bat`，内容 = §4.1 全文（逐字照抄）。
  2. 修改 `framework/cmd/build/dust.cmd`，按 §4.3 的 new 全文替换。
- **产出**：`flash.bat`（新文件）、`dust.cmd`（改）。
- **验证**：按 §8 的静态验证清单 1~5 条逐条跑，全部符合期望。

### 阶段 2：flash.ps1 对齐

- **目标**：PowerShell 侧与 build.ps1 齐平。
- **具体干什么**：新建 `framework/cmd/build/flash.ps1`，内容 = §4.2 全文。
- **产出**：`flash.ps1`（新文件）。
- **验证**：在 `E:\Zephyr\zephyr_user` 执行 `.\framework\cmd\build\flash.ps1`，能进入 `project\` 并调起 `west flash`；
  在既无 `CMakeLists.txt` 也无 `project\CMakeLists.txt` 的目录执行，报错退出码非 0。

### 阶段 3：文档同步

- **目标**：仓库内三处文档与实现一致。
- **具体干什么**：按 §4.4 表格逐文件改。
- **产出**：`framework/cmd/README.md`、`doc/zephyr_子树架构搭建指南.md`、`快速上手.md`。
- **验证**：三份文档里搜 `dust`，不出现"只封装 west build"这类过期说法。

### 阶段 4：真板验证（由用户执行）

- **目标**：确认 flash 真能烧进去。
- **具体干什么**：在项目根先 `dust build <板卡名>`，再 `dust flash`。
- **验证**：烧录成功（west flash 正常输出 + 板卡复位运行）。
- **注意**：**编译/烧录是用户的动作，AI 不代跑**。

---

## 7. 关键细节

### 7.1 为什么 flash 不需要板卡名

`west build -b <BOARD>` 是**构建期**参数，被写进 `build\CMakeCache.txt`；
`west flash` 是**烧录期**命令，它从 build 目录里读 BOARD 与 runner（runner 也可用 `--runner` 覆盖）。

所以 `build.bat` 里那套"扫 `boards\*\<name>\*.overlay` 反推 BOARD"的逻辑，**flash 一点都用不上**，
照抄过来反而多一次易错的目录扫描。

### 7.2 为什么必须先 `pushd` 再 `west flash`

`west flash` 不带 `-d` 时只在**当前目录**找 `build\`。
不 `pushd` 就在调用点找（工作区根下没有 `build\`）→ 报 `no build directory found`。
`pushd` 到解析出的项目根，就等价于用户自己 `cd` 进去，这正是本次要补的那一步。

### 7.3 为什么用 `%*` 而不是 `%1 %2 ... %9`

- flash 没有要剥离的位置参数（build 剥 `%1` 当板卡名，flash 没这个需求），全部透传才真正等价于 `west flash`。
- `%*` 没有 9 个参数的上限。

### 7.4 为什么 flash.bat 不复制 `enabledelayedexpansion`

`build.bat` 开延迟展开是因为要用 `!BOARD!`（在 `for /d` 块里赋值再读）。
`flash.bat` 没有循环、不读块内赋值的变量，开了反而可能把参数里的 `!` 吃掉。

### 7.5 报错信息

与 build 同款格式，只把 `dust build` 换成 `dust flash`，让用户一眼看出是谁没找到项目根：

```text
[ERROR] dust flash could not locate a project root from:
        current dir: <cwd>
        expected one of:
        - <cwd>\CMakeLists.txt
        - <cwd>\project\CMakeLists.txt
```

### 7.6 `build\` 产物位置没有变

`dust build` 的产物仍在**解析到的项目根**下（`<项目根>\build`），`dust flash` 也读同一个目录。
本规划**不改** `build\` 的落点，只补上"读它之前先站对位置"。

---

## 8. 静态验证清单

> 前提：对应项目已经 `dust build` 过、存在 `build\` 目录（否则 flash 本身会正常报"no build directory"，那是预期）。

| # | 启动位置 | 命令 | 期望 |
|---|---|---|---|
| 1 | `E:\Zephyr\zephyr_user` | `dust flash` | 进入 `E:\Zephyr\zephyr_user\project` 执行 `west flash` |
| 2 | `E:\Zephyr\zephyr_user\project` | `dust flash` | 留在当前目录执行 `west flash` |
| 3 | `E:\Zephyr\projects\qingyu` | `dust flash` | 进入 `E:\Zephyr\projects\qingyu\project` |
| 4 | `E:\Zephyr\projects\qingyu\project` | `dust flash` | 留在当前目录 |
| 5 | 既无 `CMakeLists.txt` 也无 `project\CMakeLists.txt` 的目录 | `dust flash` | 明确报错，**不误跳到** `zephyr_user\project` |
| 6 | `E:\Zephyr\zephyr_user` | `dust flash --runner openocd` | 参数原样透传给 `west flash` |
| 7 | `E:\Zephyr\zephyr_user` | `dust flash -d build2` | 找的是 `<项目根>\build2` |
| 8 | 任意目录 | `dust` | 打印两行 usage（build / flash）并返回非 0 |

字段对照：验证 1~5 验证**定位**，6~7 验证**透传**，8 验证**分发**。

---

## 9. 与 `dust build` 的差异对照

| 维度 | `dust build` | `dust flash` |
|---|---|---|
| 位置参数 | `<board_cfg>`（必填，默认 `hpm6e00evk`） | **无** |
| 板卡反推 | 扫 `boards\*\<name>\*.overlay` 反推 BOARD | **不做**（从 build 目录读） |
| 实际命令 | `west build -b <BOARD> ... -- -DBOARD_CFG=<name>` | `west flash <全部参数>` |
| 项目根解析 | `<cwd>` → `<cwd>\project` → 报错 | **同上，逐字复用** |
| 产物/读取目录 | 写 `<项目根>\build` | 读 `<项目根>\build` |
| 环境变量 | 设 `ZEPHYR_BASE`、`SDK_GLUE_DIR` | 同上 |
| `.ps1` 版 | `build.ps1` | `flash.ps1`（对齐） |

---

## 10. 结论

本次要加的不是"另一条 west flash 包装"，而是：

**把 `dust build` 已验证过的工作区定位语义复用到 `dust flash` 上，让烧录和编译站在同一个项目根。**

这样才同时满足：

1. 在 `E:\Zephyr\zephyr_user` 直接 `dust flash`（不用先 `cd project`）
2. 在用户工作区根直接 `dust flash`
3. 参数行为与裸 `west flash` 完全一致，不引入第二套 flash 语义
