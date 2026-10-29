/**
 * @file ecat_phy_mdio.cpp
 * @author qingyu
 * @brief 公共 MDIO 访问工具的复位实现
 * @version 0.1
 * @date 2026-10-29
 *
 * 三颗 PHY 的软复位流程完全一致（写 BMCR bit15 → 等自清零），
 * 放在这里避免每颗芯片各写一遍。
 *
 * @copyright Copyright (c) 2026
 */

#include <zephyr/kernel.h>

#include "ecat_phy_common.hpp"
#include "ecat_zephyr_port.h"

namespace
{
	/** 复位最长等待（ms），超时按失败处理 */
	constexpr uint32_t kResetTimeoutMs = 100U;
}

/**
 * @brief 软复位并等待 BMCR bit15 自清零
 * @param name  芯片名（日志用）
 * @return true=复位完成
 */
bool EcatPhyMdio::SoftReset(const char* name) const
{
	uint16_t value = 0U;

	if (!Write(ecat_phy_reg::kBmcr, ecat_phy_bmcr::kReset)) {
		return false;
	}

	for (uint32_t elapsed = 0U; elapsed < kResetTimeoutMs; ++elapsed) {
		k_msleep(1);
		if (!Read(ecat_phy_reg::kBmcr, &value) ||
		    value == ecat_phy_reg::kInvalid) {
			return false;
		}
		if ((value & ecat_phy_bmcr::kReset) == 0U) {
			return true;
		}
	}

	ECAT_LOG("%s@%u: software reset timeout (BMCR=0x%04x)\n",
		 name, static_cast<unsigned>(Address()), value);
	return false;
}
