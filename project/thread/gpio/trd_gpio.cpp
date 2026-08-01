/**
 * @file trd_gpio.cpp
 * @author qingyu
 * @brief
 * @version 0.1
 * @date 2026-04-24
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma message "Compiling Thread/Gpio"

#include "thread.hpp"
#include "Init_entry.hpp"

#ifdef CONFIG_DUST_DEV_GPIO_OUTPUT

#include "output.hpp"
#include "timer.hpp"

namespace thread::output {

static Thread<2048> thread_{};

static Output led_alert{};

static void Task(void*, void*, void*)
{
    static constexpr uint32_t kPeriodMs = 1;
    Timer timer(1000);

    for (;;)
    {
        const int64_t tick_start = k_uptime_get();

        timer.Update();

        timer.Clock(([](){
            // printk("tick\n");
        }));

        const int64_t elapsed = k_uptime_get() - tick_start;
        const int64_t remain  = static_cast<int64_t>(kPeriodMs) - elapsed;
        if (remain > 0) {
            k_msleep(remain);
        }
    }
}

bool thread_init()
{
    // led_alert.init(GPIO_GET(led_alert));
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::Low);
    return true;
}

REGISTER_INIT  (thread_init,  PreInit,   Low, "output_init");
REGISTER_THREAD(thread_start, PreThread, Low, "output_start");

} // namespace thread::output
#endif

#ifdef CONFIG_DUST_DEV_GPIO_INPUT

#include "input.hpp"

namespace thread::input {

static Thread<> thread_{};

// 占位：预留用于按键/传感器输入，待实现
static void Task(void*, void*, void*)
{
    for (;;)
    {
        k_msleep(100);
    }
}

bool thread_init()
{
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::Low);
    return true;
}

REGISTER_INIT  (thread_init,  EarlyInit,   Low, "input_init");
REGISTER_THREAD(thread_start, EarlyThread, Low, "input_start");

} // namespace thread::input
#endif
