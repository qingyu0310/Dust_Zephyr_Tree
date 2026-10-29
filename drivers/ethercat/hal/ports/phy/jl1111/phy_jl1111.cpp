/**
 * @file phy_jl1111.cpp
 * @author qingyu
 * @brief JL1111 100BASE-TX PHY 驱动实现
 * @version 0.1
 * @date 2026-10-29
 *
 * 寄存器参考 hpm_sdk/components/enet_phy/jl1111/hpm_jl1111_regs.h。
 *
 * @copyright Copyright (c) 2026
 */

#include "phy_jl1111.hpp"

#include <zephyr/kernel.h>

#include "ecat_zephyr_port.h"

namespace
{
	// PHY ID：PHYID1 = 0x937C，PHYID2[15:10] = 0x10
	constexpr uint16_t kId1    = 0x937CU;
	constexpr uint8_t  kId2Oui = 0x10U;

	// 私有页寄存器（page select 在 reg31）
	constexpr uint8_t kPageSelReg = 31U;
	constexpr uint8_t kPageLed    = 7U;     // LED 模式页
	constexpr uint8_t kLedReg     = 19U;    // page7 reg19 = LED 模式值
	constexpr uint8_t kPageItf    = 7U;     // 接口模式页
	constexpr uint8_t kRmsrReg    = 16U;    // page7 reg16 bit3: 0=MII 1=RMII
	constexpr uint8_t kPageAddr   = 128U;   // 地址页
	constexpr uint8_t kAddrReg    = 19U;    // page128 reg19 = (addr<<5)|0x1F

	// 关广播定址时写入的低 5 位固定值
	constexpr uint16_t kAddrRegLow = 0x1FU;

	/**
	 * @brief 速率档位在 BMCR 中的编码（bit13 / bit6）
	 * 
	 * @param speed 
	 * @return uint16_t 
	 */
	uint16_t SpeedBits(EcatPhySpeed speed)
	{
		switch (speed)
		{
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
PhyJl1111::PhyJl1111(EcatHal& hal, const PhyConfig& cfg) : hal_(hal), cfg_(cfg), mdio_(hal, cfg.mdio_address)
{
}

/**
 * @brief 按 dts 参数配置芯片
 * @param cfg  dts 属性值
 * @return true=成功
 */
bool PhyJl1111::Config(const PhyConfig& cfg)
{
	cfg_ = cfg;

	uint16_t id1 = 0U;
	uint16_t id2 = 0U;

	if (!mdio_.ReadId(&id1, &id2)) {
		ECAT_LOG("%s@%u: no PHY response\n", Name(), Address());
		return false;
	}
	ECAT_LOG("%s@%u: PHY ID=0x%04x%04x\n", Name(), Address(), id1, id2);
	if (id1 != kId1 || ((id2 >> 10) & 0x3FU) != kId2Oui) {
		ECAT_LOG("%s@%u: unexpected ID (expected 0x%04x/%02x)\n",
			 Name(), Address(), kId1, kId2Oui);
		return false;
	}

	/* 复位会把广播响应恢复成默认，所以地址必须在复位之后写 */
	if (!mdio_.SoftReset(Name())) {
		ECAT_LOG("%s@%u: software reset failed\n", Name(), Address());
		return false;
	}

	/* LED 模式：page 7, reg 19 */
	if (!mdio_.Write(kPageSelReg, kPageLed) ||
	    !mdio_.Write(kLedReg, cfg_.led_mode) ||
	    !mdio_.Write(kPageSelReg, 0U)) {
		ECAT_LOG("%s@%u: LED mode setup failed\n", Name(), Address());
		return false;
	}

	/* 接口模式：page 7, reg 16 bit3 = 0(MII) / 1(RMII) */
	const uint16_t itfBit = (cfg_.interface == 0U) ? 0U : (1U << 3);

	if (!mdio_.Write(kPageSelReg, kPageItf) ||
	    !mdio_.Modify(kRmsrReg, (1U << 3), itfBit) ||
	    !mdio_.Write(kPageSelReg, 0U)) {
		ECAT_LOG("%s@%u: interface setup failed\n", Name(), Address());
		return false;
	}

	/* 自协商能力：ANAR 只写 dts 给的通告位 */
	uint16_t anar = 0U;

	if (!mdio_.Write(ecat_phy_reg::kAnar, cfg_.advertise) ||
	    !mdio_.Read(ecat_phy_reg::kAnar, &anar)) {
		ECAT_LOG("%s@%u: ANAR setup failed\n", Name(), Address());
		return false;
	}
	ECAT_LOG("%s@%u: ANAR=0x%04x\n", Name(), Address(), anar);

	/* BMCR：自协商 or 强制速率/双工 */
	uint16_t bmcr = 0U;

	if (cfg_.autoneg) {
		bmcr = ecat_phy_bmcr::kAnEnable | ecat_phy_bmcr::kAnRestart;
	} else {
		bmcr = SpeedBits(cfg_.speed);
		if (cfg_.full_duplex) {
			bmcr |= ecat_phy_bmcr::kDuplex;
		}
	}
	if (!mdio_.Write(ecat_phy_reg::kBmcr, bmcr) ||
	    !mdio_.Read(ecat_phy_reg::kBmcr, &bmcr)) {
		ECAT_LOG("%s@%u: BMCR setup failed\n", Name(), Address());
		return false;
	}
	ECAT_LOG("%s@%u: BMCR=0x%04x\n", Name(), Address(), bmcr);

	/* 关广播并定址：地址 0 的 PHY 不需要，广播对它本来就是本机地址 */
	if (cfg_.mdio_address != 0U) {
		const uint16_t addrValue =
			static_cast<uint16_t>((static_cast<uint16_t>(cfg_.mdio_address) << 5) |
					      kAddrRegLow);

		if (!mdio_.Write(kPageSelReg, kPageAddr) ||
		    !mdio_.Write(kAddrReg, addrValue) ||
		    !mdio_.Write(kPageSelReg, 0U)) {
			ECAT_LOG("%s@%u: address/broadcast setup failed\n", Name(), Address());
			return false;
		}
	}

	configured_ = true;
	ECAT_LOG("%s@%u: configured (%s, %s)\n", Name(), Address(),
		 cfg_.autoneg ? "autoneg" : "forced",
		 (cfg_.interface == 0U) ? "MII" : "RMII");
	return true;
}

/**
 * @brief 读链路状态
 * @param up  输出：链路是否 up
 * @return true=读取成功
 */
bool PhyJl1111::LinkUp(bool* up)
{
	uint16_t status = 0U;

	*up = false;
	if (!configured_) {
		return false;
	}

	/* BMSR 的 LINK 位是 latch-low，连读两次取当前值 */
	if (!mdio_.Read(ecat_phy_reg::kBmsr, &status) ||
	    !mdio_.Read(ecat_phy_reg::kBmsr, &status) ||
	    status == ecat_phy_reg::kInvalid) {
		return false;
	}

	*up = (status & ecat_phy_bmsr::kLink) != 0U;
	return true;
}
