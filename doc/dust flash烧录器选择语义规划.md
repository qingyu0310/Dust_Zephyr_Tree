# dust flash 烧录器选择语义规划

> 日期：2026-10-23
> 目标：把「`dust flash` 怎么按芯片选烧录工具」这件事的语义讲清楚，并把当前散在各处的配置说明白。
> 前置：`doc/dust flash入口规划.md`（2026-09-21，已完成——`dust flash` 只负责"站对项目根"）。

---

## 0. 一句话结论

**`dust flash` 不选烧录器。烧录器由 `board.cmake` 在 `west build` 那一刻配好、固化进 `build/zephyr/runners.yaml`，`dust flash` 只做两件事：站到项目根、把参数原样丢给 `west flash`。**

`board.cmake` 就是给每个板卡配置烧录器的文件——这是它的本职，不是兼职。本规划不改这个归属，只把**配置怎么生效**讲清楚，并修掉当前配置里的三处具体缺陷。

---

## 1. 当前链路：一次 `dust flash` 到底发生了什么

### 1.1 烧录期（dust flash）

```text
dust flash [参数...]
  -> framework/cmd/build/dust.cmd          "flash" 分支分发
  -> framework/cmd/build/flash.bat         定位项目根 -> pushd -> west flash %*
  -> west flash                            读 <项目根>/build/zephyr/runners.yaml
       ├─ flash-runner: openocd            决定用哪个烧录工具
       ├─ args.openocd: [...]              决定这个工具怎么连、连哪颗芯片
       └─ config.openocd / openocd_search  决定工具本体在哪个路径
```

