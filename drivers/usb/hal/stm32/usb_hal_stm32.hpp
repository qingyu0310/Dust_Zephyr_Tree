/**
 * @file usb_hal_stm32.hpp
 * @author qingyu
 * @brief STM32 OTG FS USB 硬件抽象层
 * @version 0.3
 * @date 2026-08-14
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "usb_hal.hpp"

/**
 * @brief STM32 OTG FS USB 硬件抽象层
 *
 * 设计约束：
 *   - 无 DMA 传输（dma_enable=DISABLE），HAL 中断搬运 FIFO，收发缓冲无需 nocache
 *   - PCD 句柄为 .cpp 文件级 static（同 HPM s_handle 模式），类成员只放普通状态
 *   - ISR trampoline 使用 stm32_usb_isr(arg) → static_cast<UsbHalStm32*>(arg)->Isr()
 *   - HAL PCD 弱回调经 PCD_HandleTypeDef.pData 反查实例（见 usb_hal_stm32.cpp）
 */
class UsbHalStm32 final : public UsbHal
{
public:
    UsbHalStm32() = default;

    bool Init(const Config& cfg, EventCallback callback, void* context) override;

    bool Connect()      override;
    void Disconnect()   override;
    bool SetAddress(uint8_t address) override;
    Speed GetSpeed() const override;

    bool EpOpen(const EndpointConfig& cfg) override;
    bool EpClose(uint8_t endpoint) override;
    bool EpStall(uint8_t endpoint, bool stall) override;
    bool EpStartRx(uint8_t endpoint, uint16_t length) override;
    bool EpStartTx(uint8_t endpoint, const uint8_t* data, uint16_t length) override;
    bool Ep0StartIn(const uint8_t* data, uint16_t length) override;
    bool Ep0StartOut(uint8_t* data, uint16_t length) override;
    bool Ep0StatusIn() override;
    bool Ep0StatusOut() override;

    // 中断与 HAL 回调入口（extern "C" 自由回调函数经这些 public 方法访问私有状态）
    void Isr();
    void OnBusReset();
    void OnDataIn(uint8_t epnum);               // EP0 IN 分包续传 + 上报 TransferComplete
    void Notify(const Event& event);

    // DataOut 回调取接收数据基址：F4 slave（非 DMA）模式 HAL 在 RXFLVL 处理时
    // 已把 xfer_buff 前移到数据末尾，数据指针必须用 EpStartRx 武装时记录的缓冲基址
    const uint8_t* GetRxBufBase(uint8_t epnum) const { return out_ep_[epnum & 0x0F].buf; }

private:
    static constexpr int kNumEps = 4;           // OTG FS 双向端点数（dev_endpoints=4）

    // 端点状态（无 DMA，普通 RAM）
    struct EpState {
        uint8_t* buf    = nullptr;              // 传输缓冲指针
        uint16_t len    = 0;                    // 传输长度
        uint16_t mps    = 0;                    // 端点 MPS（bulk IN 满包 ZLP 判定用）
        bool     enable = false;                // 端点使能
    };

    EpState in_ep_[kNumEps] {};                 // IN 端点状态表
    EpState out_ep_[kNumEps] {};                // OUT 端点状态表

    Config         cfg_      {};                // 配置副本
    EventCallback  callback_ = nullptr;         // 事件回调
    void*          context_  = nullptr;         // 回调上下文
    bool           ready_    = false;           // Init 完成

    uint8_t rx_buf_[2][UsbHal::kRxBufSize] {};  // OUT 双缓冲（轮换）
    uint8_t tx_buf_[UsbHal::kTxBufSize] {};     // IN 发送拷贝缓冲
    uint8_t rx_ping_ = 0;                       // RX 双缓冲轮换索引

    // EP0 IN 分包状态（F4 LL USB_EPStartXfer 对 EP0 单次传输硬截断为 MPS=64B，超长需续传）
    uint16_t       ep0_tx_rem_ = 0;             // EP0 IN 剩余字节
    const uint8_t* ep0_tx_ptr_ = nullptr;       // EP0 IN 剩余数据指针

    bool bulk_zlp_pending_ = false;             // OnDataIn 已自动补发 bulk IN ZLP（上层重复请求吞掉）
};
