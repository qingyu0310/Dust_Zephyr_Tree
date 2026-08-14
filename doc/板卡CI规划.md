# 板卡 CI 规划（主仓库全板卡）

> 2026-08-14。**方案拍板（用户）**：project 只是维护者验证层（只留 gpio+test，不写业务逻辑），**不在 project 仓库建 CI**。板卡验证移到**主仓库全板卡**——主仓库 `ci-build.yml` 从单板 hpm5361icb 扩成 **2 板卡矩阵**（hpm5361icb + stm32f4_disco）。

## 0. 目标

- **主仓库 zephyr_user 的 `.github/workflows/ci-build.yml` 扩成 2 板卡矩阵**：每次 push/PR 编译 hpm5361icb + stm32f4_disco 两块板，st 板卡改动 CI 能发现。
- **project 仓库不建 CI**（验证层，无全板卡验证必要）。
- **GH013 status check 改用厂家通用名 `build-hpm`**（job `name: build-${{ matrix.vendor }}` 设计），Ruleset 同步更新。

## 1. 板卡矩阵（2 板卡）

| 板卡目录 | Zephyr 实际板名 | 工具链 | need_hpm | 构建依赖 | 构建命令 |
| --- | --- | --- | --- | --- | --- |
| `project/boards/hpm/hpm5361icb/` | `hpm5361icb` | riscv64-zephyr-elf | true | HPM SDK_GLUE + sdk_env(hpm_sdk) + CherryUSB + apply-patches | `west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb` |
| `project/boards/st/puzhong/` | `stm32f4_disco` | arm-zephyr-eabi | false | `zephyr_user/platform/cmsis` + `modules/hal/cmsis` + hal/stm32（west 模块）+ DMA_STM32 | `west build -p always -b stm32f4_disco -- -DBOARD_CFG=puzhong` |

