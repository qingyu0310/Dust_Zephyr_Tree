/**
 * @file trd_test.cpp
 * @author qingyu
 * @brief 测试线程
 * @version 0.1
 * @date 2026-06-01
 */

#include "thread.hpp"
#include "Init_entry.hpp"
#include "pwm.hpp"
#include <zephyr/logging/log.h>

#pragma message "Compiling Thread/Test"

LOG_MODULE_REGISTER(test, LOG_LEVEL_INF);

namespace thread::test {

static Thread<2048> thread_ {};
static Pwm pwm1_ch2_ {};
static Pwm pwm1_ch3_ {};

static void Task(void*, void*, void*)
{
    for (;;)
    {
        pwm1_ch2_.SetDuty(0.3f);
        pwm1_ch3_.SetDuty(0.7f);
        k_msleep(100);
    }
}

bool thread_init()
{
    static const struct pwm_dt_spec spec_p2 = PWM_DT_SPEC_GET(DT_NODELABEL(pwm1_p2));
    static const struct pwm_dt_spec spec_p3 = PWM_DT_SPEC_GET(DT_NODELABEL(pwm1_p3));

    if (!pwm1_ch2_.init(spec_p2)) {
        return false;
    }

    if (!pwm1_ch3_.init(spec_p3)) {
        return false;
    }

    LOG_INF("pwm ready");
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::Low);
    return true;
}

REGISTER_INIT  (thread_init,  LateInit,   High, "test_init");
REGISTER_THREAD(thread_start, LateThread, High, "test_start");

} // namespace thread::test
