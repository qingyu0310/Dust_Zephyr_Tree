/**
 * @file ecat_hal_hpm.cpp
 * @author qingyu
 * @brief HPMicro ESC 硬件抽象层实现
 * @version 0.1
 * @date 2026-10-29
 *
 * 只做转发：把 EcatHal 的虚函数转到端口层的 C 函数。
 * 不含任何 EtherCAT 协议逻辑，也不含任何 PHY 芯片逻辑。
 *
 * @copyright Copyright (c) 2026
 */

#include "ecat_hal_hpm.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "ecat_phy_bind.hpp"
#include "ecat_zephyr_port.h"
#include "hpm_ecat_hw.h"

extern "C" {
#include <zephyr/drivers/ethercat/hpm_esc.h>

/** 线程侧打印 ISR 现场（ISR 里不能打日志） */
void esc_isr_report(void);
}

namespace
{
	const struct device* const kEscDev = DEVICE_DT_GET(DT_NODELABEL(esc0));
}

/* 中断回调与上下文（端口层 ISR 通过 OnPortIrq 转发到这里） */
EcatHal::EventCallback EcatHalHpm::irq_callback_ = nullptr;
void*                  EcatHalHpm::irq_context_  = nullptr;

/**
 * @brief 初始化 ESC 硬件与 PHY
 * @param cfg       硬件配置
 * @param callback  中断回调
 * @param context   回调上下文
 * @return true=成功
 */
bool EcatHalHpm::Init(const Config& cfg, EventCallback callback, void* context)
{
	(void)cfg;
	if (ready_) {
		return true;
	}

	irq_callback_ = callback;
	irq_context_  = context;

	/* 端口层：注册 PDI/SYNC0/SYNC1/RESET 回调 */
	ecat_irq_init();

	/* 端口层 ISR 不直接调 SSC，改为回调到这里，再转发给 EcatStack */
	ecat_port_set_irq_cb([](ecat_port_irq_t irq) {
		if (irq_callback_ == nullptr) {
			return;
		}
		switch (irq) {
		case ECAT_PORT_IRQ_PDI:
			irq_callback_(irq_context_, EcatIrq::Pdi);
			break;
		case ECAT_PORT_IRQ_SYNC0:
			irq_callback_(irq_context_, EcatIrq::Sync0);
			break;
		case ECAT_PORT_IRQ_SYNC1:
			irq_callback_(irq_context_, EcatIrq::Sync1);
			break;
		case ECAT_PORT_IRQ_RESET:
			irq_callback_(irq_context_, EcatIrq::Reset);
			break;
		default:
			break;
		}
	});

	/* 端口层：时钟 + 引脚 + EEPROM 模式 + ESC 本体（PHY 由下面遍历配置） */
	if (ecat_hardware_init(kEscDev) != 0) {
		ECAT_LOG("ecat: hardware init failed\n");
		return false;
	}

	/* dts 的 phys 属性 → 驱动实例 + 各自的 PhyConfig */
	phy_count_ = ecat_phy_bind(*this, phys_, phy_cfgs_, kMaxPorts);
	for (uint8_t i = 0; i < phy_count_; i++) {
		if (!phys_[i]->Config(phy_cfgs_[i])) {
			ECAT_LOG("ecat: phy %s@%u config failed\n",
				 phys_[i]->Name(),
				 static_cast<unsigned>(phys_[i]->Address()));
		}
	}

	ready_ = true;
	ECAT_LOG("ecat: hal ready, %u phy\n", static_cast<unsigned>(phy_count_));
	return true;
}

bool EcatHalHpm::Start()
{
	hpm_esc_start(kEscDev);
	return true;
}

void EcatHalHpm::Stop()
{
	bRunApplication = FALSE;
}

void EcatHalHpm::Read(uint16_t addr, void* buf, uint16_t len)
{
	HW_EscRead(buf, addr, len);
}

void EcatHalHpm::Write(uint16_t addr, const void* buf, uint16_t len)
{
	HW_EscWrite(buf, addr, len);
}

uint16_t EcatHalHpm::AlEvent()
{
	return HW_GetALEventRegister();
}

bool EcatHalHpm::MdioRead(uint8_t phy, uint8_t reg, uint16_t* value)
{
	return hpm_esc_mdio_read(kEscDev, phy, reg, value) == 0;
}

bool EcatHalHpm::MdioWrite(uint8_t phy, uint8_t reg, uint16_t value)
{
	return hpm_esc_mdio_write(kEscDev, phy, reg, value) == 0;
}

void EcatHalHpm::SetLink(uint8_t port, bool up)
{
	hpm_esc_set_link_state(kEscDev, port, up);
}

/**
 * @brief 遍历所有 PHY 读链路并写入 ESC
 *
 * 每颗 PHY 只读自己的寄存器，这里不区分芯片型号。
 */
void EcatHalHpm::UpdateLink()
{
	static int prev[kMaxPorts] = { -1, -1, -1 };

	/* 排查用：每 100ms 打一次 ISR 现场（中断里不能打日志） */
	esc_isr_report();

	for (uint8_t i = 0; i < phy_count_; i++) {
		bool up = false;

		if (!phys_[i]->LinkUp(&up)) {
			up = false;
		}
		SetLink(i, up);

		if (static_cast<int>(up) != prev[i]) {
			ECAT_LOG("ecat: port%u (%s) link %s\n",
				 static_cast<unsigned>(i), phys_[i]->Name(),
				 up ? "up" : "down");
			prev[i] = static_cast<int>(up);
		}
	}
}

uint16_t EcatHalHpm::EepromRead(uint32_t word_addr)
{
#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	return ecat_eeprom_emulation_read(word_addr);
#else
	/* 板上 I2C EEPROM：ESC 自己走 I2C 主控，应用层不接管 */
	(void)word_addr;
	return 0U;
#endif
}

void EcatHalHpm::EepromWrite(uint32_t word_addr, uint16_t data)
{
#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	(void)data;
	(void)ecat_eeprom_emulation_write(word_addr);
#else
	(void)word_addr;
	(void)data;
#endif
}

uint32_t EcatHalHpm::EepromReload()
{
#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	return ecat_eeprom_emulation_reload();
#else
	return 0U;
#endif
}

uint32_t EcatHalHpm::TimerGet()
{
	return HW_GetTimer();
}

void EcatHalHpm::TimerClear()
{
	HW_ClearTimer();
}

void EcatHalHpm::EnterCritical()
{
	DISABLE_ESC_INT();
}

void EcatHalHpm::LeaveCritical()
{
	ENABLE_ESC_INT();
}

/**
 * @brief 端口层 ISR 转发到 HAL 事件回调
 * @param context 事件上下文（EcatStack*）
 * @param irq     中断源
 */
void EcatHalHpm::OnPortIrq(void* context, EcatIrq irq)
{
	if (irq_callback_ != nullptr) {
		irq_callback_(context != nullptr ? context : irq_context_, irq);
	}
}

/**
 * @brief 获取默认 HAL 实例
 */
EcatHal& GetDefaultEcatHal()
{
	static EcatHalHpm s_default_hal;
	return s_default_hal;
}
