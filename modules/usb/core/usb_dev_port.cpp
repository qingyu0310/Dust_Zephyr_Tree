/**
 * @file usb_dev_port.cpp
 * @author qingyu
 * @brief USB 端口管理核心 — EP0 控制传输 + 标准请求处理 + 端口生命周期
 * @version 0.1
 * @date 2026-07-27
 *
 * @copyright Copyright (c) 2026
 */

#include "usb_dev_port.hpp"
#include "usb_cdc_acm.hpp"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_dev_port, LOG_LEVEL_INF);

//  —————————————————————————— 初始化 ——————————————————————————

/**
 * @brief 初始化 USB 设备核心
 * @param hal      HAL 实例
 * @param function 功能类（CDC ACM 等）
 * @param cfg      HAL 配置
 */
bool UsbDevPort::Init(UsbHal& hal, const UsbHal::Config& hal_cfg, const UsbCdcAcmConfig& cdc_cfg)
{
    if (ready_) return true;
    hal_ = &hal;
    if (!cdc_acm_.Init(cdc_cfg)) {
        LOG_ERR("CDC ACM init failed");
        return false;
    }
    if (!hal_->Init(hal_cfg, HalEvent, this)) return false;
    ready_ = true;
    return true;
}

//  —————————————————————————— 事件分发 ——————————————————————————

/**
 * @brief 事件路由 — Reset/Setup/TransferComplete/Connected
 */
void UsbDevPort::OnEvent(const UsbHal::Event& event)
{
    switch (event.type)
    {
        case UsbHal::EventType::Reset:
        case UsbHal::EventType::Disconnected:
            ResetState();
            CloseCdcEndpoints();
            cdc_acm_.OnConfigured(false, speed_);
            break;
        case UsbHal::EventType::SetupReceived:
            HandleSetup(event.setup);
            break;
        case UsbHal::EventType::TransferComplete:
            HandleTransferComplete(event);
            break;
        case UsbHal::EventType::Connected:
            speed_ = event.speed;
            break;
        default:
            break;
    }
}

//  —————————————————————————— SETUP 处理 ——————————————————————————

/**
 * @brief 处理收到的 SETUP 包
 * @param setup_data  8 字节 SETUP
 */
void UsbDevPort::HandleSetup(const uint8_t setup_data[8])
{
    SetupPacket setup {};
    setup.bm_request_type = setup_data[0];
    setup.b_request       = setup_data[1];
    setup.w_value         = static_cast<uint16_t>(setup_data[2]) | (static_cast<uint16_t>(setup_data[3]) << 8);
    setup.w_index         = static_cast<uint16_t>(setup_data[4]) | (static_cast<uint16_t>(setup_data[5]) << 8);
    setup.w_length        = static_cast<uint16_t>(setup_data[6]) | (static_cast<uint16_t>(setup_data[7]) << 8);

    ep0_stage_ = Ep0Stage::Idle;

    if (setup.IsStandard()) {
        HandleStandardRequest(setup);
    } 
    else if (setup.IsClass()) {
        HandleClassRequest(setup);
    } 
    else {
        hal_->EpStall(0x00, true);
    }
}

//  —————————————————————————— 标准请求 ——————————————————————————

/**
 * @brief 处理 USB 标准请求（GET_DESCRIPTOR/SET_ADDRESS/SET_CONFIG 等）
 */
