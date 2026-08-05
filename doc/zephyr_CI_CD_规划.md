# zephyr_user CI/CD 实施手册（M0 + M1 完整可执行）

> **定位**：不是规划，是**实施手册**。每个里程碑给出可直接复制执行的命令、完整文件内容、通过标准。
> 所有决策已前置给出结论（§0），动手时不需要思考——照着抄。
>
> **前提（2026-08-03 确认）**：仓库公开、GitHub Actions 免费无限分钟、唯一开发板 hpm5361icb、
> HPM 底层定制走 patch 方案（4 个官方仓库保持纯净，patch 归档在 `Dust_Zephyr_HPMicro_Tree/zephyr-patches/`）。
>
> **里程碑**：M0 HPM 树 patch 化（CI 的前提）→ M1 单板编译门禁 → M2~M7 后续。

---

## 目录

- [0. 决策清单（全部前置结论）](#0-决策清单全部前置结论)
- [1. 事实清单（执行依据）](#1-事实清单执行依据)
- [2. M0 HPM 树 patch 化（完整操作）](#2-m0-hpm-树-patch-化完整操作)
- [3. M1 单板编译门禁 CI（完整操作）](#3-m1-单板编译门禁-ci完整操作)
- [4. M2~M7 后续里程碑规格](#4-m2m7-后续里程碑规格)
- [5. 首次跑挂排查表（按症状给动作）](#5-首次跑挂排查表按症状给动作)
- [6. 待确认项（需要你决策，不阻塞 M0/M1）](#6-待确认项需要你决策不阻塞-m0m1)
- [附录：命令速查](#附录命令速查)

---

## 0. 决策清单（全部前置结论）

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | CI 平台 | **GitHub Actions**（仓库在 qingyu0310，公开免费） |
| 2 | Runner | **GitHub 托管 Ubuntu**（`ubuntu-latest`） |
| 3 | 主门禁 | **单板 hpm5361icb 编译通过**（唯一已开发板） |
| 4 | 构建入口 | CI 直接 `west build` + 环境变量，**不用** dust 脚本 |
| 5 | HPM 底层定制 | **patch 方案**：clone 官方 4 仓库 → apply `zephyr-patches/`。不 fork、不建新仓库 |
| 6 | sdk_env 拉取 | `hpmicro/sdk_env` 公开，**sparse checkout 只拉 `hpm_sdk`**（整库 1.8GB） |
| 7 | framework/init 子模块 | **改 HTTPS URL**（CI 无 SSH key，不改必挂） |
| 8 | 测试顺序 | T1 algorithm 单测 → T2 native_sim 冒烟 → T3 协议 mock；T1 不依赖 HPM，可先行 |
| 9 | 板卡矩阵 | 当前只有 hpm5361icb；新板开发后往 matrix 加 include 行 |
| 10 | 成本 | 0 元（公开仓库无限分钟）；唯一注意：别泄露比赛私货/密钥 |
| 11 | 泄密扫描 | **gitleaks 直接当门禁**（公开仓库密钥进历史只能轮换） |

---

## 1. 事实清单（执行依据）

**zephyr 侧：**
- 主仓库 `qingyu0310/Dust_Zephyr_Tree`（公开），7 子模块：framework 六层 + project。
- `framework/init` 子模块 URL 是 **SSH**（`git@github.com:…`）→ M1 前必须改 HTTPS（§3.2）。

**HPM 侧：**
- 你的树 `qingyu0310/Dust_Zephyr_HPMicro_Tree`，本地路径 `E:\Zephyr_HPMicro\sdk_glue_user`，remote 已指向它。
- 树 = `sdk_glue_user`（新增文件层：boards/soc/dts）+ 将要建的 `zephyr-patches/`。
- 官方仓库被定制（4 个，全部 patch 化）：
  - `zephyr`（官方树，本地 `E:\Zephyr\zephyr`）：改 `drivers/interrupt_controller/intc_plic.c`（1 文件）
  - `hpmicro/sdk_env`（本地 `E:\Zephyr_HPMicro\sdk_env`，1.8GB）：改 `hpm_sdk/soc/HPM5300/HPM5361/hpm_misc.h`（1 文件，+9 行）
  - `hpmicro/zephyr_sdk_glue`（本地 `E:\Zephyr_HPMicro\sdk_glue`）：改 **34 个文件**（+1430/-531 行）
  - `cherry-embedded/CherryUSB`（本地 `E:\Zephyr_HPMicro\modules\lib\CherryUSB`）：改 `osal/usb_osal_zephyr.c`（1 文件）

**sdk_glue 34 文件 → 7 个 patch 的完整分配（生成命令用）：**

| patch | 文件 |
|-------|------|
| `0001-drivers-serial-hw-rx-idle.diff` | `drivers/serial/uart_hpmicro.c`、`drivers/serial/Kconfig.hpmicro` |
| `0002-drivers-can-mcan-fixes.diff` | `drivers/can/CMakeLists.txt`、`drivers/can/Kconfig.hpmicro`、`drivers/can/mcan_hpmicro.c` |
| `0003-drivers-gpio-interrupt.diff` | `drivers/gpio/gpio_hpmicro.c` |
| `0004-drivers-usb-udc-cherryusb.diff` | `drivers/usb/cherryusb/CMakeLists.txt`、`drivers/usb/cherryusb/cherryusb_hpmicro.c`、`drivers/usb/udc/Kconfig.hpmicro`、`drivers/usb/udc/udc_hpmicro.c` |
| `0005-drivers-others.diff` | `drivers/clock_control/CMakeLists.txt`、`drivers/clock_control/clock_control_hpmicro_pllv2.c`、`drivers/flash/flash_hpmicro.c`、`drivers/pwm/Kconfig.hpmicro`、`drivers/pwm/pwm_hpmicro.c`、`drivers/spi/spi_hpmicro.c` |
| `0006-dts-hpm53xx-osc-clocks.diff` | `dts/riscv/hpmicro/hpm5361.dtsi`、`dts/riscv/hpmicro/hpm53xx.dtsi`、`dts/riscv/hpmicro/hpm6exx.dtsi`、`dts/riscv/hpmicro/hpm6exx_dualcore.dtsi` |
| `0007-soc-boards.diff` | `soc/hpmicro/` 下 9 文件（CMakeLists、Kconfig、Kconfig.defconfig、Kconfig.soc、HPM6E00/soc.c、各 Kconfig.defconfig.series）+ `boards/hpmicro/` 下 5 文件（hpm6200evk/hpm6750evk2/hpm6800evk/hpm6e00evk 的 board.yml/board.cmake） |

**环境变量（CI 注入）：** `ZEPHYR_BASE`、`ZEPHYR_SDK_INSTALL_DIR`、`ZEPHYR_TOOLCHAIN_VARIANT=zephyr`、`SDK_GLUE_DIR`、`SDK_GLUE_USER_DIR`。

---

## 2. M0 HPM 树 patch 化（完整操作）

> **目标**：把 4 个官方仓库的 37 处修改归档成 patch 放进你的树，官方仓库保持纯净可复现。
> **产出**：`sdk_glue_user/zephyr-patches/` 下 10 个 patch + `apply-patches.sh` + `apply-patches.ps1`。
> **不动**：4 个官方仓库本地现有的修改（它们继续保留，patch 只是备份 + 可复现源）。

### 2.1 建目录结构

在 git bash 执行（一次性）：

```bash
mkdir -p /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/{zephyr,sdk_env,sdk_glue,cherryusb}
```

### 2.2 生成 patch（逐条复制执行）

```bash
# ===== zephyr（1 个）=====
cd /e/Zephyr/zephyr
git diff -- drivers/interrupt_controller/intc_plic.c \
  > /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/zephyr/0001-intc_plic-spurious-ack.diff

# ===== sdk_env（1 个）=====
cd /e/Zephyr_HPMicro/sdk_env
git diff -- hpm_sdk/soc/HPM5300/HPM5361/hpm_misc.h \
  > /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/sdk_env/0001-hpm_misc-dlm-ilm.diff

# ===== sdk_glue（7 个）=====
cd /e/Zephyr_HPMicro/sdk_glue
P=/e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/sdk_glue
git diff -- drivers/serial/uart_hpmicro.c drivers/serial/Kconfig.hpmicro \
  > $P/0001-drivers-serial-hw-rx-idle.diff
git diff -- drivers/can/CMakeLists.txt drivers/can/Kconfig.hpmicro drivers/can/mcan_hpmicro.c \
  > $P/0002-drivers-can-mcan-fixes.diff
git diff -- drivers/gpio/gpio_hpmicro.c \
  > $P/0003-drivers-gpio-interrupt.diff
git diff -- drivers/usb/cherryusb/CMakeLists.txt drivers/usb/cherryusb/cherryusb_hpmicro.c \
           drivers/usb/udc/Kconfig.hpmicro drivers/usb/udc/udc_hpmicro.c \
  > $P/0004-drivers-usb-udc-cherryusb.diff
git diff -- drivers/clock_control/CMakeLists.txt drivers/clock_control/clock_control_hpmicro_pllv2.c \
           drivers/flash/flash_hpmicro.c drivers/pwm/Kconfig.hpmicro drivers/pwm/pwm_hpmicro.c \
           drivers/spi/spi_hpmicro.c \
  > $P/0005-drivers-others.diff
git diff -- dts/riscv/hpmicro/hpm5361.dtsi dts/riscv/hpmicro/hpm53xx.dtsi \
           dts/riscv/hpmicro/hpm6exx.dtsi dts/riscv/hpmicro/hpm6exx_dualcore.dtsi \
  > $P/0006-dts-hpm53xx-osc-clocks.diff
git diff -- soc/hpmicro boards/hpmicro \
  > $P/0007-soc-boards.diff

# ===== CherryUSB（1 个）=====
cd /e/Zephyr_HPMicro/modules/lib/CherryUSB
git diff -- osal/usb_osal_zephyr.c \
  > /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/cherryusb/0001-usb_osal-array_size.diff
```

**检查**：`ls -la /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/*/` 应看到 1+1+7+1 = **10 个 .diff**，且每个非空。

### 2.3 apply-patches.sh（Linux/CI 用）— 完整内容

路径：`E:\Zephyr_HPMicro\sdk_glue_user\apply-patches.sh`

```bash
#!/bin/bash
# apply-patches.sh — 应用所有官方仓库补丁（HPM 底层定制）
# 用法：
#   ./apply-patches.sh --zephyr <zephyr根> --sdk-env <sdk_env根> \
#                      --sdk-glue <sdk_glue根> --cherryusb <cherryusb根>
# 每个 patch 先 --check 后 apply，失败即 exit(1)，防止官方仓库"改一半"。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHES="$ROOT/zephyr-patches"

ZEPHYR=""; SDK_ENV=""; SDK_GLUE=""; CHERRYUSB=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --zephyr)    ZEPHYR="$2";    shift 2 ;;
    --sdk-env)   SDK_ENV="$2";   shift 2 ;;
    --sdk-glue)  SDK_GLUE="$2";  shift 2 ;;
    --cherryusb) CHERRYUSB="$2"; shift 2 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
done

for v in ZEPHYR SDK_ENV SDK_GLUE CHERRYUSB; do
  if [[ -z "${!v}" ]]; then
    echo "缺少 --${v,,} 参数"; exit 1
  fi
done

apply_one() {
  local repo="$1" patch="$2"
  echo "==> $patch"
  git -C "$repo" apply --check "$patch" || { echo "CHECK FAIL: $patch"; exit 1; }
  git -C "$repo" apply "$patch"       || { echo "APPLY FAIL: $patch"; exit 1; }
}

apply_one "$ZEPHYR"    "$PATCHES/zephyr/0001-intc_plic-spurious-ack.diff"
apply_one "$SDK_ENV"   "$PATCHES/sdk_env/0001-hpm_misc-dlm-ilm.diff"

apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0001-drivers-serial-hw-rx-idle.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0002-drivers-can-mcan-fixes.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0003-drivers-gpio-interrupt.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0004-drivers-usb-udc-cherryusb.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0005-drivers-others.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0006-dts-hpm53xx-osc-clocks.diff"
apply_one "$SDK_GLUE"  "$PATCHES/sdk_glue/0007-soc-boards.diff"

apply_one "$CHERRYUSB" "$PATCHES/cherryusb/0001-usb_osal-array_size.diff"

echo "全部 10 个 patch 应用成功。"
```

### 2.4 apply-patches.ps1（Windows 本地用）— 完整内容

路径：`E:\Zephyr_HPMicro\sdk_glue_user\apply-patches.ps1`

```powershell
# apply-patches.ps1 — 应用所有官方仓库补丁（Windows 本地）
# 用法：
#   .\apply-patches.ps1 -Zephyr E:\Zephyr\zephyr -SdkEnv E:\Zephyr_HPMicro\sdk_env `
#     -SdkGlue E:\Zephyr_HPMicro\sdk_glue -CherryUsb E:\Zephyr_HPMicro\modules\lib\CherryUSB
param(
  [string]$Zephyr,
  [string]$SdkEnv,
  [string]$SdkGlue,
  [string]$CherryUsb
)
$ErrorActionPreference = "Stop"
$Root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$Patches = Join-Path $Root "zephyr-patches"

foreach ($v in @("Zephyr","SdkEnv","SdkGlue","CherryUsb")) {
  if (-not (Get-Variable $v -ValueOnly)) { throw "缺少 -$v 参数" }
}

function Apply-One([string]$Repo, [string]$Patch) {
  Write-Host "==> $Patch"
  git -C $Repo apply --check $Patch
  if ($LASTEXITCODE -ne 0) { throw "CHECK FAIL: $Patch" }
  git -C $Repo apply $Patch
  if ($LASTEXITCODE -ne 0) { throw "APPLY FAIL: $Patch" }
}

Apply-One $Zephyr    (Join-Path $Patches "zephyr/0001-intc_plic-spurious-ack.diff")
Apply-One $SdkEnv    (Join-Path $Patches "sdk_env/0001-hpm_misc-dlm-ilm.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0001-drivers-serial-hw-rx-idle.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0002-drivers-can-mcan-fixes.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0003-drivers-gpio-interrupt.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0004-drivers-usb-udc-cherryusb.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0005-drivers-others.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0006-dts-hpm53xx-osc-clocks.diff")
Apply-One $SdkGlue   (Join-Path $Patches "sdk_glue/0007-soc-boards.diff")
Apply-One $CherryUsb (Join-Path $Patches "cherryusb/0001-usb_osal-array_size.diff")

Write-Host "全部 10 个 patch 应用成功。"
```

### 2.5 干净环境验证（证明 patch 可复现，必须通过才继续）

```bash
# 临时目录，clone 4 个官方原版（sdk_env 只 sparse 拉 hpm_sdk 以省时间）
TMP=/tmp/patch-test
rm -rf $TMP && mkdir -p $TMP
git clone --depth 1 https://github.com/zephyrproject-rtos/zephyr.git $TMP/zephyr
git clone --depth 1 --filter=blob:none --sparse --branch v1.11.0 https://github.com/hpmicro/sdk_env.git $TMP/sdk_env
git -C $TMP/sdk_env sparse-checkout set hpm_sdk
git clone --depth 1 https://github.com/hpmicro/zephyr_sdk_glue.git $TMP/sdk_glue
git clone --depth 1 https://github.com/cherry-embedded/CherryUSB.git $TMP/cherryusb

# apply
cd /e/Zephyr_HPMicro/sdk_glue_user
./apply-patches.sh --zephyr $TMP/zephyr --sdk-env $TMP/sdk_env \
  --sdk-glue $TMP/sdk_glue --cherryusb $TMP/cherryusb

# 验证：apply 后每个仓库的 diff 应与本地一致
git -C $TMP/zephyr    diff --stat   # 期望: 1 file changed（intc_plic.c）
git -C $TMP/sdk_env   diff --stat   # 期望: 1 file changed（hpm_misc.h）
git -C $TMP/sdk_glue  diff --stat   # 期望: 34 files changed（与本地一致）
git -C $TMP/cherryusb diff --stat   # 期望: 1 file changed（usb_osal_zephyr.c）

# 清理
rm -rf $TMP
```

**通过标准**：四个 `diff --stat` 与本地官方仓库的 `git status --short` 文件数完全一致（zephyr=1, sdk_env=1, sdk_glue=34, cherryusb=1）。
**若 sdk_glue 不是 34**：说明某个 patch 漏文件或重复，回到 §2.2 重新生成对应 patch（用 `git diff --stat` 对账）。

### 2.6 更新 README + 提交 + 推送

```bash
cd /e/Zephyr_HPMicro/sdk_glue_user
chmod +x apply-patches.sh

# README 顶部补一段（内容见下）
```

README 开头追加（用文本编辑器，或在本节末尾手动粘贴）：

```markdown
## 底层修改 patch 机制

修改官方仓库的部分（不污染官方 sdk）归档在 `zephyr-patches/`，构建时 apply：

- `zephyr/`   → zephyr 官方树（intc_plic.c PLIC 伪中断）
- `sdk_env/`  → HPM sdk_env（hpm_misc.h DLM/ILM 地址换算）
- `sdk_glue/` → HPM sdk_glue（UART RX idle / CAN / GPIO / USB / dts / soc-boards 共 34 文件）
- `cherryusb/`→ CherryUSB（usb_osal_zephyr.c ARRAY_SIZE 宏冲突）

用法（Linux / CI）：`./apply-patches.sh --zephyr <zephyr> --sdk-env <sdk_env> --sdk-glue <sdk_glue> --cherryusb <cherryusb>`
用法（Windows）：`.\apply-patches.ps1 -Zephyr ... -SdkEnv ... -SdkGlue ... -CherryUsb ...`

重新生成 patch：改完官方仓库后 `git diff > zephyr-patches/<repo>/NNN-<desc>.diff` 更新对应文件。
```

提交：

```bash
cd /e/Zephyr_HPMicro/sdk_glue_user
git add zephyr-patches apply-patches.sh apply-patches.ps1 README.md
git commit -m "feat: HPM 底层修改 patch 化——4 官方仓库 10 个 patch + apply 脚本"
git push origin master          # 注意：本地分支是 master（不是 main）
```

**M0 完成标准**：树已推送，10 个 patch 在 GitHub 上可见；干净环境验证通过。
> 状态：**2026-08-03 已完成**（commit fa036c9，干净环境验证 37 文件完整复现）。

---

## 3. M1 单板编译门禁 CI（完整操作）

> **前置**：M0 完成（patch 已推送）。framework/init 子模块 URL 先改 HTTPS（§3.2）。
> **产出**：`zephyr_user/.github/workflows/ci-build.yml` + branch protection。

### 3.1 完整 workflow 文件

路径：`e:\Zephyr\zephyr_user\.github\workflows\ci-build.yml`

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
  build-hpm5361icb:
    name: build-hpm5361icb
    runs-on: ubuntu-latest
    env:
      ZEPHYR_BASE: ${{ github.workspace }}/zephyr
      ZEPHYR_SDK_INSTALL_DIR: /opt/zephyr-sdk-0.16.8
      ZEPHYR_TOOLCHAIN_VARIANT: zephyr
      SDK_GLUE_DIR: ${{ github.workspace }}/hpm/zephyr_sdk_glue
      SDK_GLUE_USER_DIR: ${{ github.workspace }}/hpm/sdk_glue_user
    steps:
      # zephyr 侧主仓库 + 公开子模块（免 token）
      - uses: actions/checkout@v4
        with:
          path: zephyr_user
          submodules: recursive

      # HPM 树（提供 sdk_glue_user 覆盖层 + zephyr-patches）
      - uses: actions/checkout@v4
        with:
          repository: qingyu0310/Dust_Zephyr_HPMicro_Tree
          path: hpm/sdk_glue_user

      # 系统依赖（Zephyr 官方快速开始）
      - name: Install system deps
        run: |
          sudo apt-get update
          sudo apt-get install --no-install-recommends -y \
            git cmake ninja-build gperf ccache dfu-util device-tree-compiler \
            python3-dev python3-pip python3-setuptools python3-tk xz-utils \
            file make gcc gcc-multilib g++-multilib libsdl2-dev libmagic1

      # SDK 缓存
      - name: Cache Zephyr SDK
        id: cache-sdk
        uses: actions/cache@v4
        with:
          path: /opt/zephyr-sdk-0.16.8
          key: zephyr-sdk-0.16.8-riscv

      - name: Install Zephyr SDK
        if: steps.cache-sdk.outputs.cache-hit != 'true'
        run: |
          curl -sL https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.16.8/zephyr-sdk-0.16.8_linux-x86_64_minimal.tar.xz | \
            sudo tar -xJ -C /opt
          sudo /opt/zephyr-sdk-0.16.8/setup.sh -t riscv64-zephyr-elf -c

      # west + zephyr 树 + west 模块（全部在工作区，runner 可写）
      - name: Setup west workspace
        run: |
          python3 -m pip install west
          git clone --depth 1 --branch v4.3.0 https://github.com/zephyrproject-rtos/zephyr.git ${{ github.workspace }}/zephyr
          mkdir -p ${{ github.workspace }}/.west
          printf '[manifest]\npath = zephyr\nfile = west.yml\n' > ${{ github.workspace }}/.west/config
          cd ${{ github.workspace }}/zephyr && python3 -m pip install -r scripts/requirements.txt
          west update

      # HPM 官方仓库（sdk_env 固定 v1.11.0 = 本地版本，官方 main 已删 hpm_misc 宏；只 sparse 拉 hpm_sdk）
      - name: Clone HPM official repos
        run: |
          git clone https://github.com/hpmicro/zephyr_sdk_glue.git ${{ github.workspace }}/hpm/zephyr_sdk_glue
          git clone --depth 1 --filter=blob:none --sparse --branch v1.11.0 https://github.com/hpmicro/sdk_env.git ${{ github.workspace }}/hpm/sdk_env
          git -C ${{ github.workspace }}/hpm/sdk_env sparse-checkout set hpm_sdk
          git clone https://github.com/cherry-embedded/CherryUSB.git ${{ github.workspace }}/hpm/modules/lib/CherryUSB

      # apply 你的 patch（官方仓库 → 原版 + 你的修复）
      - name: Apply HPM patches
        run: |
          bash ${{ github.workspace }}/hpm/sdk_glue_user/apply-patches.sh \
            --zephyr ${{ github.workspace }}/zephyr \
            --sdk-env ${{ github.workspace }}/hpm/sdk_env \
            --sdk-glue ${{ github.workspace }}/hpm/zephyr_sdk_glue \
            --cherryusb ${{ github.workspace }}/hpm/modules/lib/CherryUSB

      # 构建 hpm5361icb
      - name: Build hpm5361icb
        run: |
          cd ${{ github.workspace }}/zephyr_user/project
          west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb

      # 产物（upload-artifact 不支持 {elf,bin,hex} 大括号，明确列出）
      - name: Upload artifacts
        uses: actions/upload-artifact@v4
        with:
          name: firmware-hpm5361icb
          path: |
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.elf
            ${{ github.workspace }}/zephyr_user/project/build/zephyr/zephyr.map
          if-no-files-found: error
```

### 3.2 前置：framework/init 子模块改 HTTPS

```bash
cd /e/Zephyr/zephyr_user
git config -f .gitmodules submodule.framework/init.url \
  https://github.com/qingyu0310/Dust_Zephyr_Architecture_Init.git
git submodule sync framework/init
git add .gitmodules
git commit -m "chore: framework/init 子模块 URL 改 HTTPS（CI 无 SSH key）"
git push origin master
```

### 3.3 提交 workflow + 首次运行

```bash
cd /e/Zephyr/zephyr_user
git add .github/workflows/ci-build.yml
git commit -m "ci: hpm5361icb 单板编译门禁"
git push origin master
```

推送后：Actions 页出现 `CI Build` run。**首次预期**：可能红（Linux 纯净环境差异），按 §5 排查。

### 3.4 加 branch protection（编译变门禁）

1. GitHub → **Settings → Rules → Rulesets → New ruleset**，名称任意（如 `master CI 门禁`）。
2. **Enforcement status**：`Active`（不选 Active 规则不生效）。
3. **Target branches**：Add target → **Include default branch**（master）。
4. **Branch rules** 勾选三项：
   - ✅ **合并前需要提交拉取请求**（所需审批留 0）
   - ✅ **需要通过状态检查** → 展开勾 **合并前要求分支保持最新状态** → 点**添加检查** 选 `build-hpm5361icb`（搜 `build` 前缀）
   - ✅ **阻挡力推动**
5. Create。

> **状态：2026-08-03 已完成**。Ruleset 已生效，status check `build-hpm5361icb` 有绿结果。注意：status check 搜索框有缓存延迟，刚跑完可能搜不到，等 1~2 分钟或用 `build` 前缀搜索。

### 3.5 后续板卡扩展（开发了新板再动）

在 ci-build.yml 的 `jobs.build-hpm5361icb` 之后，按同样结构加 job（复制 job 块，改 name/board/BOARD_CFG/环境变量）。例：

```yaml
  build-board_rm_c:
    name: build-board_rm_c
    runs-on: ubuntu-latest
    env:
      ZEPHYR_BASE: ${{ github.workspace }}/zephyr
      ZEPHYR_SDK_INSTALL_DIR: /opt/zephyr-sdk-0.16.8
      ZEPHYR_TOOLCHAIN_VARIANT: zephyr
      # ST 板不需要 SDK_GLUE_DIR / SDK_GLUE_USER_DIR
    steps:
      # …… 同样 3.1 的 steps（去掉 HPM clone/apply 两步）……
      - name: Build board_rm_c
        run: |
          cd ${{ github.workspace }}/zephyr_user/project
          west build -p always -b stm32f407igh6 -- -DBOARD_CFG=board_rm_c
```

> ST 板不需要 HPM 依赖，把 "Clone HPM official repos" 和 "Apply HPM patches" 两个 step 删掉即可。

### 3.6 M1 完成标准

Actions run 绿色，artifact `firmware-hpm5361icb.zip` 可下载，PR 合并被 `build-hpm5361icb` 卡住。

---

## 4. M2~M7 后续里程碑规格

> 到该里程碑时再按本节细化（当前不需要动手）。顺序：M2 → M3 → M4 → M5 → M6 → M7。

### M2 algorithm 单测（T1）

- 在 `framework/algorithm/` 建 `test/`：`testcase.yaml` + `CMakeLists.txt` + `src/{main.c,test_pid.c,test_filter.c,test_kalman.c}`。
- 测试点：PID 稳态误差收敛、LPF `α·y(n-1)+(1-α)·x(n)` 展开、卡尔曼模板实例化。
- CI：`west test -p always -b native_sim framework/algorithm/test`（新 job `test-algorithm`）。
- 与 HPM 无关，可独立跑；通过后加入门禁。

### M3 native_sim 冒烟（T2）

- `west build -b native_sim` 编 host 固件 → 跑 → 断言启动日志顺序（`Pre→Early→Mid→Late→App × Init→Thread`）+ 线程 `xxx_start` + 退出码 0。
- 前置：给 framework/drivers 补 native_sim mock（这是主要工作量）。

### M4 协议 mock（T3）

- fake UART 喂合成 DR16 帧，断言 SBUS 解码（sw1→ch5/sw2→ch6/sw3→ch7、头帧 0x0F）。
- 喂 `START`→断言回 `GO`；喂 `SWITCH`→断言切波特率（含分片匹配）。
- 依赖 M3 的 mock 基建。

### M5 静态 + 泄密扫描

- clang-format / clang-tidy / cppcheck 作"报告"（不卡合并）。
- **gitleaks 直接当门禁**（公开仓库密钥进历史只能轮换）。

### M6 子模块 CI + nightly

- 子模块各自轻量自测（algorithm→ztest；init→harness；drivers→host 编译）。
- `ci-nightly.yml`：cron `17 3 * * 1-5`，子模块全推到最新头 + 编译（不提交指针）。

### M7 CD/发布

- tag 上跑干净构建 → zip → GitHub Release 附件（`fw-hpm5361icb-<sha8>-<date>` + map + build.log）。
- 可选：自托管 Windows runner（label `windows-lab`）烧录 hpm5361icb + 串口断言。

---

## 5. 首次跑挂排查表（按症状给动作）

> CI 第一次大概率红（Linux 纯净环境差异）。**不要思考，按表操作。**

| 症状 | 原因 | 动作 |
|------|------|------|
| `submodule update` 报 SSH/权限 | framework/init 还是 SSH URL | 回 §3.2 改 HTTPS 并推送 |
| `patch CHECK FAIL` | patch 与 clone 的官方版本上下文不符 | 回 M0 §2.5，用同版本官方仓库重新生成 patch |
| 缺头文件（`hpm_sdk` / `hpm_misc.h` 相关） | sdk_env sparse 拉取范围不够 | `git -C sdk_env sparse-checkout add hpm_sdk/../` 扩大范围，或整拉 hpm_sdk 再试 |
| riscv 工具链找不到 | SDK 没装 / setup.sh 没跑 riscv64-zephyr-elf | 确认 `setup.sh -t riscv64-zephyr-elf -c` 执行过（§3.1 已含） |
| HPM 特有宏 undefined（如 `HPM53_SINGLE_PRECISION_FPU`） | sdk_env 版本与本地不一致 | 把本地 sdk_env 的 commit 固定到 CI（`git -C sdk_env checkout <本地commit>`） |
| 链接/汇编错误 | 工具链或 soc 定义不一致 | 对比本地 `west build` 日志，逐项对齐 sdk_glue/sdk_env 版本 |
| `build directory targets board X` | build 缓存跨板 | 已用 `-p always` 规避；若出现删 `/work/zephyr_user/project/build` 重来 |
| 其它报错 | — | 把完整日志贴给本地 `dust build hpm5361icb` 对比，找本地能编而 CI 不能编的差异 |

### 2026-08-03 实际踩坑（M1 排障记录，全部已修）

| 症状 | 根因 | 修复 |
|------|------|------|
| `ERROR: Unknown toolchain 'riscv32-zephyr-elf'` | SDK 0.16.8 **无独立 riscv32 工具链** | 用 `riscv64-zephyr-elf`（一个覆盖 rv32/rv64） |
| `setup.sh ...tar.xz: Permission denied` | SDK 解压到 /opt 属 root，runner 无写权限 | setup.sh 前加 `sudo` |
| `could not create leading directories of '/work'` | /work 在根目录，runner 无创建权限 | 全路径用 `${{ github.workspace }}`（runner 可写，且与 checkout 一致） |
| `apply-patches.sh: Permission denied` | Windows git `core.filemode=false` 不跟踪执行位，mode=100644 | HPM 树 `git update-index --chmod=+x` + CI 用 `bash` 调用（双保险） |
| `No board named 'hpm5361icb'`（BOARD_ROOT 指向不存在路径） | project/CMakeLists 硬编码 `SDK_GLUE_USER_DIR` 相对路径，不读 env | CMakeLists 支持 `$ENV{SDK_GLUE_USER_DIR}` |
| `Could not find package ... "Zephyr-sdk"` | project/CMakeLists 硬编码 `ZEPHYR_SDK_INSTALL_DIR` 相对路径覆盖 env | CMakeLists 支持 `$ENV{ZEPHYR_SDK_INSTALL_DIR}`（CACHE FORCE） |
| `Kconfig.defconfig: .../../sdk_glue_user ... not found` | sdk_glue Kconfig 硬编码兄弟目录名 `sdk_glue_user` | user 层 checkout 到 `hpm/sdk_glue_user`（**目录名必须叫 sdk_glue_user**） |
| `undefined reference to 'ADDRESS_IN_DLM'`（编译 warning: implicit declaration） | sdk_env 版本不一致：官方 main 已删 hpm_misc.h 的地址宏，本地是 v1.11.0 | clone 固定 `--branch v1.11.0`（= 本地版本） |
| `upload-artifact: No files ... zephyr.{elf,bin,hex,map}` | **upload-artifact 不支持大括号 glob** | path 明确列出 `zephyr.elf` + `zephyr.map` |

**经验法则**：CI 的目标 = 复现本地能编的产物。本地能编、CI 不能编 → 一定是环境差异（版本/工具链/路径/patch），按差异逐个对齐。

---

## 6. 待确认项（需要你决策，不阻塞 M0/M1）

| # | 事项 | 说明 |
|---|------|------|
| 1 | Eigen 是 vendored 还是 gitlink？ | `framework/algorithm/math/eigen/.git` 是否存在；影响 `--recursive` |
| 2 | 板卡扩展节奏 | board_rm_c / hpm6e00evk 何时开发；开发前不进 CI |
| 3 | 是否要自托管烧录 runner | 有没有常驻可插靶机的 Windows 机器 |
| 4 | 版本号约定 | `v<yyyymmdd>` 或 `vX.Y.Z`（M7 用） |
| 5 | HPM 树是否公开 | 影响 CI 拉取方式与情报暴露面 |
| 6 | 比赛期隐私策略 | 板卡/参数/patch 能否公开 |

> 已解决（无需决策）：sdk_env 地址与修改范围（hpmicro/sdk_env 公开，仅 hpm_misc.h +9 行，sparse 拉 hpm_sdk）。

---

## 附录：命令速查

```bash
# ===== M0：生成 patch（在各自官方仓库内）=====
git diff -- <路径...> > /e/Zephyr_HPMicro/sdk_glue_user/zephyr-patches/<repo>/NNN-<desc>.diff

# ===== M0：干净环境验证 =====
TMP=/tmp/patch-test
git clone --depth 1 https://github.com/hpmicro/zephyr_sdk_glue.git $TMP/sdk_glue
git clone --depth 1 --filter=blob:none --sparse --branch v1.11.0 https://github.com/hpmicro/sdk_env.git $TMP/sdk_env
git -C $TMP/sdk_env sparse-checkout set hpm_sdk
cd /e/Zephyr_HPMicro/sdk_glue_user
./apply-patches.sh --zephyr $TMP/zephyr --sdk-env $TMP/sdk_env \
  --sdk-glue $TMP/sdk_glue --cherryusb $TMP/cherryusb
git -C $TMP/sdk_glue diff --stat   # 期望 34 files

# ===== 构建（CI 直接入口；CI 实际用 ${{ github.workspace }}，此处以 /work 代指工作区）=====
export ZEPHYR_BASE=/work/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk-0.16.8
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export SDK_GLUE_DIR=/work/hpm/zephyr_sdk_glue
export SDK_GLUE_USER_DIR=/work/hpm/sdk_glue_user   # 目录名必须叫 sdk_glue_user
cd /work/zephyr_user/project
west build -p always -b hpm5361icb -- -DBOARD_CFG=hpm5361icb

# ===== 本地（Windows 开发，不变）=====
dust build hpm5361icb
```

---

_更新：2026-08-03（实施手册。**M0 HPM 树 patch 化 ✅、M1 单板编译门禁 ✅ 均已完成并跑通**；9 个实际踩坑见 §5 排障记录；Ruleset 已生效）。_
