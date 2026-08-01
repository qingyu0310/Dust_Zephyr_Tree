/**
 * @file trd_chassis.cpp
 * @author qingyu
 * @brief 底盘控制线程 — 1ms 周期：遥控 → 运动学 → PID → 功率分配 → 发布
 * @version 0.2
 * @date 2026-05-13
 *
 * ## 坐标系
 *
 * 右手系，z 轴向上：
 *
 *       +y (前)
 *       ↑
 *       |
 *       O────→ +x (右)
 *
 * 车体旋转 ω 绕 z 轴，逆时针为正。
 *
 * ## 控制流
 *
 * ReadRemote()             读取遥控器 zbus → vx/vy/vw
 *     ↓
 * UpdateTarget()           逆向运动学 + 优劣弧调整
 *     ↓
 * ControlCalculate()       PID 串联：外环→内环→电流
 *     ↓
 * PowerAlloc()             功率预测 + 分配（转向组优先）
 *     ↓
 * FramePublish()           组帧 → zbus → can_tx
 *
 * ## 舵轮布局 (N_Wheel = 2)
 *
 *   轮0 (前):  y = +R
 *   轮1 (后):  y = -R
 *
 * ## 待实测的机械参数
 *
 * 以下值当前为占位符，需根据实际底盘测量后修正：
 *
 * | 参数 | 当前值 | 说明 | 备注 |
 * |------|--------|------|------|
 * | kChassisR         | 0.135m    | 舵轮距底盘中心距离 | 需实测 |
 * | kWheelR           | 0.1m      | 轮子半径          | 需实测 |
 * | kGearboxRatio     | 3591/187  | 减速比            | M3508 官方数据，已确认 |
 * | KMaxMoveVelocity  | 0.5m/s    | 最大移动速度       | 调参确定 |
 * | KMaxRotationOmega | 2.0rad/s  | 最大自旋角速度     | 调参确定 |
 * | kTorqueK          | 0.3 N·m/A | C620 转矩常数      | 手册数据，已确认 |
 * | kSteerSign        | -1        | 舵向方向补偿       | 电机安装方向确认后锁定 |
 * | kDriveSign        | +1/-1     | 行进方向补偿       | 电机安装方向确认后锁定 |
 *
 * @copyright Copyright (c) 2026
 */

#pragma message "Compiling Thread/Chassis"

#include "remote_to.hpp"
#include "thread.hpp"
#include "Init_entry.hpp"
#include "to_can_tx.hpp"
#include "power_ctrl.hpp"
#include "pid.hpp"
#include "zephyr/zbus/zbus.h"
#include "math.h"

namespace {

constexpr float kPi   = 3.1415926535f;
constexpr float kPi_2 = kPi / 2.f;
constexpr float k2Pi  = 2.f * kPi;

// 角度归一化到 [-π, π)
float NormalizeAngle(float a)
{
    a = fmodf(a, k2Pi);
    if (a >  kPi) a -= k2Pi;
    if (a < -kPi) a += k2Pi;
    return a;
}

}

