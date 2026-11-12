# 底层装配（zephyr/）—— 与芯片耦合的一层
#
# 本层自包含：上层只 add_subdirectory(zephyr)，内部子目录由本文件负责。
# 不含业务逻辑、不含协议栈。
#
# 子目录职责：
#   boards/    板卡定义（BOARD_ROOT 引入，不参与编译）
#   dts/       binding yaml（DTS_ROOT 引入，不参与编译）
#   platform/  CMSIS 垫片（ZEPHYR_EXTRA_MODULES 引入）
#   contract/  共享契约头（include 路径，不参与编译）
#   drivers/   Zephyr 模块式驱动（module.yml + DEVICE_DT_DEFINE）

if(CONFIG_DUST_FSMC
   OR CONFIG_DUST_ENCODER
   OR CONFIG_DUST_USB_DEVICE_HAL_HPM
   OR CONFIG_DUST_USB_DEVICE_HAL_STM32
   OR CONFIG_DUST_ECAT_HAL_HPM
   OR CONFIG_DUST_FATAL_ESF_HPM
   OR CONFIG_DUST_FATAL_ESF_STM32)
    add_subdirectory(drivers ${CMAKE_CURRENT_BINARY_DIR}/drivers)
endif()
