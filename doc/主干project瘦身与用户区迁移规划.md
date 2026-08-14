# 主干 project 瘦身（只留 test）+ 用户区迁移规划

> 2026-08-13。**目标**：把当前 `zephyr_user/project`（完整业务工程，8 个业务线程）**复制转移**到用户区 `E:\Zephyr\projects\qingyu\project\`；然后主干 `zephyr_user/project` 的 `thread/` **只保留 test 线程**（删其余 7 个业务线程目录），boards/prj.conf/Kconfig 构建链保留。
>
> **2026-08-13 更新（已执行）**：用户区 qingyu 已按**新模板形式**建成（用户区根平铺 framework 六模块 = 用户层自己的层 + project/），`project/CMakeLists.txt` 按 detected 模式配好（用户层六模块 `APP_*` + 架构层 `FW_ROOT=zephyr_user/framework` 共存）；旧模板 `template` 已对齐 qingyu（根六模块目录 + CMakeLists 同步）。**阶段 1（迁移用户区）已完成，当前进入阶段 2：删减维护者工作区（主干瘦身）。**

---

## 0. 背景与目标

### 设计理念依据

架构理念"**主干相同，分支不同**"（见 [[zishu-architecture-overview]]）：
- framework 架构层统一（维护者集中维护），各用户工作区独立、自维护文件层。
- 依赖方向单向：user → zephyr_user；业务线程归各用户工作区。

当前主干 `zephyr_user/project` 同时承担"业务工程"（8 个业务线程）与"主干工程"（应是最小 test 骨架）两个角色，业务与主干混在一起，且业务线程代码与 hpm 板卡耦合深。本次把业务线程迁到用户区，主干瘦身成最小 test 工程。

### 目标（两条）

1. **业务迁到用户区** `E:\Zephyr\projects\qingyu\`：用户区按**新模板**建成——用户层六模块（algorithm/cmd/drivers/init/modules/topic 平铺用户区根）+ `project/`（8 线程业务工程副本）。架构层 `zephyr_user/framework` 照常引用（用户层 + 架构层共存）。
2. **主干瘦身**：`zephyr_user/project/thread/` 只留 `test`，删 `can/chassis/gimbal/gpio/imu/pc/remote` 七个线程目录及对应装配；`boards/`、`prj.conf` 保留。

### 边界（本次不动）

- `framework/` 六个架构子模块：不动。
- `zephyr_user/drivers/`、`boards/`、`dts/`：不动。
- 主干 `project/boards/`、`project/prj.conf`、`project/CMakeLists.txt`：保留不动。

---

## 1. 现状盘点（证据）

| 项 | 现状 | 证据 |
| --- | --- | --- |
| 主干 project | `zephyr_user/project`，git 子模块 → `Dust_Zephyr_Architecture_Project.git` | `.gitmodules` / `git remote` |
| 主干 project 结构 | `CMakeLists.txt`、`Kconfig`、`boards/`、`prj.conf`、`thread/` | 目录 |
| thread/ 线程 | `can/chassis/gimbal/gpio/imu/pc/remote/test`（8 个） | `project/thread/` 目录 |
| thread 装配 | `thread/CMakeLists.txt` 8 个 `if(CONFIG_TRD_XXX)` 编译块（GPIO/CHASSIS/GIMBAL/REMOTE/IMU/CAN_TX/PC/TEST） | 文件 |
| 业务门禁 Kconfig | `project/Kconfig` `if PRJ_MAIN` 下定义 `TRD_GPIO/CHASSIS/GIMBAL/CAN_TX/REMOTE/IMU/PC/TEST` + 依赖子符号 `USE_POWERMETER`/`IMU_IDENTIFICATION` | 文件 |
| prj.conf | 只开 `CONFIG_PRJ_MAIN=y` + 基础（CPP/线程/FMU 等），无 `CONFIG_TRD_XXX` | 文件 |
| **用户区 qingyu（新模板，已建成）** | 根平铺 `algorithm/cmd/drivers/init/modules/topic`（用户层）+ `project/`（业务工程副本，CMakeLists 已按 detected 模式配好） | `E:\Zephyr\projects\qingyu\` |
| **模板 template（已对齐 qingyu）** | 根建六模块目录 + `project/CMakeLists.txt` 同步 qingyu 格式 | `E:\Zephyr\projects\template\` |
| test 线程 | `thread/test/trd_test.cpp` 空转骨架（`k_msleep(1000)`），`thread_init` 返回 true，无业务依赖 | 文件 |

**关键（新模板配置）**：用户区 project/CMakeLists.txt（detected 模式，qingyu/template 已落地）：
- `PROJECT_ROOT = ${CMAKE_CURRENT_SOURCE_DIR}/..`（用户区根）
- `ZEPHYR_USER_DIR = ../../../zephyr_user`、`FW_ROOT = ${ZEPHYR_USER_DIR}/framework`（架构层，照常装配六模块）
- 用户层六模块：`APP_ALGORITHM/APP_CMD/APP_DRIVERS/APP_INIT/APP_MODULES/APP_TOPIC = ${PROJECT_ROOT}/{algorithm,cmd,drivers,init,modules,topic}`
- `ZEPHYR_EXTRA_MODULES` 追加用户层六模块 + `add_subdirectory` 装配（`if(EXISTS .../CMakeLists.txt)` 保护，空目录跳过）
- `SDK_GLUE_DIR/USER = ../../../../Zephyr_HPMicro/{sdk_glue,sdk_glue_user}`（**Zephyr_HPMicro 在 E:\ 根，不带 Zephyr/ 前缀**）

---

## 2. 目标结构

```text
E:\Zephyr\projects\qingyu\                        ← 用户区（已完成）
├── algorithm/ cmd/ drivers/ init/ modules/ topic/   ← 用户层六模块（平铺，用户自有层）
└── project/                                        ← 业务工程副本（8 线程 + boards + 构建文件，CMakeLists 已配好）

