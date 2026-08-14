/**
 * @file usb_hal_stm32.cpp
 * @author qingyu
 * @brief STM32 OTG FS USB 硬件抽象层实现
 * @version 0.5
 * @date 2026-08-14
 *
 * @copyright Copyright (c) 2026
 */

#include "usb_hal_stm32.hpp"

#include <string.h>

#include "log.hpp"

#include <soc.h>                        // STM32F4 HAL（含 stm32f4xx_hal_pcd.h）
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>

// pinctrl 配置（板级 overlay &usbotg_fs 的 pinctrl-0）
PINCTRL_DT_DEFINE(DT_NODELABEL(usbotg_fs));
static const struct pinctrl_dev_config* usb_pcfg = PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(usbotg_fs));

// PCD 句柄（无 DMA，普通 RAM，无需 nocache）
static PCD_HandleTypeDef s_pcd;

extern "C" void stm32_usb_isr(const void* arg)
{
    if (arg) {
        static_cast<UsbHalStm32*>(const_cast<void*>(arg))->Isr();
    }
}

/**
 * @brief Get the Default Hal object
 *
 * @return UsbHal&
 */
UsbHal& GetDefaultHal()
{
    static UsbHalStm32 s_default_hal_;
    return s_default_hal_;
}

// HAL PCD 弱回调（USE_HAL_PCD_REGISTER_CALLBACKS=0，覆盖 weak 函数）

extern "C" void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef* hpcd)
{
    UsbHalStm32* self = static_cast<UsbHalStm32*>(hpcd->pData);
    UsbHal::Event ev {};
    ev.type = UsbHal::EventType::SetupReceived;
    memcpy(ev.setup, hpcd->Setup, 8);
    self->Notify(ev);
}

extern "C" void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef* hpcd, uint8_t epnum)
{
    UsbHalStm32* self = static_cast<UsbHalStm32*>(hpcd->pData);
    self->OnDataIn(epnum);
}

extern "C" void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef* hpcd, uint8_t epnum)
{
    UsbHalStm32* self = static_cast<UsbHalStm32*>(hpcd->pData);
    uint16_t cnt = static_cast<uint16_t>(HAL_PCD_EP_GetRxCount(hpcd, epnum));
    UsbHal::Event ev {};
    ev.type     = UsbHal::EventType::TransferComplete;
    ev.endpoint = epnum;
    ev.length   = cnt;
    // F4 slave（非 DMA）模式：HAL 在 RXFLVL 处理时已把 xfer_buff 前移到数据末尾，
    // 不能用 hpcd->OUT_ep[epnum].xfer_buff 当数据指针；用 EpStartRx 武装时记录的接收缓冲基址。
    ev.data     = self->GetRxBufBase(epnum);
    self->Notify(ev);
}

extern "C" void HAL_PCD_ResetCallback(PCD_HandleTypeDef* hpcd)
{
    static_cast<UsbHalStm32*>(hpcd->pData)->OnBusReset();
}

extern "C" void HAL_PCD_ConnectCallback(PCD_HandleTypeDef* hpcd)
{
    UsbHalStm32* self = static_cast<UsbHalStm32*>(hpcd->pData);
    UsbHal::Event ev {};
    ev.type  = UsbHal::EventType::Connected;
    ev.speed = Speed::Full;
    self->Notify(ev);
}

extern "C" void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef* hpcd)
{
    UsbHalStm32* self = static_cast<UsbHalStm32*>(hpcd->pData);
    UsbHal::Event ev {};
    ev.type = UsbHal::EventType::Disconnected;
    self->Notify(ev);
}


/**
 * @brief 初始化 USB 硬件
 * @param cfg       硬件配置（reg_base/irq_num/irq_priority）
 * @param callback  事件回调
 * @param context   回调上下文（UsbDevPort*）
 * @return true=成功
 */