void UsbDevPort::HandleStandardRequest(const SetupPacket& setup)
{
    switch (static_cast<StandardRequest>(setup.b_request))
    {
        case StandardRequest::GetStatus:
        {
            // 校验 recipient 合法，否则 STALL
            if (setup.Recipient() != kRecipientDevice && setup.Recipient() != kRecipientInterface && setup.Recipient() != kRecipientEndpoint) 
            {
                hal_->EpStall(0x00, true);
                break;
            }
            uint16_t status = 0;
            control_buffer_[0] = static_cast<uint8_t> (status & 0xFF);
            control_buffer_[1] = static_cast<uint8_t>((status >> 8) & 0xFF);
            SubmitDataIn(control_buffer_, 2);
            break;
        }
        case StandardRequest::ClearFeature:
        {
            if (setup.bm_request_type != 0x02 ||  // endpoint recipient
                setup.w_value != static_cast<uint16_t>(FeatureSelector::EndpointHalt)) {
                hal_->EpStall(0x00, true);
                break;
            }
            hal_->EpStall(setup.w_index & 0xFF, false);
            SubmitStatusIn();
            break;
        }
        case StandardRequest::SetFeature:
        {
            if (setup.bm_request_type != 0x02 ||
                setup.w_value != static_cast<uint16_t>(FeatureSelector::EndpointHalt)) {
                hal_->EpStall(0x00, true);
                break;
            }
            hal_->EpStall(setup.w_index & 0xFF, true);
            SubmitStatusIn();
            break;
        }
        case StandardRequest::SetAddress:
        {
            if (setup.bm_request_type != 0x00 || setup.w_value > 127 ||
                setup.w_index != 0 || setup.w_length != 0) {
                hal_->EpStall(0x00, true);
                break;
            }
            hal_->SetAddress(static_cast<uint8_t>(setup.w_value));
            state_ = DeviceState::Addressed;
            SubmitStatusIn();
            break;
        }
        case StandardRequest::GetDescriptor:
        {
            if (!SendDescriptor(setup)) hal_->EpStall(0x00, true);
            break;
        }
        case StandardRequest::SetConfiguration:
        {
            uint8_t cfg_val = setup.w_value & 0xFF;
            if (setup.bm_request_type != 0x00 || setup.w_value > 1 || setup.w_index != 0 || setup.w_length != 0) 
            {
                hal_->EpStall(0x00, true); 
                break; 
            }

            if (cfg_val != 0 && state_ == DeviceState::Configured) {
                CloseCdcEndpoints();
                cdc_acm_.OnConfigured(false, speed_);
                state_ = DeviceState::Addressed;
            }

            configuration_ = cfg_val;
            if (configuration_ != 0)
            {
                if (!cdc_acm_.OnConfigured(true, speed_)) {
                    configuration_ = 0;
                    state_ = DeviceState::Addressed;
                    hal_->EpStall(0x00, true);
                    break;
                }
                if (!SetupCdcEndpoints()) {
                    configuration_ = 0;
                    state_ = DeviceState::Addressed;
                    cdc_acm_.OnConfigured(false, speed_);
                    hal_->EpStall(0x00, true);
                    break;
                }
                state_ = DeviceState::Configured;
                cdc_acm_.NotifyConfigured();
            }
            else {
                state_ = DeviceState::Addressed;
                CloseCdcEndpoints();
                cdc_acm_.OnConfigured(false, speed_);
            }
            SubmitStatusIn();

            break;
        }
        case StandardRequest::GetConfiguration:
        {
            control_buffer_[0] = configuration_;
            SubmitDataIn(control_buffer_, 1);
            break;
        }
        case StandardRequest::GetInterface:
        {
            control_buffer_[0] = 0;
            SubmitDataIn(control_buffer_, 1);
            break;
        }
        case StandardRequest::SetInterface:
        {
            if (setup.w_value == 0) { SubmitStatusIn(); } else { hal_->EpStall(0x00, true); }
            break;
        }
        default:
            hal_->EpStall(0x00, true);
            break;
    }
}

//  —————————————————————————— Class 请求 ——————————————————————————

/**
 * @brief 将 Class 请求分发给功能类
 */
void UsbDevPort::HandleClassRequest(const SetupPacket& setup)
{

    class_setup_ = setup;
    uint16_t len = setup.w_length;
    if (len > sizeof(control_buffer_)) len = sizeof(control_buffer_);

    if (cdc_acm_.HandleClassRequest(setup, control_buffer_, len)) 
    {
        if (setup.IsDeviceToHost()) {
            SubmitDataIn(control_buffer_, len);
        } else if (len > 0) {
            SubmitDataOut(control_buffer_, len);
        } else {
            SubmitStatusIn();
        }
    } else {
        hal_->EpStall(0x00, true);
    }
}

//  —————————————————————————— 描述符查询 ——————————————————————————

/**
 * @brief 发送描述符（按 wLength 截断后通过 EP0 IN 返回）
 */
