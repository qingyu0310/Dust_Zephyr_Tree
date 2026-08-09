/**
 * @file fsmc.cpp
 * @author qingyu
 * @brief FSMC 底层设备驱动（devicetree lcd_fsmc 节点，Zephyr 设备模型注册）
 * @version 0.2
 * @date 2026-08-08
 *
 * @copyright Copyright (c) 2026
 *
 */

#define DT_DRV_COMPAT st_fsmc_lcd

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <soc.h>

namespace fsmc {

/**
 * @brief FSMC 控制器配置：全部来自 devicetree。
 */
struct Config {
    uint32_t bank;                		// 内存 bank（4 → BTCR[6]/[7]、BWTR[6]）
    uint32_t data_width;          		// 数据总线宽度（16=16bit）
    uint8_t  r_as;                		// 读时序 ADDSET
    uint8_t  r_ds;                		// 读时序 DATAST
    uint8_t  w_as;                		// 写时序 ADDSET
    uint8_t  w_ds;                		// 写时序 DATAST
    gpio_dt_spec backlight;       		// 背光 GPIO（PB15）
    const pinctrl_dev_config *pcfg;   	// pinctrl 引脚配置
};

/**
 * @brief 按 devicetree lcd_fsmc 节点初始化 FSMC
 *
 * RCC 使能 → Bank 控制（BCR）→ 读时序（BTR）/写时序（BWTR）→ 背光 GPIO 点亮。
 * 时序参数全部来自节点属性（等价原写死 15/60、3/3）。
 *
 * @param dev Zephyr 设备实例
 * @return 0 成功，负值失败
 */
static int fsmc_init(const struct device *dev)
{
    const Config *cfg = static_cast<const Config *>(dev->config);

    // 配置引脚复用（pinctrl，AF12）
    const int ret = pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT);
    if (ret < 0) {
        return ret;
    }

    // 使能 FSMC 时钟
    RCC->AHB3ENR |= RCC_AHB3ENR_FSMCEN;

    // BCR 段：bank → BTCR 索引（BCR4 = BTCR[6]）
    const uint8_t bcri = static_cast<uint8_t>(2 * (cfg->bank - 1));
    FSMC_Bank1->BTCR[bcri] = 0;              // 先清零
    FSMC_Bank1->BTCR[bcri] |= (1U << 0);     // MBKEN: Bank enable
    if (cfg->data_width == 16) {
        FSMC_Bank1->BTCR[bcri] |= (1U << 4); // MWID=01: 16-bit data bus
    }
    FSMC_Bank1->BTCR[bcri] |= (1U << 12);    // WREN: Write enable
    FSMC_Bank1->BTCR[bcri] |= (1U << 14);    // EXTMOD: 读写分离时序

    // BTR（读时序）：ADDSET=read-addr-setup-cycles, DATAST=read-data-setup-cycles
    FSMC_Bank1->BTCR[bcri + 1] = static_cast<uint16_t>((cfg->r_as << 0) | (cfg->r_ds << 8));
    // BWTR（写时序）：ADDSET=write-addr-setup-cycles, DATAST=write-data-setup-cycles
    FSMC_Bank1E->BWTR[bcri] = static_cast<uint16_t>((cfg->w_as << 0) | (cfg->w_ds << 8));

    // 背光（PB15）GPIO 输出 + 点亮
    if (gpio_pin_configure_dt(&cfg->backlight, GPIO_OUTPUT)) {
        return -EIO;
    }
    gpio_pin_set_dt(&cfg->backlight, 1);

    return 0;
}

} // namespace fsmc

/**
 * @brief 从 devicetree 编译期填充 config（C++17 位置初始化，避免 designated init）
 */
#define FSMC_CONFIG_INIT(n)                                                                  	\
{                                                                                        		\
	DT_INST_PROP(n, bank),                                                               		\
	DT_INST_PROP(n, data_width),                                                         		\
	static_cast<uint8_t>(DT_INST_PROP(n, read_addr_setup_cycles)),                       		\
	static_cast<uint8_t>(DT_INST_PROP(n, read_data_setup_cycles)),                       		\
	static_cast<uint8_t>(DT_INST_PROP(n, write_addr_setup_cycles)),                      		\
	static_cast<uint8_t>(DT_INST_PROP(n, write_data_setup_cycles)),                      		\
	GPIO_DT_SPEC_INST_GET(n, backlight_gpios),                                           		\
	PINCTRL_DT_INST_DEV_CONFIG_GET(n),                                                   		\
}

#define FSMC_DEVICE_DEFINE(n)                                                                	\
    PINCTRL_DT_INST_DEFINE(n);                                                               	\
    static const fsmc::Config fsmc_config_##n = FSMC_CONFIG_INIT(n);                         	\
    DEVICE_DT_INST_DEFINE(n, fsmc::fsmc_init, NULL, NULL, &fsmc_config_##n, POST_KERNEL,     	\
                          CONFIG_DUST_DEV_FSMC_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(FSMC_DEVICE_DEFINE)
