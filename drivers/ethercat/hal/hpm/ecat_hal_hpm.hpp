/**
 * @file ecat_hal_hpm.hpp
 * @author qingyu
 * @brief HPMicro ESC HAL：寄存器、DPRAM、事件、MDIO、SII
 * @version 0.1
 * @date 2026-10-31
 *
 * 一个平台一个文件，与 `drivers/usb/hal/hpm/usb_hal_hpm.hpp` 同构。
 * 这是唯一允许 include 厂商头、调 `hpm_esc_*` 的地方；协议核心只通过
 * `EcatEsc` 契约认识它。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>
#include <cstdio>

#include "ecat_esc.hpp"

struct device;

extern "C" void ecat_port_log(const char* text);

/**
 * @brief 格式化并输出一条诊断
 */
#define ECAT_LOG(...)												\
	do {															\
		char _ecat_text[160];										\
		std::snprintf(_ecat_text, sizeof(_ecat_text), __VA_ARGS__);	\
		ecat_port_log(_ecat_text);									\
	} while (0)

namespace ecat {

/**
 * @brief HPMicro ESC HAL
 */
class EcatEscHpm final : public ::EcatEsc
{
public:
	EcatEscHpm();
	~EcatEscHpm() override;

	EcatEscHpm(const EcatEscHpm&) = delete;
	EcatEscHpm& operator=(const EcatEscHpm&) = delete;

	bool Open(const Config& config, EventHandler handler, void* context) override;
	void Close() override;
	void StartEvents() override;
	void StopEvents() override;

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
	void StoreSii() override;

	uint32_t MaskEvents() override;
	void UnmaskEvents(uint32_t token) override;

	/**
	 * @brief 1 ms 节拍转发
	 *
	 * 由静态定时器回调调用，需要是 public（定时器 thunk 在匿名命名空间里）。
	 */
	void DispatchTick();

private:
	/**
	 * @brief Zephyr 中断回调入口（静态 thunk）
	 *
	 * 设备驱动只给 `(dev, user_data)`，没有实例指针，所以 `user_data`
	 * 存的是中断号，实例指针放在 `g_instance` 里。
	 */
	static void OnIrq(const struct device* dev, void* user_data);

	/**
	 * @brief 硬件初始化：SII 介质、EEPROM 装载、PHY 复位与配置
	 * @return true=成功
	 */
	bool HardwareInit();

	/**
	 * @brief 把事件转发给上层
	 */
	void Dispatch(EcatEvent event);

	const struct device* dev_ = nullptr;	// ESC 设备
	EventHandler handler_ = nullptr;		// 事件回调
	void* context_ = nullptr;				// 回调上下文
	bool opened_ = false;					// 已打开
	bool irq_enabled_ = false;				// 中断已使能
	bool sii_ready_ = false;				// SII 介质已初始化
};

}