证据：
- [flash.bat:24-26](../framework/cmd/build/flash.bat#L24-L26) —— `pushd "%TARGET_DIR%"` 之后直接 `west flash %*`，**没有一行读板卡**。
- [dust.cmd:11-14](../framework/cmd/build/dust.cmd#L11-L14) —— 只做 `call flash.bat %2 %3 ... %9`，参数不增不减。

### 1.2 构建期：`board.cmake` 分两个来源被 include

```text
west build -b <BOARD> -- -DBOARD_CFG=<cfg>
  │
  ├─【板卡定义层】BOARD_ROOT 下的真板卡
  │     zephyr_user/boards/st/stm32g431cbtx/board.cmake
  │     E:/Zephyr_HPMicro/sdk_glue_user/boards/hpmicro/hpm5361icb/board.cmake
  │     职责：include 官方 common runner（决定候选集合）+ board_runner_args（芯片连接参数）
  │
  └─【项目侧板级配置】project/boards/<vendor>/<cfg>/board.cmake
        project/CMakeLists.txt:41-44  glob + include 进来
        职责：BOARD_GLOBAL_INCLUDES / BOARD_FLASH_RUNNER 选默认 / 工具路径 / 找 openocd.cfg
  │
  └─ 两者产出的 runner 状态，在 find_package(Zephyr) 后固化进
        build/zephyr/runners.yaml
```

证据：
- [project/CMakeLists.txt:41-44](../project/CMakeLists.txt#L41-L44) —— 项目侧 board.cmake 是 `file(GLOB ...) + include()`，发生在 [project/CMakeLists.txt:102](../project/CMakeLists.txt#L102) `find_package(Zephyr)` **之前**。
- 板卡定义层来自 `BOARD_ROOT`（[project/CMakeLists.txt:66-70](../project/CMakeLists.txt#L66-L70) 把 `zephyr_user` 和 HPM 树都加了进去）。

**两层各管一段，不冲突**：板卡定义层回答"这颗芯片支持哪些工具"，项目侧 board.cmake 回答"这个项目用哪个工具、怎么连"。

### 1.3 固化的位置

`build/zephyr/runners.yaml` 是唯一权威。当前 `hpm5361icb` 的实际内容（节选）：

```yaml
runners:
- openocd
flash-runner: openocd
config:
  openocd: E:/Zephyr_HPMicro/sdk_env/tools/openocd/openocd.exe
  openocd_search:
    - E:/Zephyr_HPMicro/sdk_env/tools/openocd/tcl
args:
  openocd:
    - --config=.../probes/cmsis_dap.cfg
    - --config=.../soc/hpm5300.cfg
    - --config=.../boards/hpm5300evk.cfg
    - --cmd-pre-init=adapter speed 500     # ← 第 1 次
    - --target-handle=_CHIPNAME.cpu0
    - --cmd-pre-init=adapter speed 500     # ← 第 2 次（重复）
```

写这个文件的是 Zephyr 的 [cmake/flash/CMakeLists.txt:97-100](file:///E:/Zephyr/zephyr/cmake/flash/CMakeLists.txt)：

```cmake
if(DEFINED BOARD_FLASH_RUNNER)
  runners_yaml_append("\n# Default flash runner if --runner is not given.")
  runners_yaml_append("flash-runner: ${BOARD_FLASH_RUNNER}")
endif()
```

---

## 2. board.cmake 里每一项配置的语义

### 2.1 四个配置项，各管什么

| 配置 | 写在哪 | 语义 | 结果落在 runners.yaml 的 |
|------|--------|------|------------------------|
| `include(${ZEPHYR_BASE}/boards/common/xxx.board.cmake)` | 板卡定义层 | **声明这个工具可用**（候选集合） | `runners:` |
| `board_runner_args(<runner> "参数")` | 两层都可以 | **给这个工具传参数**（芯片型号、接口、速率） | `args.<runner>:` |
| `set(BOARD_FLASH_RUNNER <runner>)` | 项目侧 | **从候选里选默认** | `flash-runner:` |
| `set(OPENOCD "<路径>")` | 项目侧 | **工具本体在哪** | `config.openocd:` |

**四者是叠加的，不是互斥的**：候选集合由板卡层定，参数两层都能追加，默认和路径由项目侧定。

### 2.2 三个板卡的实际配置

| 板卡 | 候选 runner | 默认 | 芯片连接参数来源 | 工具路径来源 |
|------|------------|------|-----------------|-------------|
| `st/stm32g431cbtx` | openocd / jlink / stm32cubeprogrammer | 项目侧选 `openocd` | 板卡层（`--device=STM32G431CB` 等）+ 项目侧 `openocd.cfg` | 项目侧 `set(OPENOCD ...)` |
| `hpm/hpm5361icb` | openocd | 板卡层 `board_set_flasher_ifnset` 兜底 | 板卡层（cmsis_dap + hpm5300.cfg + speed 500） | 板卡层按 `SDK_GLUE_DIR` 相对推导 |
| `st/puzhong` | openocd / jlink / stm32cubeprogrammer | 项目侧选 `openocd` | 板卡层 + 项目侧 `openocd.cfg` | 项目侧 `set(OPENOCD ...)` |

### 2.3 `app_set_runner_args` 是什么

不是项目自造宏，是 **Zephyr 官方 hook**——[cmake/modules/extensions.cmake:841-879](file:///E:/Zephyr/zephyr/cmake/modules/extensions.cmake)：

> Within application CMakeLists.txt files, ensure that all calls to board_runner_args() are part of a macro named app_set_runner_args()... The build system tests for the existence of the macro and will invoke it at the appropriate time if it is defined.

调用点在 `board_finalize_runner_args()` 内部（同文件 :878-880），每个 runner 终结时调一次。

**为什么项目侧 board.cmake 要用它而不是直接写 `board_runner_args`**：直接写会在 `include` 时立刻追加；包进 `app_set_runner_args` 则等 Zephyr 真正终结 runner 时才追加，顺序更靠后（优先级更高）。这是官方推荐用法，**放在 board.cmake 里合法且正确**——board.cmake 本来就是在 app 作用域被 include 的。

它拿不到"当前在终结哪个 runner"，所以三个板卡都用 `get_property(_OCD_DONE ...)` 做了一次性守卫。这个守卫是必要的（否则多 runner 场景会重复追加），但**可以在 HPM 那个只有单个 runner 的板卡上去掉**（见 §3.2）。

### 2.4 `--runner` 是运行时覆盖

`west flash --runner jlink` 走命令行显式参数，优先级高于 `runners.yaml` 的 `flash-runner`。**这是 `dust flash` 不需要关心 runner 选择的技术根因**——参数原样透传，覆盖能力天然存在。

---

## 3. 当前配置里的三处具体缺陷

### 3.1 `dust flash` 对烧录器完全不可见

用户敲 `dust flash`，看不到"这次要用什么工具烧哪颗芯片"。全都藏在 `runners.yaml` 里。
出问题时（比如没插 DAP-Link）第一反应是"是不是 runner 选错了"，但命令行没有任何线索。

**这是可观测性问题，不是配置问题**——配置本身完全正确。

### 3.2 HPM 项目侧与板卡层重复写同一句参数

[project/boards/hpm/hpm5361icb/board.cmake:4-6](../project/boards/hpm/hpm5361icb/board.cmake#L4-L6)：

```cmake
macro(app_set_runner_args)
  board_runner_args(openocd "--cmd-pre-init=adapter speed 500")
endmacro()
```

而 HPM 板卡定义层（`sdk_glue_user/boards/hpmicro/hpm5361icb/board.cmake`）里已经有一模一样的：

```cmake
"--cmd-pre-init=adapter speed 500")
```

结果就是 `runners.yaml` 里 **`--cmd-pre-init=adapter speed 500` 出现两次**（见 §1.3 节选）。OpenOCD 会对同一个 `pre-init` 执行两遍 `adapter speed 500`——**当前无害，但配置重复会掩盖后续真正的调参**。

### 3.3 `BOARD_CFG_DIR` 依赖未定义的 `PROJ_DIR`

[project/boards/st/gongxun/board.cmake:12](../project/boards/st/gongxun/board.cmake#L12)：

```cmake
set(BOARD_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PROJ_DIR}/boards")
```

`PROJ_DIR` 在 `zephyr_user/project` 里**从未定义**（只在 `projects/dust/CMakeLists.txt:7` 有）。实际靠 `${PROJ_DIR}` 展开成空串拼出 `project//boards`——多一个斜杠仍是合法路径，所以"碰巧能跑"。

哪天有人给 `project/CMakeLists.txt` 补上 `set(PROJ_DIR project)`，这里立刻变成 `project/project/boards` 而失效。

---

## 4. 目标语义

### 4.1 一条清晰的因果链

```text
芯片型号
  └─ 板卡定义层 include 哪些 common runner（候选集合）
       └─ 项目侧 board.cmake 选哪个作默认（BOARD_FLASH_RUNNER）
            └─ build 期写进 runners.yaml 的 flash-runner
                 └─ dust flash 实际调起哪个工具
```

### 4.2 每条信息只有一个家

| 信息 | 归属 |
|------|------|
| 芯片是什么、有哪些可用 runner | 板卡定义层 `<board>.yaml` + `board.cmake` |
| 这个工具怎么连这颗芯片 | 板卡定义层 `board_runner_args()` |
| 本项目的板卡用哪个 runner 作默认 | 项目侧 `board.cmake` 的 `BOARD_FLASH_RUNNER` |
| 本项目板卡的工具路径与额外配置 | 项目侧 `board.cmake` 的 `set(OPENOCD)` / `app_set_runner_args` |
| 最终生效值 | `build/zephyr/runners.yaml`（只读，不手改） |

### 4.3 `dust flash` 应该给出可观测性

不改 `dust flash` 的"原样透传"语义，但**执行前把生效配置打出来**：

```text
[dust] project root : E:\Zephyr\zephyr_user\project
[dust] board        : hpm5361icb
[dust] flash runner : openocd
[dust] openocd      : E:/Zephyr_HPMicro/sdk_env/tools/openocd/openocd.exe
<west flash 的原始输出>
```

这样"用什么工具烧的"从"藏在 yaml 里"变成"命令行第一屏就有"。

---

## 5. 分阶段执行方案

> 每个阶段按「目标 → 具体干什么 → 产出 → 验证」推进，验证通过再进下一阶段。
> **编译/烧录是用户的动作，AI 不代跑。**

### 阶段 A：让 `dust flash` 可观测（不改任何构建逻辑）

- **目标**：烧录前打印生效的 board / runner / 工具路径，出问题时一眼定位。
- **具体干什么**：
  1. 改 [flash.bat](../framework/cmd/build/flash.bat)：在 `west flash %*` 之前插入一段只读打印，数据来源 = `<项目根>/build/zephyr/runners.yaml` 与 `build/build_info.yml`：
     ```bat
     rem 打印生效配置（只读，不参与烧录）
     if exist "build\zephyr\runners.yaml" (
         echo [dust] project root : %CD%
         findstr /b /c:"flash-runner:" build\zephyr\runners.yaml
         findstr /b /c:"  openocd:"     build\zephyr\runners.yaml
     ) else (
         echo [dust] no build\zephyr\runners.yaml -- run "dust build" first
     )
     ```
     板卡名从 `build\build_info.yml` 的 `name:` 抓，同样用 `findstr`。
  2. `flash.ps1` 用 `Select-String` 做同等输出。
  3. **`west flash %*` 那一行一个字不改**。
- **产出**：`flash.bat` / `flash.ps1`（各加 ~6 行只读打印）。
- **验证**：
  - 有 build 目录时，`dust flash` 先打 4 行 `[dust]` 再走 west 输出；
  - 没有 build 目录时，打出 `[dust] no build\zephyr\runners.yaml` 并**仍然**让 west 自己报 `no build directory found`（不吞错）；
  - `dust flash --runner jlink` 的参数透传行为完全不变。

### 阶段 B：去掉 HPM 项目侧的重复参数

- **目标**：`--cmd-pre-init=adapter speed 500` 在 `runners.yaml` 里只出现一次。
- **具体干什么**：
  1. 删除 [project/boards/hpm/hpm5361icb/board.cmake:4-6](../project/boards/hpm/hpm5361icb/board.cmake#L4-L6) 整个 `app_set_runner_args` 宏——板卡定义层已经给了这句，项目侧不需要补。
  2. 保留文件第 1-2 行的 `add_compile_definitions(USB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT=2)`（那是编译选项，与 runner 无关）。
  3. 用户区（`projects/*/project/boards/hpm/*/board.cmake`）如果有同款重复，同步删。
- **产出**：`project/boards/hpm/hpm5361icb/board.cmake`（只剩 2 行）。
- **验证**：删 `build/` 重新 `dust build`，`grep -c "adapter speed 500" build/zephyr/runners.yaml` 输出 `1`；`dust flash` 能烧。

### 阶段 C：修 `PROJ_DIR` 的隐式依赖

- **目标**：项目侧 board.cmake 的 `BOARD_CFG_DIR` 不再依赖一个未定义变量。
- **具体干什么**：
  1. 把 [project/boards/st/gongxun/board.cmake:12](../project/boards/st/gongxun/board.cmake#L12) 的
     `set(BOARD_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PROJ_DIR}/boards")`
     改成直接用 `CMAKE_CURRENT_SOURCE_DIR`（board.cmake 被 include 时该变量就是 `project/`，`boards/` 就在它下面）：
     ```cmake
     set(BOARD_CFG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards")
     ```
  2. `puzhong` 与用户区同款文件同步改。
- **产出**：三个（或更多）项目侧 `board.cmake`。
- **验证**：ST 板卡 `dust build` 后，`runners.yaml` 的 `args.openocd` 里仍带 `--config=<项目>/boards/st/<cfg>/openocd.cfg`；不出现 `project/project` 这类路径。

### 阶段 D：给 `BOARD_FLASH_RUNNER` 补语义（注释 + 文档）

- **目标**：读代码的人不用推理就知道"板卡层给候选、项目侧选默认"。
- **具体干什么**：
  1. 在项目侧 board.cmake 的 `set(BOARD_FLASH_RUNNER openocd)` 上方加一行注释：
     ```cmake
     # 板卡层已提供 openocd / jlink / stm32cubeprogrammer 三个候选，这里选默认
     # 临时换工具：dust flash --runner jlink
     ```
  2. `framework/cmd/README.md` 的 build 章节补一小节「烧录器怎么选」，写 §2.1 那张四行表 + `--runner` 覆盖语义。
  3. `快速上手.md` 的 flash 段落补一句结论。
- **产出**：`board.cmake` 注释、`framework/cmd/README.md`、`快速上手.md`。
- **验证**：在新版 README 里能只靠表格回答"换 jlink 怎么办"。

---

## 6. 验证清单

| # | 阶段 | 操作 | 期望 |
|---|------|------|------|
| 1 | A | 有 build 目录，`dust flash` | 先打 `[dust] project root / board / flash runner / openocd` 四行 |
| 2 | A | 无 build 目录，`dust flash` | 打 `[dust] no ... runners.yaml`，随后 west 自己报错，退出码非 0 |
| 3 | A | `dust flash --runner jlink` | 参数原样透传，行为与裸 `west flash --runner jlink` 一致 |
| 4 | B | HPM 清 build 后 `dust build` | `grep -c "adapter speed 500" build/zephyr/runners.yaml` = `1` |
| 5 | B | HPM `dust flash` | 烧录成功 |
| 6 | C | ST 清 build 后 `dust build` | `runners.yaml` 里 openocd 参数含正确 `openocd.cfg` 绝对路径，无 `project/project` |
| 7 | D | 读 `framework/cmd/README.md` | 能只靠表格回答「怎么临时换 jlink」 |

字段对照：1-3 验证**可观测性**，4-5 验证**去重**，6 验证**路径正确性**，7 验证**文档可回答性**。

---

## 7. 关键细节

### 7.1 为什么 `dust flash` 不该自己"按芯片选工具"

`west flash` 已经把这个决策做完了，并且在 `runners.yaml` 里落了盘。`dust flash` 再去"选一次"就是**第二套真相**——两套真相迟早不一致（比如用户 `--runner` 覆盖了，dust 还按自己那套打印）。

`dust flash` 正确的参与方式是**只读地展示**，不是重新决策。

### 7.2 为什么工具路径写绝对路径是当前的做法

`set(OPENOCD "C:/Program Files/Path/openocd/bin/openocd.exe")` 是项目侧 board.cmake 的合法配置——它回答"这个项目的板卡用哪份 openocd"。

代价是换机器/换安装位置要改这行。这是**可接受的取舍**：它换来的是"不用猜系统 PATH 里那份 openocd 是什么版本"。只要换环境时记得改这一行就行。

### 7.3 `runners.yaml` 是只读产物

它由 `west build` 生成，手改会在下次 build 时被覆盖。任何"我想改 runner 参数"的需求，都只能改 `board.cmake` 再 build——**这是本规划所有阶段都不碰 `runners.yaml` 的原因**。

### 7.4 `app_set_runner_args` 的守卫为什么可以按板卡简化

`get_property(_OCD_DONE ...)` 守卫的目的是"确保这段只追加一次"。它防的是 `board_finalize_runner_args()` 被多个 runner 调用时重复执行。

- ST 板卡有三个 candidate runner → 守卫必要；
- HPM 只有一个 runner，且板卡层已经给了同样的参数 → 项目侧这段可以直接删（阶段 B），不需要守卫也不需要参数。

### 7.5 与 shell 拆分的关系

2026-10-23 shell 从 cmd 拆成独立子模块，`framework/cmd/build/` 下的 `build.bat` / `flash.bat` / `dust.cmd` **留在 cmd**——它们是构建与烧录入口，不是调试控制台。本规划不改这些脚本的位置，只改 `flash.bat` 的内容（阶段 A）。

---

## 8. 结论

`board.cmake` 配置烧录器这件事，**归属本来就是对的**：板卡定义层给候选，项目侧 board.cmake 选默认 + 配路径 + 配芯片参数。

当前需要修的只有三处落地缺陷：

1. **不可观测**——决策结果藏在 `runners.yaml`，命令行看不到（阶段 A）；
2. **HPM 参数重复**——同一句 `adapter speed 500` 写了两遍（阶段 B）；
3. **`PROJ_DIR` 隐式依赖**——未定义变量靠空串碰巧正确（阶段 C）。

再加一处文档补充（阶段 D），让"候选 → 默认 → 覆盖"这条链不用读代码就能懂。
