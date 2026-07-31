/**
 * @file usb_descriptor.cpp
 * @author qingyu
 * @brief USB 描述符生成实现
 * @version 0.1
 * @date 2026-07-27
 *
 * 所有描述符数据手动填入 uint8_t 数组，
 * 不使用 struct packing、不使用 CherryUSB 宏。
 * 避免编译器 padding、大小端、不同 MCU ABI 差异。
 *
 * @copyright Copyright (c) 2026
 */

#include "usb_descriptor.hpp"
#include <cstdint>
#include <string.h>

//  —————————————————————————— 构造 ——————————————————————————

/**
 * @brief 构造并生成所有描述符
 * @param cfg  描述符配置
 */
UsbCdcAcmDescriptorSet::UsbCdcAcmDescriptorSet(const UsbCdcAcmConfig& cfg) : cfg_(cfg)
{
    BuildDevice(cfg.vid, cfg.pid, cfg.bcd_device, 1, 2, 3);
    BuildConfig(config_fs_, 64);
    BuildConfig(config_hs_, 512);
    BuildQualifier();
    BuildOtherSpeed(other_speed_fs_, 64);
    BuildOtherSpeed(other_speed_hs_, 512);
    BuildStrings(cfg);
}

//  —————————————————————————— 构建器 ——————————————————————————

/**
 * @brief 构建配置描述符（含 CDC ACM 复合描述符）
 * @param buf  输出缓冲区
 * @param mps  批量端点最大包大小（FS=64, HS=512）
 */
