# zephyr_user — 用户自研层（与 zephyr 同级的小树）

> **SELF-MAINTAINED — qingyu**
> 这是与 zephyr 同级的用户自研层（`<workspace>/zephyr_user`，workspace 如 `E:\Zephyr_Test`），结构镜像 zephyr（`boards/`、`dts/bindings/`…），
> 只包含用户自研的板卡与 binding，**独立 git 仓库管理**，与官方 zephyr 仓库无关。

## 结构

```
zephyr_user/
├── boards/         用户自研板卡（boards/<vendor>/<name>/）
├── dts/bindings/   用户自研 devicetree binding
├── modules/        用户自研 Zephyr module（modules/<name>/）
└── README.md
```

## 内容清单

| 内容 | 位置 | 说明 |
| --- | --- | --- |
| STM32F407IGH6 板卡 | `boards/st/stm32f407igh6/` | 自定义板卡（176 引脚 BGA），非官方板 |
| W25Q128 binding | `dts/bindings/mtd/winbond,w25q128.yaml` | SPI NOR Flash binding，上游无此型号 |
| 自研 USB 协议栈 | `modules/usb/` | 自研 Zephyr USB module（含 module.yml），原 Dust_Zephyr_Architecture_User |

## 发现机制

应用工程 CMakeLists 通过 `BOARD_ROOT` / `DTS_ROOT` 引用本目录：

```cmake
list(APPEND BOARD_ROOT "<workspace>/zephyr_user")
list(APPEND DTS_ROOT  "<workspace>/zephyr_user/dts")
```

之后 `west build -b stm32f407igh6` 可直接按板名找到，无需放入 zephyr 官方 `boards/`。

## 管理方式

- 本目录是**独立 git 仓库**（`zephyr_user/.git`）。
- 新增板卡/binding 在本目录内 `git add` / `git commit`，`git push` 到用户自己的远端仓库。
- 与 zephyr 树完全独立（位于 zephyr 树同级目录），官方树 git 不受影响。

## 备注

- HPM 平台（hpm5361icb 等）板卡定义在 sdk_glue，不在此处。
- 本层只放"用户自研、独立管理"的内容，避免与官方 zephyr 结构混在一起。