bool UsbDevPort::SendDescriptor(const SetupPacket& setup)
{
    uint8_t         desc_type   = (setup.w_value >> 8) & 0xFF;
    uint8_t         desc_index  = setup.w_value & 0xFF;
    DescriptorType  type        = static_cast<DescriptorType>(desc_type);
    uint16_t        length      = 0;
    const uint8_t*  data        = cdc_acm_.GetDescriptor(type, speed_, desc_index, length);

    if (data == nullptr || length == 0) return false;
    if (length > setup.w_length) length = setup.w_length;

    return SubmitDataIn(data, length);
}

//  —————————————————————————— EP0 统一提交 ——————————————————————————

/**
 * @brief EP0 DATA IN，失败时 STALL
 */
bool UsbDevPort::SubmitDataIn(const uint8_t* data, uint16_t len)
{
    ep0_stage_ = Ep0Stage::DataIn;

    if (!hal_->Ep0StartIn(data, len)) 
    {
        LOG_ERR("SubmitDataIn(len=%u) failed", len);
        ep0_stage_ = Ep0Stage::Idle;
        hal_->EpStall(0x00, true);
        return false;
    }
    return true;
}

/**
 * @brief EP0 DATA OUT，失败时 STALL
 */
bool UsbDevPort::SubmitDataOut(uint8_t* data, uint16_t len)
{
    ep0_stage_ = Ep0Stage::DataOut;

    if (!hal_->Ep0StartOut(data, len)) 
    {
        LOG_ERR("SubmitDataOut(len=%u) failed", len);
        ep0_stage_ = Ep0Stage::Idle;
        hal_->EpStall(0x00, true);
        return false;
    }
    return true;
}

/**
 * @brief EP0 STATUS IN，失败时 STALL
 */
bool UsbDevPort::SubmitStatusIn()
{
    ep0_stage_ = Ep0Stage::StatusIn;

    if (!hal_->Ep0StatusIn()) 
    {
        LOG_ERR("SubmitStatusIn failed");
        ep0_stage_ = Ep0Stage::Idle;
        hal_->EpStall(0x00, true);
        return false;
    }
    return true;
}

/**
 * @brief EP0 STATUS OUT，失败时 STALL
 */
bool UsbDevPort::SubmitStatusOut()
{
    ep0_stage_ = Ep0Stage::StatusOut;

    if (!hal_->Ep0StatusOut()) 
    {
        LOG_ERR("SubmitStatusOut failed");
        ep0_stage_ = Ep0Stage::Idle;
        hal_->EpStall(0x00, true);
        return false;
    }
    return true;
}

//  —————————————————————————— 传输完成 ——————————————————————————

/**
 * @brief 端点传输完成 — EP0 状态机推进/非 EP0 分发功能类
 */
void UsbDevPort::HandleTransferComplete(const UsbHal::Event& event)
{
    if (event.endpoint == kEpDirOut || event.endpoint == kEpDirIn)
    {
        switch (ep0_stage_)
        {
            case Ep0Stage::DataIn:
            {
                if (event.error) {
                    ep0_stage_ = Ep0Stage::Idle;
                    hal_->EpStall(0x00, true);
                    break;
                }
                SubmitStatusOut();
                break;
            }
            case Ep0Stage::DataOut:
            {
                if (event.error || event.data == nullptr || event.length == 0) {
                    ep0_stage_ = Ep0Stage::Idle;
                    hal_->EpStall(0x00, true);
                    break;
                }
                if (!cdc_acm_.CompleteControlOut(class_setup_, event.data, event.length)) {
                    ep0_stage_ = Ep0Stage::Idle;
                    hal_->EpStall(0x00, true);
                    break;
                }
                SubmitStatusIn();
                break;
            }
            case Ep0Stage::StatusIn:
            case Ep0Stage::StatusOut:
            {
                if (event.error) {
                    LOG_ERR("EP0 STATUS error");
                    hal_->EpStall(0x00, true);
                }
                ep0_stage_ = Ep0Stage::Idle;
                break;
            }
            default:
                break;
        }
    } else {
        cdc_acm_.OnEndpointComplete(event.endpoint, event.data, event.length, event.error);

        if (event.error) {
            if ((event.endpoint & 0x80) == 0) {
                // OUT 错误 → 恢复 RX 端点
                RecoverRxEndpoint(event.endpoint);
            } else {
                // IN 错误 → 只重开端点，不提交 RX
                hal_->EpStall(event.endpoint, false);
                hal_->EpClose(event.endpoint);
                EndpointConfig ep_cfg {event.endpoint, EndpointType::Bulk,
                                       cdc_acm_.GetBulkMps(), 0};
                if (!hal_->EpOpen(ep_cfg)) {
                    LOG_ERR("IN ep 0x%02x reopen failed", event.endpoint);
                }
            }
        } else if (event.endpoint == cdc_acm_.GetBulkOutEp() && state_ == DeviceState::Configured) {
            RequeueRx(event.endpoint);
        }
    }
}

