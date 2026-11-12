/**
 * @file ecat_profile.hpp
 * @author qingyu
 * @brief 过程数据布局契约：业务类型在编译期折算成字节数与字数
 * @version 0.1
 * @date 2026-11-02
 *
 * 业务结构体留在用户区，核心只认字节与字数。本文件是两者的唯一定义转换点。
 *
 * **本文件属于共享契约层**（见 `contract/README.md`）：底层实现可依赖它，
 * 上层协议栈可依赖它。P2 步 14 会迁到 `zephyr/contract/`。
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <cstdint>
#include <type_traits>

#include "spec/ecat_limits.hpp"

namespace ecat {

/**
 * @brief 单方向的 PDO 布局契约
 * @tparam T 业务结构体
 */
template <typename T>
struct PdoProfile
{
	static_assert(std::is_trivially_copyable_v<T>,    "PDO 类型必须可平凡复制");
	static_assert(sizeof(T)  % sizeof(uint32_t) == 0, "PDO 必须按 32 位量对齐");
	static_assert(sizeof(T) <= kMaxProcessBytes,      "PDO 超过单方向容量上限");

	static constexpr uint16_t kBytes   = sizeof(T);						// 线上字节数
	static constexpr uint8_t  kWords   = sizeof(T) / sizeof(uint32_t);	// 32 位字数
	static constexpr uint16_t kCaBytes = 2 + 4 * kWords;				// CA 长度：子索引 0 占 2 字节
};

/**
 * @brief 从站 Profile：两个方向由模板参数固定
 * @tparam Rx 主站→从站（SM2 / RxPDO）
 * @tparam Tx 从站→主站（SM3 / TxPDO）
 */
template <typename Rx, typename Tx>
struct SlaveProfile
{
	using Outputs = Rx;
	using Inputs  = Tx;

	static constexpr uint16_t kRxBytes   = PdoProfile<Rx>::kBytes;
	static constexpr uint16_t kTxBytes   = PdoProfile<Tx>::kBytes;
	static constexpr uint8_t  kRxWords   = PdoProfile<Rx>::kWords;
	static constexpr uint8_t  kTxWords   = PdoProfile<Tx>::kWords;
	static constexpr uint16_t kRxCaBytes = PdoProfile<Rx>::kCaBytes;
	static constexpr uint16_t kTxCaBytes = PdoProfile<Tx>::kCaBytes;

	static constexpr uint16_t kMaxCaBytes = (kRxCaBytes > kTxCaBytes) ? kRxCaBytes : kTxCaBytes;	// 整对象 CA 上限，供 SDO 缓冲定长
};

/**
 * @brief 运行期传给核心的布局值（非模板）
 */
struct DeviceProfile
{
	uint16_t rx_bytes = 0;	// 主站→从站字节数
	uint16_t tx_bytes = 0;	// 从站→主站字节数
	uint8_t  rx_words = 0;	// 主站→从站 32 位字数
	uint8_t  tx_words = 0;	// 从站→主站 32 位字数
};

}
