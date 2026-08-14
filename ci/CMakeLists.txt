cmake_minimum_required(VERSION 3.20.0)

# 全框架聚合骨架：无业务 thread，native_sim 编译验证 framework 合体
set(FRAMEWORK "${CMAKE_CURRENT_SOURCE_DIR}/../framework")   # 指向 CI 组装的 framework/

# 各子模块作为 Zephyr 模块挂载（ci/zephyr/module.yml 已聚合 Kconfig）
set(ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}" CACHE STRING "" FORCE)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ci_framework)

# 头文件路径：init/Init_entry.cpp 无条件 include cmd 的 buzzer.hpp/log.hpp + init 的 System_startup.h
# （必加，否则阶段 1 native_sim 编译必炸；与 project/CMakeLists.txt:121-124 同构）
target_include_directories(app PRIVATE
    ${FRAMEWORK}/init
    ${FRAMEWORK}/cmd
)

# 装配 framework 各层（与 project/CMakeLists.txt:110-115 同构，但无 project/thread）
# ⚠️ 六目录必须全在（子模块 CI 统一 clone 全部六子模块，见 §2.3）
add_subdirectory(${FRAMEWORK}/drivers   ${CMAKE_CURRENT_BINARY_DIR}/framework/drivers)
add_subdirectory(${FRAMEWORK}/algorithm ${CMAKE_CURRENT_BINARY_DIR}/framework/algorithm)
add_subdirectory(${FRAMEWORK}/modules   ${CMAKE_CURRENT_BINARY_DIR}/framework/modules)
add_subdirectory(${FRAMEWORK}/topic     ${CMAKE_CURRENT_BINARY_DIR}/framework/topic)
add_subdirectory(${FRAMEWORK}/cmd       ${CMAKE_CURRENT_BINARY_DIR}/framework/cmd)
add_subdirectory(${FRAMEWORK}/init      ${CMAKE_CURRENT_BINARY_DIR}/framework/init)