- **CMakeLists 板卡匹配**（[project/CMakeLists.txt:27-31](project/CMakeLists.txt#L27-L31)）：`file(GLOB ... boards/*/${BOARD_CFG}/${BOARD}.overlay)`——`BOARD_CFG` 是板卡目录名（hpm5361icb/puzhong），`BOARD` 是 Zephyr 实际板名。**`-DBOARD_CFG` 必传**，漏传则 glob 找不到 overlay。
- **hpm5361icb** 是自定义板（board 定义在 HPM 树 `sdk_glue_user/boards`）→ 需 HPM 树 + HPM SDK + patches。
- **stm32f4_disco** 是 Zephyr 官方板（overlay/conf 在 puzhong 目录覆盖），只依赖主仓库 `platform/cmsis` + west 模块 hal/stm32 → 不需 HPM 树。

## 2. ci-build.yml 改造（old → new）

**文件**：`zephyr_user/.github/workflows/ci-build.yml`

### 2.1 old（当前，单板）

单 job `build-hpm5361icb`（runs-on ubuntu-latest），steps：checkout → Install system deps → Cache/Install SDK（`-t riscv64-zephyr-elf`）→ Setup west → Clone HPM repos → Apply patches → Build hpm5361icb → Upload。无矩阵，st 板卡不编译。

### 2.2 new（矩阵 2 板卡，完整文件）

```yaml
name: CI Build

on:
  push:
    branches: [ master ]
  pull_request:
  workflow_dispatch:

concurrency:
  group: ${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  build:
    # name 用厂家通用名 → check 名 build-hpm / build-st，不绑定具体板卡
    name: build-${{ matrix.vendor }}
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false          # 一块板失败不取消其他
      matrix:
        include:
          - board: hpm5361icb
            cfg: hpm5361icb
            vendor: hpm
            sdk_target: riscv64-zephyr-elf
            need_hpm: true
          - board: stm32f4_disco
            cfg: puzhong
            vendor: st
            sdk_target: arm-zephyr-eabi
            need_hpm: false
    env:
      ZEPHYR_BASE: ${{ github.workspace }}/zephyr
      ZEPHYR_SDK_INSTALL_DIR: /opt/zephyr-sdk-0.16.8
      ZEPHYR_TOOLCHAIN_VARIANT: zephyr
      SDK_GLUE_DIR: ${{ github.workspace }}/hpm/zephyr_sdk_glue
      SDK_GLUE_USER_DIR: ${{ github.workspace }}/hpm/sdk_glue_user
    steps:
      # zephyr 侧主仓库 + 公开子模块（framework 六模块 + project，免 token）
      - uses: actions/checkout@v4
        with:
          path: zephyr_user
          submodules: recursive

      # HPM 树（提供 sdk_glue_user 覆盖层 + zephyr-patches，仅 hpm 板卡）
      - name: Checkout HPM tree
        if: matrix.need_hpm
        uses: actions/checkout@v4
        with:
          repository: qingyu0310/Dust_Zephyr_HPMicro_Tree
          path: hpm/sdk_glue_user

      # 系统依赖
      - name: Install system deps
        run: |
          sudo apt-get update
          sudo apt-get install --no-install-recommends -y \
            git cmake ninja-build gperf ccache dfu-util device-tree-compiler \
            python3-dev python3-pip python3-setuptools python3-tk xz-utils \
            file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1

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

      # west + zephyr 树（west update 拉 hal/stm32，st 板卡需要）
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

      # apply patches（官方仓库 → 原版 + 修复）
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

      # 产物（upload-artifact 不支持大括号 glob，明确列文件名）
      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware-${{ matrix.board }}
          path: |
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.elf
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.map
          if-no-files-found: error
```

## 3. 关键实现点

- **check 名 = 厂家通用名**：job `name: build-${{ matrix.vendor }}` → 矩阵 check 名 = `build-hpm` / `build-st`（vendor 取 hpm/st，对应 `project/boards/` 下第一层目录名）。**不绑定具体板卡**——以后加 HPM/ST 板卡 check 名不变。
- **`if: matrix.need_hpm` 门禁**：只有 hpm5361icb job 走 HPM 树 + HPM SDK + patches；st job 跳过——st 板卡不装 HPM SDK，轻量。
- **工具链按矩阵**：hpm 装 riscv64，st 装 arm（`setup.sh -t ${{ matrix.sdk_target }}`），SDK 缓存 key `zephyr-sdk-0.16.8-${{ matrix.sdk_target }}` 区分避免互相覆盖。
- **`-DBOARD_CFG` 必传**：匹配板卡目录（hpm5361icb/puzhong），漏传则 glob 找不到 overlay（[project/CMakeLists.txt:27-31](project/CMakeLists.txt#L27-L31)）。
- **west update 拉 hal/stm32**：stm32f4_disco 依赖 west 模块 hal/stm32，公共步骤已含。

## 4. 分阶段执行方案

> 按阶段推进，每阶段验证通过再进下一阶段。

### 阶段 1：ci-build.yml 矩阵化

**① 目标**：主仓库 CI 从单板 hpm5361icb 扩成 2 板卡矩阵，check 名改厂家通用名 build-hpm / build-st。

**② 具体干什么**（改 `zephyr_user/.github/workflows/ci-build.yml`，old → new 对照 §2）：
1. job `build-hpm5361icb`（单 job）→ `build`（矩阵），加 `strategy.matrix.include` 2 项（hpm5361icb + stm32f4_disco），每项带 `board/cfg/vendor/sdk_target/need_hpm` 五字段（§2.2）。
2. job 名改 `name: build-${{ matrix.vendor }}`。
3. HPM 专属步骤（Checkout HPM tree / Clone HPM official repos / Apply HPM patches）全部加 `if: matrix.need_hpm`。
4. SDK 安装 `setup.sh -t riscv64-zephyr-elf -c` → `-t ${{ matrix.sdk_target }} -c`；缓存 key 加 `-${{ matrix.sdk_target }}` 后缀。
5. Build 步骤 `-b hpm5361icb` → `-b ${{ matrix.board }}`，`-DBOARD_CFG=${{ matrix.cfg }}`。
6. Upload artifacts name → `firmware-${{ matrix.board }}`。

**③ 产出**：`ci-build.yml` 矩阵版（§2.2 完整文件）。

**④ 验证**：yaml 语法正确（可 `actionlint` 本地校验）；主仓库 PR 触发 2 个 job（build-hpm + build-st）。

### 阶段 2：GitHub Ruleset 更新

**① 目标**：GH013 required status check 从 `build-hpm5361icb` 改为 `build-hpm`，PR 合并门禁不失效。

**② 具体干什么**（GitHub 设置页，用户操作）：
1. Settings → Rulesets → 主仓库 master 规则（GH013）。
2. required status checks：`build-hpm5361icb` → `build-hpm`。
3. 保存。

**③ 产出**：Ruleset required check = `build-hpm`。

**④ 验证**：主仓库 PR 的 `build-hpm` check 红时不可合并（门禁仍生效）。

### 阶段 3：推送验证全绿

**① 目标**：2 板卡全编译绿，st 板卡改动可被 CI 暴露。

**② 具体干什么**：
1. **上传前查 HPM 树对齐**（硬规定）：`git -C E:/Zephyr_HPMicro/sdk_glue_user status`，有未推送改动先推，否则 hpm job 编译过不了。
2. 主仓库走 PR（GH013）：分支 push → 开 PR → Enable auto-merge → CI 过 → 合并。
3. 观察 Actions：`build-hpm` + `build-st` 全绿。

**③ 产出**：主仓库 CI 全绿，板卡 CI 闭环。

**④ 验证**：
- [ ] build-hpm + build-st 全编译绿
- [ ] st job 日志无 sdk_env/CherryUSB/apply-patches 步骤（need_hpm 门禁生效）
- [ ] 只改 st 板卡相关代码 → CI 能暴露（此前只编 hpm5361 发现不了）

## 5. 执行清单（逐条勾）

- [ ] 阶段 1：改 `ci-build.yml` 矩阵化（§2.2 yaml），主仓库 PR 合并
- [ ] 阶段 2：Ruleset required status check → `build-hpm`（GitHub 设置页）
- [ ] 阶段 3：HPM 树对齐检查 → 主仓库 PR → 2 job 全绿

## 6. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| GH013 status check 名变化 | job 名改 `build-${{ matrix.vendor }}` → check 名变 `build-hpm`/`build-st`，原 `build-hpm5361icb` 消失 | **阶段 2 同步更新 GitHub Ruleset** required check 为 `build-hpm` |
| 阶段顺序颠倒 | 先改 ci-build.yml 未改 Ruleset → 旧 required check `build-hpm5361icb` 不再出现，PR 无法合并 | 阶段 2 必须在阶段 1 的 PR 合并前完成（或同 PR 处理） |
| HPM SDK 环境重 | hpm5361icb 需 sdk_glue + sdk_env + CherryUSB + patches（几分钟） | `need_hpm` 矩阵门禁：仅 hpm job 装；st job 轻量；SDK 缓存复用 |
| st 板卡依赖 `platform/cmsis` | puzhong 的 BOARD_GLOBAL_INCLUDES 引用 `zephyr_user/platform/cmsis` + `modules/hal/cmsis` | 主仓库 self-checkout 自带 platform/cmsis，无需额外处理 |
| 主仓库 CI 用主仓库记录的 project 指针 | project 改动需主仓库更新指针后才被编译 | 预期行为：project 是验证层，改动走主仓库 PR 统一验证 |
