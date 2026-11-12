/**
 * @file ecat_types.hpp
 * @author qingyu
 * @brief EtherCAT 协议基础类型，不依赖 SSC、不依赖任何厂商头
 * @version 0.2
 * @date 2026-10-30
 *
 * 只放 EtherCAT 规范（ETG.1000）定义的数据类型。
 * 不放寄存器、不放 SSC 结构体、不放任何 hpm_ 前缀符号。
 *
 * **本文件属于共享契约层**（见 `contract/README.md`）：底层实现可依赖它，
 * 上层协议栈可依赖它。P2 步 14 会迁到 `zephyr/contract/`。
 *
 * @copyright Copyright (c) 2026
 */

#pragma once

#include <stdint.h>

/**
 * @brief EtherCAT 应用层状态（0x0130 低四位）
 *
 * 低四位是状态本身；0x10 位在 0x0120 里表示"确认错误"，不属于状态取值。
 * 因此这里只列四个合法状态，错误确认位由常量单独给出。
 */
enum class AlState : uint8_t
{
	Init   = 0x01,                          	// 初始化
	PreOp  = 0x02,                          	// 预运行（邮箱可用）
	SafeOp = 0x04,                          	// 安全运行（输入有效）
	Op     = 0x08,                          	// 运行（输入输出都有效）
	Boot   = 0x03,                          	// 引导（固件升级）
};

/**
 * @brief AL Control 寄存器（0x0120）的位掩码
 *
 * 低四位是目标状态，其余位是控制语义，不参与状态比较。
 */
namespace al_control {

constexpr uint8_t kStateMask        = 0x0F; 	// 目标状态掩码
constexpr uint8_t kErrorAck         = 0x10; 	// 主站确认错误位
constexpr uint8_t kDeviceIdRequest  = 0x20; 	// 请求显式设备 ID（0x0120 位 5）

}

/**
 * @brief AL Status Code（0x0134）
 *
 * 只列本从站会返回的那些；未列出的按规范数值直接写。
 */
namespace al_status {

constexpr uint16_t kNoError                     = 0x0000; 				// 无错误
constexpr uint16_t kUnspecifiedError            = 0x0001; 				// 未指明的错误
constexpr uint16_t kNoMemory                    = 0x0002; 				// 内存不足
constexpr uint16_t kInvalidAlControl            = 0x0011; 				// 非法状态转换请求
constexpr uint16_t kUnknownAlControl            = 0x0012; 				// 未知目标状态
constexpr uint16_t kBootNotSupported            = 0x0013; 				// 不支持 BOOT
constexpr uint16_t kInvalidMbxCfgInPreOp        = 0x0016; 				// PREOP 下邮箱配置非法
constexpr uint16_t kEeError                     = 0x0029; 				// EEPROM 未正确加载，需重启设备
constexpr uint16_t kInvalidSmCfg                = 0x0017; 				// 同步管理器配置非法
constexpr uint16_t kNoValidInputs               = 0x0018; 				// 没有有效输入
constexpr uint16_t kNoValidOutputs              = 0x0019; 				// 没有有效输出
constexpr uint16_t kSyncError                   = 0x001A; 				// 同步错误
constexpr uint16_t kSmWatchdog                  = 0x001B; 				// 同步管理器看门狗
constexpr uint16_t kSyncTypesNotCompatible      = 0x001C; 				// 同步类型不兼容
constexpr uint16_t kInvalidSmOutCfg             = 0x001D; 				// 输出同步管理器配置非法
constexpr uint16_t kInvalidSmInCfg              = 0x001E; 				// 输入同步管理器配置非法
constexpr uint16_t kInvalidWdCfg                = 0x001F; 				// 看门狗配置非法
constexpr uint16_t kInvalidInputMapping         = 0x0024; 				// 输入映射非法
constexpr uint16_t kInvalidOutputMapping        = 0x0025; 				// 输出映射非法
constexpr uint16_t kInconsistentSettings        = 0x0026; 				// 设置不一致
constexpr uint16_t kFreeRunNotSupported         = 0x0027; 				// 不支持自由运行
constexpr uint16_t kSyncModeNotSupported        = 0x0028; 				// 不支持该同步模式
constexpr uint16_t kDcInvalidSyncCfg            = 0x0030; 				// DC 同步配置非法
constexpr uint16_t kDcSync0CycleTime            = 0x0036; 				// DC Sync0 周期非法
constexpr uint16_t kDcSync1CycleTime            = 0x0037; 				// DC Sync1 周期非法

}

