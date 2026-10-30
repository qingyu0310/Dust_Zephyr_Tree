/**
 * @file ecat_hal_hpm.cpp
 * @author qingyu
 * @brief HPMicro ESC HAL 实现
 * @version 0.1
 * @date 2026-10-31
 *
 * @copyright Copyright (c) 2026
 */

#include "ecat_hal_hpm.hpp"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>

#include "log.hpp"
#include "spec/ecat_registers.hpp"

extern "C" {

#include <zephyr/drivers/ethercat/hpm_esc.h>
#include "hpm_common.h"
#include "ecat_config.h"

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
#include "ecat_e2p_emulation.h"
#else
/* 板载 EEPROM 模式的装载接口，由端口层提供 */
hpm_stat_t esc_eeprom_content_init(void);
#endif

}

namespace ecat {

namespace {

	const struct device* const kEscDevice = DEVICE_DT_GET(DT_NODELABEL(esc0));

	/** 中断号编码：`user_data` 只带低 8 位 */
	constexpr uintptr_t kIrqMask = 0xFFU;

	/** 每次 SII 读写搬运的字节数上限（Flash 模拟的最小擦写单位） */
	constexpr uint16_t kSiiChunkSize = 8;

	EcatEscHpm* g_instance = nullptr;

	/**
	 * @brief 中断号到事件的映射
	 */
	EcatEvent MapIrq(enum hpm_esc_irq irq)
	{
		switch (irq) 
		{
			case HPM_ESC_IRQ_PDI:   return EcatEvent::Pdi;
			case HPM_ESC_IRQ_SYNC0: return EcatEvent::Sync0;
			case HPM_ESC_IRQ_SYNC1: return EcatEvent::Sync1;
			case HPM_ESC_IRQ_RESET:
			default:                return EcatEvent::Reset;
		}
	}

	EcatEscHpm* g_timer_instance = nullptr;

	/**
	 * @brief 1 ms 定时器回调
	 *
	 * 定时器不归 ESC 端口管（它属于协议核心的节拍），
	 * 但 Zephyr 的 `k_timer` 需要静态回调，所以放在这里再转发。
	 */
	void OnMillisecond(struct k_timer* timer)
	{
		(void)timer;

		if (g_timer_instance != nullptr) {
			g_timer_instance->DispatchTick();
		}
	}

	K_TIMER_DEFINE(g_ms_timer, OnMillisecond, nullptr);
}

EcatEscHpm::EcatEscHpm()
	: dev_(kEscDevice)
{
}

EcatEscHpm::~EcatEscHpm()
{
	Close();
}

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

	/*
	 * 顺序照已验证的参考实现：
	 *   1) 先关中断（此时还没有回调，但要保证 ESC 不产生事件）
	 *   2) 挂 PDI / SYNC0 / SYNC1 / RESET 四个回调
	 *   3) 硬件初始化（SII 介质、EEPROM 模式、ESC 本体）
	 */
	hpm_esc_irq_enable(dev_, false);

	g_instance = this;

	for (uintptr_t irq = 0; irq < HPM_ESC_IRQ_COUNT; ++irq) {
		hpm_esc_set_irq_callback(dev_, static_cast<enum hpm_esc_irq>(irq), OnIrq, reinterpret_cast<void*>(irq & kIrqMask));
	}

