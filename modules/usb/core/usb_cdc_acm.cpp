/**
 * @file usb_cdc_acm.cpp
 * @author qingyu
 * @brief CDC ACM 协议实现
 * @version 0.1
 * @date 2026-07-27
 *
 * @copyright Copyright (c) 2026
 */

#include "usb_cdc_acm.hpp"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_cdc_acm, LOG_LEVEL_INF);

static constexpr uint8_t  kCdcReqSetLineCoding        = 0x20;    // 设置线编码
static constexpr uint8_t  kCdcReqGetLineCoding        = 0x21;    // 获取线编码
static constexpr uint8_t  kCdcReqSetControlLineState  = 0x22;    // 设置控制线状态
static constexpr uint8_t  kCdcReqSendBreak            = 0x23;    // 发送 BREAK
static constexpr uint16_t kLineCodingLen              = 7;       // 线编码固定长度

// bmRequestType 组合值：H2D + Class + Interface
static constexpr uint8_t kCdcBmHostToDev = kDirectionHostToDevice | kTypeClass | kRecipientInterface;
// bmRequestType 组合值：D2H + Class + Interface
static constexpr uint8_t kCdcBmDevToHost = kDirectionDeviceToHost | kTypeClass | kRecipientInterface;

/**
 * @brief 初始化
 * @param cfg  CDC 配置（Init 时拷贝，自身持有）
 * @return true=成功
 */
bool UsbCdcAcm::Init(const UsbCdcAcmConfig& cfg)
{
    if (ready_) {
        return true;
    }

    auto ep_num = [](uint8_t ep) { return ep & 0x0F; };
    
    if (!(cfg.notification_ep & 0x80)) { LOG_ERR("notification not IN");    return false; }
    if (!(cfg.bulk_in_addr    & 0x80)) { LOG_ERR("bulk_in not IN");         return false; }
    if (  cfg.bulk_out_addr   & 0x80)  { LOG_ERR("bulk_out not OUT");       return false; }

    if (ep_num(cfg.notification_ep) == 0 || ep_num(cfg.notification_ep) > 15)   { LOG_ERR("bad notification ep num");   return false; }
    if (ep_num(cfg.bulk_out_addr)   == 0 || ep_num(cfg.bulk_out_addr)   > 15)   { LOG_ERR("bad bulk_out ep num");       return false; }
    if (ep_num(cfg.bulk_in_addr)    == 0 || ep_num(cfg.bulk_in_addr)    > 15)   { LOG_ERR("bad bulk_in ep num");        return false; }

    if (cfg.notification_ep == cfg.bulk_in_addr)     { LOG_ERR("IN eps collide");           return false; }
    if (cfg.notification_ep == cfg.bulk_out_addr)    { LOG_ERR("notification=bulk_out");    return false; }
    if (cfg.bulk_in_addr    == cfg.bulk_out_addr)    { LOG_ERR("bulk IN=OUT");              return false; }
    if (cfg.control_interface == cfg.data_interface) { LOG_ERR("interfaces same");          return false; }
    if (cfg.notification_mps == 0)                   { LOG_ERR("notification_mps=0");       return false; }
    if (cfg.notification_interval == 0)              { LOG_ERR("notification_interval=0");  return false; }

    cfg_    = cfg;

    // 用同一份配置创建设描述符集
    desc_set_ = UsbCdcAcmDescriptorSet(cfg);

    // 初始化线编码
    line_coding_.dte_rate    = 115200;
    line_coding_.char_format = 0;
    line_coding_.parity_type = 0;
    line_coding_.data_bits   = 8;

    ready_ = true;
    return true;
}

//  —————————————————————————— 配置状态 ——————————————————————————

/**
 * @brief 配置状态变化通知
 * @param configured  true=已配置，false=取消配置
 * @param speed       USB 速度（UsbDevPort 传入）
 */
bool UsbCdcAcm::OnConfigured(bool configured, Speed speed)
{
    if (configured)
    {
        bulk_mps_ = (speed == Speed::High) ? 512 : 64;

        LOG_INF("CDC configured, speed=%s MPS=%u", (speed == Speed::High) ? "HS" : "FS", bulk_mps_);
        return true;
    }
    else
    {
        dtr_         = false;
        rts_         = false;

        if (cfg_cb_) cfg_cb_(cfg_ctx_, false, 0);
    }

    return true;
}

/**
 * @brief 通知上层配置完成（在 SetConfiguration 后续步骤成功后调用）
 *
 * OnConfigured 先返回 true 让 UsbDevPort 继续开端点，
 * 开端点成功后 UsbDevPort 调用 NotifyConfigured 通知 Usb 层配置就绪。
 */
void UsbCdcAcm::NotifyConfigured()
{
    if (cfg_cb_) {
        cfg_cb_(cfg_ctx_, true, bulk_mps_);
    }
}

/**
 * @brief 端点完成通知
 * @param endpoint  端点号
 * @param data      数据指针
 * @param length    数据长度
 * @param error     传输错误标志
 *
 * 只转发数据给上层回调，不操作硬件。
 * RX 重提和错误恢复由 UsbDevPort 处理。
 */
