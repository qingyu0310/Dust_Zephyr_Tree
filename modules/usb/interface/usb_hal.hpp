/**
 * @file usb_hal.hpp
 * @author qingyu
 * @brief USB 硬件抽象层接口 — 纯虚基类，不依赖任何平台
 * @version 0.1
 * @date 2026-07-27
 *
 * UsbHal 是 USB device core 调用硬件的唯一接口。
 * 具体 MCU 通过继承此类实现：UsbHalHpm、UsbHalStm32、UsbHalCherry（迁移期）。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "usb_types.hpp"

/**
 * @brief USB 硬件抽象层
 *
 * 职责边界：
 *   - 必须处理：时钟、PHY、device mode、端点、DMA、cache、IRQ
 *   - 不得处理：GET_DESCRIPTOR、SET_CONFIGURATION、CDC 请求、Read/Send
 *
 * HAL 不知道 Usb、CDC、Stream，只负责上报硬件事件。
 */
class UsbHal
{
public:
    /**
     * @brief HAL 事件类型
     */
    enum class EventType : uint8_t {
        Reset,                              // USB 总线复位
        SetupReceived,                      // 收到 SETUP 包
        TransferComplete,                   // 端点传输完成
        Connected,                          // 主机连接
        Disconnected,                       // 主机断开
    };

    /**
     * @brief HAL 事件
     */
    struct Event {
        EventType      type     = EventType::Reset;
        uint8_t        endpoint = 0;                        // 端点号（含方向）
        const uint8_t* data     = nullptr;                  // 数据指针（仅 TransferComplete 有效）
        uint16_t       length   = 0;                        // 数据长度
        bool           error    = false;                    // 传输错误标志
        uint8_t        setup[8] {};                         // SETUP 包（仅 SetupReceived 有效）
        Speed     speed    = Speed::Full;         // 连接速度
    };

    // 事件回调类型
    using EventCallback = void (*)(void* context, const Event& event);

    /**
     * @brief HAL 配置
     */
    struct Config {
        uint8_t  busid    = 0;              // USB 总线号
        uint32_t reg_base = 0;              // 控制器寄存器基址
        uint32_t irq_num      = 0;          // 中断号
        uint32_t irq_priority = 1;          // PLIC 中断优先级（必须 >0，0 表示禁用）
    };

    virtual ~UsbHal() = default;

    /**
     * @brief 初始化 USB 硬件
     * @param cfg       硬件配置
     * @param callback  事件回调
     * @param context   回调上下文（UsbDevPort*）
     * @return true=成功
     */
    virtual bool Init(const Config& cfg, EventCallback callback, void* context) = 0;

    /**
     * @brief 使能 USB 设备连接/attach
     */
    virtual bool Connect() = 0;

    /**
     * @brief 断开 USB
     */
    virtual void Disconnect() = 0;

    /**
     * @brief 设置 USB 设备地址
     * @param address  地址
     */
    virtual bool SetAddress(uint8_t address) = 0;

    /**
     * @brief 获取当前 USB 速度
     */
    virtual Speed GetSpeed() const = 0;

    /**
     * @brief 打开端点
     * @param cfg  端点配置
     */
    virtual bool EpOpen(const EndpointConfig& cfg) = 0;

    /**
     * @brief 关闭端点
     * @param endpoint  端点号（含方向）
     */
    virtual bool EpClose(uint8_t endpoint) = 0;

    /**
     * @brief 设置端点 STALL
     * @param endpoint  端点号（含方向）
     * @param stall     true=STALL, false=清除 STALL
     */
    virtual bool EpStall(uint8_t endpoint, bool stall) = 0;

    /**
     * @brief 启动端点接收（OUT）
     *
     * HAL 内部管理 DMA 缓冲（含 nocache/对齐等平台要求）。
     * 传输完成后通过 Event{TransferComplete, data, length} 回调通知。
     * RX 重提和错误恢复由 CDC 设备核心负责，HAL 不自动重提。
     *
     * @param endpoint  端点号
     * @param length    最大接收长度
     */
    virtual bool EpStartRx(uint8_t endpoint, uint16_t length) = 0;

    /**
     * @brief 启动端点发送（IN）
     * @param endpoint  端点号
     * @param data      发送数据
     * @param length    数据长度
     */
    virtual bool EpStartTx(uint8_t endpoint, const uint8_t* data, uint16_t length) = 0;

    /**
     * @brief EP0 DATA IN 阶段
     * @param data   数据
     * @param length 长度
     */
    virtual bool Ep0StartIn(const uint8_t* data, uint16_t length) = 0;

    /**
     * @brief EP0 DATA OUT 阶段
     * @param data    接收缓冲区
     * @param length  预期长度
     */
    virtual bool Ep0StartOut(uint8_t* data, uint16_t length) = 0;

    /**
     * @brief EP0 STATUS IN（无数据，主机确认收完）
     */
    virtual bool Ep0StatusIn() = 0;

    /**
     * @brief EP0 STATUS OUT（无数据，主机确认发完）
     */
    virtual bool Ep0StatusOut() = 0;
};

/**
 * @brief 获取默认 HAL 实例
 *
 * 由各平台 HAL 实现提供（如 usb_hal_hpm.cpp）。
 * 用户不需要手动创建 HAL 实例。
 */
UsbHal& GetDefaultHal();
