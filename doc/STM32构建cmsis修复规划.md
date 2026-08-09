# STM32 构建 cmsis 修复规划

> 2026-08-08。目标：`dust build puzhong -p`（stm32f4_disco）编译通过。
> 依据：`doc/zephyr_子树架构搭建指南.md` §9.2/§9.4/§9.5/§9.7 + 样板 `E:\Zephyr\projects\temp\`。

## 1. 现象

`dust build puzhong -p` 失败于第一编译单元：

```
arch/arm/core/offsets/offsets.c
  → zephyr/arch/arm/asm_inline_gcc.h:24
  → fatal error: cmsis_core.h: No such file or directory
```

## 2. 根因

`cmsis_core.h` 在 `zephyr_user/platform/cmsis/`，`core_cm4.h` 在 west 模块
`modules/hal/cmsis/CMSIS/Core/Include`。这两个目录**都没进 include 路径**。

对比已配好的样板 `E:\Zephyr\projects\temp\`（位置 B），`project`（位置 A）缺两处：

| 项 | 样板 temp（有） | 当前 project（缺） |
|---|---|---|
| 根 CMakeLists `foreach BOARD_GLOBAL_INCLUDES → zephyr_include_directories` | `temp/CMakeLists.txt:50-53` | `project/CMakeLists.txt` 无 |
| st 板 board.cmake 设 `BOARD_GLOBAL_INCLUDES` | `temp/boards/st/board_rm_c/board.cmake` | `project/boards/st/puzhong` 与 `board_rm_c` 都只有 openocd |

依赖模块 `hal_st`、`cmsis`（v5）、`platform/cmsis` **均已拉取**，无需 west update。

> 注意：不用 cmsis_6（用户已踩过坑，搭建指南用 cmsis v5 + 遮蔽方案）。

## 3. 修复方案总览

在 project 补两处，完全照抄 temp 样板：

1. `project/CMakeLists.txt` 加 `foreach(_dir IN LISTS BOARD_GLOBAL_INCLUDES)` 处理（放在 `find_package(Zephyr)` 之后，`zephyr_include_directories` 才可用）。
2. `project/boards/st/puzhong/board.cmake` 与 `project/boards/st/board_rm_c/board.cmake` 各加 `BOARD_GLOBAL_INCLUDES` 两行。

## 4. 分阶段执行方案

### 阶段 1：project/CMakeLists.txt 加 foreach 处理

**① 目标**：让板卡 board.cmake 的 `BOARD_GLOBAL_INCLUDES` 真正挂进编译 include。

**② 干什么**：编辑 `E:\Zephyr\zephyr_user\project\CMakeLists.txt`。

old（第 73-78 行）：
```cmake
# ---- (7) 业务源码与系统入口 ------------------------------------------------
# main() 由架构层 init 提供（framework/init/main.c），业务项目不再有 src/
target_include_directories(app PRIVATE
  ${FW_ROOT}/init
  ${FW_ROOT}/cmd
)
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
```

new（在 target_include_directories 之后插入 foreach，照抄 temp 第 50-53 行）：
```cmake
# ---- (7) 业务源码与系统入口 ------------------------------------------------
# main() 由架构层 init 提供（framework/init/main.c），业务项目不再有 src/
target_include_directories(app PRIVATE
  ${FW_ROOT}/init
  ${FW_ROOT}/cmd
)
# 板卡专属全局 include（由 boards/<board>/board.cmake 的 BOARD_GLOBAL_INCLUDES 提供）
foreach(_dir IN LISTS BOARD_GLOBAL_INCLUDES)
  zephyr_include_directories(${_dir})
endforeach()
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
```

**③ 产出**：`project/CMakeLists.txt` 含 foreach 段。

**④ 验证**：`grep -n "BOARD_GLOBAL_INCLUDES" project/CMakeLists.txt` 见 foreach 两行。

---

### 阶段 2：st 板 board.cmake 加 BOARD_GLOBAL_INCLUDES

**① 目标**：stm32 板的 cmsis_core.h（platform/cmsis）与 core_cm4.h（cmsis 模块）进 include。

**② 干什么**：编辑两个文件，内容相同。

文件 A：`E:\Zephyr\zephyr_user\project\boards\st\puzhong\board.cmake`
文件 B：`E:\Zephyr\zephyr_user\project\boards\st\board_rm_c\board.cmake`

old（两文件第 1-3 行，版权后直接是 runner）：
```cmake
# SPDX-License-Identifier: Apache-2.0

