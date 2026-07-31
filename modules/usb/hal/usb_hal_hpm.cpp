/**
 * @file usb_hal_hpm.cpp
 * @author qingyu
 * @brief HPMicro EHCI USB 硬件抽象层实现
 * @version 0.1
 * @date 2026-07-27
 *
 * @copyright Copyright (c) 2026
 */

#include "usb_hal_hpm.hpp"

#include <string.h>

#include <zephyr/logging/log.h>

#include <hpm_soc.h>
#include <hpm_clock_drv.h>
#include <hpm_usb_drv.h>
#include <hpm_usb_device.h>
#include <hpm_l1c_drv.h>
#include <hpm_common.h>
#include <dt-bindings/clock/hpm5361-clocks.h>

LOG_MODULE_REGISTER(usb_hal_hpm, LOG_LEVEL_INF);

static constexpr uint32_t kIntrUsb        = USB_USBINTR_UE_MASK;
static constexpr uint32_t kIntrError      = USB_USBINTR_UEE_MASK;
static constexpr uint32_t kIntrPortChange = USB_USBINTR_PCE_MASK;
static constexpr uint32_t kIntrReset      = USB_USBINTR_URE_MASK;
// 硬件常量（不直接使用 SDK 宏）
static constexpr uint32_t kCacheLineSize  = 32;           // HPM5361 数据 cacheline
static constexpr uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
static constexpr uint32_t AlignUpCache(uint32_t v)        { return AlignUp(v, kCacheLineSize); }

// Nocache 资源
__attribute__((section(".nocache"), aligned(2048)))           static uint8_t             s_dcd_data[AlignUp(sizeof(dcd_data_t), 2048)];
__attribute__((section(".nocache")))                          static usb_device_handle_t s_handle;
__attribute__((section(".nocache"), aligned(kCacheLineSize))) static uint8_t             s_rx_buf[2][512];
__attribute__((section(".nocache"), aligned(kCacheLineSize))) static uint8_t             s_tx_buf[512];
__attribute__((section(".nocache"), aligned(kCacheLineSize))) static uint8_t             s_setup_buf[8];

extern "C" void usb_isr_entry(const void* arg)
{
    if (arg) {
        static_cast<UsbHalHpm*>(const_cast<void*>(arg))->Isr();
    }
}

/**
 * @brief Get the Default Hal object
 * 
 * @return UsbHal& 
 */
UsbHal& GetDefaultHal() 
{ 
    static UsbHalHpm s_default_hal_;
    return s_default_hal_; 
}

//  —————————————————————————— 初始化 ——————————————————————————

/**
 * @brief 初始化 USB 硬件
 * @param cfg       硬件配置（reg_base/irq_num/irq_priority）
 * @param callback  事件回调
 * @param context   回调上下文（UsbDevPort*）
 * @return true=成功
 */
bool UsbHalHpm::Init(const Config& cfg, EventCallback callback, void* context)
{
    if (ready_)
        return true;

    reg_base_ = cfg.reg_base;
    callback_ = callback;
    context_  = context;
    cfg_      = cfg;

    InitClockAndPhy();

    if (irq_connect_dynamic(cfg.irq_num, cfg.irq_priority, usb_isr_entry, this, 0) < 0) {
        LOG_ERR("IRQ connect failed (irq=%u)", cfg.irq_num);
        return false;
    }

    USB_Type* regs = (USB_Type*)reg_base_;
    memset(&s_handle, 0, sizeof(s_handle));
    s_handle.regs     = regs;
    s_handle.dcd_data = (dcd_data_t*)s_dcd_data;

    uint32_t mask = kIntrUsb | kIntrError | kIntrPortChange | kIntrReset;
    if (!usb_device_init(&s_handle, mask)) {
        LOG_ERR("usb_device_init failed");
        Rollback();
        return false;
    }

    ready_ = true;
    irq_enable(cfg.irq_num);
    LOG_INF("HPM USB init done");
    return true;
}

/**
 * @brief Init 失败回滚
 *
 * 清理已分配的 IRQ、控制器和 dcd_data 资源。
 */
