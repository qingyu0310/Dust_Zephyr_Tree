/**
 * @file ecat_hal_hpm.hpp
 * @author qingyu
 * @brief HPMicro ESC HAL：寄存器、DPRAM、事件、MDIO、SII
 * @version 0.4
 * @date 2026-10-31
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <cstdint>
#include <zephyr/device.h>

#include "ecat_esc.hpp"

namespace ecat {

/**
 * @brief HPMicro ESC HAL
 */
class EcatEscHpm final : public ::EcatEsc
{
public:
	EcatEscHpm() = default;
	~EcatEscHpm() override = default;

	EcatEscHpm(const EcatEscHpm&) = delete;
	EcatEscHpm& operator=(const EcatEscHpm&) = delete;

	bool Open(const Config& config, EventHandler handler, void* context) override;
	void Close() 	   override;
	void StartEvents() override;
	void StopEvents()  override;

	void Read(uint16_t address, void* data, uint16_t length) override;
	void Write(uint16_t address, const void* data, uint16_t length) override;
	uint16_t ReadAlEvent() override;

	bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* value) override;
	bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t value) override;
	void SetLinkState(uint8_t port, bool up) override;

	SiiMode GetSiiMode() const override;
	bool ReadSiiWord(uint32_t word_address, uint8_t* data, uint16_t length) override;
	bool WriteSiiWord(uint32_t word_address, const uint8_t* data, uint16_t length) override;
	bool ReloadSii() override;
	void StoreSii()  override;

	uint32_t MaskEvents() override;
	void UnmaskEvents(uint32_t token) override;

	void DispatchTick();											// 1 ms 节拍转发，供匿名命名空间的定时器 thunk 调用

private:
	static void OnPdiIrq  (const device* dev, void* user_data);		// Zephyr 中断回调，user_data 是实例
	static void OnSync0Irq(const device* dev, void* user_data);
	static void OnSync1Irq(const device* dev, void* user_data);
	static void OnResetIrq(const device* dev, void* user_data);
	static void DispatchThunk(void* user_data, EcatEvent event);

	bool HardwareInit();											// 硬件初始化：SII 介质、EEPROM 装载、PHY 复位与配置
	void Dispatch(EcatEvent event);									// 把事件转发给上层

	const device* dev_    = DEVICE_DT_GET(DT_NODELABEL(esc0));		// ESC 设备

	EventHandler handler_ = nullptr;								// 事件回调

	void* context_ 		  = nullptr;								// 回调上下文
	bool  opened_ 		  = false;									// 已打开
	bool  irq_enabled_ 	  = false;									// 中断已使能
	bool  sii_ready_      = false;									// SII 介质已初始化
};

}