/**
 * @brief 端点完成通知
 *
 * 错误时也转发上层（data=nullptr, length=0），由 Usb::OnDataEvent 清理 tx_busy_ 等状态。
 * 硬件恢复（重开/重提 RX）由 UsbDevPort 在 HandleTransferComplete 中处理。
 */
void UsbCdcAcm::OnEndpointComplete(uint8_t endpoint, const uint8_t* data, uint16_t length, bool error)
{
    if (data_cb_ != nullptr) {
        if (error) {
            data_cb_(data_ctx_, endpoint, nullptr, 0);
        } else {
            data_cb_(data_ctx_, endpoint, data, length);
        }
    }
}

//  —————————————————————————— CDC 类请求处理 ——————————————————————————

/**
 * @brief 处理 CDC 类请求
 */
bool UsbCdcAcm::HandleClassRequest(const SetupPacket& setup, uint8_t* data, uint16_t& length)
{
    switch (setup.b_request)
    {
        case kCdcReqSetLineCoding:
            return OnSetLineCoding(setup, data, length);
        case kCdcReqGetLineCoding:
            return OnGetLineCoding(setup, data, length);
        case kCdcReqSetControlLineState:
            return OnSetControlLineState(setup, data, length);
        case kCdcReqSendBreak:
            return OnSendBreak(setup, data, length);

        default:
            return false;
    }
}

/**
 * @brief 处理 SET_LINE_CODING 请求
 */
bool UsbCdcAcm::OnSetLineCoding(const SetupPacket& setup, uint8_t* data, uint16_t& length)
{
    if (setup.bm_request_type != kCdcBmHostToDev) return false;
    if (setup.w_index != cfg_.control_interface) return false;
    if (setup.w_length != kLineCodingLen) return false;
    length = kLineCodingLen;

    return true;
}

/**
 * @brief 处理 GET_LINE_CODING 请求
 */
bool UsbCdcAcm::OnGetLineCoding(const SetupPacket& setup, uint8_t* data, uint16_t& length)
{
    if (setup.bm_request_type != kCdcBmDevToHost) return false;
    if (setup.w_index != cfg_.control_interface) return false;
    if (setup.w_length != kLineCodingLen) return false;

    EncodeLineCoding(line_coding_, data);
    length = kLineCodingLen;

    return true;
}

/**
 * @brief 处理 SET_CONTROL_LINE_STATE 请求
 */
bool UsbCdcAcm::OnSetControlLineState(const SetupPacket& setup, uint8_t* data, uint16_t& length)
{
    (void)data;
    if (setup.bm_request_type != kCdcBmHostToDev)   return false;
    if (setup.w_index != cfg_.control_interface)    return false;
    if (setup.w_length != 0)                        return false;
    if (setup.w_value & ~0x03)                      return false;      // 只允许 D0(DTR) / D1(RTS)

    dtr_ = (setup.w_value & 0x01) != 0;
    rts_ = (setup.w_value & 0x02) != 0;
    LOG_INF("DTR=%d RTS=%d", dtr_, rts_);
    length = 0;

    return true;
}

/**
 * @brief 处理 SEND_BREAK 请求
 */
bool UsbCdcAcm::OnSendBreak(const SetupPacket& setup, uint8_t* data, uint16_t& length)
{
    (void)data;
    if (setup.bm_request_type != kCdcBmHostToDev)   return false;
    if (setup.w_index != cfg_.control_interface)    return false;
    if (setup.w_length != 0)                        return false;

    LOG_INF("BREAK value=%u", setup.w_value);
    length = 0;

    return true;
}

/**
 * @brief EP0 DATA OUT 完成（冷路径：SET_LINE_CODING 将数据写入线编码）
 */
bool UsbCdcAcm::CompleteControlOut(const SetupPacket& setup, const uint8_t* data, uint16_t length)
{
    if (setup.bm_request_type == kCdcBmHostToDev && setup.b_request == kCdcReqSetLineCoding && setup.w_index == cfg_.control_interface &&
        setup.w_length == kLineCodingLen && length == kLineCodingLen && data != nullptr && DecodeLineCoding(data, line_coding_)) 
    {
        LOG_INF("line_coding updated: rate=%u", line_coding_.dte_rate);
        return true;
    }
    return false;
}

//  —————————————————————————— 描述符查询 ——————————————————————————

/**
 * @brief 获取描述符
 */
const uint8_t* UsbCdcAcm::GetDescriptor(DescriptorType type, Speed speed, uint8_t index, uint16_t& length) const
{
    switch (type)
    {
        case DescriptorType::Device:
            return desc_set_.GetDeviceDescriptor(length);
        case DescriptorType::Configuration:
            return desc_set_.GetConfigurationDescriptor(speed, length);
        case DescriptorType::DeviceQualifier:
            return desc_set_.GetQualifierDescriptor(length);
        case DescriptorType::OtherSpeedConfiguration:
            return desc_set_.GetOtherSpeedDescriptor(speed, length);
        case DescriptorType::String:
            return desc_set_.GetStringDescriptor(index, length);
            
        default:
            length = 0;
            return nullptr;
    }
}