void UsbHalHpm::Rollback()
{
    LOG_ERR("Rolling back USB HAL init");
    ready_ = false;
    if (cfg_.irq_num) {
        irq_disable(cfg_.irq_num);
    }
    usb_device_deinit(&s_handle);
    memset(&s_handle, 0, sizeof(s_handle));
    memset(&s_dcd_data, 0, sizeof(s_dcd_data));
    callback_ = nullptr;
    context_  = nullptr;
}

/**
 * @brief 初始化时钟和 PHY
 */
void UsbHalHpm::InitClockAndPhy()
{
    USB_Type* regs = (USB_Type*)reg_base_;

    clock_add_to_group((clock_name_t)CLOCK_USB0, 0);
    usb_hcd_set_power_ctrl_polarity(regs, true);
    usb_phy_deinit(regs);
    usb_phy_init(regs, false);
    k_sleep(K_MSEC(100));
    LOG_INF("PHY init done");
}

//  —————————————————————————— ISR ——————————————————————————

/**
 * @brief USB 中断处理
 *
 * 读 USBSTS 判断中断类型，分别处理 Reset/PortChange/Setup/TransferComplete。
 */
void UsbHalHpm::Isr()
{
    USB_Type* regs = s_handle.regs;
    uint32_t sts = regs->USBSTS & regs->USBINTR;
    regs->USBSTS = sts;


    if (sts & kIntrError) {
        LOG_ERR("USB error 0x%08x", sts);
    }

    if (sts & kIntrReset) {
        LOG_INF("ISR: USB RESET");
        HandleReset();
    }

    if (sts & kIntrPortChange) {
        LOG_INF("ISR: PortChange CCS=%d", !!(regs->PORTSC1 & USB_PORTSC1_CCS_MASK));
        Event ev {};
        if (regs->PORTSC1 & USB_PORTSC1_CCS_MASK) {
            ev.type  = EventType::Connected;
            ev.speed = GetSpeed();
        } else {
            ev.type = EventType::Disconnected;
        }
        if (callback_) callback_(context_, ev);
    }

    if (sts & kIntrUsb) {
        uint32_t comp   = regs->ENDPTCOMPLETE;
        uint32_t setup  = regs->ENDPTSETUPSTAT;


        if (comp) {
            regs->ENDPTCOMPLETE = comp;
            HandleTransferComplete(comp);
        }
        if (setup) {
            regs->ENDPTSETUPSTAT = setup;
            HandleSetupReceived();
        }
    }
}

/**
 * @brief USB 总线复位处理
 *
 * 清端点状态、重置控制器 QHD、重新打开 EP0 IN/OUT。
 */
void UsbHalHpm::HandleReset()
{
    for (auto& ep : in_ep_)  ep = {};
    for (auto& ep : out_ep_) ep = {};

    usb_device_bus_reset(&s_handle, 64);

    usb_endpoint_config_t ep0_cfg {};
    ep0_cfg.xfer            = 0;
    ep0_cfg.max_packet_size = 64;

    ep0_cfg.ep_addr = 0x00;
    bool ok = usb_device_edpt_open(&s_handle, &ep0_cfg);
    LOG_INF("HandleReset: EP0 OUT open=%d", ok);
    out_ep_[0].enable = ok;

    ep0_cfg.ep_addr = 0x80;
    ok = usb_device_edpt_open(&s_handle, &ep0_cfg);
    LOG_INF("HandleReset: EP0 IN open=%d ENDPTCTRL0=0x%08x", ok, s_handle.regs->ENDPTCTRL[0]);
    in_ep_[0].enable = ok;

    Event ev {};
    ev.type = EventType::Reset;
    if (callback_) callback_(context_, ev);
}

/**
 * @brief 接收 SETUP 包
 *
 * 从 qHD[0].setup_request 拷贝 8 字节，包装为 Event 回调。
 */
