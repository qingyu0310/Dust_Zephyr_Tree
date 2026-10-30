/**
 * @file ecat_hal_hpm.cpp
 * @author qingyu
 * @brief HPMicro ESC HAL 实现
 * @version 0.2
 * @date 2026-10-31
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "ecat_hal_hpm.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "log.hpp"

extern "C" {

#include <zephyr/drivers/ethercat/hpm_esc.h>
#include "hpm_common.h"
#include "ecat_config.h"

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)

#include "ecat_e2p_emulation.h"
#else
hpm_stat_t esc_eeprom_content_init(void);		// 板载 EEPROM 模式的装载接口，由端口层提供

#endif

}

/**
 * @brief 厂商 C 侧诊断输出入口
 * @param text 已格式化的文本
 */
extern "C" void ecat_port_log(const char* text)
{
	DUST_LOG_RAW("%s", text);
}

namespace ecat {

namespace {

constexpr uint16_t kSiiChunkSize = 8;		// SII 每次搬运的字节数（Flash 模拟的最小擦写单位）

/**
 * @brief 1 ms 定时器回调
 *
 * k_timer 需要静态回调，这里转发给当前实例。
 *
 * @param timer 定时器句柄（未使用）
 */
void OnMillisecond(struct k_timer* timer)
{
	EcatEscHpm* instance = static_cast<EcatEscHpm*>(k_timer_user_data_get(timer));

	if (instance != nullptr) {
		instance->DispatchTick();
	}
}

K_TIMER_DEFINE(g_ms_timer, OnMillisecond, nullptr);

}

/**
 * @brief 打开 ESC 设备并完成硬件初始化
 * @param config  端口配置（当前未使用，SII 介质由编译期分支决定）
 * @param handler 事件回调
 * @param context 回调上下文
 * @return true=成功
 */
bool EcatEscHpm::Open(const Config& config, EventHandler handler, void* context)
{
	(void)config;

	if (opened_) {
		return true;
	}
	if (!device_is_ready(dev_)) {
		return false;
	}

	handler_ = handler;
	context_ = context;

	// 顺序照已验证的参考实现：先关中断 → 挂四个回调 → 硬件初始化
	hpm_esc_irq_enable(dev_, false);

	hpm_esc_set_irq_callback(dev_, HPM_ESC_IRQ_PDI, OnPdiIrq, this);
	hpm_esc_set_irq_callback(dev_, HPM_ESC_IRQ_SYNC0, OnSync0Irq, this);
	hpm_esc_set_irq_callback(dev_, HPM_ESC_IRQ_SYNC1, OnSync1Irq, this);
	hpm_esc_set_irq_callback(dev_, HPM_ESC_IRQ_RESET, OnResetIrq, this);

	if (!HardwareInit()) {
		DUST_LOG_RAW("ecat: hardware init failed");
		return false;
	}

	opened_ = true;
	return true;
}

/**
 * @brief 硬件初始化：SII 介质、EEPROM 装载、PHY 复位与配置
 * @return true=成功
 */
bool EcatEscHpm::HardwareInit()
{
	// 两种 SII 介质在编译期分叉，GetSiiMode() 返回同一分支
	#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	{
		if (ecat_flash_eeprom_init() != 0) {
			DUST_LOG_RAW("ecat: flash eeprom init failed");
			return false;
		}

		hpm_esc_configure_eeprom(dev_, true, false);
		ecat_eeprom_emulation_reload_response();
	}
	#else
	{
		if (esc_eeprom_content_init() != 0) {
			DUST_LOG_RAW("ecat: i2c eeprom content init failed");
			return false;
		}

		init_esc_eeprom_pin();
		hpm_esc_configure_eeprom(dev_, false, ECAT_EEPROM_SIZE_BYTE > 256);
	}
	#endif

	sii_ready_ = true;

	const int eeprom_status = hpm_esc_check_eeprom(dev_);

	if (eeprom_status != 0) 
	{
		if (eeprom_status == -EBADMSG) {
			DUST_LOG_RAW("ecat: eeprom checksum error, communication still possible");
		} else {
			DUST_LOG_RAW("ecat: eeprom loading failed");
			return false;
		}
	}