bool UsbHalStm32::Init(const Config& cfg, EventCallback callback, void* context)
{
    if (ready_)
        return true;

    cfg_      = cfg;
    callback_ = callback;
    context_  = context;

    __HAL_RCC_USB_OTG_FS_CLK_ENABLE();

    if (pinctrl_apply_state(usb_pcfg, PINCTRL_STATE_DEFAULT) < 0) {
        DUST_LOG_ERR("USB pinctrl setup failed");
        return false;
    }

    memset(&s_pcd, 0, sizeof(s_pcd));
    s_pcd.Instance                 = USB_OTG_FS;
    s_pcd.Init.dev_endpoints       = 4;
    s_pcd.Init.speed               = PCD_SPEED_FULL;
    s_pcd.Init.dma_enable          = DISABLE;
    s_pcd.Init.phy_itface          = PCD_PHY_EMBEDDED;
    s_pcd.Init.Sof_enable          = DISABLE;
    s_pcd.Init.low_power_enable    = DISABLE;
    s_pcd.Init.lpm_enable          = DISABLE;
    s_pcd.Init.vbus_sensing_enable = DISABLE;
    s_pcd.Init.use_dedicated_ep1   = DISABLE;

    if (HAL_PCD_Init(&s_pcd) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_Init failed");
        return false;
    }

    s_pcd.pData = this;                     // HAL 弱回调经 pData 反查实例

    HAL_PCDEx_SetRxFiFo(&s_pcd, 0x80);      // RX = 128 words（512B）
    HAL_PCDEx_SetTxFiFo(&s_pcd, 0, 0x40);   // EP0 IN = 64 words（256B）
    HAL_PCDEx_SetTxFiFo(&s_pcd, 1, 0x80);   // EP1 IN = 128 words（512B）CDC 数据

    if (irq_connect_dynamic(cfg.irq_num, cfg.irq_priority, stm32_usb_isr, this, 0) < 0) {
        DUST_LOG_ERR("IRQ connect failed (irq=%u)", cfg.irq_num);
        return false;
    }

    ready_ = true;
    irq_enable(cfg.irq_num);
    DUST_LOG_INF("STM32 USB init done");
    return true;
}

/**
 * @brief USB 中断处理
 *
 * 由 stm32_usb_isr trampoline 调用，HAL 内部完成 OTG 状态机并触发弱回调。
 */
void UsbHalStm32::Isr()
{
    HAL_PCD_IRQHandler(&s_pcd);
}

/**
 * @brief 事件上报
 * @param event  事件
 */
void UsbHalStm32::Notify(const Event& event)
{
    if (callback_) callback_(context_, event);
}

/**
 * @brief USB 总线复位处理
 *
 * 清非 EP0 端点状态，重新打开 EP0 IN/OUT，上报 Reset 事件。
 */
void UsbHalStm32::OnBusReset()
{
    bulk_zlp_pending_ = false;
    for (int i = 1; i < kNumEps; i++) {
        in_ep_[i]  = {};
        out_ep_[i] = {};
    }
    // Reset 事件必须 Open EP0 双向激活（USBAEP）——照 STM32Cube USBD_LL_Reset / CherryUSB usbd_event_reset_handler。
    // F4 规则：端点先 USBAEP 激活，EP_StartXfer 的 EPENA 才生效；不 Open 则 EP0 IN 传输无效（DataIn 传 0 字节）
    HAL_PCD_EP_Open(&s_pcd, 0x00, 64, EP_TYPE_CTRL);
    HAL_PCD_EP_Open(&s_pcd, 0x80, 64, EP_TYPE_CTRL);
    in_ep_[0].enable  = true;
    out_ep_[0].enable = true;

    UsbHal::Event ev {};
    ev.type = UsbHal::EventType::Reset;
    Notify(ev);
}

/**
 * @brief EP0 IN 传输完成处理（含分包续传）
 * @param epnum  端点号
 *
 * F4 OTG 的 EP0 IN 单次传输上限为 MPS(64B)（LL USB_EPStartXfer 对 EP0 硬截断），
 * 超过 64B 的描述符/数据需在此分包续传，全部传完才上报一次 TransferComplete。
 */
void UsbHalStm32::OnDataIn(uint8_t epnum)
{
    if (epnum == 0 && ep0_tx_rem_ > 0) {
        uint16_t chunk = (ep0_tx_rem_ > 64) ? 64 : ep0_tx_rem_;
        EpStartTx(0x80, ep0_tx_ptr_, chunk);
        ep0_tx_ptr_ += chunk;
        ep0_tx_rem_ -= chunk;
        return;   // 分包未完，暂不上报 TransferComplete
    }
    ep0_tx_rem_ = 0;
    ep0_tx_ptr_ = nullptr;

    uint16_t xfer_count = s_pcd.IN_ep[epnum].xfer_count;

    // bulk IN 满包（len 是 MPS 整数倍）→ 在完成回调内同步补发 ZLP 收尾，
    // 对齐 STM32Cube USBD_LL_DataInStage / CherryUSB in_complete_handler：
    // 不等上层 OnBulkIn（Notify 转发链路）再发，缩短主机等 ZLP 的窗口，避免超时复位打断。
    // 上层 OnBulkIn 的重复 ZLP 请求由 EpStartTx 的 bulk_zlp_pending_ 吞掉，不会重复提交。
    if (epnum != 0 && xfer_count != 0 && in_ep_[epnum].mps != 0 &&
        (xfer_count % in_ep_[epnum].mps) == 0) {
        bulk_zlp_pending_ = false;   // 先清残留，再补发（EpStartTx 提交成功后才重新置位）
        if (EpStartTx(static_cast<uint8_t>(epnum | 0x80), nullptr, 0)) {
            bulk_zlp_pending_ = true;
        }
    }

    UsbHal::Event ev {};
    ev.type     = UsbHal::EventType::TransferComplete;
    ev.endpoint = static_cast<uint8_t>(epnum | 0x80);
    ev.length   = xfer_count;
    ev.data     = s_pcd.IN_ep[epnum].xfer_buff;
    Notify(ev);
}