E:\Zephyr\projects\template\                       ← 模板（已对齐 qingyu）
├── algorithm/ cmd/ drivers/ init/ modules/ topic/   ← 六模块空目录
└── project/                                        ← CMakeLists 同 qingyu 格式

zephyr_user\project\                                ← 主干瘦身（本次阶段 2）
├── CMakeLists.txt                                  ← 不变
├── Kconfig                                         ← 只留 TRD_TEST 段（删 7 业务 TRD_XXX + 依赖子符号）
├── prj.conf                                        ← 不变
├── boards/                                         ← 不变
└── thread/
    ├── CMakeLists.txt                              ← 只留 if(CONFIG_TRD_TEST) 块
    └── test/                                       ← 唯一线程
```

---

## 3. 分阶段执行

### 阶段 1（已完成）：迁移用户区

**结果**：
1. 复制主干 project → `E:\Zephyr\projects\qingyu\project\`（排除 build/、.git，8 线程 + boards + 构建文件齐全）。
2. `qingyu/project/CMakeLists.txt` 按**新模板 detected 模式**配好（§1 关键）：PROJECT_ROOT=用户区根、架构层 FW_ROOT=zephyr_user/framework 照常、用户层六模块 APP_* 平铺引用 + if(EXISTS) 保护、SDK_GLUE 指向 `E:\Zephyr_HPMicro`。
3. 模板 `template` 对齐 qingyu：根建六模块空目录 + `project/CMakeLists.txt` 同步格式。
4. 用户编译验证 qingyu（hpm5361icb）通过。

**注**：qingyu/template 若从 hpm5361 复制过 build/，需先删 build/（残留 CMakeCache 指向旧路径）再 -p 编译。

---

### 阶段 2（当前）：删减维护者工作区（主干瘦身，只留 test）

**目标**：主干 `thread/` 只留 test；业务线程目录、装配编译块、业务门禁 Kconfig 删干净；boards/prj.conf/CMakeLists.txt 不动。

**任务 2.1 — 删线程目录**：
```bash
cd E:/Zephyr/zephyr_user/project/thread
rm -rf can chassis gimbal gpio imu pc remote
```
保留：`test/`、`CMakeLists.txt`。

**任务 2.2 — `thread/CMakeLists.txt` 删业务编译块**（old → new）：

old（现文件，GPIO/CHASSIS/GIMBAL/REMOTE/IMU/CAN_TX/PC 七块 + TEST 块）：
```cmake
if(CONFIG_TRD_GPIO)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gpio/trd_gpio.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/gpio
	)