//  —————————————————————————— 端口生命周期 ——————————————————————————

/**
 * @brief 打开 CDC 三端点 + 启动首轮 RX
 *
 * 从 CDC 查询端点地址和 MPS，依次打开通知 IN、批量 OUT、批量 IN，
 * 最后对批量 OUT 提交首轮接收。
 *
 * @return true = 全部成功，false = 任一 EpOpen 或首轮 RX 失败
 */
bool UsbDevPort::SetupCdcEndpoints()
{
    uint8_t out_ep = cdc_acm_.GetBulkOutEp();
    uint16_t mps   = cdc_acm_.GetBulkMps();

    EndpointConfig int_ep { cdc_acm_.GetNotificationEp(), EndpointType::Interrupt,
                            cdc_acm_.GetNotificationMps(), cdc_acm_.GetNotificationInterval() };
    EndpointConfig out_ep_cfg { out_ep, EndpointType::Bulk, mps, 0 };
    EndpointConfig in_ep_cfg  { cdc_acm_.GetBulkInEp(), EndpointType::Bulk, mps, 0 };

    bool int_open = hal_->EpOpen(int_ep);
    bool out_open = int_open && hal_->EpOpen(out_ep_cfg);
    bool in_open  = out_open && hal_->EpOpen(in_ep_cfg);

    if (!int_open || !out_open || !in_open) {
        LOG_ERR("CDC endpoint open failed");
        if (in_open)  hal_->EpClose(in_ep_cfg.address);
        if (out_open) hal_->EpClose(out_ep_cfg.address);
        if (int_open) hal_->EpClose(int_ep.address);
        return false;
    }

    if (!hal_->EpStartRx(out_ep, mps)) {
        LOG_ERR("First EpStartRx failed");
        hal_->EpClose(in_ep_cfg.address);
        hal_->EpClose(out_ep_cfg.address);
        hal_->EpClose(int_ep.address);
        return false;
    }
    return true;
}

/**
 * @brief 批量 OUT 重提 RX
 *
 * 每次批量数据到达并转发后调用，为下一次接收做准备。
 * 失败时只打日志，不 STALL——硬件可能短暂繁忙。
 *
 * @param ep  批量 OUT 端点号
 */
void UsbDevPort::RequeueRx(uint8_t ep)
{
    if (!hal_->EpStartRx(ep, cdc_acm_.GetBulkMps())) {
        LOG_ERR("RX re-submit failed on ep 0x%02x, recovering", ep);
        RecoverRxEndpoint(ep);
    }
}

/**
 * @brief 批量 OUT 错误恢复
 *
 * 流程：STALL 清除 → 关端点 → 开端点 → 重提 RX。
 * 任一环节失败都只打日志，不继续传播——USB 协议层会重试。
 *
 * @param ep  批量 OUT 端点号
 */
void UsbDevPort::RecoverRxEndpoint(uint8_t ep)
{
    LOG_ERR("RX error on ep 0x%02x, recovering", ep);
    hal_->EpStall(ep, false);
    hal_->EpClose(ep);

    EndpointConfig ep_cfg {ep, EndpointType::Bulk, cdc_acm_.GetBulkMps(), 0};
    if (hal_->EpOpen(ep_cfg)) {
        if (!hal_->EpStartRx(ep, cdc_acm_.GetBulkMps())) {
            LOG_ERR("RX recovery submit failed on ep 0x%02x", ep);
        }
    } else {
        LOG_ERR("RX recovery failed on ep 0x%02x", ep);
    }
}

void UsbDevPort::Stop()
{
    if (!ready_) {
        return;
    }

    CloseCdcEndpoints();
    cdc_acm_.OnConfigured(false, speed_);
    ResetState();
    hal_->Disconnect();
}
