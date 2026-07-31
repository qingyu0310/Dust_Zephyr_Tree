/**
 * @file usb_types.hpp
 * @author qingyu
 * @brief USB 协议基础类型，不依赖任何硬件和协议栈
 * @version 0.1
 * @date 2026-07-27
 *
 * 只放 USB 规范定义的数据类型和小型工具函数。
 * 不放控制器寄存器、不放描述符生成逻辑、不放任何 CherryUSB 引用。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

/**
 * @brief USB 速度
 */
enum class Speed : uint8_t {
    Full = 0,                               // 全速（12 Mbps）
    High = 1,                               // 高速（480 Mbps）
};

enum class EndpointType : uint8_t {
    Control     = 0,                        // 控制传输
    Isochronous = 1,                        // 等时传输
    Bulk        = 2,                        // 批量传输
    Interrupt   = 3,                        // 中断传输
};

enum class DeviceState : uint8_t {
    Default,                                // 默认/复位后
    Addressed,                              // 已分配地址
    Configured,                             // 已配置
};

enum class DescriptorType : uint8_t {
    Device                   = 1,           // 设备描述符
    Configuration            = 2,           // 配置描述符
    String                   = 3,           // 字符串描述符
    Interface                = 4,           // 接口描述符
    Endpoint                 = 5,           // 端点描述符
    DeviceQualifier          = 6,           // 设备限定符（USB 2.0 HS）
    OtherSpeedConfiguration  = 7,           // 其他速度配置
};

enum class StandardRequest : uint8_t {
    GetStatus                = 0,           // 获取状态
    ClearFeature             = 1,           // 清除特性
    SetFeature               = 3,           // 设置特性
    SetAddress               = 5,           // 设置地址
    GetDescriptor            = 6,           // 获取描述符
    GetConfiguration         = 8,           // 获取配置
    SetConfiguration         = 9,           // 设置配置
    GetInterface             = 10,          // 获取接口
    SetInterface             = 11,          // 设置接口
};

enum class FeatureSelector : uint8_t {
    EndpointHalt       = 0,                 // 端点停止
    DeviceRemoteWakeup = 1,                 // 远程唤醒
    TestMode           = 2,                 // 测试模式
};

static constexpr uint8_t kDirectionMask             = 0x80;     // bmRequestType 方向掩码
static constexpr uint8_t kDirectionHostToDevice     = 0x00;     // 主机→设备
static constexpr uint8_t kDirectionDeviceToHost     = 0x80;     // 设备→主机

static constexpr uint8_t kTypeMask                  = 0x60;     // bmRequestType 类型掩码
static constexpr uint8_t kTypeStandard              = 0x00;     // 标准请求
static constexpr uint8_t kTypeClass                 = 0x20;     // 类请求
static constexpr uint8_t kTypeVendor                = 0x40;     // 厂商请求

static constexpr uint8_t kRecipientMask             = 0x1F;     // bmRequestType 接收者掩码
static constexpr uint8_t kRecipientDevice           = 0x00;     // 设备
static constexpr uint8_t kRecipientInterface        = 0x01;     // 接口
static constexpr uint8_t kRecipientEndpoint         = 0x02;     // 端点
static constexpr uint8_t kRecipientOther            = 0x03;     // 其他

static constexpr uint8_t kEpDirOut                  = 0x00;     // 端点方向 OUT
static constexpr uint8_t kEpDirIn                   = 0x80;     // 端点方向 IN

/**
 * @brief 端点配置
 */
struct EndpointConfig {
    uint8_t      address            = 0;                    // 端点地址（含方向）
    EndpointType type               = EndpointType::Bulk;   // 传输类型
    uint16_t     max_packet_size    = 64;                   // 最大包大小
    uint8_t      interval           = 0;                    // 轮询间隔
};

struct SetupPacket {
    uint8_t  bm_request_type    = 0;                        // 请求特征
    uint8_t  b_request          = 0;                        // 请求码
    uint16_t w_value            = 0;                        // 值（取决于请求）
    uint16_t w_index            = 0;                        // 索引（取决于请求）
    uint16_t w_length           = 0;                        // 数据阶段长度

    bool IsDeviceToHost() const
    {
        return (bm_request_type & kDirectionMask) == kDirectionDeviceToHost;
    }
    bool IsStandard() const
    {
        return (bm_request_type & kTypeMask) == kTypeStandard;
    }
    bool IsClass() const
    {
        return (bm_request_type & kTypeMask) == kTypeClass;
    }
    bool IsVendor() const
    {
        return (bm_request_type & kTypeMask) == kTypeVendor;
    }
    uint8_t Recipient() const
    {
        return bm_request_type & kRecipientMask;
    }
};

/**
 * @brief CDC ACM 线编码
 *
 * 手工序列化 7 字节，不使用 packed 结构体，
 * 避免编译器 padding 和大小端差异。
 */
struct LineCoding {
    uint32_t dte_rate    = 115200;
    uint8_t  char_format = 0;           // 0=1 stop, 1=1.5, 2=2
    uint8_t  parity_type = 0;           // 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space
    uint8_t  data_bits   = 8;
};

/**
 * @brief 将 LineCoding 序列化为 7 字节 USB 线编码格式（小端）
 * @param value  输入
 * @param out    输出缓冲区（必须 >= 7 字节）
 */
inline void EncodeLineCoding(const LineCoding& value, uint8_t out[7])
{
    // dwDTERate — 小端 uint32_t
    out[0] = static_cast<uint8_t> (value.dte_rate & 0xFF);
    out[1] = static_cast<uint8_t>((value.dte_rate >> 8)  & 0xFF);
    out[2] = static_cast<uint8_t>((value.dte_rate >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((value.dte_rate >> 24) & 0xFF);
    
    out[4] = value.char_format;     // bCharFormat
    out[5] = value.parity_type;     // bParityType
    out[6] = value.data_bits;       // bDataBits
}

/**
 * @brief 将 7 字节 USB 线编码反序列化为 LineCoding
 * @param in   输入缓冲区（必须 >= 7 字节）
 * @param out  输出
 * @return true=成功
 */
inline bool DecodeLineCoding(const uint8_t in[7], LineCoding& out)
{
    if (in == nullptr) {
        return false;
    }

    out.dte_rate     =  static_cast<uint32_t>(in[0])        | (static_cast<uint32_t>(in[1]) << 8) 
                     | (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
    out.char_format  = in[4];
    out.parity_type  = in[5];
    out.data_bits    = in[6];

    return true;
}
