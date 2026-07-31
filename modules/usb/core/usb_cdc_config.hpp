/**
 * @file usb_cdc_config.hpp
 * @author qingyu
 * @brief USB CDC 配置 — 单一日志，统一描述符、端点计划、Stream 查询
 * @version 0.1
 * @date 2026-07-27
 *
 * UsbCdcAcmConfig 是 CDC ACM 配置的唯一事实来源。
 * 一份配置同时驱动：
 *   - UsbDescriptorSet（生成描述符字节）
 *   - UsbCdcAcm（CDC 协议校验/端点查询）
 *   - Usb（bulk endpoint 地址/ZLP/Read/Send）
 *
 * 修改端点地址或接口号时只需要改此文件的默认值或调用方传入的实例。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

/**
 * @brief CDC ACM 配置
 *
 * 所有 CDC 相关参数在此统一控制。
 * 修改配置后，描述符、HAL 开端点、Stream 查询全部同步改变。
 */
struct UsbCdcAcmConfig {
    // USB 标识
    uint16_t vid                = 0x34B7;           // 厂商 ID
    uint16_t pid                = 0xFFFF;           // 产品 ID
    uint16_t bcd_device         = 0x0100;           // 设备版本号

    // 接口号
    uint8_t  control_interface  = 0;                // CDC 控制接口编号
    uint8_t  data_interface     = 1;                // CDC 数据接口编号

    // 端点地址
    uint8_t  notification_ep    = 0x83;             // 中断 IN  端点地址
    uint8_t  bulk_out_addr      = 0x01;             // 批量 OUT 端点地址
    uint8_t  bulk_in_addr       = 0x81;             // 批量 IN  端点地址
    uint8_t  notification_mps  = 8;                // 中断 IN 最大包大小
    uint8_t  notification_interval = 10;           // 中断 IN 轮询间隔

    const char* manufacturer    = "MCHCK";          // 厂商名
    const char* product         = "USB CDC ACM";    // 产品名
    const char* serial_number   = "qingyu_king";    // 序列号

    // CDC 协议选项
    bool require_dtr            = false;            // 是否等待 DTR 才允许发送

};