	hpm_esc_phy_reset(dev_, true);
	k_msleep(10);
	hpm_esc_phy_reset(dev_, false);
	k_msleep(150);

	hpm_esc_configure_phy(dev_);
	hpm_esc_configure_signals(dev_);

	return true;
}

/**
 * @brief 停止事件并关闭端口
 */
void EcatEscHpm::Close()
{
	if (!opened_) {
		return;
	}

	StopEvents();
	opened_ = false;
}

/**
 * @brief 启动 1 ms 节拍与 ESC 中断
 */
void EcatEscHpm::StartEvents()
{
	if (!opened_) {
		return;
	}

	// 1 ms 节拍：协议栈的超时/看门狗都靠它
	k_timer_user_data_set(&g_ms_timer, this);
	k_timer_start(&g_ms_timer, K_MSEC(1), K_MSEC(1));

	hpm_esc_irq_enable(dev_, true);
	irq_enabled_ = true;
}

/**
 * @brief 关闭 ESC 中断并停止 1 ms 节拍
 */
void EcatEscHpm::StopEvents()
{
	if (!opened_) {
		return;
	}

	hpm_esc_irq_enable(dev_, false);
	irq_enabled_ = false;
	k_timer_stop(&g_ms_timer);
	k_timer_user_data_set(&g_ms_timer, nullptr);
}

/**
 * @brief 读 ESC 地址空间
 * @param address ESC 地址
 * @param data    目标缓冲
 * @param length  字节数
 */
void EcatEscHpm::Read(uint16_t address, void* data, uint16_t length)
{
	#if defined(HPM_IP_FEATURE_ESC_BYTE_READ) && HPM_IP_FEATURE_ESC_BYTE_READ
	{
		hpm_esc_read(dev_, address, data, length);
	}
	#else
	{
		// 硬件只能按 4 字节对齐整块读；非对齐地址先读进临时缓冲再按偏移拷出
		const uint16_t offset = static_cast<uint16_t>(address & 3U);

		if (offset == 0) {
			hpm_esc_read_block(dev_, address, data, length);
			return;
		}

		uint8_t scratch[4] = {};
		uint8_t* destination = static_cast<uint8_t*>(data);
		uint16_t remaining = length;
		uint16_t aligned = static_cast<uint16_t>(address & ~3U);
		uint16_t skip = offset;

		while (remaining > 0)
		{
			hpm_esc_read_block(dev_, aligned, scratch, sizeof(scratch));

			for (uint8_t i = skip; i < 4 && remaining > 0; ++i)
			{
				*destination = scratch[i];
				++destination;
				--remaining;
			}

			skip = 0;
			aligned = static_cast<uint16_t>(aligned + 4);
		}
	}
	#endif
}

/**
 * @brief 写 ESC 地址空间
 * @param address ESC 地址
 * @param data    源缓冲
 * @param length  字节数
 */
void EcatEscHpm::Write(uint16_t address, const void* data, uint16_t length)
{
	// 非对齐写必须读-改-写，否则对齐块里的其他字节会被一起写坏
	const uint16_t offset = static_cast<uint16_t>(address & 3U);

	if (offset == 0) {
		hpm_esc_write(dev_, address, data, length);
		return;
	}

	uint8_t scratch[4] = {};
	const uint8_t* source = static_cast<const uint8_t*>(data);
	uint16_t remaining = length;
	uint16_t aligned = static_cast<uint16_t>(address & ~3U);
	uint16_t skip = offset;

	while (remaining > 0)
	{
		// 首块/中间块要保留对齐块里不属于本次写入的字节
		hpm_esc_read_block(dev_, aligned, scratch, sizeof(scratch));

		for (uint8_t i = skip; i < 4 && remaining > 0; ++i)
		{
			scratch[i] = *source;
			++source;
			--remaining;
		}

		hpm_esc_write(dev_, aligned, scratch, sizeof(scratch));

		skip = 0;
		aligned = static_cast<uint16_t>(aligned + 4);
	}
}