/**
 * @brief 状态机内部返回值：转换尚未完成，需要应用侧继续推进
 *
 * 对应 SSC 的 NOERROR_INWORK（0xFF），只在状态机内部流转，不写进 AL Status Code。
 */
constexpr uint16_t kAlStatusInWork = 0x00FF;

/**
 * @brief 事件源
 *
 * Pdi 合并了 AL Control、邮箱、过程数据、EEPROM 命令四类事件，
 * 具体是哪一个由 0x0220 的位决定；这里只给出"谁进来通知"的入口。
 */
enum class EcatEvent : uint8_t
{
	Pdi    = 0,                             		// PDI 中断（AL/邮箱/过程数据/EEPROM 命令）
	Sync0  = 1,                             		// 分布式时钟 SYNC0
	Sync1  = 2,                             		// 分布式时钟 SYNC1
	Reset  = 3,                             		// ESC 复位
	Tick1ms = 4,                            		// 1 ms 定时事件（无硬件中断，由定时器产生）
};

/**
 * @brief 事件源旧名，切换（P7 步骤 23）之后删除
 */
using EcatIrq = EcatEvent;

/**
 * @brief AL Event Request 寄存器（0x0220）的位掩码
 */
namespace al_event {

constexpr uint16_t kAlControl       = 0x0001; 		// AL Control 事件
constexpr uint16_t kSync0           = 0x0004; 		// SYNC0 事件
constexpr uint16_t kSync1           = 0x0008; 		// SYNC1 事件
constexpr uint16_t kSmChange        = 0x0010; 		// 同步管理器变化
constexpr uint16_t kEepromCommand   = 0x0020; 		// EEPROM 命令待处理
constexpr uint16_t kMailboxWrite    = 0x0100; 		// 邮箱写事件（主站→从站）
constexpr uint16_t kMailboxRead     = 0x0200; 		// 邮箱读事件（从站→主站）
constexpr uint16_t kProcessOutput   = 0x0400; 		// 过程数据输出写入事件
constexpr uint16_t kProcessInput    = 0x0800; 		// 过程数据输入读取事件

}

/**
 * @brief 同步管理器通道编号
 */
namespace sync_manager {

constexpr uint8_t kMailboxOut = 0;      			// SM0：邮箱写（主站→从站）
constexpr uint8_t kMailboxIn  = 1;      			// SM1：邮箱读（从站→主站）
constexpr uint8_t kProcessOut = 2;      			// SM2：过程数据输出（主站→从站）
constexpr uint8_t kProcessIn  = 3;      			// SM3：过程数据输入（从站→主站）

}

/**
 * @brief 同步类型（0x1C32:01 / 0x1C33:01）
 */
enum class SyncType : uint16_t
{
	FreeRun        = 0x0000,                		// 自由运行
	SmSynchron     = 0x0001,                		// 同步到对应同步管理器的 SM 事件
	DcSync0        = 0x0002,                		// 同步到 SYNC0
	DcSync1        = 0x0003,                		// 同步到 SYNC1
	Sm2Synchron    = 0x0022,                		// 同步到 SM2 事件（仅 0x1C33:01 合法）
};

/**
 * @brief 邮箱协议类型（邮箱头 Flags[1] 低四位）
 */
enum class MailboxProtocol : uint8_t
{
	None = 0,                               		// 未指定
	AoE  = 1,                               		// ADS over EtherCAT
	EoE  = 2,                               		// Ethernet over EtherCAT
	CoE  = 3,                               		// CANopen over EtherCAT
	FoE  = 4,                               		// File over EtherCAT
	SoE  = 5,                               		// Servo over EtherCAT
	VoE  = 15,                              		// Vendor over EtherCAT
};

/**
 * @brief PHY 速率
 */
enum class EcatPhySpeed : uint8_t
{
	Mbps10   = 0,                           		// 10 Mbit/s
	Mbps100  = 1,                           		// 100 Mbit/s
	Mbps1000 = 2,                           		// 1000 Mbit/s
};

/**
 * @brief 过程数据布局（SM2 / SM3 字节数）
 */
struct PdoLayout
{
	uint16_t rx_size = 0;                   		// 主站→从站（SM2 / RxPDO）
	uint16_t tx_size = 0;                   		// 从站→主站（SM3 / TxPDO）
};

/**
 * @brief 端口链路状态
 */
struct EcatPortState
{
	bool link[3] = { false, false, false }; // 各端口链路
};