endif()

if(CONFIG_TRD_CHASSIS)
	...（chassis）
endif()

if(CONFIG_TRD_GIMBAL)
	...（gimbal）
endif()

if(CONFIG_TRD_REMOTE)
	...（remote）
endif()

if(CONFIG_TRD_IMU)
	...（imu）
endif()

if(CONFIG_TRD_CAN_TX)
	...（can）
endif()

if(CONFIG_TRD_PC)
	target_sources(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/pc/trd_pc.cpp
	)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/pc
	)
endif()

if(CONFIG_TRD_TEST)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/trd_test.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/test
	)
endif()
```

new（只留 TEST 块，含文件头注释）：
```cmake
# 业务线程装配 — 门禁 TRD_XXX 保持原名（不加 DUST_ 前缀）

if(CONFIG_TRD_TEST)
	target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/trd_test.cpp)
	target_include_directories(app PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/test
	)
endif()
```

**任务 2.3 — `project/Kconfig` 删业务门禁段**（old → new）：

old（`if PRJ_MAIN` 内，除 TRD_TEST 外的各段）：
```kconfig
config TRD_GPIO        # 整段删
config TRD_CHASSIS     # 整段删（含 USE_POWERMETER 子段）
config TRD_GIMBAL      # 整段删
config TRD_CAN_TX      # 整段删
config TRD_REMOTE      # 整段删
config TRD_IMU         # 整段删（含 IMU_IDENTIFICATION 子段）
config TRD_PC          # 整段删
```
new（只留 TEST 段 + PRJ_MAIN 骨架 + USE_CMD_*）：
```kconfig
config PRJ_MAIN
    bool "Main business project"
    default n

if PRJ_MAIN

config USE_CMD_SHELL   # 保留
config USE_CMD_BUZZER  # 保留
config USE_CMD_FLASH   # 保留

config TRD_TEST
    bool "User test add config"
    default n
    help
      Temporary user test switch.

endif # PRJ_MAIN

