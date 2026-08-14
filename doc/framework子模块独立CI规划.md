# CI/CD 规划（板卡 CI：project 多板卡真板 CI + 主仓库门禁）

> 2026-08-14 重写。**背景**：原"framework 子模块独立 CI + project 多板卡 CI"三层规划，经执行阶段 2（algorithm 试点）实测后发现致命缺陷：
>
> **层 1（framework 子模块 native_sim CI）名不副实，已废弃**。原因：native_sim 只能编纯逻辑层（algorithm/topic/init），cmd/drivers/modules 依赖硬件设备（shell→DT_ALIAS(shell_uart)+UartDma、buzzer→PWM、flash→SPI、drivers 全硬件、modules 全硬件），**它们的 CONFIG 在骨架里一个都没开 → 源码根本不进编译 → CI 绿了不代表它们自己编得过**。六子模块各配一份 native_sim CI 对一半子模块是空转。
>
> **结论（用户拍板 2026-08-14）**：六模块独立 CI 删除，**只做板卡 CI**——project 仓库多板卡真板 CI 才是能真正编译所有模块源码的验证途径（真板编译会把 project 引用的全部 framework 模块源码编进去）。
>
> 本规划 = 板卡 CI 单层方案。原层 1 产物（`zephyr_user/ci/` 骨架、各子模块 `.github/workflows/ci.yml`）**全部删除**。

## 0. 目标

- **唯一编译验证链**：project 仓库 `Dust_Zephyr_Architecture_Project` 自带多板卡真板 CI——**每次提交（push/PR）编译所有板卡**（hpm5361icb + stm32 ×2），project 不单耦合 hpm5361。
- 主仓库 PR 门禁 `build-hpm5361icb` 保留，验证主仓库自身改动 + 子模块指针。
- 删除原层 1 全部产物（详见 §3）。

## 1. 板卡结构（决定矩阵）

`zephyr_user/project/boards/`（瘦身版，新仓库 `Dust_Zephyr_Architecture_Project` 接管）：

| 板卡目录 | 实际板名 | 工具链 | 构建依赖 | 构建命令 |
| --- | --- | --- | --- | --- |
| `boards/hpm/hpm5361icb/` | `hpm5361icb` | riscv64-zephyr-elf | HPM SDK_GLUE + CherryUSB + apply-patches | `west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb` |
| `boards/st/board_rm_c/` | `stm32f407igh6` | arm-zephyr-eabi | `zephyr_user/platform/cmsis`（board.cmake 的 BOARD_GLOBAL_INCLUDES）+ BXCAN/BMI088 | `west build -p always -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c` |
| `boards/st/puzhong/` | `stm32f4_disco` | arm-zephyr-eabi | hal/stm32（west 模块）+ DMA_STM32 | `west build -p always -b stm32f4_disco -- -DBOARD_CFG=puzhong` |

