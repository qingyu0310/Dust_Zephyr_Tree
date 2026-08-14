# ci/ — framework 聚合骨架工程

> 层 1：framework 子模块独立 CI 的共享骨架。六子模块 CI 都在这上面 native_sim 编译。
> 规划文档：`doc/framework子模块独立CI规划.md`。

## 这是什么

无业务 thread 的 framework 合体，`native_sim` 上编译验证六个子模块（drivers/algorithm/modules/topic/cmd/init）改动不炸。编译错误天然定位到炸的文件。

## 怎么组装 framework/

六子模块目录必须在 `ci/` 的上一级（`../framework/`）齐整：

```text
workspace/
├── zephyr/           # git clone zephyr@v4.3.0（native_sim board 在 Zephyr 树内）
├── framework/        # git clone 六子模块（当前子模块 checkout 当前 SHA，其余 master）
│   ├── drivers/ algorithm/ modules/ topic/ cmd/ init/
├── ci/               # 本目录（sparse checkout 主仓库的 ci/）
└── build/            # west build 输出
```

本地调试（zephyr_user 内）：`ci/` 与 `framework/` 天然同级，`../framework/` 直接指向 `zephyr_user/framework/`，无需组装。

## 怎么编译

```bash
ZEPHYR_BASE=<zephyr 树路径> west build -p always -b native_sim -d build ci
```

- **native_sim 用宿主编译器**，免 zephyr-sdk 与 HPM SDK（`ZEPHYR_TOOLCHAIN_VARIANT=host`）。
- 必须 clone Zephyr 源码（`zephyr/boards/native/native_sim/` 在 Zephyr 树内）。

## CONFIG 边界（prj.conf）

只开 native_sim 能编的**纯逻辑层** CONFIG（algorithm 算法/滤波/辨识 + topic 数据通道）。

**不开**：

- shell/log/var/buzzer/flash —— `DUST_CMD_SHELL` select `DUST_COM_UART_DMA`，`shell.cpp` 用 `DT_ALIAS(shell_uart)`，native_sim 无此 alias（只有 chosen `zephyr,shell-uart`）→ 开必炸。
- drivers 硬件类（CAN / UART DMA / USB / PWM / SPI / GPIO Output）—— native_sim 无对应设备。

改动 prj.conf 后要同步规划文档 §4 阶段 1 任务 1.3。

## 装配链

- `ci/zephyr/module.yml` 挂载 `ci/zephyr/Kconfig`（ZEPHYR_EXTRA_MODULES 指向 ci/）。
- `ci/zephyr/Kconfig` rsource 聚合六个子模块 Kconfig（与 `framework/zephyr/Kconfig` 同构）。
- `ci/CMakeLists.txt` `target_include_directories` 补 `../framework/init` + `../framework/cmd`（`init/Init_entry.cpp` 无条件 include cmd 的 buzzer.hpp/log.hpp）。
