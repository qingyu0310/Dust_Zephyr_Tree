/**
 * @file usb_dev_port.hpp
 * @author qingyu
 * @brief USB 端口管理核心 — EP0 控制传输 + 标准请求 + 端口生命周期
 * @version 0.1
 * @date 2026-07-27
 *
 * UsbDevPort 对应 CherryUSB 的 usbd_core 层。
 * 负责：
 *   - USB device 状态管理
 *   - EP0 控制传输状态机
 *   - 标准请求处理（GET_DESCRIPTOR、SET_ADDRESS、SET_CONFIGURATION 等）
 *   - Class 请求分发
 *   - Endpoint complete 分发
 *   - Reset/Disconnect 统一复位
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>
#include "usb_hal.hpp"
#include "usb_types.hpp"
#include "usb_cdc_acm.hpp"

enum class Ep0Stage : uint8_t {
    Idle,                           // 空闲 / 等待 SETUP
    DataIn,                         // DATA   IN  阶段
    DataOut,                        // DATA   OUT 阶段
    StatusIn,                       // STATUS IN  阶段
    StatusOut,                      // STATUS OUT 阶段
};

/**
 * @brief USB 设备核心
 */
class UsbDevPort final
{
public:
    /**
     * @brief 初始化
     * @param hal       HAL 实例
     * @param hal_cfg   HAL 配置
     * @param cdc_cfg   CDC ACM 配置
     * @return true=成功
     */
    bool Init(UsbHal& hal, const UsbHal::Config& hal_cfg, const UsbCdcAcmConfig& cdc_cfg);

    bool        Start()     { return ready_ ? hal_->Connect() : false; }
    void        Stop();
    UsbHal&     GetHal()    { return *hal_; }
    UsbCdcAcm&  GetCdcAcm() { return cdc_acm_; }
    Speed       GetSpeed() const { return speed_; }

    // CDC 回调注册代理（Usb Stream 层使用）
    using DataCallback = UsbCdcAcm::DataCallback;
    using ConfigureCallback = UsbCdcAcm::ConfigureCallback;
    
    void SetDataCallback(DataCallback cb, void* ctx)            { cdc_acm_.SetDataCallback(cb, ctx); }
    void SetConfigureCallback(ConfigureCallback cb, void* ctx)  { cdc_acm_.SetConfigureCallback(cb, ctx); }

private:
    UsbHal*      hal_            = nullptr;
    UsbCdcAcm    cdc_acm_        {};                        // CDC ACM 协议（值成员）
    DeviceState  state_          = DeviceState::Default;
    uint8_t      configuration_  = 0;
    Speed        speed_          = Speed::Full;

    /// EP0 控制传输上下文（含 stage/sequence/分段状态）
    Ep0Stage ep0_stage_ = Ep0Stage::Idle;

    /// 当前 class 请求 SETUP 包（用于 DATA OUT 完成后的回调）
    SetupPacket class_setup_ {};
    bool ready_ = false;

    // EP0 控制传输数据缓冲（GET_DESCRIPTOR 外的数据收发）
    // cacheline 对齐——DMA 和 cache 操作需要
    alignas(32) uint8_t control_buffer_[512] {};

    void OnEvent(const UsbHal::Event& event);
    void HandleSetup(const uint8_t setup_data[8]);
    void HandleStandardRequest(const SetupPacket& setup);
    void HandleClassRequest(const SetupPacket& setup);
    void HandleTransferComplete(const UsbHal::Event& event);

    void ResetState(){
        state_          = DeviceState::Default;
        configuration_  = 0;
        ep0_stage_      = Ep0Stage::Idle;
    }

    bool SendDescriptor(const SetupPacket& setup);

    bool SubmitDataIn(const uint8_t* data, uint16_t len);
    bool SubmitDataOut(uint8_t* data, uint16_t len);
    bool SubmitStatusIn();
    bool SubmitStatusOut();

    bool SetupCdcEndpoints();                               // 开 CDC 三端点 + 首轮 RX
    
    static void HalEvent(void* ctx, const UsbHal::Event& ev)
    {
        if (ctx) static_cast<UsbDevPort*>(ctx)->OnEvent(ev);
    }

    void CloseCdcEndpoints()
    {
        hal_->EpClose(cdc_acm_.GetBulkOutEp());
        hal_->EpClose(cdc_acm_.GetBulkInEp());
        hal_->EpClose(cdc_acm_.GetNotificationEp());
    }

    void RequeueRx(uint8_t ep);
    void RecoverRxEndpoint(uint8_t ep);
};
