/**
 * @file phy_jl1111.hpp
 * @author qingyu
 * @brief JL1111 100BASE-TX PHY 驱动（MII/RMII）
 * @version 0.1
 * @date 2026-10-29
 *
 * 用于 HPM6E00EVK 的 PORTA / PORTB 两颗 PHY。
 * 板卡在 dts 里写 compatible = "hpmicro,ecat-phy-jl1111" 选本驱动。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_phy.hpp"
#include "ecat_phy_common.hpp"

/**
 * @brief JL1111 PHY 驱动
 *
 * 配置顺序：软复位 → LED 模式 → 接口模式 → ANAR 通告 → 启协商 → 关广播定址。
 * 复位后两颗 PHY 都开着广播响应，所以任意 MDIO 地址都会读到同一个 ID；
 * 地址非 0 的 PHY 必须在配置末尾把广播关掉并写入本机地址。
 */
class PhyJl1111 final : public EcatPhy
{
public:
	/**
	 * @param hal  ESC 硬件抽象层（提供 MDIO）
	 * @param cfg  dts 属性值
	 */
	PhyJl1111(EcatHal& hal, const PhyConfig& cfg);

	const char* Name() const override { return "JL1111"; }

	uint8_t Address() const override { return cfg_.mdio_address; }

	bool Config(const PhyConfig& cfg) override;

	bool LinkUp(bool* up) override;

private:
	EcatHal&    hal_;
	PhyConfig   cfg_;
	EcatPhyMdio mdio_;
	bool        configured_ = false;
};
