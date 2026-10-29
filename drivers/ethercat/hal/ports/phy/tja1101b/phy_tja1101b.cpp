/**
 * @file phy_tja1101b.cpp
 * @author qingyu
 * @brief TJA1101B 100BASE-T1 PHY 驱动实现
 * @version 0.1
 * @date 2026-10-29
 *
 * @copyright Copyright (c) 2026
 */

#include "phy_tja1101b.hpp"

#include <zephyr/kernel.h>

#include "ecat_zephyr_port.h"

namespace
{
	/* NXP 100BASE-T1 PHY 家族 OUI 高 16 位（TJA1101 / TJA1101B） */
	constexpr uint16_t kId1 = 0x001BU;

	/** 速率档位在 BMCR 中的编码（bit13 / bit6） */
	uint16_t SpeedBits(EcatPhySpeed speed)
	{
		switch (speed) {
		case EcatPhySpeed::Mbps1000:
			return ecat_phy_bmcr::kSpeedLsb | ecat_phy_bmcr::kSpeedMsb;
		case EcatPhySpeed::Mbps100:
			return ecat_phy_bmcr::kSpeedLsb;
		case EcatPhySpeed::Mbps10:
		default:
			return 0U;
		}
	}
}

/**
 * @brief 构造
 * @param hal  ESC 硬件抽象层
 * @param cfg  dts 属性值
 */
PhyTja1101b::PhyTja1101b(EcatHal& hal, const PhyConfig& cfg)
	: hal_(hal), cfg_(cfg), mdio_(hal, cfg.mdio_address)
{
}

/**
 * @brief 按 dts 参数配置芯片
 * @param cfg  dts 属性值
 * @return true=成功
 */
bool PhyTja1101b::Config(const PhyConfig& cfg)
{
	cfg_ = cfg;

	uint16_t id1 = 0U;
	uint16_t id2 = 0U;

	if (!mdio_.ReadId(&id1, &id2)) {
		ECAT_LOG("%s@%u: no PHY response\n", Name(), Address());
		return false;
	}
	ECAT_LOG("%s@%u: PHY ID=0x%04x%04x\n", Name(), Address(), id1, id2);
	if (id1 != kId1) {
		ECAT_LOG("%s@%u: unexpected ID1 0x%04x (expected 0x%04x)\n",
			 Name(), Address(), id1, kId1);
		return false;
	}

	if (!mdio_.SoftReset(Name())) {
		ECAT_LOG("%s@%u: software reset failed\n", Name(), Address());
		return false;
	}

	/* 100BASE-T1 链路固定 100M 全双工，主机接口默认 MII。
	 * 自协商关闭时按 dts 强制速率/双工，否则保持复位默认（自动主从协商）。
	 */
	if (!cfg_.autoneg) {
		uint16_t bmcr = SpeedBits(cfg_.speed);

		if (cfg_.full_duplex) {
			bmcr |= ecat_phy_bmcr::kDuplex;
		}
		if (!mdio_.Write(ecat_phy_reg::kBmcr, bmcr)) {
			ECAT_LOG("%s@%u: BMCR setup failed\n", Name(), Address());
			return false;
		}
	}

	configured_ = true;
	ECAT_LOG("%s@%u: configured (100BASE-T1, master/slave auto)\n", Name(), Address());
	return true;
}

/**
 * @brief 读链路状态
 * @param up  输出：链路是否 up
 * @return true=读取成功
 */
bool PhyTja1101b::LinkUp(bool* up)
{
	uint16_t status = 0U;

	*up = false;
	if (!configured_) {
		return false;
	}

	/* 链路恒为 100M 全双工，没有速率/双工寄存器可交叉确认，BMSR bit2 即够 */
	if (!mdio_.Read(ecat_phy_reg::kBmsr, &status) ||
	    !mdio_.Read(ecat_phy_reg::kBmsr, &status) ||
	    status == ecat_phy_reg::kInvalid) {
		return false;
	}

	*up = (status & ecat_phy_bmsr::kLink) != 0U;
	return true;
}