void UsbCdcAcmDescriptorSet::BuildConfig(uint8_t* buf, uint16_t mps)
{
    uint16_t total_len = kUsbConfigTotalLen;

    buf[0] = 9;                                                   // 描述符长度
    buf[1] = 2;                                                   // 描述符类型 = 配置
    buf[2] = static_cast<uint8_t>(total_len & 0xFF);              // 总长度 L
    buf[3] = static_cast<uint8_t>((total_len >> 8) & 0xFF);       // 总长度 H
    buf[4] = 2;                                                   // 接口数量
    buf[5] = 1;                                                   // 配置值
    buf[6] = 0;                                                   // 配置字符串索引
    buf[7] = 0x80;                                                // 属性（自供电）
    buf[8] = 100;                                                 // 最大功耗（200mA/2）

    uint16_t pos = 9;

    // IAD（接口关联描述符）
    buf[pos + 0] = 8;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 11;                                pos += 1;   // 描述符类型 = IAD
    buf[pos + 0] = cfg_.control_interface;            pos += 1;   // 首个接口号
    buf[pos + 0] = 2;                                 pos += 1;   // 接口数量
    buf[pos + 0] = 2;                                 pos += 1;   // 功能类 = CDC
    buf[pos + 0] = 2;                                 pos += 1;   // 功能子类 = ACM
    buf[pos + 0] = 0;                                 pos += 1;   // 功能协议
    buf[pos + 0] = 0;                                 pos += 1;   // 功能字符串索引

    // CDC 控制接口
    buf[pos + 0] = 9;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 4;                                 pos += 1;   // 描述符类型 = 接口
    buf[pos + 0] = cfg_.control_interface;             pos += 1;   // 接口号
    buf[pos + 0] = 0;                                 pos += 1;   // 备用设置
    buf[pos + 0] = 1;                                 pos += 1;   // 端点数量
    buf[pos + 0] = 2;                                 pos += 1;   // 接口类 = CDC
    buf[pos + 0] = 2;                                 pos += 1;   // 接口子类 = ACM
    buf[pos + 0] = 0;                                 pos += 1;   // 接口协议
    buf[pos + 0] = 0;                                 pos += 1;   // 接口字符串索引

    // 头功能描述符
    buf[pos + 0] = 5;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 0x24;                              pos += 1;   // CS_INTERFACE
    buf[pos + 0] = 0;                                 pos += 1;   // 子类型 = 头
    buf[pos + 0] = 0x10;                              pos += 1;   // CDC 版本 L
    buf[pos + 0] = 0x01;                              pos += 1;   // CDC 版本 H = 1.10

    // 呼叫管理功能描述符
    buf[pos + 0] = 5;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 0x24;                              pos += 1;   // CS_INTERFACE
    buf[pos + 0] = 1;                                 pos += 1;   // 子类型 = 呼叫管理
    buf[pos + 0] = 0x00;                              pos += 1;   // 能力
    buf[pos + 0] = cfg_.data_interface;               pos += 1;   // 数据接口号

    // 抽象控制管理描述符
    buf[pos + 0] = 4;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 0x24;                              pos += 1;   // CS_INTERFACE
    buf[pos + 0] = 2;                                 pos += 1;   // 子类型 = 抽象控制管理
    buf[pos + 0] = 0x06;                              pos += 1;   // 能力

    // 联合功能描述符
    buf[pos + 0] = 5;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 0x24;                              pos += 1;   // CS_INTERFACE
    buf[pos + 0] = 6;                                 pos += 1;   // 子类型 = 联合
    buf[pos + 0] = cfg_.control_interface;             pos += 1;   // 主接口
    buf[pos + 0] = cfg_.data_interface;               pos += 1;   // 从接口

    // 中断 IN 端点
    buf[pos + 0] = 7;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 5;                                 pos += 1;   // 描述符类型 = 端点
    buf[pos + 0] = cfg_.notification_ep;              pos += 1;   // 端点地址
    buf[pos + 0] = 3;                                 pos += 1;   // 属性 = 中断
    buf[pos + 0] = cfg_.notification_mps;             pos += 1;   // 最大包大小 L
    buf[pos + 0] = 0;                                 pos += 1;   // 最大包大小 H
    buf[pos + 0] = cfg_.notification_interval;        pos += 1;   // 轮询间隔

    // 数据接口
    buf[pos + 0] = 9;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 4;                                 pos += 1;   // 描述符类型 = 接口
    buf[pos + 0] = cfg_.data_interface;               pos += 1;   // 接口号
    buf[pos + 0] = 0;                                 pos += 1;   // 备用设置
    buf[pos + 0] = 2;                                 pos += 1;   // 端点数量
    buf[pos + 0] = 0x0A;                              pos += 1;   // 接口类 = 数据
    buf[pos + 0] = 0;                                 pos += 1;   // 接口子类
    buf[pos + 0] = 0;                                 pos += 1;   // 接口协议
    buf[pos + 0] = 0;                                 pos += 1;   // 接口字符串索引

    // 批量 OUT 端点
    buf[pos + 0] = 7;                                 pos += 1;   // 描述符长度
    buf[pos + 0] = 5;                                 pos += 1;   // 描述符类型 = 端点
    buf[pos + 0] = cfg_.bulk_out_addr;                pos += 1;   // 端点地址
    buf[pos + 0] = 2;                                 pos += 1;   // 属性 = 批量
    buf[pos + 0] = static_cast<uint8_t>(mps & 0xFF);  pos += 1;   // 最大包大小 L
    buf[pos + 0] = static_cast<uint8_t>((mps >> 8) & 0xFF);  pos += 1;   // 最大包大小 H
    buf[pos + 0] = 0;                                 pos += 1;   // 轮询间隔

    // 批量 IN 端点
    buf[pos + 0] = 7;                                 pos += 1;             // 描述符长度
    buf[pos + 0] = 5;                                 pos += 1;             // 描述符类型 = 端点
    buf[pos + 0] = cfg_.bulk_in_addr;                 pos += 1;             // 端点地址
    buf[pos + 0] = 2;                                 pos += 1;             // 属性 = 批量
    buf[pos + 0] = static_cast<uint8_t>(mps & 0xFF);  pos += 1;             // 最大包大小 L
    buf[pos + 0] = static_cast<uint8_t>((mps >> 8) & 0xFF);  pos += 1;      // 最大包大小 H
    buf[pos + 0] = 0;                                 pos += 1;             // 轮询间隔
}

/**
 * @brief 构建设备描述符（18 字节）
 */