void UsbHalHpm::HandleSetupReceived()
{
    dcd_qhd_t* qhd = usb_device_qhd_get(&s_handle, 0);

    memcpy(s_setup_buf, (uint8_t*)&qhd->setup_request, 8);

    Event ev {};
    ev.type = EventType::SetupReceived;
    memcpy(ev.setup, s_setup_buf, 8);
    if (callback_) callback_(context_, ev);
}

/**
 * @brief 遍历 ENDPTCOMPLETE 位，分发完成事件
 * @param comp  ENDPTCOMPLETE 寄存器值
 */
void UsbHalHpm::HandleTransferComplete(uint32_t comp)
{
    for (uint8_t i = 0; i < USB_SOC_DCD_MAX_ENDPOINT_COUNT * 2; i++) 
    {
        uint32_t bit = (i / 2) + ((i % 2) ? 16 : 0);

        if (!(comp & (1 << bit)))
            continue;

        uint8_t ep     = (i / 2) | ((i & 0x01) ? 0x80 : 0);
        bool    qtd_error = false;
        uint32_t len   = CalcTransferLength(i, &qtd_error);

        Event ev {};
        ev.type     = EventType::TransferComplete;
        ev.endpoint = ep;
        ev.length   = (uint16_t)len;

        if (qtd_error) {
            ev.error  = true;
            ev.length = 0;
            ev.data   = nullptr;
            if (callback_) callback_(context_, ev);
            continue;
        }

        if ((ep & 0x80) == 0 && len > 0) {
            uint8_t idx = ep & 0x0F;
            if (out_ep_[idx].buf) {
                ev.data = out_ep_[idx].buf;
                l1c_dc_invalidate((uintptr_t)ev.data, AlignUpCache(len));
            }
        }

        if (callback_) callback_(context_, ev);
    }
}

/**
 * @brief 计算 qTD 链传输总长度，检查错误
 * @param idx    端点索引
 * @param error  输出：是否发生 transaction/buffer error
 * @return 传输字节数
 */
uint32_t UsbHalHpm::CalcTransferLength(uint8_t idx, bool* error)
{
    dcd_qhd_t* qhd = usb_device_qhd_get(&s_handle, idx);
    dcd_qtd_t* qtd = qhd->attached_qtd;
    uint32_t total    = 0;
    bool     has_error = false;

    while (qtd && !qtd->active) 
    {
        if (qtd->halted || qtd->xact_err || qtd->buffer_err) 
        {
            LOG_ERR("qTD error: halted=%d xact_err=%d buf_err=%d", qtd->halted, qtd->xact_err, qtd->buffer_err);
            qtd->in_use = false;
            while (qtd->next != USB_SOC_DCD_QTD_NEXT_INVALID) 
            {
                qtd = (dcd_qtd_t*)(uintptr_t)qtd->next;
                qtd->in_use = false;
            }
            has_error = true;
            break;
        }
        total += qtd->expected_bytes - qtd->total_bytes;
        qtd->in_use = false;

        if (qtd->next == USB_SOC_DCD_QTD_NEXT_INVALID) break;
        qtd = (dcd_qtd_t*)(uintptr_t)qtd->next;
    }

    if (error) *error = has_error;
    return total;
}

//  —————————————————————————— EP0 控制传输 ——————————————————————————

/**
 * @brief EP0 DATA IN
 * @param data  数据（拷贝到 nocache tx_buf 后 DMA）
 * @param len   数据长度
 * @return true=提交成功
 */
bool UsbHalHpm::Ep0StartIn(const uint8_t* data, uint16_t len)
{
    if (!ready_ || !in_ep_[0].enable) 
    {
        LOG_ERR("Ep0StartIn rejected: ready=%d enable=%d len=%u",
                ready_, in_ep_[0].enable, len);
        return false;
    }

    if (len > sizeof(s_tx_buf)) {
        LOG_ERR("Ep0StartIn too large");
        return false;
    }

    bool ok;
    if (data != nullptr && len > 0) 
    {
        memcpy(s_tx_buf, data, len);
        ok = usb_device_edpt_xfer(&s_handle, 0x80, s_tx_buf, len);
    } 
    else {
        ok = usb_device_edpt_xfer(&s_handle, 0x80, nullptr, 0);
    }

    if (!ok) LOG_ERR("Ep0StartIn(len=%u) failed", len);
    return ok;
}

