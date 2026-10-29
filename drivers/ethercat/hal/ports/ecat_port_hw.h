/**
 * @file ecat_port_hw.h
 * @author qingyu
 * @brief EtherCAT 端口层的硬件常量 — 与 PHY 芯片无关
 * @version 0.1
 * @date 2026-10-29
 *
 * 这里只放「ESC 本体」需要的常量：Flash 基址、SII 分区地址、
 * ESC 的 I2C EEPROM 引脚、忙等延时。
 *
 * PHY 的型号、地址、接口模式、自协商参数一律在板卡 dts 的 PHY 节点里，
 * 本文件不含任何板卡判断、不含任何芯片型号分支。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#include "hpm_soc.h"
#include "hpm_clock_drv.h"

#define BOARD_FLASH_BASE_ADDRESS (0x80000000UL)

/* SII 固件模拟 EEPROM 在 Flash 中的地址：直接取 dts 的 ethercat_eeprom_partition，
 * 各板卡只要在 dts 里给分区，C 侧不用再改 */
#define BOARD_ECAT_FLASH_EMULATE_EEPROM_ADDR \
	DT_REG_ADDR(DT_NODELABEL(ethercat_eeprom_partition))

/* ESC 的 MDIO 地址偏移（由 dts 的 phy-addr-offset 给） */
#define BOARD_ECAT_PHY_ADDR_OFFSET DT_PROP(DT_NODELABEL(esc0), phy_addr_offset)

/* 板上 I2C EEPROM（CAT24C128）：仅 CONFIG_HPM_ESC_EEPROM_EMULATION=n 时使用。
 * 这是 ESC 自己的 I2C 通道，与 PHY 无关。 */
#if !defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
#include "hpm_i2c_drv.h"
#include "hpm_iomux.h"

#define BOARD_ECAT_INIT_EEPROM_I2C      HPM_I2C1
#define BOARD_ECAT_INIT_EEPROM_I2C_CLK  clock_i2c1

/**
 * @brief ESC 自己读 EEPROM 时的引脚：PA12=ESC0_SDA、PA13=ESC0_SCL
 */
static inline void init_esc_eeprom_pin(void)
{
	HPM_IOC->PAD[IOC_PAD_PA12].FUNC_CTL = IOC_PA12_FUNC_CTL_ESC0_SDA;
	HPM_IOC->PAD[IOC_PAD_PA13].FUNC_CTL = IOC_PA13_FUNC_CTL_ESC0_SCL;
}

/**
 * @brief MCU 用 I2C 外设预烧 EEPROM 时的引脚：PA12/PA13 切到 I2C1，上拉 + 开漏
 */
static inline void init_esc_eeprom_as_i2c_pin(void)
{
	HPM_IOC->PAD[IOC_PAD_PA12].FUNC_CTL =
		IOC_PA12_FUNC_CTL_I2C1_SDA | IOC_PAD_FUNC_CTL_LOOP_BACK_MASK;
	HPM_IOC->PAD[IOC_PAD_PA12].PAD_CTL =
		IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_OD_SET(1);

	HPM_IOC->PAD[IOC_PAD_PA13].FUNC_CTL =
		IOC_PA13_FUNC_CTL_I2C1_SCL | IOC_PAD_FUNC_CTL_LOOP_BACK_MASK;
	HPM_IOC->PAD[IOC_PAD_PA13].PAD_CTL =
		IOC_PAD_PAD_CTL_PE_SET(1) | IOC_PAD_PAD_CTL_PS_SET(1) | IOC_PAD_PAD_CTL_OD_SET(1);
}
#endif /* !CONFIG_HPM_ESC_EEPROM_EMULATION */

/* busy-wait delay, usable in ISR context */
static inline void board_delay_ms(uint32_t ms)
{
	k_busy_wait(ms * 1000);
}