void UsbCdcAcmDescriptorSet::BuildDevice(uint16_t vid, uint16_t pid, uint16_t bcd_device, uint8_t mfr_idx, uint8_t prod_idx, uint8_t ser_idx)
{
    uint8_t d[18] {};
    d[0]  = 18;                                                 // 描述符长度
    d[1]  = 1;                                                  // 描述符类型 = 设备
    d[2]  = 0x00;                                               // USB 版本 L
    d[3]  = 0x02;                                               // USB 版本 H = 0x0200
    d[4]  = 0xEF;                                               // 设备类 = MISC
    d[5]  = 2;                                                  // 设备子类
    d[6]  = 1;                                                  // 设备协议
    d[7]  = 64;                                                 // 端点 0 最大包大小
    d[8]  = static_cast<uint8_t> (vid & 0xFF);                  // 厂商 ID L
    d[9]  = static_cast<uint8_t>((vid >> 8)  & 0xFF);           // 厂商 ID H
    d[10] = static_cast<uint8_t> (pid & 0xFF);                  // 产品 ID L
    d[11] = static_cast<uint8_t>((pid >> 8)  & 0xFF);           // 产品 ID H
    d[12] = static_cast<uint8_t> (bcd_device & 0xFF);           // 设备版本 L
    d[13] = static_cast<uint8_t>((bcd_device >> 8) & 0xFF);     // 设备版本 H
    d[14] = mfr_idx;                                            // 厂商字符串索引
    d[15] = prod_idx;                                           // 产品字符串索引
    d[16] = ser_idx;                                            // 序列号字符串索引
    d[17] = 1;                                                  // 配置数量
    memcpy(device_desc_, d, sizeof(d));
}

/**
 * @brief 构建设备限定符描述符（10 字节）
 */
void UsbCdcAcmDescriptorSet::BuildQualifier()
{
    uint8_t d[10] {};
    d[0] = 10;                          // 描述符长度
    d[1] = 6;                           // 描述符类型 = 设备限定符
    d[2] = 0x00;                        // USB 版本 L
    d[3] = 0x02;                        // USB 版本 H = 0x0200
    d[4] = 0xEF;                        // 设备类
    d[5] = 2;                           // 设备子类
    d[6] = 1;                           // 设备协议
    d[7] = 64;                          // 端点 0 最大包大小
    d[8] = 1;                           // 配置数量
    d[9] = 0;                           // 保留
    memcpy(qualifier_, d, sizeof(d));
}

/**
 * @brief 构建其他速度配置描述符
 * @param buf  输出缓冲区
 * @param mps  另一速度下的批量端点 MPS
 */
void UsbCdcAcmDescriptorSet::BuildOtherSpeed(uint8_t* buf, uint16_t mps)
{
    BuildConfig(buf, mps);
    buf[1] = 7;                         // 描述符类型 = 其他速度配置
}

/**
 * @brief 构建所有字符串描述符（语言 ID + 厂商 + 产品 + 序列号）
 * @param cfg  描述符配置
 */
void UsbCdcAcmDescriptorSet::BuildStrings(const UsbCdcAcmConfig& cfg)
{
    string_desc_[0][0] = 4;             // 描述符长度
    string_desc_[0][1] = 3;             // 描述符类型 = 字符串
    string_desc_[0][2] = 0x09;          // 语言 ID L = English
    string_desc_[0][3] = 0x04;          // 语言 ID H = US
    string_count_ = 1;
    BuildString(1, cfg.manufacturer);
    BuildString(2, cfg.product);
    BuildString(3, cfg.serial_number);
}

/**
 * @brief 构建单个字符串描述符（UTF-16LE）
 */
void UsbCdcAcmDescriptorSet::BuildString(uint8_t index, const char* text)
{
    if (index >= 4 || text == nullptr) return;
    WriteAsciiUtf16le(string_desc_[index], text);

    if (index + 1 > string_count_) {
        string_count_ = static_cast<uint8_t>(index + 1);
    }
}

/**
 * @brief 将 ASCII 字符串写入 UTF-16LE 描述符格式
 */
uint16_t UsbCdcAcmDescriptorSet::WriteAsciiUtf16le(uint8_t* buf, const char* text)
{
    if (buf == nullptr || text == nullptr) return 0;
    uint32_t ascii_len = strlen(text);

    constexpr uint32_t kMaxAsciiLen = (kMaxStringLen - 2) / 2;
    if (ascii_len > kMaxAsciiLen) ascii_len = kMaxAsciiLen;

    uint16_t total_len = static_cast<uint16_t>(2 + ascii_len * 2);
    buf[0] = static_cast<uint8_t>(total_len);
    buf[1] = 0x03;  // 描述符类型 = 字符串

    for (uint32_t i = 0; i < ascii_len; i++) {
        buf[2 + i * 2]     = static_cast<uint8_t>(text[i]);
        buf[2 + i * 2 + 1] = 0x00;
    }
    return total_len;
}