/**
 * @brief EP0 DATA OUT
 * @param data  接收缓冲区
 * @param len   预期接收长度
 * @return true=提交成功
 */
bool UsbHalHpm::Ep0StartOut(uint8_t* data, uint16_t len)
{
    if (!ready_ || !out_ep_[0].enable)
    {
        LOG_ERR("Ep0StartOut rejected: ready=%d enable=%d", ready_, out_ep_[0].enable);
        return false;
    }
    out_ep_[0].buf = data;
    out_ep_[0].len = len;

    if (data != nullptr && len > 0) {
        l1c_dc_invalidate((uintptr_t)data, AlignUpCache(len));
    }

    bool ok = usb_device_edpt_xfer(&s_handle, 0x00, data, len);
    if (!ok) LOG_ERR("Ep0StartOut(len=%u) failed", len);
    return ok;
}

/**
 * @brief EP0 STATUS IN — 设备发 ZLP 确认收到数据
 * @return true=提交成功
 */
bool UsbHalHpm::Ep0StatusIn()
{
    if (!ready_ || !in_ep_[0].enable)
    {
        LOG_ERR("Ep0StatusIn rejected: ready=%d enable=%d", ready_, in_ep_[0].enable);
        return false;
    }
    bool ok = usb_device_edpt_xfer(&s_handle, 0x80, nullptr, 0);
    if (!ok) LOG_ERR("Ep0StatusIn failed");
    return ok;
}

/**
 * @brief EP0 STATUS OUT — 等待主机发 ZLP 确认
 * @return true=提交成功
 */
bool UsbHalHpm::Ep0StatusOut()
{
    if (!ready_ || !out_ep_[0].enable)
    {
        LOG_ERR("Ep0StatusOut rejected: ready=%d enable=%d", ready_, out_ep_[0].enable);
        return false;
    }
    bool ok = usb_device_edpt_xfer(&s_handle, 0x00, nullptr, 0);
    if (!ok) LOG_ERR("Ep0StatusOut failed");
    return ok;
}

//  —————————————————————————— 批量端点传输 ——————————————————————————

/**
 * @brief 启动批量 OUT 接收
 * @param ep   端点号
 * @param len  最大接收长度
 * @return true=提交成功
 */
bool UsbHalHpm::EpStartRx(uint8_t ep, uint16_t len)
{
    if (!ready_) return false;

    uint8_t idx = ep & 0x0F;
    if (!out_ep_[idx].enable) return false;

    static uint8_t ping = 0;
    uint8_t* buf = s_rx_buf[ping];
    ping = (ping == 0) ? 1 : 0;

    out_ep_[idx].buf = buf;
    out_ep_[idx].len = len;

    l1c_dc_invalidate((uintptr_t)buf, AlignUpCache(len));
    return usb_device_edpt_xfer(&s_handle, ep, buf, len);
}

/**
 * @brief 启动批量 IN 发送
 * @param ep    端点号
 * @param data  发送数据
 * @param len   数据长度
 * @return true=提交成功
 */
bool UsbHalHpm::EpStartTx(uint8_t ep, const uint8_t* data, uint16_t len)
{
    if (!ready_) return false;

    uint8_t idx = ep & 0x0F;
    if (!in_ep_[idx].enable) return false;

    if (data != nullptr && len > 0) {
        memcpy(s_tx_buf, data, len);

        in_ep_[idx].buf = s_tx_buf;
        in_ep_[idx].len = len;

        l1c_dc_writeback((uintptr_t)s_tx_buf, AlignUpCache(len));
        return usb_device_edpt_xfer(&s_handle, ep, s_tx_buf, len);
    }

    return usb_device_edpt_xfer(&s_handle, ep, nullptr, 0);
}

//  —————————————————————————— 端点生命周期 ——————————————————————————

/**
 * @brief 打开端点
 * @param cfg  端点配置（地址/类型/MPS）
 * @return true=成功
 */