/**
 * @brief 读 SII 内容
 * @param word_address 起始字地址
 * @param data         目标缓冲
 * @param length       字节数
 * @return true=成功
 */
bool EcatEscHpm::ReadSiiWord(uint32_t word_address, uint8_t* data, uint16_t length)
{
	if (!sii_ready_ || data == nullptr) {
		return false;
	}

	#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	{
		// 后端固定搬 kSiiChunkSize 字节，忽略请求长度；先搬到 scratch 再按请求长度拷出
		for (uint16_t offset = 0; offset < length; offset = static_cast<uint16_t>(offset + kSiiChunkSize))
		{
			uint8_t scratch[kSiiChunkSize] = {};

			if (ecat_flash_eeprom_read(word_address + offset, scratch) != 0) {
				return false;
			}

			const uint16_t remaining = static_cast<uint16_t>(length - offset);
			const uint16_t copy = (remaining < kSiiChunkSize) ? remaining : kSiiChunkSize;

			for (uint16_t i = 0; i < copy; ++i) {
				data[offset + i] = scratch[i];
			}
		}
		return true;

	}
	#else
	{
		// 板载 EEPROM：ESC 自己走 I2C 主控，应用层不需要参与
		(void)word_address;
		(void)length;
		return true;
	}
	#endif
}

/**
 * @brief 写 SII 内容
 * @param word_address 起始字地址
 * @param data         源缓冲
 * @param length       字节数
 * @return true=成功
 */
bool EcatEscHpm::WriteSiiWord(uint32_t word_address, const uint8_t* data, uint16_t length)
{
	if (!sii_ready_ || data == nullptr) {
		return false;
	}

	#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	{
		// 后端固定写 kSiiChunkSize 字节；请求不足一块时先读出原值，只覆盖请求范围
		for (uint16_t offset = 0; offset < length; offset = static_cast<uint16_t>(offset + kSiiChunkSize))
		{
			uint8_t scratch[kSiiChunkSize] = {};
			const uint16_t remaining = static_cast<uint16_t>(length - offset);
			const uint16_t copy = (remaining < kSiiChunkSize) ? remaining : kSiiChunkSize;

			if (copy < kSiiChunkSize) {
				if (ecat_flash_eeprom_read(word_address + offset, scratch) != 0) {
					return false;
				}
			}

			for (uint16_t i = 0; i < copy; ++i) {
				scratch[i] = data[offset + i];
			}

			if (ecat_flash_eeprom_write(word_address + offset, scratch) != 0) {
				return false;
			}
		}
		return true;
	}
	#else
	{
		(void)word_address;
		(void)length;
		return true;
	}
	#endif
}

/**
 * @brief 让 ESC 重新装载 SII 内容
 * @return true=成功
 */
bool EcatEscHpm::ReloadSii()
{
	if (!sii_ready_) {
		return false;
	}

	#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	{
		ecat_eeprom_emulation_reload_response();
		return true;
	}
	#else
	{
		// 板载 EEPROM：ESC 自己完成重载
		return true;
	}
	#endif
}

/**
 * @brief 把 SII 内容落盘
 */
void EcatEscHpm::StoreSii()
{
	if (!sii_ready_) {
		return;
	}

	#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	{
		ecat_flash_eeprom_flush();
	}
	#endif
}

/**
 * @brief 关 ESC 中断
 * @return 关闭前的使能状态，作为恢复 token
 */
uint32_t EcatEscHpm::MaskEvents()
{
	// 关中断，把原状态作为 token 返回，让嵌套调用能正确还原
	const bool was_enabled = irq_enabled_;

	hpm_esc_irq_enable(dev_, false);
	irq_enabled_ = false;

	return was_enabled ? 1U : 0U;
}

/**
 * @brief 按 token 恢复 ESC 中断
 * @param token MaskEvents 的返回值
 */
void EcatEscHpm::UnmaskEvents(uint32_t token)
{
	const bool enable = (token != 0U);

	hpm_esc_irq_enable(dev_, enable);
	irq_enabled_ = enable;
}

}

