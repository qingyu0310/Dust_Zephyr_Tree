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