/**
 * @brief 启动 USB 设备连接
 * @return true=成功
 */
bool UsbHalStm32::Connect()
{
    if (!ready_)
        return false;

    if (HAL_PCD_Start(&s_pcd) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_Start failed");
        return false;
    }
    DUST_LOG_INF("USB Connect (PCD start ok)");
    return true;
}

/**
 * @brief 断开 USB 设备
 */
void UsbHalStm32::Disconnect()
{
    HAL_PCD_Stop(&s_pcd);
}

/**
 * @brief 设置 USB 设备地址
 * @param address  地址（0-127）
 * @return true=成功
 */
bool UsbHalStm32::SetAddress(uint8_t address)
{
    if (HAL_PCD_SetAddress(&s_pcd, address) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_SetAddress(%u) failed", address);
        return false;
    }
    return true;
}

Speed UsbHalStm32::GetSpeed() const
{
    return Speed::Full;
}

/**
 * @brief 打开端点
 * @param cfg  端点配置（地址/类型/MPS）
 * @return true=成功
 */
bool UsbHalStm32::EpOpen(const EndpointConfig& cfg)
{
    if (!ready_)
        return false;

    uint8_t type;
    switch (cfg.type) {
        case EndpointType::Control:     type = EP_TYPE_CTRL; break;
        case EndpointType::Isochronous: type = EP_TYPE_ISOC; break;
        case EndpointType::Interrupt:   type = EP_TYPE_INTR; break;
        case EndpointType::Bulk:        type = EP_TYPE_BULK; break;
        default:                        return false;
    }

    if (HAL_PCD_EP_Open(&s_pcd, cfg.address, cfg.max_packet_size, type) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_EP_Open(0x%02x) failed", cfg.address);
        return false;
    }

    uint8_t idx = cfg.address & 0x0F;
    if (cfg.address & 0x80) {
        in_ep_[idx].enable = true;
        in_ep_[idx].mps    = cfg.max_packet_size;
    } else {
        out_ep_[idx].enable = true;
        out_ep_[idx].mps    = cfg.max_packet_size;
    }
    return true;
}

/**
 * @brief 关闭端点
 * @param ep  端点号（含方向）
 * @return true=成功
 */
bool UsbHalStm32::EpClose(uint8_t ep)
{
    if (HAL_PCD_EP_Close(&s_pcd, ep) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_EP_Close(0x%02x) failed", ep);
        return false;
    }

    bulk_zlp_pending_ = false;
    uint8_t idx = ep & 0x0F;
    if (ep & 0x80) {
        in_ep_[idx] = {};
    } else {
        out_ep_[idx] = {};
    }
    return true;
}

/**
 * @brief 设置/清除端点 STALL
 * @param ep     端点号（含方向）
 * @param stall  true=STALL, false=清除
 * @return true=成功
 */
bool UsbHalStm32::EpStall(uint8_t ep, bool stall)
{
    HAL_StatusTypeDef status = stall ? HAL_PCD_EP_SetStall(&s_pcd, ep)
                                     : HAL_PCD_EP_ClrStall(&s_pcd, ep);
    if (status != HAL_OK) {
        DUST_LOG_ERR("EP 0x%02x stall set/clear failed", ep);
        return false;
    }
    return true;
}

/**
 * @brief 启动端点接收（OUT）
 * @param ep   端点号
 * @param len  最大接收长度
 * @return true=提交成功
 */
bool UsbHalStm32::EpStartRx(uint8_t ep, uint16_t len)
{
    if (!ready_)
        return false;

    // 同 EpStartTx：RX 寄存器（DOEPTSIZ/DOEPCTL）与 ISR 并发，关中断互斥。
    unsigned int key = irq_lock();

    uint8_t idx = ep & 0x0F;
    if (!out_ep_[idx].enable) {
        irq_unlock(key);
        return false;
    }

    uint8_t* buf = rx_buf_[rx_ping_];
    rx_ping_ = static_cast<uint8_t>((rx_ping_ + 1) & 1);

    out_ep_[idx].buf = buf;
    out_ep_[idx].len = len;

    bool ok = (HAL_PCD_EP_Receive(&s_pcd, ep, buf, len) == HAL_OK);
    if (!ok) {
        DUST_LOG_ERR("HAL_PCD_EP_Receive(0x%02x) failed", ep);
    }
    irq_unlock(key);
    return ok;
}

