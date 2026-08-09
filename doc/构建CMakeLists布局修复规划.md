# 构建修复规划：find_package 位置 — 模块加载后、app 装配前

日期：2026-08-09
项目：detected（`dust build puzhong`）

## 一、根因

`project/CMakeLists.txt` 的 `find_package(Zephyr)` 放到了文件末尾（145 行），导致两个错：

1. **`target_include_directories(app)`（98 行）、`add_subdirectory(framework)`（104 行）、`zephyr_include_directories`（131 行）、`target_compile_options(app)`（133 行）全部在 find 之前**——`app` target 是 `find_package` 创建的，这些语句在 find 前执行报 `target app not found` / `unknown command zephyr_include_directories`。
2. **模块加载（`set/list(APPEND ZEPHYR_EXTRA_MODULES)`，89/123 行）在 find 之前**——这个放对了，Kconfig 能加载（hal_stm32 已恢复）。

**`find_package` 唯一能编译的位置**（两条硬约束）：

- 在**所有模块加载之后**（ZEPHYR_EXTRA_MODULES 在 find 里被读，晚设无效）
- 在**所有 `target_*(app)` / `add_subdirectory` / `zephyr_include_directories` 之前**（app target 在 find 里创建）

## 二、目标

`dust build puzhong` 通过编译。四块语义分区保留；只调整 `find_package` 与装配的位置。

## 三、改法：整个文件 old → new

**文件**：`e:/Zephyr/projects/detected/project/CMakeLists.txt`（146 行 → 替换为下面结构）

### old（现状，146 行）

```
1-21    路径规划
23-54   Zephyr 树自带（板级 overlay/conf/board.cmake + SDK）——无 find_package
56-60   子树模块 header
62-87   SDK glue + BOARD_ROOT/SOC_ROOT/DTS_ROOT
89-95   外来模块加载 set(ZEPHYR_EXTRA_MODULES ...)
97-101  target_include_directories(app ...)        ← 应在 find 后
103-113 add_subdirectory(framework ...)            ← 应在 find 后
115-119 用户工作区模块 header
121-127 内部模块加载 list(APPEND ZEPHYR_EXTRA_MODULES ...)
129-132 foreach BOARD_GLOBAL_INCLUDES               ← 应在 find 后
133     target_compile_options(app ...)             ← 应在 find 后
135-141 add_subdirectory(工作区 ...)                ← 应在 find 后
143-146 find_package(Zephyr) + project(my_project)  ← 放错了，改到模块加载后
```

### new（完整内容）

```cmake
cmake_minimum_required(VERSION 3.20.0)

# ============================================================================
#
# 路径规划
#
# ============================================================================

# 当前项目（detected/project）
set(PROJECT_ROOT 	"${CMAKE_CURRENT_SOURCE_DIR}/..")              # detected/
# 子树（zephyr_user）
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../zephyr_user")
set(FW_ROOT         "${ZEPHYR_USER_DIR}/framework")
# 用户工作区内部目录
set(APP_THIRD_PARTY "${PROJECT_ROOT}/third_party")                 # detected/third_party/
set(APP_MODULES     "${PROJECT_ROOT}/modules")                     # detected/modules/
set(APP_DRIVERS     "${PROJECT_ROOT}/drivers")                     # detected/drivers/
set(APP_THREAD      "${CMAKE_CURRENT_SOURCE_DIR}/thread")          # detected/project/thread/

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
  	list(APPEND BOARD_ROOT "${SDK_GLUE_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_DIR}/dts")
  	list(APPEND BOARD_ROOT "${SDK_GLUE_USER_DIR}")
  	list(APPEND SOC_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}")
  	list(APPEND DTS_ROOT   "${SDK_GLUE_USER_DIR}/dts")
endif()

# 外来模块加载（module.yml 自动加载 Kconfig）
set(ZEPHYR_EXTRA_MODULES
  "${SDK_GLUE_DIR}"
  "${FW_ROOT}"
  "${ZEPHYR_USER_DIR}/platform/cmsis"
  "${ZEPHYR_USER_DIR}/drivers"
)

# ============================================================================
#
# 用户工作区模块（detected：thread / tflm / modules / drivers）
#
# ============================================================================

# 内部模块加载（追加进 ZEPHYR_EXTRA_MODULES；禁止 set ZEPHYR_MODULES——
# 覆盖语义顶掉 west 官方模块 hal_stm32，pinctrl dtsi 找不到）
list(APPEND ZEPHYR_EXTRA_MODULES
  "${APP_THIRD_PARTY}/tflm"
  "${APP_MODULES}"
  "${APP_DRIVERS}"
)

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

# 装配用户工作区
if(CONFIG_${CONFIG_SYM})
  	add_subdirectory(${APP_THREAD})
  	add_subdirectory(${APP_MODULES} ${CMAKE_CURRENT_BINARY_DIR}/modules)
  	add_subdirectory(${APP_DRIVERS} ${CMAKE_CURRENT_BINARY_DIR}/drivers)
	add_subdirectory(${APP_THIRD_PARTY}/tflm ${CMAKE_CURRENT_BINARY_DIR}/third_party/tflm)
endif()
```

**变动清单**：
1. `find_package(Zephyr)` + `project(my_project)` 从文件末尾（145 行）移到内部模块加载之后、装配之前
2. `target_include_directories(app ...)`、`add_subdirectory(framework ...)` 从子树块（97-113 行）下移到 find 之后
3. 其余内容不变，四块语义分区保留

## 四、验证

```bash
rm -rf e:/Zephyr/projects/detected/project/build
cd e:/Zephyr/projects/detected/project && dust build puzhong
```

- 不再报 `target app not found` / `zephyr_include_directories` 未知命令
- `CONFIG_MOD_LCD=y`、`CONFIG_MOD_AI_LCD=y`、`CONFIG_TFLM=y`、`CONFIG_DUST_*` 出现在 `.config`
- `lcd.hpp` 找到（`modules/CMakeLists.txt` 的 `if(CONFIG_MOD_LCD)` 生效）

## 五、执行清单

- [ ] 替换 `project/CMakeLists.txt` 为新结构
- [ ] `rm -rf build` + `dust build puzhong`
- [ ] 确认 .config 出现 MOD_LCD / DUST_* / TFLM
- [ ] 编译通过
