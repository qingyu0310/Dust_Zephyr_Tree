/**
 * @file phy_rtl8201f.hpp
 * @author qingyu
 * @brief RTL8201F 100BASE-TX PHY 驱动（MII/RMII）
 * @version 0.1
 * @date 2026-10-29
 *
 * 用于 HPM5E31 自定义板的 PORTA。
 * 板卡在 dts 里写 compatible = "hpmicro,ecat-phy-rtl8201f" 选本驱动。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_phy.hpp"
#include "ecat_phy_common.hpp"

/**
 * @brief RTL8201F PHY 驱动
 *
 * 配置顺序：软复位 → page7 强制 MII → page4 关 EEE → page0 关省电 → ANAR → 启协商。
 */
class PhyRtl8201f final : public EcatPhy
{
public:
	/**
	 * @param hal  ESC 硬件抽象层（提供 MDIO）
	 * @param cfg  dts 属性值
	 */
	PhyRtl8201f(EcatHal& hal, const PhyConfig& cfg);

	const char* Name() const override { return "RTL8201F"; }

	uint8_t Address() const override { return cfg_.mdio_address; }

	bool Config(const PhyConfig& cfg) override;

	bool LinkUp(bool* up) override;

private:
	EcatHal&    hal_;
	PhyConfig   cfg_;
	EcatPhyMdio mdio_;
	bool        configured_ = false;
};