/**
 * @brief 启动端点发送（IN）
 * @param ep    端点号
 * @param data  发送数据
 * @param len   数据长度
 * @return true=提交成功
 */
bool UsbHalStm32::EpStartTx(uint8_t ep, const uint8_t* data, uint16_t len)
{
    if (!ready_)
        return false;

    // F4 OTG 无 DMA：IN 传输靠 TXFE 中断填 FIFO，XFRC 依赖数据填满。
    // DIEPEMPMSK/DIEPCTL 等寄存器由 ISR（XFRC/TXFE/EPDISD 处理）与 Task 线程（Send 提交）
    // 并发读写，必须关中断互斥——否则 DIEPEMPMSK RMW 丢失更新 → TXFE 不触发 → FIFO 不填
    // → XFRC 丢失（bulk IN 满包回传卡住根因）。ISR 内调用 irq_lock 嵌套安全。
    unsigned int key = irq_lock();

    uint8_t idx = ep & 0x0F;
    if (!in_ep_[idx].enable) {
        DUST_LOG_INF("EpStartTx ep=0x%02x SKIP enable=0", ep);
        irq_unlock(key);
        return false;
    }

    // HAL 已在 OnDataIn 满包回调内自动补发 ZLP：上层 OnBulkIn 的重复 ZLP 请求直接吞掉，
    // 避免同一 ZLP 提交两次（该请求只用于清上层状态，HAL 侧传输已完成）。
    if (len == 0 && data == nullptr && bulk_zlp_pending_) {
        bulk_zlp_pending_ = false;
        irq_unlock(key);
        return true;
    }

    uint8_t* buf = nullptr;
    if (data != nullptr && len > 0) {
        if (len > sizeof(tx_buf_)) {
            DUST_LOG_ERR("EpStartTx too large");
            irq_unlock(key);
            return false;
        }
        memcpy(tx_buf_, data, len);
        buf = tx_buf_;
    }

    bool ok = (HAL_PCD_EP_Transmit(&s_pcd, ep, buf, len) == HAL_OK);
    if (!ok) {
        DUST_LOG_ERR("HAL_PCD_EP_Transmit(0x%02x) failed", ep);
    } else {
        in_ep_[idx].buf = buf;
        in_ep_[idx].len = len;
    }
    irq_unlock(key);
    return ok;
}

bool UsbHalStm32::Ep0StartIn(const uint8_t* data, uint16_t len)
{
    if (!ready_ || !in_ep_[0].enable)
        return false;
    // EP0 IN 单次只传 MPS(64B)（F4 LL USB_EPStartXfer 对 EP0 硬截断），超过 64B 分包，剩余在 OnDataIn 续传
    uint16_t chunk = (len > 64) ? 64 : len;
    ep0_tx_rem_ = len - chunk;
    ep0_tx_ptr_ = (ep0_tx_rem_ > 0) ? (data + chunk) : nullptr;
    return EpStartTx(0x80, data, chunk);
}

/**
 * @brief EP0 DATA OUT
 * @param data  接收缓冲区
 * @param len   预期接收长度
 * @return true=提交成功
 */
bool UsbHalStm32::Ep0StartOut(uint8_t* data, uint16_t len)
{
    if (!ready_ || !out_ep_[0].enable)
        return false;

    out_ep_[0].buf = data;
    out_ep_[0].len = len;

    if (HAL_PCD_EP_Receive(&s_pcd, 0x00, data, len) != HAL_OK) {
        DUST_LOG_ERR("HAL_PCD_EP_Receive(0x00) failed");
        return false;
    }
    return true;
}

/**
 * @brief EP0 STATUS IN — 设备发 ZLP 确认收到数据
 * @return true=提交成功
 */
bool UsbHalStm32::Ep0StatusIn()
{
    if (!ready_ || !in_ep_[0].enable)
        return false;

    if (HAL_PCD_EP_Transmit(&s_pcd, 0x80, nullptr, 0) != HAL_OK) {
        DUST_LOG_ERR("Ep0StatusIn failed");
        return false;
    }
    return true;
}

/**
 * @brief EP0 STATUS OUT — 等待主机发 ZLP 确认
 * @return true=提交成功
 */
bool UsbHalStm32::Ep0StatusOut()
{
    if (!ready_ || !out_ep_[0].enable)
        return false;

    if (HAL_PCD_EP_Receive(&s_pcd, 0x00, nullptr, 0) != HAL_OK) {
        DUST_LOG_ERR("Ep0StatusOut failed");
        return false;
    }
    return true;
}
