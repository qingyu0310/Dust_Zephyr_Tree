/**
 * @file trd_pc.cpp
 * @author qingyu
 * @brief PC 通信线程 — USB CDC ACM 回环测试
 * @version 0.2
 * @date 2026-06-11
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma message "Compiling Thread/Pc"

#include "thread.hpp"
#include "Init_entry.hpp"
#include <zephyr/kernel.h>
#include "usb.hpp"
#include <zephyr/devicetree.h>

namespace thread::pc {

static Thread<2048> thread_ {};
static usb::Usb usb_ {};

static void Task(void*, void*, void*)
{
    constexpr uint8_t start[] = "start\r\n";
    uint8_t rx_buf[512];

    // 通知 PC USB 就绪
    usb_.Send(start, sizeof(start) - 1);

    for (;;)
    {
        // 阻塞等数据，收到即回传
        k_sem_take(&usb_.sem_, K_FOREVER);
        uint16_t n = usb_.Read(rx_buf, sizeof(rx_buf));
        if (n > 0) {
            usb_.Send(rx_buf, n);
        }
    }
}

bool thread_init()
{
    UsbHal::Config cfg {};
    cfg.busid    = 0;
    cfg.reg_base = DT_REG_ADDR(DT_NODELABEL(qingyuusb_usb0));
    cfg.irq_num  = DT_IRQN(DT_NODELABEL(qingyuusb_usb0));

    while (!usb_.Init(cfg)) {
        k_msleep(100);
    }
    return true;
}

bool thread_start()
{
    if (!usb_.IsReady()) {
        return false;
    }

    thread_.Start(Task, ThreadPrio::High);
    return true;
}

REGISTER_INIT  (thread_init,  PreInit,    High, "pc_init");
REGISTER_THREAD(thread_start, LateThread, High, "pc_start");

} // namespace thread::pc