namespace thread::chassis {

using namespace instance::chassis;

static Thread<2048> thread_{};

// 电机参数
static constexpr float    kTorqueK          = 0.3f;                             // C6xx 转矩常数 N·m/A
static constexpr float    kCurrentScale     = 16384.0f / 20.0f;                 // 电流缩放系数

static constexpr uint8_t  kTotalBudget      = 20;                               // 底盘总功率预算 W
static constexpr float    kChassisR         = 0.135f;                           // 舵轮距车体中心距离
static constexpr uint16_t kChassisTxId      = 0x200;                            // 底盘can发送id

// 舵轮位置（右手系）
static constexpr struct { float x, y; } kWheelPos[N_Wheel] = {
    {0.0f,  0.135f},                                                  // 轮0 (前)
    {0.0f, -0.135f},                                                  // 轮1 (后)
};

// 速度限幅
static constexpr float KMaxMoveVelocity     = 0.5f;                             // 最大移动线速度 m/s
static constexpr float KMaxRotationOmega    = 2.0f;                             // 最大旋转角速度 rad/s

// 方向补偿（电机安装方向导致编码器正方向与底盘坐标系相反）
static constexpr int8_t kSteerSign[N_Wheel] = {-1, -1};                 // 舵向
static constexpr int8_t kDriveSign[N_Wheel] = { 1, -1};                 // 行进

// 功率控制器
static alg::power_ctrl::PowerCtrl<N_Wheel> SteerPwrCtrl {};                     // 转向组
static alg::power_ctrl::PowerCtrl<N_Wheel> DrivePwrCtrl {};                     // 行进组

// 运动学中间变量
static struct { float angle; float velocity; } g_wh_target[N_Wheel] {};
static float g_k_factor[N_Wheel]     {};                                        // 优劣弧方向因子 (±1)
static float g_steer_target[N_Wheel] {};                                        // 优劣弧调整后目标角度

// 底盘速度指令
static float g_vx = 0.0f, g_vy = 0.0f, g_vw = 0.0f;

/**
 * @brief 优劣弧判断
 *
 * 当目标角与当前角之差 > 90° 时，将目标角翻转 ±π 并反转行进电机，
 * 使舵轮走短路径而不是转 90° 以上。
 *
 * @param current 当前角度 (rad)
 * @param target  目标角度 (rad)，走劣弧时会被修改
 * @return 方向因子：1.0 (正转) / -1.0 (反转)
 */
static float OptimalArc(float current, float& target)
{
    float err = target - current;
    err = NormalizeAngle(err);

    if (fabsf(err) > kPi_2) {
        target = NormalizeAngle(current + err - (err > 0.f ? kPi : -kPi));
        return -1.f;
    }
    return 1.f;
}

/**
 * @brief 从 zbus 读取遥控器数据 → vx/vy/vw
 */
static void ReadRemote()
{
    static topic::remote_to::Message msg{};
    const zbus_channel *chan = nullptr;

    zbus_sub_wait(&sub_remote_to, &chan, K_NO_WAIT);
    if (chan) {
        zbus_chan_read(chan, &msg, K_NO_WAIT);
        g_vx =  msg.chassisx  * KMaxMoveVelocity;
        g_vy =  msg.chassisy  * KMaxMoveVelocity;
        g_vw = (msg.chassis_mode == topic::remote_to::ChassisMode::Spin) ? KMaxRotationOmega : 0.0f;
    }
}

/**
 * @brief 运动学解算 + 优劣弧调整
 *
 * 对每个轮子：
 *   1. 逆向运动学：V轮 = V车 + ω × r轮
 *   2. 优劣弧判断：角度差 > 90° 则翻转目标 + 反转行进
 */
static void UpdateTarget()
{
    for (uint8_t wi = 0; wi < N_Wheel; wi++)
    {
        // 逆向运动学
        const float vx_w   = g_vx - g_vw * kWheelPos[wi].y;
        const float vy_w   = g_vy + g_vw * kWheelPos[wi].x;
        const float spd    = sqrtf( vx_w * vx_w + vy_w * vy_w );

        if (spd > 1e-6f) {
            g_wh_target[wi].angle = kSteerSign[wi] * atan2f(vx_w, vy_w);
            g_wh_target[wi].angle = NormalizeAngle(g_wh_target[wi].angle);
        }
        g_wh_target[wi].velocity  = spd * kDriveSign[wi];

        // 优劣弧调整
        g_k_factor[wi]     = 1.f;
        g_steer_target[wi] = g_wh_target[wi].angle;

        const auto  snap = chassis_wheel[wi].steer_motor.ReadAll();
        const float chassis_angle = NormalizeAngle(kSteerSign[wi] * snap.angle);

        g_k_factor[wi] = OptimalArc(chassis_angle, g_steer_target[wi]);
    }
}

/**
 * @brief PID 串联控制
 *
 * 每轮两组串联 PID：
 *   转向：角度环 → 力矩环 → 电流
 *   行进：速度环 → 力矩环 → 电流
 */
static void ControlCalculate()
{
    for (uint8_t wi = 0; wi < N_Wheel; wi++)
    {
        // 转向：角度 → 力矩
        {
            const auto  snap = chassis_wheel[wi].steer_motor.ReadAll();
            const float chassis_angle = NormalizeAngle(kSteerSign[wi] * snap.angle);

            wheel_alg[wi].steer_angle.SetTarget(g_steer_target[wi]);
            wheel_alg[wi].steer_angle.SetNow(chassis_angle);
            const float torque_ref  = wheel_alg[wi].steer_angle. Calc();
            const float current_ref = wheel_alg[wi].steer_torque.Calc(torque_ref, snap.torque) / kTorqueK;
            SteerPwrCtrl.SetTarget(wi, current_ref);
            SteerPwrCtrl.SetMotorData(wi, snap.torque, snap.omega, wheel_alg[wi].steer_angle.GetError());
        }

        // 行进：速度 → 力矩
        {
            const auto  snap = chassis_wheel[wi].drive_motor.ReadAll();
            const float chassis_velocity = g_wh_target[wi].velocity * g_k_factor[wi];

            wheel_alg[wi].drive_velocity.SetTarget(chassis_velocity);
            wheel_alg[wi].drive_velocity.SetNow(snap.velocity);
            const float torque_ref  = wheel_alg[wi].drive_velocity.Calc();
            const float current_ref = wheel_alg[wi].drive_torque.  Calc(torque_ref, snap.torque) / kTorqueK;
            DrivePwrCtrl.SetTarget(wi, current_ref);
            DrivePwrCtrl.SetMotorData(wi, snap.torque, snap.omega, wheel_alg[wi].drive_velocity.GetError());
        }
    }
}

/**
 * @brief 功率预测 + 分配
 *
 * 策略：
 *   1. 预测两组电机所需功率
 *   2. 转向组优先分配（上限 80% 总功率）
 *   3. 行进组分剩余
 */
static void PowerAlloc()
{
    #if CONFIG_USE_POWERMETER
    {
        SteerPwrCtrl.SetMeasuredPower(SteerPwrMeter.GetPower());
        DrivePwrCtrl.SetMeasuredPower(DrivePwrMeter.GetPower());
    }
    #endif
    SteerPwrCtrl.Predict();
    DrivePwrCtrl.Predict();

    constexpr float kTurnRatio = 0.8f;
    constexpr float kSteerMax = kTotalBudget * kTurnRatio;

    const float SteerPred = SteerPwrCtrl.GetTotalPower();
    SteerPwrCtrl.Allocate(kSteerMax);               // 转向分配上限 80%，超出部分截断

    // 余额全部分给行进（转向未用满的余量也流入行进）
    const float DriveBudget = kTotalBudget - MIN(SteerPred, kSteerMax);
    DrivePwrCtrl.Allocate(DriveBudget);
}

/**
 * @brief 组帧 → 发布到 CAN 发送 topic
 */
static void FramePublish()
{
    topic::to_can_tx::Message msg{};

    auto set_out = [&](uint8_t idx, float current_A) {
        int16_t raw = static_cast<int16_t>(current_A * kCurrentScale);
        msg.data[idx * 2 + 0] = static_cast<uint8_t>(raw >> 8);
        msg.data[idx * 2 + 1] = static_cast<uint8_t>(raw & 0xFF);
    };
    
    for (uint8_t wi = 0; wi < N_Wheel; wi++)
    {
        set_out(kSteerDataIdx[wi], SteerPwrCtrl.GetLimitedCurrent(wi));
        set_out(kDriveDataIdx[wi], DrivePwrCtrl.GetLimitedCurrent(wi));
    }

    msg.tx_id = kChassisTxId;
    k_msgq_put(topic::to_can_tx::chassis_tx, &msg, K_NO_WAIT);
}

/**
 * @brief 底盘控制主循环
 *
 * 顺序：遥控 → 运动学 → PID → 功率 → 发布，固定 1ms 周期。
 */
static void Task(void*, void*, void*)
{
    static constexpr uint32_t kPeriodMs = 1;

    for (;;)
    {
        const int64_t tick_start = k_uptime_get();

        ReadRemote();
        UpdateTarget();
        ControlCalculate();
        PowerAlloc();
        FramePublish();

        const int64_t elapsed = k_uptime_get() - tick_start;
        const int64_t remain  = static_cast<int64_t>(kPeriodMs) - elapsed;
        if (remain > 0) {
            k_msleep(remain);
        }
    }
}

bool thread_init()
{
    // 功率计初始化
    #if CONFIG_USE_POWERMETER
    {
        SteerPwrMeter.Init(KSteerPwrMeterId);
        DrivePwrMeter.Init(KDrivePwrMeterId);
    }
    #endif

    // 功率预测模型初始化

    // 转向组：角度误差敏感，上限 80%
    {
        constexpr float k1 = 1.453009e-07f;
        constexpr float k2 = 5.171939e-03f;
        constexpr float k3 = 3.0f;

        alg::power_ctrl::PowerCtrl<N_Wheel>::Config cfg{};
        cfg.k1Init      = k1;
        cfg.k2Init      = k2;
        cfg.torqueK     = kTorqueK;
        cfg.k3          = k3;
        cfg.errUpper    = 50.0f;
        cfg.errLower    = 0.01f;
        cfg.rlsLambda   = 0.999f;
        cfg.rlsEnable   = IS_ENABLED(CONFIG_USE_POWERMETER);
        cfg.tauOmegaEnable = cfg.rlsEnable;
        SteerPwrCtrl.Init(cfg);
    }

    // 行进组：拿剩余功率，速度误差范围大
    {
        constexpr float k1 = 1.453009e-07f;
        constexpr float k2 = 5.171939e-03f;
        constexpr float k3 = 3.5f;

        alg::power_ctrl::PowerCtrl<N_Wheel>::Config cfg{};
        cfg.k1Init      = k1;
        cfg.k2Init      = k2;
        cfg.torqueK     = kTorqueK;
        cfg.k3          = k3;
        cfg.errUpper    = 500.0f;
        cfg.errLower    = 0.001f;
        cfg.rlsEnable   = IS_ENABLED(CONFIG_USE_POWERMETER);
        cfg.rlsLambda   = 0.999f;
        cfg.tauOmegaEnable = cfg.rlsEnable;
        DrivePwrCtrl.Init(cfg);
    }

    // 电机 + PID 初始化，每轮先转向后行进
    for (uint8_t wi = 0; wi < N_Wheel; wi++)
    {
        // 转向组
        {
            constexpr float kWheelR       = 0.1f;
            constexpr float kGearboxRatio = 3591.f / 187.f;

            DjiC6xx::Config motor_cfg {};
            motor_cfg.rx_id         = kSteerCanId[wi];
            motor_cfg.wheel_r       = kWheelR;
            motor_cfg.gearbox_ratio = kGearboxRatio;

            alg::pid::Pid::Config angle_cfg {};
            angle_cfg.kp  = 1.0f;
            angle_cfg.ki  = 0.0f;
            angle_cfg.kd  = 0.0f;

            alg::pid::Pid::Config torque_cfg{};
            torque_cfg.kp = 0.2f;
            torque_cfg.ki = 0.0f;
            torque_cfg.kd = 0.0f;

            chassis_wheel[wi].steer_motor.Init(motor_cfg);
            wheel_alg[wi].steer_angle    .Init(angle_cfg);
            wheel_alg[wi].steer_torque   .Init(torque_cfg);
        }

        // 行进组
        {
            constexpr float kWheelR       = 0.1f;
            constexpr float kGearboxRatio = 3591.f / 187.f;

            DjiC6xx::Config motor_cfg {};
            motor_cfg.rx_id         = kDriveCanId[wi];
            motor_cfg.wheel_r       = kWheelR;
            motor_cfg.gearbox_ratio = kGearboxRatio;

            alg::pid::Pid::Config speed_cfg {};
            speed_cfg.kp  = 5.0f;
            speed_cfg.ki  = 0.0f;
            speed_cfg.kd  = 0.0f;

            alg::pid::Pid::Config torque_cfg{};
            torque_cfg.kp = 0.5f;
            torque_cfg.ki = 0.0f;
            torque_cfg.kd = 0.0f;

            chassis_wheel[wi].drive_motor.Init(motor_cfg);
            wheel_alg[wi].drive_velocity .Init(speed_cfg);
            wheel_alg[wi].drive_torque   .Init(torque_cfg);
        }
    }
    return true;
}

bool thread_start()
{
    thread_.Start(Task, ThreadPrio::Normal);
    return true;
}

REGISTER_INIT(thread_init,  MidInit,    Mid, "chassis_init");
REGISTER_THREAD(thread_start, MidThread,  Mid, "chassis_start");

} // namespace thread::chassis
