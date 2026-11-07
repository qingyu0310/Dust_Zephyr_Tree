# EtherCAT PDO 搬运带宽调研（2026-11-07）

> 起因：主站周期 100us 时 `rx > tx`，从站跟不上。需要判断 256B PDO 的搬运开销是软件问题还是硬件带宽上限，并列出可选的加速路径。

## 一、现象

主站分别以 1ms / 200us / 100us 周期运行，从站侧每秒统计（`ecat speed` / `ecat isr` 两行）：

| 主站标称周期 | 实测帧率 | 实测周期 | rx : tx | 结论 |
|---|---|---|---|---|
| 1ms | 825/s | 1.21ms | 1 : 1 | 无丢失 |
| 200us | 1948/s | 513us | 1 : 1 | 无丢失 |
| 100us | 4400/s | 227us | **0.90** | **丢帧** |

判据：`rx` 与 `tx` 相等说明主站每一轮都完成了"写 SM2 + 读 SM3"，从站没有积压；`tx < rx` 说明主站读 SM3 时从站还没把新数据写进去。

## 二、ISR 耗时实测

在 `EcatDevice::OnPdi` 及其内部两个数据搬运点加 `k_cycle_get_32()` 计时，取每秒窗口内最大值（`ecat isr` 行）。

| 项 | 耗时 | 占比 | 覆盖范围 |
|---|---|---|---|
| `total` | **240us** | 100% | 整个 PDI 中断 |
| `consume` | 91us | 38% | `ConsumeOutputs`（读 SM2）+ `OnProcessOutput` |
| `publish` | 88us | 37% | `PublishInputs`（写 SM3） |
| 其余 | 61us | 25% | `ReadAlEvent`、`ReadSyncManager`、状态机、AL 事件分发 |

`total=240us` 大于主站周期 227us，**必然漏帧**，这就是 100us 档 `tx/rx=0.90` 的直接原因。

### 2.1 再往下拆一层

在厂商 C 侧（`esc_hpmicro.c` 的 `esc_read_words` / `esc_write_words`）单独计时，得到纯 MMIO 搬运的周期数（`ecat esc` 行）：

| 项 | 周期数 | 换算（24MHz） | 占上层耗时比例 |
|---|---|---|---|
| `read` 256B | 1475 cyc | 61.5us | 占 `consume` 91us 的 68% |
| `write` 256B | 1352 cyc | 56.3us | 占 `publish` 88us 的 64% |

**单次 32 位 MMIO：1475 / 64 = 23 个周期。** 写为 1352 / 64 = 21 个周期。

### 2.2 关于计时基准

`k_cycle_get_32()` 读的是 **mchtmr** 计数器。`soc/hpmicro/HPM6E00/soc.c` 里明确把 mchtmr 配置为 24MHz 晶振源：

```c
/* Configure mchtmr to 24MHz */
clock_set_source_divider(clock_mchtmr0, clk_src_osc24m, 1);
```

而 CPU 核心实际运行在 600MHz（板上 `clock_get_frequency(clock_cpu0)` 实测）。两者是独立时钟域，`CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC` 对齐的是 mchtmr 而非 CPU——**该值 24000000 是正确的，不要改成 CPU 频率**（曾误改为 600000000，导致所有 `K_MSEC()` 实际延长 25 倍，启动慢到无法接受）。

## 三、已排除项

| 假设 | 验证方式 | 结论 |
|---|---|---|
| 逐字节访问导致慢 | 打开 `CONFIG_HPM_ESC_PDRAM_WORD_ACCESS`，日志确认 `esc_read FAST offset=0x1100 len=256` | **已生效**，但耗时不变——原本就不是逐字节瓶颈 |
| PDRAM 快路径条件不满足 | SM2=0x1100 / SM3=0x1400，地址 ≥0x1000、4 字节对齐、len=256 满足 `esc_pdram_word_access()` 全部条件 | 条件成立 |
| 布局计算错误 | `ecat layout: rx_words=64 tx_words=64 rx_size=256 tx_size=256` | 布局正确 |
| D-Cache 一致性 | `CONFIG_DCACHE` 未设置（关闭），`CONFIG_NOCACHE_MEMORY=y` | 不涉及 |
| CPU 主频不足 | mchtmr 24MHz 计时为准，23 周期/次是总线等待 | CPU 600MHz，不是算力问题 |

## 四、根因

**ESC PDRAM 的 CPU 直访延迟就落在 20~25 个 AHB 周期/32 位。**

256 字节需要 64 次访问：

