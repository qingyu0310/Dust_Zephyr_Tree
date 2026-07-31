# SPDX-License-Identifier: Apache-2.0

# USB 协议栈 — 用户自维护库
# 这不是 Zephyr 原生 USB 驱动，不通过 DEVICE_DT_DEFINE 初始化。
# 应用的 trd_test.cpp 通过 Usb::Init 手动初始化。

if(CONFIG_COM_USB)
    zephyr_library()
    zephyr_library_sources(
        core/usb_dev_port.cpp
        core/usb_cdc_acm.cpp
        core/usb_descriptor.cpp
    )
    zephyr_library_include_directories(
        .
        interface
        core
    )
    # 全局暴露子目录，供 drivers/usb.hpp / trd_test.cpp 以裸文件名引用
    zephyr_include_directories(
        interface
        core
        hal
    )

    # bipbuf 依赖（UsbRxQueue 使用 BipBuffer<1024>）— E 盘相对路径
    zephyr_include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../projects/dust/algorithm/buffer/bipbuf
    )
endif()

if(CONFIG_COM_USB_HAL_HPM)
    zephyr_library_sources(
        hal/usb_hal_hpm.cpp
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device/hpm_usb_device.c
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/hpm_usb_drv.c
    )
    zephyr_library_include_directories(
        hal
    )
    # HPM SDK USB device 组件 + driver 路径
    zephyr_include_directories(
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/inc
    )
endif()
