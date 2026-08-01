/**
 * @file trd_can_tx.cpp
 * @author qingyu
 * @brief
 * @version 0.1
 * @date 2026-04-30
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma message "Compiling Thread/Can"

#include "to_can_tx.hpp"
#include "thread.hpp"
#include "Init_entry.hpp"
#include <string.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include "can.hpp"
#include "Irq_handlers.h"
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(can_tx, LOG_LEVEL_INF);

namespace thread::can {

// k_msgq vs zbus：zbus 多个发布者共用一个 channel 会互相覆盖；
// k_msgq 内部拷贝数据，多 put 一 get 天然支持多发布者，且满时丢帧不阻塞。
static Thread<> thread_{};
static Can user_can1{};

static void Task(void*, void*, void*)
{
    can_frame tx {
        .dlc = 8,
    };
    topic::to_can_tx::Message msg{};

    for (;;)
    {
        k_msgq_get(&user_can1_msgq, &msg, K_FOREVER);
        tx.id  = msg.tx_id;
        memcpy(tx.data, msg.data, 8);

        user_can1.Send(&tx);
    }
}

bool thread_init()
{
    {
        const device* dev = DEVICE_DT_GET(DT_ALIAS(user_can1));
        if (!device_is_ready(dev)) {
            LOG_ERR("user_can1 not ready");
            return false;
        }
        const can_filter filter { .id = 0, .mask = 0, .flags = 0 };
        user_can1.Init(dev, filter);
        user_can1.SetRxCallback(user_can1_rx_callback);
        LOG_INF("user_can1 ready");
    }
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::High);
    return true;
}

REGISTER_INIT(thread_init,  PreInit,    High, "can_init");
REGISTER_THREAD(thread_start, PreThread,  High, "can_start");

} // namespace thread::can
