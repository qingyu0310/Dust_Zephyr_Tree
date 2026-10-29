/**
 * @file phy_tja1101b.hpp
 * @author qingyu
 * @brief TJA1101B 100BASE-T1 PHY 驱动
 * @version 0.1
 * @date 2026-10-29
 *
 * 用于 HPM5E31 自定义板的 PORTB（车载单对以太网）。
 * 板卡在 dts 里写 compatible = "hpmicro,ecat-phy-tja1101b" 选本驱动。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_phy.hpp"
#include "ecat_phy_common.hpp"

/**
 * @brief TJA1101B PHY 驱动
 *
 * 100BASE-T1（IEEE 802.3bw）链路固定 100M 全双工，
 * 复位后自动与对端协商 master/slave，主机接口默认 MII。
 * 扩展寄存器（MII/RMII 模式、主从强制、线缆测试）经 REGCR(0x0D)/ADDAR(0x0E)
 * 间接访问，此处保持复位默认值。
 */
class PhyTja1101b final : public EcatPhy
{
public:
	/**
	 * @param hal  ESC 硬件抽象层（提供 MDIO）
	 * @param cfg  dts 属性值
	 */
	PhyTja1101b(EcatHal& hal, const PhyConfig& cfg);

	const char* Name() const override { return "TJA1101B"; }

	uint8_t Address() const override { return cfg_.mdio_address; }

	bool Config(const PhyConfig& cfg) override;

	bool LinkUp(bool* up) override;

private:
	EcatHal&    hal_;
	PhyConfig   cfg_;
	EcatPhyMdio mdio_;
	bool        configured_ = false;
};
