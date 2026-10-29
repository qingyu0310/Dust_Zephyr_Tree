/**
 * @file ecat_phy_bind.cpp
 * @author qingyu
 * @brief 从 dts 构造 PHY 驱动实例
 * @version 0.1
 * @date 2026-10-29
 *
 * 编译期完成全部绑定：compatible 选驱动、属性填参数、静态存储。
 * 没有运行期字符串比较，也没有任何板卡判断。
 *
 * dts 的节点标识只能在 DT_* 宏里用，不能当模板参数，
 * 所以这里用宏展开而不是模板元编程。
 *
 * @copyright Copyright (c) 2026
 */

#include "ecat_phy_bind.hpp"

#include <zephyr/devicetree.h>
#include <new>

#include "ecat_zephyr_port.h"
#include "jl1111/phy_jl1111.hpp"
#include "rtl8201f/phy_rtl8201f.hpp"
#include "tja1101b/phy_tja1101b.hpp"

namespace
{
	/** 最多支持的 PHY 数（ESC 三端口） */
	constexpr uint8_t kMaxPhy = 3;

	/** 单个 PHY 对象的最大尺寸/对齐（三者取大） */
	constexpr size_t kPhyObjSize  = sizeof(PhyJl1111) > sizeof(PhyRtl8201f)
						? (sizeof(PhyJl1111) > sizeof(PhyTja1101b)
							   ? sizeof(PhyJl1111)
							   : sizeof(PhyTja1101b))
						: (sizeof(PhyRtl8201f) > sizeof(PhyTja1101b)
							   ? sizeof(PhyRtl8201f)
							   : sizeof(PhyTja1101b));
	constexpr size_t kPhyObjAlign = alignof(PhyJl1111) > alignof(PhyRtl8201f)
						? (alignof(PhyJl1111) > alignof(PhyTja1101b)
							   ? alignof(PhyJl1111)
							   : alignof(PhyTja1101b))
						: (alignof(PhyRtl8201f) > alignof(PhyTja1101b)
							   ? alignof(PhyRtl8201f)
							   : alignof(PhyTja1101b));

	/** 静态存储池：所有 PHY 类型共用，不引入堆 */
	alignas(kPhyObjAlign) uint8_t s_phy_pool[kMaxPhy][kPhyObjSize];

	/**
	 * @brief 取第 slot 个槽位
	 */
	void* PhySlot(uint8_t slot)
	{
		return (slot < kMaxPhy) ? s_phy_pool[slot] : nullptr;
	}
}

/**
 * @brief 从 PHY 节点读配置
 * @param node_id  dts 节点标识（DT_* 宏展开）
 */
#define ECAT_PHY_READ_CFG(node_id)                                                     \
	[]() -> PhyConfig {                                                             \
		PhyConfig c {};                                                         \
		c.mdio_address = static_cast<uint8_t>(DT_PROP(node_id, mdio_address));   \
		c.advertise    = static_cast<uint16_t>(DT_PROP_OR(node_id, advertise, 0x0100)); \
		c.autoneg      = DT_NODE_HAS_PROP(node_id, autoneg);                     \
		c.interface    = static_cast<uint8_t>(DT_ENUM_IDX_OR(node_id, phy_mode, 0)); \
		c.led_mode     = static_cast<uint8_t>(DT_PROP_OR(node_id, led_mode, 0)); \
		c.speed        = static_cast<EcatPhySpeed>(DT_ENUM_IDX_OR(node_id, speed, 1)); \
		c.full_duplex  = DT_ENUM_IDX_OR(node_id, duplex, 1) == 1;                \
		return c;                                                               \
	}()

/**
 * @brief 按 compatible 构造驱动实例
 * @param node_id  dts 节点标识
 * @param slot     静态池槽位
 */
#define ECAT_PHY_MAKE(node_id, slot)                                                   \
	(IS_ENABLED(DT_NODE_HAS_COMPAT(node_id, hpmicro_ecat_phy_jl1111))                \
		 ? static_cast<EcatPhy*>(                                               \
			   new (PhySlot(slot)) PhyJl1111(hal, cfg))                      \
	 : IS_ENABLED(DT_NODE_HAS_COMPAT(node_id, hpmicro_ecat_phy_rtl8201f))            \
		 ? static_cast<EcatPhy*>(                                               \
			   new (PhySlot(slot)) PhyRtl8201f(hal, cfg))                    \
	 : IS_ENABLED(DT_NODE_HAS_COMPAT(node_id, hpmicro_ecat_phy_tja1101b))            \
		 ? static_cast<EcatPhy*>(                                               \
			   new (PhySlot(slot)) PhyTja1101b(hal, cfg))                    \
		 : nullptr)

/**
 * @brief 绑定 dts 里第 idx 个 PHY
 */
#define ECAT_PHY_BIND_ONE(idx)                                                         \
	do {                                                                            \
		const PhyConfig cfg = ECAT_PHY_READ_CFG(DT_PROP_BY_IDX(DT_NODELABEL(esc0), phys, idx)); \
		EcatPhy* phy = ECAT_PHY_MAKE(DT_PROP_BY_IDX(DT_NODELABEL(esc0), phys, idx), (idx)); \
		if (phy == nullptr) {                                                   \
			ECAT_LOG("ecat: phy[%u] compatible not supported\n",            \
				 static_cast<unsigned>(idx));                           \
		} else {                                                                \
			phys[phy_count] = phy;                                          \
			cfgs[phy_count] = cfg;                                          \
			phy_count++;                                                    \
		}                                                                       \
	} while (0)

/**
 * @brief 从 dts 的 esc0 phys 属性构造 PHY 驱动
 * @param hal       硬件抽象层
 * @param phys      输出：驱动实例指针数组
 * @param cfgs      输出：各 PHY 的配置
 * @param max_count 数组容量
 * @return 实际构造的数量
 */
uint8_t ecat_phy_bind(EcatHal& hal, EcatPhy** phys, PhyConfig* cfgs, uint8_t max_count)
{
#if DT_NODE_HAS_PROP(DT_NODELABEL(esc0), phys)

	uint8_t phy_count = 0;

	/* 必须用预处理器 #if：DT_PROP_BY_IDX 的索引越界会在编译期报错，
	 * 运行期 if 挡不住。 */
#if DT_PROP_LEN(DT_NODELABEL(esc0), phys) > 0
	if (max_count > 0U) {
		ECAT_PHY_BIND_ONE(0);
	}
#endif
#if DT_PROP_LEN(DT_NODELABEL(esc0), phys) > 1
	if (max_count > 1U) {
		ECAT_PHY_BIND_ONE(1);
	}
#endif
#if DT_PROP_LEN(DT_NODELABEL(esc0), phys) > 2
	if (max_count > 2U) {
		ECAT_PHY_BIND_ONE(2);
	}
#endif

	return phy_count;

#else

	(void)hal;
	(void)phys;
	(void)cfgs;
	(void)max_count;
	ECAT_LOG("ecat: esc0 has no phys property, no PHY driver bound\n");
	return 0;

#endif
}
