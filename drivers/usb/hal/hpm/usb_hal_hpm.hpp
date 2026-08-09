/**
 * @file usb_hal_hpm.hpp
 * @author qingyu
 * @brief HPMicro EHCI USB 硬件抽象层
 * @version 0.1
 * @date 2026-07-27
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>
#include "usb_hal.hpp"

/**
 * @brief HPMicro USB 硬件抽象层
 *
 * 设计约束：
 *   - dcd_data/handle/tx_buf 必须位于 .nocache 段（通过文件级静态实现）
 *   - 端点状态（ep_state_）可以位于普通内存
 *   - ISR trampoline 使用 usb_isr_entry(arg) → static_cast<UsbHalHpm*>(arg)->Isr()
 */
class UsbHalHpm final : public UsbHal
{
public:
    UsbHalHpm() = default;

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

    void Isr();

private:
    static constexpr int kNumEps = 16;

    // 端点状态（无 .nocache 要求）
    struct EpState {
        uint8_t* buf    = nullptr;      // DMA 缓冲指针
        uint16_t len    = 0;            // 传输长度
        bool     enable = false;        // 端点使能
    };
    
    EpState in_ep_[kNumEps] {};         // IN 端点状态表
    EpState out_ep_[kNumEps] {};        // OUT 端点状态表

    Config cfg_ {};                     // 配置副本（用于回滚）

    EventCallback callback_ = nullptr;  // 事件回调
    void*         context_  = nullptr;  // 回调上下文
    uint32_t      reg_base_ = 0;        // 控制器寄存器基址

    bool ready_ = false;                // Init 完成

    void InitClockAndPhy();
    void HandleReset();
    void HandleSetupReceived();
    void HandleTransferComplete(uint32_t edpt_complete);
    uint32_t CalcTransferLength(uint8_t ep_idx, bool* error = nullptr);
    void Rollback();                    // Init 失败后回滚
};
