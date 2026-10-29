/**
 * @file ecat_hal_hpm.hpp
 * @author qingyu
 * @brief HPMicro ESC 硬件抽象层
 * @version 0.1
 * @date 2026-10-29
 *
 * 包装子树 hal/ports/ 下的 C 端口层，持有由 dts 决定的 EcatPhy* 数组。
 * 不含任何 PHY 芯片逻辑（那在 hal/ports/phy/<芯片名>/ 里按芯片分文件）。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

#include "ecat_hal.hpp"
#include "ecat_phy.hpp"

/**
 * @brief HPMicro EtherCAT 硬件抽象层
 *
 * 只做转发：把 EcatHal 的虚函数转到端口层的 C 函数。
 * PHY 配置与链路轮询遍历 phys_[]，不知道具体是哪颗芯片。
 */
class EcatHalHpm final : public EcatHal
{
public:
	EcatHalHpm() = default;

	bool Init(const Config& cfg, EventCallback callback, void* context) override;
	bool Start() override;
	void Stop() override;

	void Read(uint16_t addr, void* buf, uint16_t len) override;
	void Write(uint16_t addr, const void* buf, uint16_t len) override;
	uint16_t AlEvent() override;

	bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* value) override;
	bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t value) override;

	void SetLink(uint8_t port, bool up) override;
	void UpdateLink() override;

	uint16_t EepromRead(uint32_t word_addr) override;
	void     EepromWrite(uint32_t word_addr, uint16_t data) override;
	uint32_t EepromReload() override;

	uint32_t TimerGet() override;
	void     TimerClear() override;

	void EnterCritical() override;
	void LeaveCritical() override;

private:
	/** 最多支持的 PHY 数（ESC 三端口） */
	static constexpr uint8_t kMaxPorts = 3;

	EcatPhy*  phys_[kMaxPorts] = { nullptr, nullptr, nullptr };
	PhyConfig phy_cfgs_[kMaxPorts] {};      // 从 dts 读出的每颗 PHY 参数
	uint8_t   phy_count_ = 0;
	bool      ready_ = false;

	/** 中断回调（由 ecat_irq_init 注册到端口层） */
	static EventCallback irq_callback_;
	static void*         irq_context_;

	static void OnPortIrq(void* context, EcatIrq irq);
};