set(BOARD_FLASH_RUNNER openocd)
```

new（版权后插入 BOARD_GLOBAL_INCLUDES，照抄 temp 样板）：
```cmake
# SPDX-License-Identifier: Apache-2.0

# 板卡专属全局 include（根 CMakeLists 的 foreach 统一 zephyr_include_directories）
set(BOARD_GLOBAL_INCLUDES
  "${ZEPHYR_USER_DIR}/platform/cmsis"
  "${CMAKE_SOURCE_DIR}/../../modules/hal/cmsis/CMSIS/Core/Include"
)

set(BOARD_FLASH_RUNNER openocd)
```

路径换算（project 根 = `E:\Zephyr\zephyr_user\project`）：
- `${ZEPHYR_USER_DIR}/platform/cmsis` → `E:\Zephyr\zephyr_user\platform\cmsis` ✓
- `${CMAKE_SOURCE_DIR}/../../modules/hal/cmsis/CMSIS/Core/Include` → `E:\Zephyr\modules\hal\cmsis\CMSIS\Core\Include` ✓（core_cm4.h 所在）

**③ 产出**：两个 board.cmake 各含 5 行 BOARD_GLOBAL_INCLUDES。

**④ 验证**：`grep -n "BOARD_GLOBAL_INCLUDES\|platform/cmsis\|Core/Include" project/boards/st/puzhong/board.cmake project/boards/st/board_rm_c/board.cmake` 各见 4 行。

---

### 阶段 3：编译验证 puzhong

**① 目标**：`dust build puzhong -p` 过 `cmsis_core.h`，继续往下走。

**② 干什么**：用户执行（AI 不编译）：
```powershell
cd E:\Zephyr\zephyr_user\project
dust build puzhong -p
```

**③ 预期结果**：
- `cmsis_core.h` 不再报错。
- 若报 `core_cm4.h: No such file` → 阶段 2 未生效，回查 foreach/board.cmake。
- 若报 `sdk_glue__drivers__can: No SOURCES given` warning → **无害**，见 §6 待议。
- 若报其他错误 → 记录，进阶段 4。

**④ 验证标准**：编译通过（无 fatal error）。

---

### 阶段 4（按需）：后续错误处理

**① 目标**：处理阶段 3 暴露的剩余错误。

**② 干什么**：按搭建指南 FAQ 对照：
- §9.3 `stm32f4xx.h` 找不到 → `west update hal_st`（已拉取，通常不会出）
- §9.4 `core_cm4.h` 找不到 → 阶段 2 已覆盖
- §9.7 `No SOURCES given to Zephyr library sdk_glue__drivers__can` → STM32 不该加 sdk_glue，见 §6

**③ 产出**：无额外代码改动（除非发现新根因）。

**④ 验证**：编译通过。

## 5. 执行清单

- [ ] 阶段 1：`project/CMakeLists.txt` 加 foreach 段
- [ ] 阶段 2A：`puzhong/board.cmake` 加 BOARD_GLOBAL_INCLUDES
- [ ] 阶段 2B：`board_rm_c/board.cmake` 加 BOARD_GLOBAL_INCLUDES
- [ ] 阶段 3：`dust build puzhong -p` 编译验证
- [ ] 阶段 3b：`dust build board_rm_c -p` 编译验证（同链路板卡，确认未破坏）

## 6. 待议项

1. **sdk_glue 在 STM32 构建里多余**：project/CMakeLists.txt 第 26-35 行无条件加载 sdk_glue
   （HPM 板需要）。STM32 板会出现 `sdk_glue__drivers__can: No SOURCES` warning（§9.7）。
   位置 A 是 HPM/ST 共存的维护者工程，**本轮不动**；如用户要消除 STM32 侧 warning，单独规划。
2. **HPM 板 board.cmake**：hpm5361icb 不需要 cmsis（riscv），其 board.cmake 没有
   BOARD_GLOBAL_INCLUDES 时 foreach 空循环，无影响，不动。

## 7. 风险

- `zephyr_include_directories` 必须在 `find_package(Zephyr)` 之后调用——阶段 1 的 foreach
  已放在其后（第 69 行 find_package 之后），安全。
- 改动只涉及 include 挂载，不碰业务代码、不碰 framework、不碰 west 模块。
