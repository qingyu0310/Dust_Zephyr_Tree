/**
 * @file usb_cdc_acm.hpp
 * @author qingyu
 * @brief CDC ACM 协议实现 — line coding / DTR/RTS / BREAK / notification
 * @version 0.1
 * @date 2026-07-27
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>
#include <zephyr/kernel.h>

#include "usb_types.hpp"
#include "usb_descriptor.hpp"
#include "usb_cdc_config.hpp"

/**
 * @brief CDC ACM 协议处理器
 *
 * 职责：
 *   - CDC 类请求（SET/GET_LINE_CODING、SET_CONTROL_LINE_STATE、SEND_BREAK）
 *   - bulk OUT/IN 完成转发到 OnEndpointComplete
 *
 * 收发缓冲和 Stream 接口在顶层 Usb 中管理，不在此类。
 * 配置使用 UsbCdcAcmConfig，不再自行维护端点常量。
 */
class UsbCdcAcm final
{
public:
    bool Init(const UsbCdcAcmConfig& cfg);

    /**
     * @brief 数据事件回调类型
     */
    using DataCallback = void (*)(void* ctx, uint8_t ep, const uint8_t* data, uint16_t len);
    using ConfigureCallback = void (*)(void* ctx, bool configured, uint16_t bulk_mps);

    void SetDataCallback(DataCallback cb, void* ctx) { 
        data_cb_ = cb; 
        data_ctx_ = ctx; 
    }
    void SetConfigureCallback(ConfigureCallback cb, void* ctx) { 
        cfg_cb_ = cb; 
        cfg_ctx_ = ctx; 
    }

    const uint8_t*  GetDescriptor(DescriptorType type, Speed speed, uint8_t index, uint16_t& length) const;
    bool            HandleClassRequest(const SetupPacket& setup, uint8_t* data, uint16_t& length);
    bool            OnConfigured(bool configured, Speed speed);
    void            NotifyConfigured();
    void            OnEndpointComplete(uint8_t endpoint, const uint8_t* data, uint16_t length, bool error = false);
    bool            CompleteControlOut(const SetupPacket& setup, const uint8_t* data, uint16_t length);

    // 从 CDC 配置查询端点信息（Usb 层使用）
    uint8_t  GetNotificationEp() const { return cfg_.notification_ep; }
    uint8_t  GetBulkOutEp()      const { return cfg_.bulk_out_addr; }
    uint8_t  GetBulkInEp()       const { return cfg_.bulk_in_addr; }
    uint8_t  GetNotificationMps() const { return cfg_.notification_mps; }
    uint8_t  GetNotificationInterval() const { return cfg_.notification_interval; }
    uint16_t GetBulkMps()        const { return bulk_mps_; }
    bool     CanSend()           const { return !cfg_.require_dtr || dtr_; }

private:

    // 描述符集合
    UsbCdcAcmDescriptorSet desc_set_ {};

    // 配置（Init 时拷贝，自身持有）
    UsbCdcAcmConfig cfg_        {};

    // 端点 MPS（运行时由 OnConfigured 设置）
    uint16_t bulk_mps_          = 64;

    // CDC 状态
    LineCoding line_coding_     {};
    bool dtr_                   = false;
    bool rts_                   = false;

    // 设备状态
    bool ready_                 = false;

    // 数据事件回调（顶层 Usb 注册，用于收发）
    DataCallback data_cb_       = nullptr;
    void*        data_ctx_      = nullptr;

    // 配置状态回调（顶层 Usb 注册）
    ConfigureCallback cfg_cb_   = nullptr;
    void*             cfg_ctx_  = nullptr;

    // CDC 请求处理
    bool OnSetLineCoding(const SetupPacket& setup, uint8_t* data, uint16_t& length);
    bool OnGetLineCoding(const SetupPacket& setup, uint8_t* data, uint16_t& length);
    bool OnSetControlLineState(const SetupPacket& setup, uint8_t* data, uint16_t& length);
    bool OnSendBreak(const SetupPacket& setup, uint8_t* data, uint16_t& length);
};
