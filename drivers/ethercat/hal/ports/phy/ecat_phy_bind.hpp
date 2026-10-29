/**
 * @file ecat_phy_bind.hpp
 * @author qingyu
 * @brief 从 dts 构造 PHY 驱动实例 — compatible 选驱动，属性填参数
 * @version 0.1
 * @date 2026-10-29
 *
 * 板卡 dts 里每颗 PHY 一个节点：
 *
 *   ecat_phy0: ethercat-phy@0 {
 *       compatible = "hpmicro,ecat-phy-jl1111";
 *       mdio-address = <0>;
 *       phy-mode = "mii";
 *       autoneg;
 *       advertise = <0x0100>;
 *   };
 *
 * 本文件把节点翻译成 EcatPhy 实例 + PhyConfig，
 * 不含任何板卡判断、不含任何芯片型号的 #if。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_hal.hpp"
#include "ecat_phy.hpp"

/**
 * @brief 从 dts 的 esc0 phys 属性构造 PHY 驱动
 *
 * @param hal       硬件抽象层（PHY 驱动用它做 MDIO 访问）
 * @param phys      输出：驱动实例指针数组（指向静态对象，不释放）
 * @param cfgs      输出：各 PHY 的配置
 * @param max_count 数组容量
 * @return 实际构造的数量（0 表示 dts 里没配 PHY）
 */
uint8_t ecat_phy_bind(EcatHal& hal, EcatPhy** phys, PhyConfig* cfgs, uint8_t max_count);
