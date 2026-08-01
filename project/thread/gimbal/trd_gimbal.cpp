/**
 * @file trd_gimbal.cpp
 * @author qingyu
 * @brief 
 * @version 0.1
 * @date 2026-05-14
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#pragma message "Compiling Thread/Gimbal"

#include "dm.hpp"
#include "pid.hpp"
#include "thread.hpp"
#include "Init_entry.hpp"
#include "to_can_tx.hpp"
#include "remote_to.hpp"
#include "zephyr/kernel.h"
#include "zephyr/sys/printk.h"
#include "zephyr/zbus/zbus.h"
#include "timer.hpp"

namespace thread::gimbal {

using namespace instance::gimbal;

static Thread<> thread_{};

constexpr uint8_t kTaskRunningCycle = 1;                                    // 任务运行周期（ms）
constexpr float   kYawRadPerCycle   = 3.0f * kTaskRunningCycle * 0.001f;    // yaw   每周期弧度 0.001为 ms 转换到 s 的常数
constexpr float   kPitchRadPerCycle = 2.0f * kTaskRunningCycle * 0.001f;    // pitch 每周期弧度 0.001为 ms 转换到 s 的常数

// static float g_syaw_radian  = 0.0f;
static float g_byaw_radian  = 0.0f;
static float g_pitch_radian = 0.0f;

static void ProcessMotor(GimbalModule& mod, float& radian, topic::to_can_tx::Message& msg, Timer& timer)
{
    const auto snap = mod.motor.ReadAll();

    switch (snap.err)
    {
        case DmErrorStatus::Enable:
        {
            const float diff = radian - snap.radian;
            mod.ctrl.position.SetTarget(0);
            mod.ctrl.position.SetNow(-diff);
            const float omega_ref = mod.ctrl.position.Calc();

            mod.ctrl.omega.SetTarget(omega_ref);
            mod.ctrl.omega.SetNow(snap.omega);
            const float tor_ref = mod.ctrl.omega.Calc();

            mod.motor.SetTargetTorque(tor_ref);
            mod.motor.PackCtrlFrame(msg.data);
            msg.tx_id = mod.motor.GetTxId();
            k_msgq_put(topic::to_can_tx::gimbal_tx, &msg, K_NO_WAIT);
            break;
        }
        case DmErrorStatus::Disable:
        {
            timer.Clock([&]()
            {
                msg.tx_id = mod.motor.GetTxId();
                mod.motor.PackCmdFrame(msg.data, DmMotor::Cmd::Enable);
                k_msgq_put(topic::to_can_tx::gimbal_tx, &msg, K_NO_WAIT);
            });
            break;
        }
        default:
            break;
    }
}

static void Task(void*, void*, void*)
{
    Timer timer (1000);
    topic::to_can_tx::Message msg{};

    for (;;)
    {
        timer.Update();

        const zbus_channel *chan = nullptr;
        zbus_sub_wait(&sub_remote_to, &chan, K_NO_WAIT);
        if (chan) {
            topic::remote_to::Message rx{};
            zbus_chan_read(chan, &rx, K_NO_WAIT);
            g_byaw_radian  += rx.yaw   * kYawRadPerCycle;
            g_pitch_radian += rx.pitch * kPitchRadPerCycle;
        }

        ProcessMotor(big_yaw_, g_byaw_radian, msg, timer);
        // ProcessMotor(small_yaw_, g_syaw_radian, msg, timer);
        // ProcessMotor(pitch_, g_pitch_radian, msg, timer);

        k_msleep(kTaskRunningCycle);
    }
}

bool thread_init()
{
    {
        alg::pid::Pid::Config position_cfg {};
        position_cfg.kp  = 1.0f;
        position_cfg.ki  = 0.0f;
        position_cfg.kd  = 0.0f;

        alg::pid::Pid::Config omega_cfg{};
        omega_cfg.kp = 0.02f;
        omega_cfg.ki = 0.0f;
        omega_cfg.kd = 0.0f;

        DmMotor::Config cfg {
            .ctrl_met       = ControlMethon::Mit,
            .can_id         = kBYawCanId,
            .master_id      = kBYawMasterId,
            .gearbox_ratio  = 1.0f,
            .wheel_r        = 1,
            .kp             = 0.0f,
            .kd             = 0.0f,
            .PMAX        = 12.5,
            .VMAX        = 45,
            .TMAX        = 10,
        };

        big_yaw_.motor.Init(cfg);
        big_yaw_.ctrl.position.Init(position_cfg);
        big_yaw_.ctrl.omega.Init(omega_cfg);
    }
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::Normal);
    return true;
}

REGISTER_INIT(thread_init,  MidInit,    Mid, "gimbal_init");
REGISTER_THREAD(thread_start, MidThread,  Mid, "gimbal_start");

} // namespace thread::gimbal










