	if (!HardwareInit()) {
		ECAT_LOG("ecat: hardware init failed\n");
		g_instance = nullptr;
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
	if (ecat_flash_eeprom_init() != 0) {
		ECAT_LOG("ecat: flash eeprom init failed\n");
		return false;
	}

	hpm_esc_configure_eeprom(dev_, true, false);
	ecat_eeprom_emulation_reload_response();
#else
	if (esc_eeprom_content_init() != 0) {
		ECAT_LOG("ecat: i2c eeprom content init failed\n");
		return false;
	}

	init_esc_eeprom_pin();
	hpm_esc_configure_eeprom(dev_, false, ECAT_EEPROM_SIZE_BYTE > 256);
#endif

	sii_ready_ = true;

	const int eeprom_status = hpm_esc_check_eeprom(dev_);

	if (eeprom_status != 0) {
		if (eeprom_status == -EBADMSG) {
			ECAT_LOG("ecat: eeprom checksum error, communication still possible\n");
		} else {
			ECAT_LOG("ecat: eeprom loading failed\n");
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

void EcatEscHpm::Close()
{
	if (!opened_) {
		return;
	}

	StopEvents();
	g_instance = nullptr;
	opened_ = false;
}

void EcatEscHpm::StartEvents()
{
	if (!opened_) {
		return;
	}

	// 1 ms 节拍：协议栈的超时/看门狗都靠它
	g_timer_instance = this;
	k_timer_start(&g_ms_timer, K_MSEC(1), K_MSEC(1));

	hpm_esc_irq_enable(dev_, true);
	irq_enabled_ = true;
}

void EcatEscHpm::StopEvents()
{
	if (!opened_) {
		return;
	}

	hpm_esc_irq_enable(dev_, false);
	irq_enabled_ = false;
	k_timer_stop(&g_ms_timer);

	if (g_timer_instance == this) {
		g_timer_instance = nullptr;
	}
}

void EcatEscHpm::Read(uint16_t address, void* data, uint16_t length)
{
#if defined(HPM_IP_FEATURE_ESC_BYTE_READ) && HPM_IP_FEATURE_ESC_BYTE_READ
	hpm_esc_read(dev_, address, data, length);
#else
	/*
	 * 不支持逐字节读时，硬件只能按 4 字节对齐整块读。
	 * 非对齐地址要先读进临时缓冲，再按偏移拷出——直接 `& ~3` 整块读会让
	 * 调用方拿到前移了 address & 3 字节的错位数据。
	 */
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
#endif
}

void EcatEscHpm::Write(uint16_t address, const void* data, uint16_t length)
{
	/*
	 * 非对齐写必须读-改-写，否则 `& ~3` 会把前面对齐块里的其他字节一起写坏。
	 */
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

uint16_t EcatEscHpm::ReadAlEvent()
{
	uint16_t value = 0;

	hpm_esc_read(dev_, reg::kAlEvent, &value, sizeof(value));
	return value;
}

bool EcatEscHpm::MdioRead(uint8_t phy, uint8_t reg, uint16_t* value)
{
	return hpm_esc_mdio_read(dev_, phy, reg, value) == 0;
}

bool EcatEscHpm::MdioWrite(uint8_t phy, uint8_t reg, uint16_t value)
{
	return hpm_esc_mdio_write(dev_, phy, reg, value) == 0;
}

void EcatEscHpm::SetLinkState(uint8_t port, bool up)
{
	hpm_esc_set_link_state(dev_, port, up);
}

EcatEsc::SiiMode EcatEscHpm::GetSiiMode() const
{
#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	return SiiMode::Emulated;
#else
	return SiiMode::OnBoard;
#endif
}

bool EcatEscHpm::ReadSiiWord(uint32_t word_address, uint8_t* data, uint16_t length)
{
	if (!sii_ready_ || data == nullptr) {
		return false;
	}

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	/*
	 * 后端一次固定搬 kSiiChunkSize(8) 字节，忽略请求长度。
	 * 直接用 data + offset 当目标会在 length 小于 8 时越界写，所以先搬到
	 * scratch，再按请求长度拷出。
	 */
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
#else
	/*
	 * 板载 EEPROM：ESC 自己走 I2C 主控，应用层在运行期不接管。
	 * 这里的"成功"表示"不需要应用参与"，不是"读到了数据"。
	 */
	(void)word_address;
	(void)length;
	return true;
#endif
}

bool EcatEscHpm::WriteSiiWord(uint32_t word_address, const uint8_t* data, uint16_t length)
{
	if (!sii_ready_ || data == nullptr) {
		return false;
	}

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	/*
	 * 后端一次固定写 kSiiChunkSize(8) 字节。请求长度不足 8 时要先读出原值、
	 * 只覆盖请求范围内的字节，否则会把相邻数据一起写掉。
	 */
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
#else
	(void)word_address;
	(void)length;
	return true;
#endif
}

bool EcatEscHpm::ReloadSii()
{
	if (!sii_ready_) {
		return false;
	}

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	ecat_eeprom_emulation_reload_response();
	return true;
#else
	// 板载 EEPROM：ESC 自己完成重载
	return true;
#endif
}

void EcatEscHpm::StoreSii()
{
	if (!sii_ready_) {
		return;
	}

#if defined(CONFIG_HPM_ESC_EEPROM_EMULATION)
	ecat_flash_eeprom_flush();
#endif
}

uint32_t EcatEscHpm::MaskEvents()
{
	/*
	 * 关 ESC 中断并把原状态作为 token 返回。
	 * 旧的 `DISABLE_ESC_INT` 无返回值，嵌套就会在第一次恢复时
	 * 提前把中断打开——这里保留 token 让调用方能正确还原。
	 */
	const bool was_enabled = irq_enabled_;

	hpm_esc_irq_enable(dev_, false);
	irq_enabled_ = false;

	return was_enabled ? 1U : 0U;
}

void EcatEscHpm::UnmaskEvents(uint32_t token)
{
	const bool enable = (token != 0U);

	hpm_esc_irq_enable(dev_, enable);
	irq_enabled_ = enable;
}

void EcatEscHpm::OnIrq(const struct device* dev, void* user_data)
{
	(void)dev;

	if (g_instance == nullptr) {
		return;
	}

	const uintptr_t irq_number = reinterpret_cast<uintptr_t>(user_data) & kIrqMask;

	g_instance->Dispatch(MapIrq(static_cast<enum hpm_esc_irq>(irq_number)));
}

void EcatEscHpm::Dispatch(EcatEvent event)
{
	if (handler_ != nullptr) {
		handler_(context_, event);
	}
}

void EcatEscHpm::DispatchTick()
{
	Dispatch(EcatEvent::Tick1ms);
}

}

/**
 * @brief 端口层诊断输出转发
 *
 * @param text 以 '\0' 结尾的字符串
 */
void ecat_port_log(const char* text)
{
	shell::Log::SendCommandLine(text);
}
