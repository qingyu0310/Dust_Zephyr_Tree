/**
 * @file ecat_esc.hpp
 * @author qingyu
 * @brief ESC 能力契约：寄存器、DPRAM、事件、MDIO、SII
 * @version 0.2
 * @date 2026-10-31
 *
 * 这是协议核心唯一允许依赖的硬件接口。零厂商符号、零寄存器定义、
 * 零协议状态——只有"能读写内存"、"能收发中断"、"能访问 SII"这几件事。
 *
 * 与旧 `EcatHal` 的差别（每条都是具体的病）：
 *
 *   | 旧接口 | 问题 | 新接口 |
 *   |---|---|---|
 *   | `TimerGet()` / `TimerClear()` | 模拟"全局时基"，但语义就是取毫秒 | 删掉，用 `k_uptime_get()` |
 *   | `EnterCritical()` / `LeaveCritical()` | 无返回值，嵌套就错 | `MaskEvents()` 返回 token + `UnmaskEvents(token)` |
 *   | `AlEvent()` | 名字没体现"读即确认" | `ReadAlEvent()` |
 *   | `EepromRead/Write/Reload` | 空壳接口，两种介质在内部选即可 | 并入本接口，模式由 `Config::sii_mode` 决定 |
 *
 * **本文件属于共享契约层**（见 `contract/README.md`）：底层实现可依赖它，
 * 上层协议栈可依赖它。P2 步 14 会迁到 `zephyr/contract/`。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <cstdint>

#include "ecat_types.hpp"

/**
 * @brief ESC 硬件能力
 */
class EcatEsc
{
public:
	/**
	 * @brief 事件回调
	 * @param context 回调上下文
	 * @param event   事件源
	 */
	using EventHandler = void (*)(void* context, EcatEvent event);

	/**
	 * @brief SII 介质模式
	 */
	enum class SiiMode : uint8_t
	{
		Emulated,	// Flash 分区模拟，应用接管读写
		OnBoard,	// 板上 I2C EEPROM，ESC 自己走 I2C 主控
	};

	/**
	 * @brief 打开硬件所需的配置
	 */
	struct Config
	{
		uint8_t port_count = 2;					// 使能端口数（1~3）
		SiiMode sii_mode   = SiiMode::OnBoard;	// SII 介质模式
	};

	virtual ~EcatEsc() = default;

	/**
	 * @brief 打开 ESC：时钟、引脚、复位、中断注册
	 *
	 * 打开后**不启用**任何事件，由 `StartEvents` 显式启动。
	 *
	 * @param config   硬件配置
	 * @param handler  事件回调
	 * @param context  回调上下文
	 * @return true=成功
	 */
	virtual bool Open(const Config& config, EventHandler handler, void* context) = 0;

	/**
	 * @brief 关闭 ESC，停用全部事件
	 */
	virtual void Close() = 0;

	/**
	 * @brief 开始上报事件
	 */
	virtual void StartEvents() = 0;

	/**
	 * @brief 停止上报事件
	 */
	virtual void StopEvents() = 0;

	/**
	 * @brief 读 ESC 寄存器或 DPRAM
	 * @param address 偏移
	 * @param data    目标缓冲
	 * @param length  字节数
	 */
	virtual void Read(uint16_t address, void* data, uint16_t length) = 0;

	/**
	 * @brief 写 ESC 寄存器或 DPRAM
	 * @param address 偏移
	 * @param data    源缓冲
	 * @param length  字节数
	 */
	virtual void Write(uint16_t address, const void* data, uint16_t length) = 0;

	/**
	 * @brief 读 AL Event Request（0x0220）
	 *
	 * **读取即确认**：读一次就把事件位清掉。调用方必须一次读完，
	 * 不许"先探一下有没有、过一会儿再处理"。
	 *
	 * @return 事件位
	 */
	virtual uint16_t ReadAlEvent() = 0;

	/**
	 * @brief MDIO 读（PHY 寄存器）
	 */
	virtual bool MdioRead(uint8_t phy, uint8_t reg, uint16_t* value) = 0;

	/**
	 * @brief MDIO 写
	 */
	virtual bool MdioWrite(uint8_t phy, uint8_t reg, uint16_t value) = 0;

	/**
	 * @brief 读端口 PHY 的 MDIO 地址
	 *
	 * 地址由板卡原理图 strap 决定，与端口号不一定相等。
	 * 板级 dts 的 `phy-addresses` 属性给出，缺省时退化为端口号。
	 *
	 * @param port 端口号（0 起）
	 * @return MDIO 地址
	 */
	virtual uint8_t GetPhyAddress(uint8_t port) const = 0;

	/**
	 * @brief 把指定端口的链路状态写进 ESC
	 * @param port 端口号（0 起）
	 * @param up   true=链路建立
	 */
	virtual void SetLinkState(uint8_t port, bool up) = 0;

	/**
	 * @brief 当前 SII 介质模式
	 */
	virtual SiiMode GetSiiMode() const = 0;

	/**
	 * @brief 读一段 SII 数据
	 * @param word_address SII 字地址
	 * @param data         目标缓冲
	 * @param length       字节数（4 或 8）
	 * @return true=成功
	 */
	virtual bool ReadSiiWord(uint32_t word_address, uint8_t* data, uint16_t length) = 0;

	/**
	 * @brief 写一段 SII 数据
	 * @param word_address SII 字地址
	 * @param data         源缓冲
	 * @param length       字节数
	 * @return true=成功
	 */
	virtual bool WriteSiiWord(uint32_t word_address, const uint8_t* data, uint16_t length) = 0;

	/**
	 * @brief 重载 SII（把介质内容重新装载到 ESC 的数据窗口）
	 * @return true=成功
	 */
	virtual bool ReloadSii() = 0;

	/**
	 * @brief 落盘（Flash 模拟模式才需要）
	 */
	virtual void StoreSii() = 0;

	/**
	 * @brief 屏蔽事件源
	 * @return 用于恢复的 token
	 */
	virtual uint32_t MaskEvents() = 0;

	/**
	 * @brief 恢复事件源
	 * @param token `MaskEvents` 的返回值
	 */
	virtual void UnmaskEvents(uint32_t token) = 0;
};
