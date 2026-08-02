# Zephyr HPM SDK 底层修改详解

> 承接《[zephyr_HPM_搭建指南.md](./zephyr_HPM_搭建指南.md)》第 7 章。那一章只列了"有哪些底层 bug、
> 放到最后做"；这一篇把**每个修改改哪个文件、为什么、贴什么代码**讲全，照着复制即可。
>
> **建议顺序**：先把《搭建指南》第 4/5 章的编译必需链做完、`dust build hpm5361icb` 出固件 → 再逐项
> 应用下面的修改 → 增量编译 → 烧录验证。**每个修改都标了所属仓库，别改错地方。**

---

## 目录

- [1. zephyr：PLIC 伪中断 acknowledge（intc_plic.c）](#1-zephyrplic-伪中断-acknowledgeintc_plicc)
- [2. sdk_env：DLM/ILM 地址换算（hpm_misc.h）](#2-sdk_envdlmilmmisc.h)
- [3. sdk_glue：UART 硬件 RX idle（uart_hpmicro.c + Kconfig.hpmicro）](#3-sdk_glueuart-rx-idle)
- [4. CherryUSB：ARRAY_SIZE 宏冲突（usb_osal_zephyr.c）](#4-cherryusbarray_sizeusb_osal_zephyrc)
- [5. sdk_glue：CAN 自旋/波特率/状态（mcan_hpmicro.c）](#5-sdk_gluecanmcan_hpmicroc)
- [6. sdk_glue：GPIO 中断管理（gpio_hpmicro.c）](#6-sdk_gluegpiogpio_hpmicroc)
- [7. sdk_glue：UDC 真正连接/断开（udc_hpmicro.c）](#7-sdk_glueudcudc_hpmicroc)

---

## 1. zephyr：PLIC 伪中断 acknowledge（intc_plic.c）

**文件**：`E:\Zephyr\zephyr\drivers\interrupt_controller\intc_plic.c`
**位置**：`plic_irq_handler()`，`_sw_isr_table` 分发处（约 550-561 行）
**为什么**：读 claim 寄存器拿到 `local_irq` 后，原代码**无条件**调用 `ite->isr()`。若该 IRQ 在
`_sw_isr_table` 里没注册 ISR，`isr` 指向 `z_irq_spurious`——它不返回、也不写 claim_complete，
导致 level 触发的中断永远无法 acknowledge，PLIC 认为中断还在处理中，**中断线挂死**，后续同源中断
无法再触发。修复在分发前判断 `isr == z_irq_spurious`，先写 claim_complete 完成 acknowledge 再 return。

**改前**：

```c
	/* Call the corresponding IRQ handler in _sw_isr_table */
	ite = &config->isr_table[local_irq];
	ite->isr(ite->arg);

	/*
	 * Write to claim_complete register to indicate to
	 * PLIC controller that the IRQ has been handled
	 * for level triggered interrupts.
	 */
#ifdef CONFIG_PLIC_SUPPORTS_TRIG_EDGE
	/* Handle only if level-triggered */
	if (trig_val == PLIC_TRIG_LEVEL) {
		sys_write32(local_irq, claim_complete_addr);
	}
#else
	sys_write32(local_irq, claim_complete_addr);
#endif /* #ifdef CONFIG_PLIC_SUPPORTS_TRIG_EDGE */
```

**改后**（在调用 isr 前加伪中断判断，命中则 acknowledge 后 return）：

```c
	/* Call the corresponding IRQ handler in _sw_isr_table */
	ite = &config->isr_table[local_irq];
	if (ite->isr == z_irq_spurious) {
#ifdef CONFIG_PLIC_SUPPORTS_TRIG_EDGE
		if (trig_val == PLIC_TRIG_LEVEL) {
			sys_write32(local_irq, claim_complete_addr);
		}
#else
		sys_write32(local_irq, claim_complete_addr);
#endif
		return;
	}
	ite->isr(ite->arg);

	/*
	 * Write to claim_complete register to indicate to
	 * PLIC controller that the IRQ has been handled
	 * for level triggered interrupts.
	 */
#ifdef CONFIG_PLIC_SUPPORTS_TRIG_EDGE
	/* Handle only if level-triggered */
	if (trig_val == PLIC_TRIG_LEVEL) {
		sys_write32(local_irq, claim_complete_addr);
	}
#else
	sys_write32(local_irq, claim_complete_addr);
#endif /* #ifdef CONFIG_PLIC_SUPPORTS_TRIG_EDGE */
```

> `z_irq_spurious` 是 v4.3 标准符号（`riscv/arch.h`），纯新增判断、不动 Kconfig/CMake/DT，
> 编译结果与不修完全一致，所以可以放到最后打。

---

## 2. sdk_env：DLM/ILM 地址换算（hpm_misc.h）

**文件**：`E:\Zephyr_HPMicro\sdk_env\hpm_sdk\soc\HPM5300\HPM5361\hpm_misc.h`
**位置**：文件顶部宏区之后的两个 static inline 函数（约 38-58 行）
**为什么**：HPM5361 原版这两个函数只透传（`return addr;`），无法把 core 本地 DLM（0x80000-0xFFFFF）/
ILM（0x0-0x1FFFF）换算成 system 侧地址（DLM→0x1060000、ILM→0x1040000）。Zephyr 里依赖
`core_local_mem_to_sys_address` 做指针换算（共享内存/缓存刷写路径）时拿到的是本地地址，导致
DMA/USB 访问 core-local 内存地址错。文件顶部本就定义了 `DLM_TO_SYSTEM`/`ILM_TO_SYSTEM`/
`SYSTEM_TO_DLM`/`ADDRESS_IN_*` 这些宏，补丁只是把换算逻辑用起来。

**改前**：

```c
/* map core local memory(DLM/ILM) to system address */
static inline uint32_t core_local_mem_to_sys_address(uint8_t core_id, uint32_t addr)
{
    (void) core_id;
    return addr;
}

/* map system address to core local memory(DLM/ILM) */
static inline uint32_t sys_address_to_core_local_mem(uint8_t core_id, uint32_t addr)
{
    (void) core_id;
    return addr;
}
```

**改后**：

```c
/* map core local memory(DLM/ILM) to system address */
static inline uint32_t core_local_mem_to_sys_address(uint8_t core_id, uint32_t addr)
{
    (void) core_id;
    if (ADDRESS_IN_DLM(addr)) {
        return DLM_TO_SYSTEM(addr);
    }
    if (ADDRESS_IN_ILM(addr)) {
        return ILM_TO_SYSTEM(addr);
    }
    return addr;
}

/* map system address to core local memory(DLM/ILM) */
static inline uint32_t sys_address_to_core_local_mem(uint8_t core_id, uint32_t addr)
{
    (void) core_id;
    if (ADDRESS_IN_CORE0_DLM_SYSTEM(addr)) {
        return SYSTEM_TO_DLM(addr);
    }
    return addr;
}
```

> 只改函数体，宏定义区不动。`ADDRESS_IN_*`/`*_TO_SYSTEM` 这些宏已在 5361 头文件里定义，
> 加不加都能编译——是纯运行时内存换算修复。

---

## 3. sdk_glue：UART 硬件 RX idle（uart_hpmicro.c + Kconfig.hpmicro）

**文件**：`E:\Zephyr_HPMicro\sdk_glue\drivers\serial\uart_hpmicro.c` + `...\drivers\serial\Kconfig.hpmicro`
**为什么**：HPM5361 系 UART IP 自带 RX idle 检测与中断（`HPM_IP_FEATURE_UART_RX_IDLE_DETECT==1`），
原驱动无条件走"TRGM 把 UART RX 引入 GPTMR 捕获/比较做软件 idle"。改成硬件 RX line idle 优先：
`uart_hpm_isr` 判 `uart_is_rxline_idle` → 停 DMA → flush → 换 buffer；GPTMR 降级为周期性轮询看门狗兜底。
非 HPM5361 保留旧 TRGM/GPTMR 路径（包在 `#if !UART_HPM_USE_HW_RX_IDLE` 里避免编译）。这是
**UART+DMA 能稳定收发/烧录的核心运行时修复**。

### 3.1 顶部编译开关（uart_hpmicro.c 第 20-25 行）

```c
#if defined(CONFIG_UART_ASYNC_API) && defined(HPM_IP_FEATURE_UART_RX_IDLE_DETECT) && \
	(HPM_IP_FEATURE_UART_RX_IDLE_DETECT == 1)
#define UART_HPM_USE_HW_RX_IDLE 1
#else
#define UART_HPM_USE_HW_RX_IDLE 0
#endif
```

### 3.2 Kconfig.hpmicro（第 13-17 行 select 改写）

**关键：async 路径现在依赖 `uart_hpm_isr` 处理硬件 idle 中断，必须自动拉起中断驱动框架。**

```kconfig
	select SERIAL_HAS_DRIVER
	select SERIAL_SUPPORT_INTERRUPT
	select SERIAL_SUPPORT_ASYNC
	select UART_INTERRUPT_DRIVEN if UART_ASYNC_API
	select HAS_HPMSDK_GPTMR if UART_ASYNC_API
	select HAS_HPMSDK_DMA if (UART_ASYNC_API && DMA_HPMICRO)
	select HAS_HPMSDK_DMAV2 if (UART_ASYNC_API && DMAV2_HPMICRO)
	select DMA if UART_ASYNC_API
```

### 3.3 `uart_hpm_rx_enable` 硬件 idle 段

在 `data->idle_time_out_us = idle_timeout_us;` 之后、`dma_start` 之前，原两行 GPTMR 配置
改为按 `UART_HPM_USE_HW_RX_IDLE` 分支：

```c
	async_evt_rx_buf_request(dev);
#if UART_HPM_USE_HW_RX_IDLE
	clock_add_to_group(data->gptmr_info.clock_name, 0);
	irq_enable(config->irq_num);
	{
		uint8_t threshold_bits = UART_HPM_HW_IDLE_THRESHOLD_BITS;   /* =3U */
		uint32_t baudrate = data->uart_info.baudrate;
		uart_rxline_idle_config_t idle_cfg;
		if (baudrate == 0U) {
			baudrate = config->baud_rate;
		}
		idle_cfg.detect_enable = true;
		idle_cfg.detect_irq_enable = true;
		idle_cfg.idle_cond = uart_rxline_idle_cond_state_machine_idle;
		idle_cfg.threshold = threshold_bits;
		uart_init_rxline_idle_detection(uart_base, idle_cfg);
		uart_enable_irq(uart_base, uart_intr_rx_data_avail_or_timeout);
		uart_enable_irq(uart_base, uart_intr_rx_line_idle);
		uart_clear_rxline_idle_flag(uart_base);
		data->rx_idle_poll_us = (baudrate > 0U) ?
			(((uint32_t)threshold_bits * 1000000U + baudrate - 1U) / baudrate) : 1U;
		data->rx_idle_poll_us = (data->rx_idle_poll_us + 3U) / 4U;
		if (data->rx_idle_poll_us == 0U) {
			data->rx_idle_poll_us = 1U;
		}
	}
#else
	config_gptmr_to_detect_uart_rx_start(&data->gptmr_info, &data->trgm_info);
    config_gptmr_to_detect_uart_rx_idle(&data->gptmr_info, &data->trgm_info, data->idle_time_out_us);
#endif
	uart_hpm_dma_rx_load(dev, data->dma_rxinfo.current_buf, data->dma_rxinfo.current_total_size);
	dma_start(data->dma_rx.dma_dev, data->dma_rx.channel);
	data->dma_rxinfo.current_mode = 1;
#if UART_HPM_USE_HW_RX_IDLE
	uart_enable_rxline_idle_detection(uart_base);
	if (data->rx_idle_timer_enabled) {
		data->rx_timeout_work_enabled = false;
	} else {
		data->rx_timeout_work_enabled = true;
		k_work_reschedule(&data->rx_timeout_work, K_USEC(data->rx_idle_poll_us));
	}
#endif
	irq_unlock(key);
```

### 3.4 `uart_hpm_isr` 新增硬件 idle 处理（加在 user_cb 分发之前）

```c
#if defined(CONFIG_UART_ASYNC_API) && UART_HPM_USE_HW_RX_IDLE
	const struct uart_hpm_cfg *cfg = dev->config;
	UART_Type *base = cfg->base;
	struct dma_status stat;
	size_t pending = dev_data->dma_rxinfo.current_total_size;

	if (uart_is_rxline_idle(base)) {
		if (dma_get_status(dev_data->dma_rx.dma_dev, dev_data->dma_rx.channel, &stat) == 0) {
			pending = stat.pending_length;
			if (pending < dev_data->dma_rxinfo.current_total_size) {
				dev_data->rx_idle_data_seen = true;
			}
		}
		uart_clear_rxline_idle_flag(base);
		if (dev_data->rx_idle_data_seen && !dev_data->rx_idle_work_busy) {
			dev_data->rx_idle_work_busy = true;
			uart_hpm_hw_rx_idle_handler(dev);
			dev_data->rx_idle_work_busy = false;
		}
	}
#endif
```

### 3.5 新增 `uart_hpm_hw_rx_idle_handler`（硬件 idle 核心处理）

```c
#if UART_HPM_USE_HW_RX_IDLE
static void uart_hpm_hw_rx_idle_handler(const struct device *dev)
{
	struct uart_hpm_data *data = dev->data;
	const struct uart_hpm_cfg *config = dev->config;
	UART_Type *uart_base = config->base;

	if (data->dma_rxinfo.current_mode == 0U) {
		return;
	}
	uart_disable_rxline_idle_detection(uart_base);
	if (dma_suspend(data->dma_rx.dma_dev, data->dma_rx.channel) != 0) {
		uart_enable_rxline_idle_detection(uart_base);
		return;
	}
	uart_clear_rxline_idle_flag(uart_base);
	(void)uart_flush(uart_base);
	uart_hpm_async_rx_flush(dev);
	if ((data->dma_rxinfo.current_size > 0U) &&
	    (data->dma_rxinfo.next_dst_addr != NULL)) {
		async_evt_rx_buf_release(dev);
		if (uart_hpm_dma_replace_rx_buffer(dev) == 0) {
			return;
		}
	}
	(void)dma_resume(data->dma_rx.dma_dev, data->dma_rx.channel);
	uart_enable_rxline_idle_detection(uart_base);
}
#endif
```

### 3.6 新增 GPTMR 轮询兜底（RLD 周期中断）

```c
static void config_gptmr_to_poll_uart_rx_idle(gptmr_info_t *gptmr_info, uint32_t poll_us)
{
	uint32_t gptmr_freq;
	uint32_t ticks;
	gptmr_channel_config_t config;
	if ((gptmr_info->ptr == NULL) || (poll_us == 0U)) {
		return;
	}
	gptmr_freq = clock_get_frequency(gptmr_info->clock_name);
	ticks = (gptmr_freq / 1000000U) * poll_us;
	if (ticks == 0U) {
		ticks = 1U;
	}
	gptmr_channel_get_default_config(gptmr_info->ptr, &config);
	config.reload = ticks;
	config.enable_cmp_output = false;
	gptmr_disable_irq(gptmr_info->ptr, GPTMR_CH_RLD_IRQ_MASK(gptmr_info->cmp_ch));
	gptmr_stop_counter(gptmr_info->ptr, gptmr_info->cmp_ch);
	gptmr_clear_status(gptmr_info->ptr, GPTMR_CH_RLD_STAT_MASK(gptmr_info->cmp_ch));
	gptmr_channel_reset_count(gptmr_info->ptr, gptmr_info->cmp_ch);
	gptmr_channel_config(gptmr_info->ptr, gptmr_info->cmp_ch, &config, false);
	gptmr_enable_irq(gptmr_info->ptr, GPTMR_CH_RLD_IRQ_MASK(gptmr_info->cmp_ch));
	intc_m_enable_irq_with_priority(gptmr_info->irq_index, 1);
	gptmr_start_counter(gptmr_info->ptr, gptmr_info->cmp_ch);
}
```

### 3.7 `uart_hpm_timer_isr` 硬件 idle 版（GPTMR 降级为轮询看门狗）

```c
static void uart_hpm_timer_isr(const struct device *dev)
{
	struct uart_hpm_data *data = dev->data;
	const struct uart_hpm_cfg *config = dev->config;
	gptmr_info_t gptmr_info = data->gptmr_info;
	struct dma_status stat;
	bool rx_idle;
	size_t pending = data->dma_rxinfo.current_total_size;

	if (gptmr_check_status(gptmr_info.ptr, GPTMR_CH_RLD_STAT_MASK(gptmr_info.cmp_ch))) {
		gptmr_clear_status(gptmr_info.ptr, GPTMR_CH_RLD_STAT_MASK(gptmr_info.cmp_ch));
	}
	if (!data->rx_idle_timer_enabled || (data->dma_rxinfo.current_mode == 0U)) {
		return;
	}
	if (dma_get_status(data->dma_rx.dma_dev, data->dma_rx.channel, &stat) == 0) {
		pending = stat.pending_length;
		if (!data->rx_idle_data_seen && (pending < data->dma_rxinfo.current_total_size)) {
			data->rx_idle_data_seen = true;
		}
	}
	rx_idle = uart_is_rxline_idle(config->base);
	if (!data->rx_idle_data_seen) {
		if (rx_idle) {
			uart_clear_rxline_idle_flag(config->base);
		}
		return;
	}
	if (rx_idle) {
		if (!data->rx_idle_work_busy) {
			data->rx_idle_work_busy = true;
			gptmr_disable_irq(gptmr_info.ptr, GPTMR_CH_RLD_IRQ_MASK(gptmr_info.cmp_ch));
			gptmr_stop_counter(gptmr_info.ptr, gptmr_info.cmp_ch);
			gptmr_clear_status(gptmr_info.ptr, GPTMR_CH_RLD_STAT_MASK(gptmr_info.cmp_ch));
			uart_hpm_hw_rx_idle_handler(dev);
			if (data->rx_idle_timer_enabled) {
				config_gptmr_to_poll_uart_rx_idle(&data->gptmr_info, data->rx_idle_poll_us);
			}
			data->rx_idle_work_busy = false;
		}
	}
}
```

### 3.8 IRQ 连接宏拆分（idle 定时器按属性条件连接）

```c
#if UART_HPM_USE_HW_RX_IDLE
#define UART_HPMICRO_IDLE_IRQ_CONNECT(n) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, uart_idle_gptmr_reg), \
		( \
		IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, 1), \
		DT_INST_IRQ_BY_NAME(n, idletimer, priority),	\
		uart_hpm_timer_isr, DEVICE_DT_INST_GET(n),	\
		UART_HPMICRO_IRQ_FLAGS(n));		\
		irq_enable(DT_INST_IRQN_BY_IDX(n, 1)); \
		intc_m_enable_irq_with_priority(DT_INST_IRQ_BY_NAME(n, idletimer, irq), \
						DT_INST_IRQ_BY_NAME(n, idletimer, priority)); \
		), ())
#else
#define UART_HPMICRO_IDLE_IRQ_CONNECT(n) \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, uart_idle_trgm_reg), \
		( \
		IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, 1), \
		DT_INST_IRQ_BY_NAME(n, idletimer, priority),	\
		uart_hpm_timer_isr, DEVICE_DT_INST_GET(n),	\
		UART_HPMICRO_IRQ_FLAGS(n));		\
		irq_enable(DT_INST_IRQN_BY_IDX(n, 1)); \
		intc_m_enable_irq_with_priority(DT_INST_IRQ_BY_NAME(n, idletimer, irq), \
						DT_INST_IRQ_BY_NAME(n, idletimer, priority)); \
		), ())
#endif

#define UART_HPMICRO_IRQ_FUNC_DEFINE(n)	\
	static void irq_config_func##n(const struct device *dev)	\
	{	\
		ARG_UNUSED(dev);	\
		IRQ_CONNECT(DT_INST_IRQN_BY_IDX(n, 0), \
		DT_INST_IRQ_BY_NAME(n, uart, priority),	\
		uart_hpm_isr, DEVICE_DT_INST_GET(n),	\
		UART_HPMICRO_IRQ_FLAGS(n));		\
		irq_enable(DT_INST_IRQN_BY_IDX(n, 0));	\
		UART_HPMICRO_IDLE_IRQ_CONNECT(n) \
	}
```

> 顺带修复：`uart_hpm_async_rx_flush` 的 pending 越界防护、`uart_hpm_async_tx_flush`/`tx_abort`
> 里 DMA dev 从 `dev` 换成 `data->dma_tx.dma_dev`、删两个 `assert(buf!=NULL)`。

---

## 4. CherryUSB：ARRAY_SIZE 宏冲突（usb_osal_zephyr.c）

**文件**：`E:\Zephyr_HPMicro\modules\lib\CherryUSB\osal\usb_osal_zephyr.c`
**位置**：文件头 4 个 include 之后、`#include <version.h>` 之前（约 11-13 行）
**为什么**：CherryUSB 的 `usb_config.h`/`usb_osal.h` 链路已定义过 `ARRAY_SIZE` 宏，Zephyr 的
`<zephyr/kernel.h>` 也定义同名宏，直接 include 导致宏重定义诊断（-Werror 下编译失败）。
在 include Zephyr 内核头前 `#undef`，让 Zephyr 的定义生效。

**改前**：

```c
#include "usb_osal.h"
#include "usb_errno.h"
#include "usb_config.h"
#include "usb_log.h"

#include <version.h>
#if (KERNELVERSION >= 0x3020000)
#include <zephyr/kernel.h>
#else
#include <kernel.h>
#endif
```

**改后**：

```c
#include "usb_osal.h"
#include "usb_errno.h"
#include "usb_config.h"
#include "usb_log.h"

#ifdef ARRAY_SIZE
#undef ARRAY_SIZE
#endif

#include <version.h>
#if (KERNELVERSION >= 0x3020000)
#include <zephyr/kernel.h>
#else
#include <kernel.h>
#endif
```

---

## 5. sdk_glue：CAN 自旋/波特率/状态（mcan_hpmicro.c）

**文件**：`E:\Zephyr_HPMicro\sdk_glue\drivers\can\mcan_hpmicro.c`
**涉及**：`hpm_mcan_send`（~531）、`hpm_mcan_init`（~346）、`hpm_mcan_get_state`（~780）、`hpm_mcan_isr`

### 5.1 删除 TX 发送自旋忙等（杜绝死循环挂死 CPU）

```c
/* 删除全局：static volatile bool has_sent_out; */
/* hpm_mcan_isr 中，删掉 has_sent_out = true; 只剩事件分发： */
	if ((flags & MCAN_EVENT_TRANSMIT) != 0U) {
		hpm_mcan_tc_event_handler(dev, 0);
	}
/* hpm_mcan_init 中，删掉 has_sent_out = true; */
/* hpm_mcan_send 中，删掉 while (!has_sent_out) {} 和 has_sent_out = false; */
```

> 原代码 `while (!has_sent_out)` 自旋等 TX 中断置位，**TX 中断不触发就永久死循环**。

### 5.2 init 写波特率 + 发送失败放回信号量

```c
	mcan_get_default_config(can, config);
	config->baudrate = cfg->common.bitrate;   /* DT 波特率真正生效 */
	clock_add_to_group(cfg->clock_name, 0);
	...
	k_mutex_unlock(&data->tx_mutex);
	if (status != 0) {
		k_sem_give(&data->tx_sem);            /* 失败放回，防后续发送永久阻塞 */
		return -EIO;
	}
```

> 原代码调完 `mcan_get_default_config` 后从未把 DT 波特率写进配置，位时序错误；发送失败也不放回
> 信号量，后续发送永久阻塞。

### 5.3 `hpm_mcan_get_state` 重构（完整函数体）

```c
static int hpm_mcan_get_state(const struct device *dev,
                             enum can_state *state,
                             struct can_bus_err_cnt *err_cnt)
{
	int ret = 0;
	const struct hpm_mcan_config *cfg = dev->config;
	MCAN_Type *can = cfg->base;
	const struct hpm_mcan_data *data = dev->data;

	if (!data->started) {
		if (state != NULL) {
			*state = CAN_STATE_STOPPED;
		}
		if (err_cnt != NULL) {
			err_cnt->tx_err_cnt = 0;
			err_cnt->rx_err_cnt = 0;
		}
		return 0;
	}

	if (state != NULL) {
		mcan_protocol_status_t protocol_status;
		mcan_parse_protocol_status(can->PSR, &protocol_status);
		if (protocol_status.in_bus_off_state) {
			*state = CAN_STATE_BUS_OFF;
		} else if (protocol_status.in_warning_state) {
			*state = CAN_STATE_ERROR_WARNING;
		} else if (protocol_status.in_error_passive_state) {
			*state = CAN_STATE_ERROR_PASSIVE;
		} else {
			*state = CAN_STATE_ERROR_ACTIVE;
		}
	}

	if (err_cnt != NULL) {
		mcan_error_count_t error_count;
		mcan_get_error_counter(can, &error_count);
		err_cnt->tx_err_cnt = error_count.transmit_error_count;
		err_cnt->rx_err_cnt = error_count.receive_error_count;
	}

	return ret;
}
```

> 原代码 `state==NULL` 时也解引用 `*state`，未启动时仍读硬件错误计数；启动后依赖
> `MCAN_EVENT_ERROR` 中断标志锁存才解析 PSR，不可靠。重构后：未启动早退清零、启动后无条件解析 PSR。

---

## 6. sdk_glue：GPIO 中断管理（gpio_hpmicro.c）

**文件**：`E:\Zephyr_HPMicro\sdk_glue\drivers\gpio\gpio_hpmicro.c`
**涉及**：`gpio_hpm_port_isr`（~233）、`gpio_hpm_pin_interrupt_configure`（~202）、
`GPIO_HPMICRO_IRQ_INIT`（~258）、新增 `gpio_hpm_disable_port_interrupts`

**为什么**：原 ISR 用原始 pending 与 IE 未相与，已禁用引脚也会触发回调/唤醒；disable 不清标志、
不关 NVIC IRQ。

**新增函数**：

```c
static void gpio_hpm_disable_port_interrupts(GPIO_Type *gpio_base, uint32_t port_base)
{
	gpio_base->IE[port_base].CLEAR = UINT32_MAX;
	gpio_base->AS[port_base].CLEAR = UINT32_MAX;
#ifdef GPIO_PD_VALUE_IRQ_DUAL_MASK
	gpio_base->PD[port_base].CLEAR = UINT32_MAX;
#endif
	gpio_base->IF[port_base].VALUE = UINT32_MAX;
}
```

**`gpio_hpm_pin_interrupt_configure` 中**（struct 配置需加 `uint32_t irq_num;` 字段）：

```c
	if (mode == GPIO_INT_MODE_DISABLED) {
		gpio_disable_pin_interrupt(gpio_base, port_base, pin);
		gpio_clear_pin_interrupt_flag(gpio_base, port_base, pin);
		if (gpio_base->IE[port_base].VALUE == 0U) {
			irq_disable(config->irq_num);
		}
	} else {
		gpio_clear_pin_interrupt_flag(gpio_base, port_base, pin);
		gpio_config_pin_interrupt(gpio_base, port_base, pin, trigger);
		gpio_clear_pin_interrupt_flag(gpio_base, port_base, pin);
		gpio_enable_pin_interrupt(gpio_base, port_base, pin);
		irq_enable(config->irq_num);
	}
```

**ISR**（pending 与 IE 相与后才触发回调）：

```c
	uint32_t raw_status = gpio_base->IF[port_base].VALUE;
	uint32_t int_status = raw_status & gpio_base->IE[port_base].VALUE;
	gpio_base->IF[port_base].VALUE = raw_status;
	if (int_status != 0U) {
		gpio_fire_callbacks(&data->callbacks, dev, int_status);
	}
```

**IRQ INIT 宏**（init 统一预清端口中断并关 IRQ）：

```c
#define GPIO_HPMICRO_IRQ_INIT(n)                        \
	do {                                                \
		const struct gpio_hpm_config *cfg = DEVICE_DT_INST_GET(n)->config; \
		GPIO_Type *gpio = cfg->gpio_base;               \
		uint32_t port = cfg->port_base;                 \
		                                                \
		irq_disable(DT_INST_IRQN(n));                   \
		gpio_hpm_disable_port_interrupts(gpio, port);   \
		                                                \
		IRQ_CONNECT(DT_INST_IRQN(n),                    \
		            DT_INST_IRQ(n, priority),           \
		            gpio_hpm_port_isr,                  \
		            DEVICE_DT_INST_GET(n), 0);          \
		irq_disable(DT_INST_IRQN(n));                   \
	} while (0)
```

> config 宏里加 `.irq_num = DT_INST_IRQN(n),`。

---

## 7. sdk_glue：UDC 真正连接/断开（udc_hpmicro.c）

**文件**：`E:\Zephyr_HPMicro\sdk_glue\drivers\usb\udc\udc_hpmicro.c`
**涉及**：`udc_hpm_enable`（~793）、`udc_hpm_disable`（~806）、`udc_hpm_init`（~825）
**为什么**：原 `enable`/`disable` 只置/清 `USBCMD.RS`（只启停控制器、从不做 D+ 上拉，**主机根本无法
枚举**）。改为清 `OTGSC.VD` 后调 `usb_device_connect()/usb_device_disconnect()`，并在 init 使能 IRQ
后补处理一次锁存的中断（避免错失 IRQ 使能前的 bus reset/setup 事件）。

```c
static int udc_hpm_enable(const struct device *dev)
{
	struct udc_hpm_data *priv = udc_get_private(dev);
	usb_device_handle_t *handle = &priv->handle;
	handle->regs->OTGSC &= ~USB_OTGSC_VD_MASK;   /* 撤掉 VBUS 检测覆盖 */
	usb_device_connect(handle);                  /* D+ 上拉，主机可见 */
	return 0;
}
static int udc_hpm_disable(const struct device *dev)
{
	struct udc_hpm_data *priv = udc_get_private(dev);
	usb_device_handle_t *handle = &priv->handle;
	usb_device_disconnect(handle);
	return 0;
}
```

**init 中，`config->irq_enable_func(dev);` 之后补处理锁存中断**：

```c
	/* enable USB interrupt */
	config->irq_enable_func(dev);
	uint32_t pending = usb_device_status_flags(handle) & usb_device_interrupts(handle);
	if (pending != 0U) {
		udc_hpm_isr(dev);                        /* 处理 IRQ 使能前锁存的事件 */
	}
```

---

## 附：发现的残留 bug（未修，仅报告）

`E:\Zephyr_HPMicro\sdk_glue\drivers\can\mcan_hpmicro.c` 约 L920，`hpm_mcan_set_timing_data`：

```c
	uint32_t can_clk_freq = clock_get_frequency(cfg->clock_name);
	       dev->name, can_clk_freq, config->baudrate, (unsigned int)can->NBTP, (unsigned int)can->TDCR);
```

第二行是**截断的半截 printk**，`CONFIG_CAN_FD_MODE` 下无法编译。应整行删除。

---

_底层修改详解更新至 2026-08-02。每个修改都直接贴了改前/改后代码，可复制应用。_
