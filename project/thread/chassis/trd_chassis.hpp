/**
 * @file trd_chassis.hpp
 * @author qingyu
 * @brief
 * @version 0.1
 * @date 2026-04-30
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include "Irq_handlers.h"
#include "to_can_tx.hpp"
#include "dji_c6xx.hpp"
#include "pid.hpp"
#include <cstdint>

#if CONFIG_USE_POWERMETER
#include "powermeter.hpp"
#endif

namespace instance::chassis
{
    #define CHASSIS_RX  USER_RX_CAN1

    constexpr auto *chassis_tx = &user_can1_msgq;

    // 功率计 CAN 接收 ID
    #if CONFIG_USE_POWERMETER
    constexpr uint16_t KSteerPwrMeterId = 0x01;
    constexpr uint16_t KDrivePwrMeterId = 0x02;
    inline PowerMeter  SteerPwrMeter {};
    inline PowerMeter  DrivePwrMeter {};
    #endif

    constexpr uint8_t  N_Wheel = 2;                                              // 舵轮数量

    // CAN ID 映射（index 0=前轮, 1=后轮）
    constexpr uint16_t kSteerCanId[N_Wheel]   = {0x202, 0x204};          // 转向电机ID
    constexpr uint16_t kDriveCanId[N_Wheel]   = {0x201, 0x203};          // 驱动电机ID

    // chassis_to_can 消息内的数据槽索引（CAN ID 偏移量）
    constexpr uint8_t  kSteerDataIdx[N_Wheel] = {static_cast<uint8_t>(kSteerCanId[0] - 0x201), static_cast<uint8_t>(kSteerCanId[1] - 0x201)};
    constexpr uint8_t  kDriveDataIdx[N_Wheel] = {static_cast<uint8_t>(kDriveCanId[0] - 0x201), static_cast<uint8_t>(kDriveCanId[1] - 0x201)};

    // 单轮模块：转向 + 驱动
    struct WheelModule {
        DjiC6xx steer_motor {};
        DjiC6xx drive_motor {};
    };

    // 单轮算法状态：PID + 后续扩展
    struct WheelAlg {
        alg::pid::Pid steer_angle    {};
        alg::pid::Pid steer_torque   {};
        alg::pid::Pid drive_velocity {};
        alg::pid::Pid drive_torque   {};
    };

    inline WheelAlg    wheel_alg[N_Wheel]     {};
    inline WheelModule chassis_wheel[N_Wheel] {}; 
}