- **CMakeLists 板卡匹配机制**：`file(GLOB ... boards/*/${BOARD_CFG}/${BOARD}.overlay)`——`BOARD_CFG` 是板卡目录名（hpm5361icb/board_rm_c/puzhong），`BOARD` 是 Zephyr 实际板名。CI 必须传 `-DBOARD_CFG`（见 [project/CMakeLists.txt:27-31](project/CMakeLists.txt#L27-L31)）。
- **project 不是自包含构建单元**：project/CMakeLists 引用 `${ZEPHYR_USER_DIR}`（= zephyr_user）下的 framework 六子模块 + `platform/cmsis`——**CI 必须 checkout 主仓库 `Dust_Zephyr_Tree`（submodules recursive 拉 framework）+ 覆盖当前 project commit**。
- **stm32f407igh6 是自定义板**（`board_rm_c`，真实比赛板）：board.cmake 的 BOARD_GLOBAL_INCLUDES 引用 `${ZEPHYR_USER_DIR}/platform/cmsis` 与兄弟目录 `modules/hal/cmsis/CMSIS/Core/Include`；stm32f4_disco 是 Zephyr 官方板（overlay/conf 在 puzhong 目录覆盖）。

## 2. project 仓库多板卡 CI（核心）

### 2.1 板卡矩阵

**目标**：`Dust_Zephyr_Architecture_Project` 仓库自带 CI——**每次提交（push/PR）编译所有板卡**，project 不再单耦合 hpm5361。

**矩阵**（新增板卡 = 加一行）：

| board | cfg（BOARD_CFG） | sdk_target | need_hpm | 依赖 |
| --- | --- | --- | --- | --- |
| hpm5361icb | hpm5361icb | riscv64-zephyr-elf | true | HPM SDK_GLUE |
| stm32f407igh6 | board_rm_c | arm-zephyr-eabi | false | platform/cmsis |
| stm32f4_disco | puzhong | arm-zephyr-eabi | false | hal/stm32 |

### 2.2 workflow 设计

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
      # ⚠️ 前置：主仓库 project 子模块指针必须先更新到新仓库瘦身 commit，否则 recursive 拉不到（见 §2.3）
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

### 2.3 环境组装与关键点

- **checkout 主仓库而非自包含**：project 依赖 zephyr_user 的 framework 六子模块 + `platform/cmsis`（§1），CI 组装 = checkout 主仓库（submodules recursive 拉 framework，project 子模块拉的是主仓库记录指针）+ `Pin current project` 覆盖当前 commit。
- **⚠️ 硬前置：主仓库 project 子模块指针必须已更新到新仓库瘦身 commit**（qingyu 接管规划的"后续动作"，已完成）。新仓库是孤儿分支瘦身版，主仓库 gitlink 若还指旧 commit（新仓库不存在）→ `submodules recursive` 拉 project 失败。
- **HPM 步骤全部 `if: matrix.need_hpm` 门禁**：只有 hpm5361icb job 走 HPM 树 + HPM SDK + patches，st job 跳过——**st 板卡不装 HPM SDK，轻**。
- **工具链按矩阵**：hpm 装 riscv64，st 装 arm（`setup.sh -t ${{ matrix.sdk_target }}`），SDK 缓存 key 区分避免互相覆盖。
- **`-DBOARD_CFG` 必须传**：匹配板卡目录（hpm5361icb/board_rm_c/puzhong），漏传则 glob 找不到 overlay。
- **framework 用主仓库指针版本**：project CI 用主仓库 .gitmodules 记录的 framework 指针；framework 自身改动靠主仓库指针更新后 project CI 自然跟随。

### 2.4 验证

- [ ] project 仓库 push 触发 3 个 job，hpm5361icb/stm32f407igh6/stm32f4_disco 全编译绿
- [ ] st 板卡 job 不装 HPM SDK（日志无 sdk_env/CherryUSB 步骤）
- [ ] 只改 st 板卡相关代码 → CI 能暴露（此前只编 hpm5361 发现不了）
- [ ] project 仓库 PR（非 push）触发时 `Pin current project` 成功（head.sha 生效）

## 3. 删除原层 1 产物（六模块独立 CI 废弃）

**目标**：清理 native_sim 骨架层 1 全部产物——它们是废弃设计的遗留。

| 产物 | 位置 | 动作 |
| --- | --- | --- |
| 骨架工程 | 主仓库 `zephyr_user/ci/`（CMakeLists/prj.conf/zephyr/README） | 删除，走主仓库 PR |
| algorithm ci.yml | `Dust_Zephyr_Architecture_Algorithm/.github/workflows/ci.yml` | 删除，直推 main |
| 规划文档 | `doc/framework子模块独立CI规划.md` | 已重写为本规划（板卡 CI） |

**注意**：主仓库 `ci/` 骨架已合并进 master（PR #8），删除需新 PR；algorithm 仓库 `ci.yml` 已推 main 3 次，删除需新提交。

## 4. 分阶段执行

### 阶段 1：删除 layer 1 产物

- [ ] project 仓库：无（project 从未有子模块 CI）
- [ ] framework/algorithm 仓库：删 `.github/workflows/ci.yml`，直推 main
- [ ] 主仓库：删 `ci/` 目录（走 PR + auto-merge）；同步删规划文档里的 native_sim 描述（本规划已重写）
- [ ] 上传前：HPM 树对齐先查（老规矩）

### 阶段 2：project 仓库多板卡 CI（核心）

**前置（已完成）**：新仓库 `Dust_Zephyr_Architecture_Project` 已接管当前 project（瘦身版，3 板卡），见 [qingyu接管project仓库规划](qingyu接管project仓库规划.md)。主仓库 project 子模块指针已更新到新仓库瘦身 commit。

**任务 2.1**：`Dust_Zephyr_Architecture_Project/.github/workflows/ci.yml` 按 §2.2 完整 workflow 落地（矩阵 3 板卡 + need_hpm 门禁 + head.sha 修正）。

**任务 2.2**：push 触发 3 job，全绿。

**产出**：project 仓库 `.github/workflows/ci.yml`。

**验证**：3 板卡全编译绿；st job 无 HPM SDK 步骤；只改 st 相关代码 CI 能暴露；PR 事件 Pin 成功。

## 5. 上传分支规矩（所有阶段通用）

> 与 git 记忆一致，配套执行。

**各仓库 push 方式**：

| 仓库 | push 方式 | 说明 |
| --- | --- | --- |
| 主仓库 zephyr_user | **必须 PR**（GH013）+ 每个 PR 点 Enable auto-merge | master 直推被 GitHub 服务端拒绝；CI 通过后不会自动合并，必须 PR 上点 auto-merge |
| project / HPM 树 | **直推 main/master，无门禁** | 不用 PR；project push 即触发多板卡 CI |

**旧分支残留坑（最容易踩，务必先防）**：
- 合并删分支的旧分支名**不能继续用**——在残留本地分支上干活再 push，输出 `[new branch]` 重建同名分支 → 与 master 冲突。
- **主动预防**：在旧分支干活/上传前，先 `git fetch origin && git log --oneline origin/master..HEAD` 验证——若本分支提交已带 PR 编号（#N）或 origin/master 已包含本分支内容，立即 `git checkout master && git pull` 从最新 master 开新分支。**不要等 push 报 `[new branch]` 才反应**。

**上传前 HPM 树对齐（硬规定）**：上传任何仓库前先看 `Dust_Zephyr_HPMicro_Tree`（`E:\Zephyr_HPMicro\sdk_glue_user`）有无未推送改动；有改动先推 HPM 树，否则 CI 编译过不了（缺底层 pinctrl/dtsi）。

## 6. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| project CI 拉主仓库旧 project 指针 | submodules recursive 拉主仓库记录的 project 旧 commit | `Pin current project` 显式 fetch + checkout（head.sha 修正 PR 事件） |
| **主仓库 project 指针未更新** | 主仓库 gitlink 指向旧 commit，新仓库（孤儿分支瘦身版）无此 commit → `submodules recursive` 拉 project 失败 | **硬前置：先更新主仓库 project 指针**（已完成） |
| **PR 事件下 `github.sha` 是 merge SHA** | project 仓库 `git fetch origin <merge-sha>` 拉不到 → Pin 步骤失败 | 改用 `${{ github.event.pull_request.head.sha \|\| github.sha }}` |
| HPM SDK 环境重 | hpm5361icb 需 sdk_glue + sdk_env + CherryUSB + patches（几分钟） | `need_hpm` 矩阵门禁：仅 hpm job 装；st job 轻量；SDK 缓存复用 |
| st 板卡依赖 `platform/cmsis` | board_rm_c 的 BOARD_GLOBAL_INCLUDES 引用 zephyr_user/platform/cmsis + modules/hal/cmsis | CI checkout 主仓库全量（framework + platform/cmsis），不做 sparse |
| project CI 用主仓库 framework 指针 | project 编译用主仓库记录的 framework 版本，非 framework 最新 master | 预期行为：framework 改动靠主仓库指针更新后 project CI 自然跟随 |
| 删层 1 产物遗漏 | `ci/` 骨架已合并 master、algorithm ci.yml 已推 main，删除需各自 PR/提交 | 按 §3 清单逐一清，别漏 |

## 7. 执行清单（逐条勾）

- [ ] 上传前分支检查（§5，每次 push 前）：子模块/主仓库确认 push 分支；旧分支先 fetch 对比；上传前查 HPM 树对齐
- [ ] 阶段 1：algorithm 仓库删 `.github/workflows/ci.yml`，直推 main
- [ ] 阶段 1：主仓库删 `ci/` 目录，走 PR + auto-merge
- [ ] 阶段 2：project 仓库加 `ci.yml`（§2.2 多板卡 workflow），用户 push 验证 3 板卡全绿
- [ ] 用户验证：project 3 板卡全绿 + 主仓库 build-hpm5361icb 仍绿