bool UsbHalHpm::EpOpen(const EndpointConfig& cfg)
{
    if (!ready_) return false;

    usb_endpoint_config_t ep {};
    switch (cfg.type) {
        case EndpointType::Control:     ep.xfer = 0; break;
        case EndpointType::Isochronous: ep.xfer = 1; break;
        case EndpointType::Bulk:        ep.xfer = 2; break;
        case EndpointType::Interrupt:   ep.xfer = 3; break;
    }
    ep.ep_addr         = cfg.address;
    ep.max_packet_size = cfg.max_packet_size;

    if (!usb_device_edpt_open(&s_handle, &ep))
        return false;

    uint8_t idx = cfg.address & 0x0F;
    if (cfg.address & 0x80) {
        in_ep_[idx].enable = true;
    } else {
        out_ep_[idx].enable = true;
    }

    return true;
}

/**
 * @brief 关闭端点
 * @param ep  端点号（含方向）
 * @return true=成功
 */
bool UsbHalHpm::EpClose(uint8_t ep)
{
    uint8_t idx    = ep & 0x0F;
    uint8_t epnum  = idx;
    uint8_t dir    = (ep & 0x80) ? 1 : 0;
    uint8_t ep_idx = 2 * epnum + dir;

    uint32_t flush_bit = HPM_BITSMASK(1, (epnum / 2 + ((epnum % 2) ? 16 : 0)));
    s_handle.regs->ENDPTFLUSH = flush_bit;
    
    for (int retry = 0; retry < 10000; retry++) {
        if (!(s_handle.regs->ENDPTFLUSH & flush_bit)) break;
    }
    if (s_handle.regs->ENDPTFLUSH & flush_bit) {
        LOG_ERR("EpClose ep 0x%02x flush timeout", ep);
    }

    dcd_qhd_t* qhd = usb_device_qhd_get(&s_handle, ep_idx);
    dcd_qtd_t* qtd = qhd->attached_qtd;
    while (qtd) {
        qtd->in_use = false;
        if (qtd->next == USB_SOC_DCD_QTD_NEXT_INVALID) break;
        qtd = (dcd_qtd_t*)(uintptr_t)qtd->next;
    }
    qhd->attached_qtd    = nullptr;
    qhd->attached_buffer = 0;

    if (ep & 0x80) {
        in_ep_[idx] = {};
    } else {
        out_ep_[idx] = {};
    }

    usb_dcd_edpt_close((USB_Type*)reg_base_, ep);
    return true;
}

/**
 * @brief 设置/清除端点 STALL
 * @param ep     端点号（含方向）
 * @param stall  true=STALL, false=清除
 * @return true=成功
 */
bool UsbHalHpm::EpStall(uint8_t ep, bool stall)
{
    if (stall) {
        usb_dcd_edpt_stall((USB_Type*)reg_base_, ep);
    } else {
        usb_dcd_edpt_clear_stall((USB_Type*)reg_base_, ep);
    }
    return true;
}

//  —————————————————————————— 设备控制 ——————————————————————————

/**
 * @brief 断开 USB 连接
 */
void UsbHalHpm::Disconnect()
{
    usb_device_disconnect(&s_handle);
}

bool UsbHalHpm::Connect()
{
    if (!ready_) {
        return false;
    }

    usb_device_connect(&s_handle);
    return true;
}

/**
 * @brief 设置 USB 设备地址
 * @param address  地址（0-127）
 * @return true=成功
 */
bool UsbHalHpm::SetAddress(uint8_t address)
{
    usb_dcd_set_address((USB_Type*)reg_base_, address);
    return true;
}

/**
 * @brief 获取当前 USB 速度
 * @return Speed::High 或 Speed::Full
 */
Speed UsbHalHpm::GetSpeed() const
{
    if (!ready_) return Speed::Full;
    USB_Type* regs = (USB_Type*)reg_base_;
    uint32_t pspd = (regs->PORTSC1 & USB_PORTSC1_PSPD_MASK) >> USB_PORTSC1_PSPD_SHIFT;
    return (pspd == 2) ? Speed::High : Speed::Full;
}

