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