- 读：64 × 23 = 1472 周期 = 61.5us
- 写：64 × 21 = 1344 周期 = 56.3us
- 合计 **118us**，已超过 100us 周期；加上上层固定开销，ISR 总长 240us

这不是软件实现缺陷，是 **ESC PDI 接口对 CPU 直访有固定插等待**。ESC 的 PDI 是为外部主控设计的低速并行接口，不是给本机 CPU 做高带宽内存用的。

## 五、可选加速路径调研

### 5.1 PDI DMA —— **不可行**

查 `soc/HPM6E00/HPM6E80/hpm_dmamux_src.h`，DMA 触发源中与 ESC 相关的只有两项：

```c
#define HPM_DMA_SRC_ESC_SYNC0   (0x76UL)  /* Ethercat sync0 */
#define HPM_DMA_SRC_ESC_SYNC1   (0x77UL)  /* Ethercat sync1 */
```

**没有 ESC PDRAM 的 DMA 搬运通道。** 这两项只能把 SYNC0/SYNC1 信号当触发源（用于触发其他外设的 DMA），不能用来搬 PDRAM 数据。

`hpm_esc_drv.h` 全文也没有任何 DMA 相关 API。

结论：**HPM6E80 的 ESC 不支持 PDI DMA 搬运过程数据。**

### 5.2 Burst / 预取模式 —— **不支持**

查 `soc/HPM6E00/ip/hpm_esc_regs.h`（299 个寄存器/字段）：

- `PDI_CTRL`(0x140) —— 只读，标识 PDI 类型
- `PDI_CFG`(0x150) —— 只读，含 `BUS`（片上总线类型）与 `CLK`（同步倍频系数），**均为 RO，不可配置**
- `PDI_INFO`(0x14E) —— 只读信息
- 无 burst / prefetch / cache 相关配置位

结论：**ESC 不提供 CPU 侧的块传输加速模式，访问粒度就是 32 位。**

### 5.3 缩小 PDO —— 业务不允许

用户明确：**有效数据长度不能改，实际使用不知道有多大。**

且反过来说，即使缩到 32B（8 次访问 ≈ 8us），也只是把上限从 100us 推到约 300us 量级，仍然没有质变。

### 5.4 事件合并 / 降低 ISR 频次 —— 不可行

PDI 中断由 ESC 在 SM 事件时拉高。若在 ISR 里攒多次再搬，SM2 缓冲会被主站下一次写覆盖（缓冲模式决定），数据直接丢失，比丢帧更严重。

### 5.5 减少每次 ISR 的固定开销 —— 有收益但有限

`total - consume - publish = 61us`，这部分包括：

- `ReadAlEvent()` 读 0x0220（1 次 16 位读）
- `ReadSyncManager()` 读 0x0800+8n（2 次 32 位读，SM2/SM3 各一次）
- `HandleControl` / `MarkFirstOutputs` / `CompletePendingOp` 等状态机判断
- `sync_.OnProcessOutput()`、`AcknowledgeSyncManagerArea`

按 23 周期/次访问估算，这些寄存器读约 4~6 次 = 100~140 周期 ≈ 4~6us，**远小于 61us**。剩余约 55us 是 C++ 逻辑执行（状态机查询、函数调用链）。优化这部分能省 20~30us，但 118us 的搬运开销仍在，**无法让 100us 周期成立**。

## 六、结论

| 周期 | 可行 |
|---|---|
| 1ms | ✅ |
| 200us | ✅ 实测 rx:tx 严格 1:1 |
| **100us** | ❌ ISR 240us > 周期 227us，硬件带宽决定，软件无法弥补 |

**能力边界：256B × 2 方向的 PDO，在当前 ESC 访问方式下，主站周期下限约 250us。**

若一定要跑 100us，只剩一条路：**缩小 PDO 尺寸**（业务已否决）。

## 七、附带发现（与本次问题无关但已修正）

- `CONFIG_HPM_ESC_PDRAM_WORD_ACCESS` 在 `sdk_glue_user/drivers/ethercat/Kconfig` 中**从未定义**，只有 `esc_hpmicro.c` 里 3 处 `#if defined` 在等它。本次补上定义并在 `uhpm6e00evk.conf` 打开。收益：写路径从逐字节（256 次）变为 32 位整字（64 次）。
- 已归档到 `zephyr-patches/sdk_glue/0007-soc-boards.diff` 的补丁顺带补齐了两处此前漏归档的文件：`modules/hpm_sdk/Kconfig`（`HAS_HPMSDK_ESC`）、`soc/hpmicro/soc.yml`（hpm5e00 系列）。
