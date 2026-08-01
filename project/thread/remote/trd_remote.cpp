/**
 * @file trd_remote.cpp
 * @author qingyu
 * @brief 遥控器线程 — UART 接收、协议解析
 * @version 0.1
 * @date 2026-06-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma message "Compiling Thread/Remote"

#include "remote.hpp"
#include "Init_entry.hpp"
#include "uart.hpp"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(trd_remote, LOG_LEVEL_INF);

namespace thread::remote {

static ::remote::Remote remote_ {};

/**
 * @brief 初始化 UART DMA，并将遥控器解码器配置为自动识别模式。
 */
bool thread_init()
{
    static UartDma      rx {};
    constexpr uint16_t  kTimeout = 1000;

    UartDma::Config cfg = {
        .line_cfg = {
            .baudrate    = 100000,
            .parity      = UART_CFG_PARITY_EVEN,
            .stop_bits   = UART_CFG_STOP_BITS_2,
            .data_bits   = UART_CFG_DATA_BITS_8,
            .flow_ctrl   = UART_CFG_FLOW_CTRL_NONE,
        },
        .base_cfg = { .rx_timeout = kTimeout },
    };

    if (!rx.Init(DEVICE_DT_GET(DT_ALIAS(remote_uart)), cfg)) {
        LOG_ERR("uart init failed");
        return false;
    }

    remote_.Init(rx);
    return true;
}

bool thread_start()
{
    return remote_.Start(ThreadPrio::High);
}

REGISTER_INIT(thread_init,  PreInit,    High, "remote_init");
REGISTER_THREAD(thread_start, PreThread,  High, "remote_start");

} // namespace thread::remote
