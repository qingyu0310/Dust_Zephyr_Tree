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

# EtherCAT 板卡底层 HAL（HPM ESC，2026-10-29 下沉自 sdk_glue_user）
# PHY 驱动按芯片分目录，由 dts 的 compatible 自动选中是否编译。
if(CONFIG_DUST_ECAT_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/ecat_hal_hpm.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm/esc_hpmicro.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/hpm_ecat_hw.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/ecat_zephyr_port.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/hpm_ecat_e2p_emulation.c
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/ecat_phy_mdio.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/ecat_phy_bind.cpp
        ${HPM_SDK_DIR}/drivers/src/hpm_esc_drv.c
        ${HPM_SDK_DIR}/components/eeprom_emulation/eeprom_emulation.c
        ${HPM_SDK_DIR}/utils/hpm_crc32.c)

    if(CONFIG_DUST_ECAT_PHY_JL1111)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/jl1111/phy_jl1111.cpp)
    endif()
    if(CONFIG_DUST_ECAT_PHY_RTL8201F)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/rtl8201f/phy_rtl8201f.cpp)
    endif()
    if(CONFIG_DUST_ECAT_PHY_TJA1101B)
        target_sources(app PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy/tja1101b/phy_tja1101b.cpp)
    endif()

    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/hpm
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/phy
        ${CMAKE_CURRENT_SOURCE_DIR}/ethercat/hal/ports/include
        ${FW_ROOT}/drivers/communication/ethercat/interface
        ${FW_ROOT}/drivers/communication/ethercat/core
        ${FW_ROOT}/drivers/communication/ethercat/ssc
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
