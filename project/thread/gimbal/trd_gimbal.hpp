/**
 * @file trd_gimbal.hpp
 * @author qingyu
 * @brief 
 * @version 0.1
 * @date 2026-05-14
 * 
 * @copyright Copyright (c) 2026
 * 
 */

#pragma once

#include "dm.hpp"
#include "pid.hpp"
#include "to_can_tx.hpp"
#include "Irq_handlers.h"

namespace instance::gimbal {

    #define         GIMBAL_RX                           USER_RX_CAN1
    constexpr auto *gimbal_tx  = &user_can1_msgq;             // 云台 CAN TX
    
    constexpr uint16_t kSYawCanId       = 0x02;
    constexpr uint16_t kSYawMasterId    = 0x03;

    constexpr uint16_t kBYawCanId       = 0x01;
    constexpr uint16_t kBYawMasterId    = 0x00;

    constexpr uint16_t kPitchCanId      = 0x04;
    constexpr uint16_t kPitchMasterId   = 0x05;

    // 控制算法
    struct CtrlAlg {
        alg::pid::Pid omega    {};      // 角速度环 (rad/s)
        alg::pid::Pid position {};      // 位置环 (rad 或 °，取决于输入)
    };

    struct GimbalModule {
        DmMotor motor;
        CtrlAlg ctrl;
    };

    // 小yaw
    inline GimbalModule small_yaw_ {};
    // 大yaw
    inline GimbalModule big_yaw_   {};
    // pitch
    inline GimbalModule pitch_     {};
}


























