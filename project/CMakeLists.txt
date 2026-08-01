cmake_minimum_required(VERSION 3.20.0)

# ---- (0) 框架与 zephyr_user 路径（位置 A：zephyr_user/project）---------
set(FW_ROOT         "${CMAKE_CURRENT_SOURCE_DIR}/../framework")
set(ZEPHYR_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ---- (1) 业务门禁符号 -----------------------------------------------------
set(CONFIG_SYM PRJ_MAIN)

# ---- (2) SDK glue（HPM 平台可选）------------------------------------------
if(DEFINED ENV{SDK_GLUE_DIR})
  set(SDK_GLUE_DIR "$ENV{SDK_GLUE_DIR}")
else()
  set(SDK_GLUE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Zephyr_HPMicro/sdk_glue")
endif()
set(SDK_GLUE_USER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../Zephyr_HPMicro/sdk_glue_user")

# ---- (3) BOARD_ROOT / SOC_ROOT / DTS_ROOT 指向 zephyr_user -----------------
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

# ---- (4) ZEPHYR_EXTRA_MODULES ----------------------------------------------
# framework（module.yml 只承载 dts_root）/ platform/cmsis（CMSIS 兼容层）/ sdk_glue（HPM 官方 glue）
# CherryUSB 已移除（自研 usb 栈替代），旧 zephyr_user/modules/usb、modules/cmsis 亦不挂
set(ZEPHYR_EXTRA_MODULES
  "${SDK_GLUE_DIR}"
  "${FW_ROOT}"
  "${ZEPHYR_USER_DIR}/platform/cmsis"
)
set(ZEPHYR_SDK_INSTALL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../zephyr-sdk-0.16.8" CACHE PATH "")

# ---- (5) 板级 overlay / conf / board.cmake ---------------------------------
if(NOT DEFINED BOARD_CFG)
  set(BOARD_CFG ${BOARD})
endif()
file(GLOB OVERLAY_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/boards/*/${BOARD_CFG}/${BOARD}.overlay)
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

# ---- (6) 接入 Zephyr 构建系统 ---------------------------------------------
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(my_project)

# ---- (7) 业务源码与系统入口 ------------------------------------------------
# main() 由架构层 init 提供（framework/init/main.c），业务项目不再有 src/
target_include_directories(app PRIVATE
  ${FW_ROOT}/init
  ${FW_ROOT}/cmd
)
target_compile_options(app PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)

# ---- (8) 装配架构层各层（门禁打开时 add_subdirectory）----------------------
if(CONFIG_${CONFIG_SYM})
  add_subdirectory(${FW_ROOT}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
  add_subdirectory(${FW_ROOT}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
  add_subdirectory(${FW_ROOT}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
  add_subdirectory(${FW_ROOT}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
  add_subdirectory(${FW_ROOT}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
  add_subdirectory(${FW_ROOT}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)
  add_subdirectory(thread)
endif()