source "Kconfig.zephyr"
```
说明：TRD_GPIO/CHASSIS/GIMBAL/CAN_TX/REMOTE/IMU/PC 及依赖子符号 `USE_POWERMETER`（depends on TRD_CHASSIS）、`IMU_IDENTIFICATION`（depends on TRD_IMU）**整段删除**——它们只服务已删线程，留着是无引用的死配置。

**产出**：主干 `thread/` 只有 `test/` + 单块 CMakeLists；`Kconfig` 只有 `TRD_TEST` 门禁；`boards/`、`prj.conf`、`CMakeLists.txt` 未动。

**验证**：`find project/thread` 只有 test 与 CMakeLists.txt；`grep -n "TRD_GPIO\|TRD_CHASSIS\|..." project/thread/CMakeLists.txt project/Kconfig` 无残留；`boards/`、`prj.conf` git diff 未动。

---

### 阶段 3：验证（用户编译主干 test-only）

**目标**：test-only 主干可编译。

**任务**：
1. 用户编译主干 test-only 工程（如 hpm5361icb）：确认 `CONFIG_TRD_TEST` 可开、无 "no such file"（删线程目录后无残留引用）、无链接错误。
2. 用户区 qingyu 业务工程独立可编译（阶段 1 已验证）。

**产出**：主干 test 骨架可用；用户区业务工程完整。

---

## 4. 验证标准（汇总）

- [ ] 主干 `project/thread/` 只有 `test/` + `CMakeLists.txt`（`find` 确认）
- [ ] 主干 `thread/CMakeLists.txt` 只有 `if(CONFIG_TRD_TEST)` 块（`grep` 确认无 TRD_GPIO 等）
- [ ] 主干 `project/Kconfig` 无 TRD_GPIO/CHASSIS/GIMBAL/CAN_TX/REMOTE/IMU/PC/USE_POWERMETER/IMU_IDENTIFICATION（`grep` 确认）
- [ ] 主干 `boards/`、`prj.conf`、`project/CMakeLists.txt` 未改动（git diff 确认）
- [ ] 用户区 qingyu 业务工程可独立编译（hpm5361icb）
- [ ] 用户编译 test-only 主干（hpm5361icb）通过

## 5. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| 与 STM32 USB HAL 规划交叉 | trd_pc.cpp 是 `doc/STM32_USB_HAL实现规划.md` 阶段 3 的改动对象，本次从主干删除 | 两规划协调：STM32 HAL 验证改在用户区 qingyu 的 trd_pc 或改 test 线程承载；主干不再有 trd_pc |
| git 子模块提交 | 主干 project 是子模块（Dust_Zephyr_Architecture_Project） | 瘦身改动提交到 project 子模块（直推，无门禁）+ 主仓库更新指针走 PR（GH013，见 [[cicd-pr-gate-upload]]）；用户区 qingyu/template 是否 git 化由用户定 |
| 残留 build/ CMakeCache | 从 hpm5361 复制的用户区 build/ 残留 CMakeCache 指向旧路径，-p 编译报 cache 冲突 | 删 build/ 再 -p 编译 |
| 用户层六模块空目录 | qingyu/template 根六模块为空，add_subdirectory 会失败 | CMakeLists 已加 `if(EXISTS .../CMakeLists.txt)` 保护，空目录跳过，填内容后自动装配 |
| 主干 test 线程依赖 | test 线程只依赖 thread.hpp/Init_entry.hpp（framework/init），无业务依赖 | 已核实 trd_test.cpp；保留即可编译 |

## 6. 执行清单（逐条勾）

- [x] 阶段 1：`mkdir -p E:/Zephyr/projects/qingyu/project`
- [x] 阶段 1：复制 project（排除 build/、.git）→ qingyu（8 线程 + boards + 构建文件齐全）
- [x] 阶段 1：`qingyu/project/CMakeLists.txt` 按新模板 detected 模式配好（用户层六模块 APP_* + 架构层 FW_ROOT + SDK_GLUE 指向 E:\Zephyr_HPMicro）
- [x] 阶段 1：模板 template 对齐 qingyu（根六模块目录 + CMakeLists 同步）
- [x] 阶段 1：用户编译 qingyu 验证（路径解析正确，hpm5361icb 找到）
- [ ] 阶段 2：`rm -rf project/thread/{can,chassis,gimbal,gpio,imu,pc,remote}`
- [ ] 阶段 2：`thread/CMakeLists.txt` 删 7 个业务编译块（只留 TEST 块）
- [ ] 阶段 2：`project/Kconfig` 删 7 个业务 TRD_XXX 段 + USE_POWERMETER/IMU_IDENTIFICATION
- [ ] 阶段 2：grep 确认无 TRD_GPIO 等残留；git diff 确认 boards/prj.conf/CMakeLists.txt 未动
- [ ] 阶段 3：用户编译 test-only 主干（hpm5361icb）
- [ ] 后续：project 子模块提交瘦身改动 + 主仓库指针 PR（用户确认后再执行 git 操作）
