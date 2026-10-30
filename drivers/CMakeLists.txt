if(CONFIG_DUST_DEV_FSMC)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fsmc/fsmc.cpp
    )
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fsmc
    )
endif()

# USB 板卡底层 HAL（HPM EHCI，2026-08-09 下沉自 framework）
if(CONFIG_DUST_USB_DEVICE_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm/usb_hal_hpm.cpp
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device/hpm_usb_device.c
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/hpm_usb_drv.c)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm
        ${FW_ROOT}/drivers/communication/stream/usb/interface
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/inc)
endif()

# USB 板卡底层 HAL（STM32 OTG FS，2026-08-13）
if(CONFIG_DUST_USB_DEVICE_HAL_STM32)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/stm32/usb_hal_stm32.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/stm32
        ${FW_ROOT}/drivers/communication/stream/usb/interface)
endif()

# EtherCAT ESC HAL（唯一允许调厂商 API 的层）
#
# 厂商 C 代码（esc_hpmicro.c、ecat_e2p_emulation.c 等）在 sdk_glue_user/drivers/ethercat/。
# PHY 是 framework 的设备模块（framework/drivers/device/ecat_phy），不在这里。
if(CONFIG_DUST_ECAT_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/ecat_hal_hpm.cpp
        ${HPM_SDK_DIR}/drivers/src/hpm_esc_drv.c
        ${HPM_SDK_DIR}/utils/hpm_crc32.c)

    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm
        ${FW_ROOT}/drivers/communication/ethercat/interface
        ${FW_ROOT}/drivers/communication/ethercat/core
        ${SDK_GLUE_USER_DIR}/drivers/ethercat
        ${SDK_GLUE_USER_DIR}/drivers/ethercat/include
        ${HPM_SDK_DIR}/components/eeprom_emulation
        ${HPM_SDK_DIR}/components/eeprom_emulation/port
        ${HPM_SDK_DIR}/utils)
endif()

# fatal 异常栈帧转储（芯片相关，2026-10-20）
# 接口在 framework/cmd/fatal/esf_dump.hpp，实现在这里按厂商分目录
if(CONFIG_DUST_FATAL_ESF_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm/esf_dump_riscv.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/hpm
        ${FW_ROOT}/cmd/fatal)
endif()

if(CONFIG_DUST_FATAL_ESF_STM32)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st/esf_dump_cortex_m.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fatal/st
        ${FW_ROOT}/cmd/fatal)
endif()
