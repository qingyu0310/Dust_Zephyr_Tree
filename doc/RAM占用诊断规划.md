# RAM 占用诊断规划（v3 · 带实验依据）

## 一、现状

`dust build hpm5361icb` 产物：

| 区域 | 使用 | 总大小 | 占用率 |
|------|------|--------|--------|
| ROM | 161476 B | 1 MB | 15.40% |
| **RAM** | **109828 B** | **128 KB** | **83.79%** |
| ITCM | 1838 B | 128 KB | 1.40% |
| AHB_SRAM | 2560 B | 32 KB | 7.81% |

目标：压到 <75%（省 ~12 KB）。

## 二、占用归因（实验依据：`nm --size-sort` 实测 zephyr.elf + 源码审读 + Zephyr 内核源码）

### 2.1 核心机制：PMP_STACK_GUARD 每栈 +1KB（本次新发现，实锤）

- `.config` 里 **`CONFIG_PMP_STACK_GUARD=y`** + `CONFIG_PMP_POWER_OF_TWO_ALIGNMENT=y` + `CONFIG_PMP_STACK_GUARD_MIN_SIZE=512`。
- [riscv/arch.h:51-74](E:/Zephyr/zephyr/include/zephyr/arch/riscv/arch.h#L51-L74)：`Z_RISCV_STACK_GUARD_SIZE = Z_POW2_CEIL(esf + 512) = 1024`；`ARCH_KERNEL_STACK_RESERVED = 1024`；栈对象按 1024 对齐。
- **每个线程栈实际分配 = `ROUND_UP(StackSize, 16) + 1024`，再 1024 对齐**。
- **实测验证（nm 与配置精确吻合 +1KB）**：

| 栈 | 配置 | nm 符号大小 | 差值 |
|----|------|-----------|------|
| 主栈 | `MAIN_STACK=8192` | `z_main_stack` 9216 | +1024 |
| 中断栈 | `ISR_STACK=2048` | `z_interrupt_stacks` 3072 | +1024 |
| idle | `IDLE_STACK=2048` | `z_idle_stacks` 3072 | +1024 |
| work_q | `SYSTEM_WORKQUEUE_STACK=1024` | `sys_work_q_stack` 2048 | +1024 |

- **PMP guard 总开销 ≈ 每栈 1KB × ~12 个栈 ≈ 12 KB**——全工程最大隐藏项。

### 2.2 线程栈实际占用表（StackSize + 1KB guard）

| 对象 | 栈配置 | 实际占用 | 说明 |
|------|--------|---------|------|
| remote_ | `Thread<1024*5>` = 5120 | ~6 KB | [remote.hpp:113](e:/Zephyr/zephyr_user/framework/modules/remotes/remote.hpp#L113) |
| imu_ | `Thread<4096>` | ~5 KB | [imu.hpp:58](e:/Zephyr/zephyr_user/framework/modules/imu/drivers/imu.hpp#L58) |
| chassis | `Thread<1024*4>` = 4096 | ~5 KB | [trd_chassis.cpp:61](e:/Zephyr/zephyr_user/project/thread/chassis/trd_chassis.cpp#L61) |
| shell_ | `Thread<2048>` | ~3 KB | [shell.hpp:46](e:/Zephyr/zephyr_user/framework/cmd/shell/shell.hpp#L46) |
| gpio/pc/test | `Thread<2048>` ×3 | 各 ~3 KB | |
| can | `Thread<>` = 1024 | ~2 KB | |
| 主栈 / 中断 / idle / work_q | — | 见 2.1 | |

### 2.3 remote_/imu_ 内部拆解（无内存池/大数组）

| 对象 | 符号大小 | 构成 |
|------|---------|------|
| remote_ | 8 KB | 大头 = `Thread<5120>` 栈（5K+guard≈6K）+ k_thread 控制块 + `frame_buf_[64]`（64B）+ pub/detect。**无池** |
| imu_ | 9 KB | 大头 = `Thread<4096>` 栈（4K+guard≈5K）+ k_thread + EKF(6×6, ~1KB) + Sample(28B) + heater。**无池** |

### 2.4 非栈大项（全工程 bss/data 符号已全扫）

| 对象 | 大小 | 结论 |
|------|------|------|
| **USB DCD `s_dcd_data`** | 10 KB | `dcd_data_t` 本身（QHD 32×64B=2K + QTD 256×32B=8K），2048 对齐无损；必须 RAM 可写（USB 控制器 DMA 回写），w25q128 不可行（SPI 总线够不着 + 只读）。硬成本或关 USB |
| **系统堆 `kheap`** | 8 KB | `CONFIG_HEAP_MEM_POOL_SIZE=8192`。**grep 全工程无 `k_malloc`/`k_heap` 使用**（zbus 用静态 msgq）→ 大概率空置 |
| MCAN 数据 | 12.5 KB | `hpm_mcan_data_0/1`（各 5KB）+ `board_app_mcan_msg_buf`（2.5KB），HPM SDK 固定，2 路 CAN |
| USB rx/tx buf | 1.5 KB | `.nocache` DMA 缓冲，必须 |
| 其余 | <1 KB | imu EKF(~1KB)、chassis_pid、zbus msgq 等均小 |

**全工程数据符号扫描结论**：除线程栈对象外，**没有其他 >2.5KB 的大数组**。RAM 大头 = 线程栈类（~50KB，含 PMP guard ~12KB）+ USB DCD（10KB）+ 系统堆（8KB）+ MCAN（12.5KB）。

## 三、优化方案（按优先级）

### P0：系统堆 8 KB → 0（零风险，纯 Kconfig）
- **依据**：grep `k_malloc`/`k_heap`/`K_HEAP` 在 framework/ project/ **零命中**；zbus 通道用静态 msgq 不占堆。
- **改动**：`project/prj.conf:29` `CONFIG_HEAP_MEM_POOL_SIZE=8192` → `0`（若编译/运行报缺堆再降回 2048）。
- **省**：8 KB。**风险**：无（用户代码不碰堆）。

### P1：PMP_STACK_GUARD 评估（~12 KB，需用户权衡安全）
- **依据**：每栈 +1KB guard，~12 个栈 ≈ 12KB（§2.1 实测验证）。
- **改动**：`.config` 关 `CONFIG_PMP_STACK_GUARD`（连带 `CONFIG_HW_STACK_PROTECTION` 关闭，[.config:94](e:/Zephyr/zephyr_user/project/build/zephyr/.config#L94)）。
- **省**：~12 KB（每栈 1KB，主栈/中断/idle/work_q/8 个业务栈）。
- **风险**：**失去栈溢出即时保护**——溢出不再触发 fault 而是静默破坏内存。权衡：配合"栈检测实验"把各栈降到真实峰值×1.5 后，溢出概率大幅下降，可接受与否由用户拍板。

### P2：主栈 9 KB → 4096（先测后降）
- **依据**：`main()` 只调 `System_Startup()`，调用链 = 各 `thread_init` 序列，峰值预期 <2 KB。
- **改动**：`prj.conf:20` `CONFIG_MAIN_STACK_SIZE=8192` → `4096`。
- **省**：~5 KB（实际 9216→5120，含 guard）。**风险**：低。

### P3：线程栈峰值实测降档（需要 §四 栈检测实验）
- **对象**：remote(5120)、imu(4096)、chassis(4096)、shell/gpio×2/pc/test(2048)、can(1024)。
- **依据**：栈是 RAM 最大可控项；当前配置偏保守，需实测峰值再降。
- **改动**：按 §四 数据，把 `Thread<StackSize>` 降到 `峰值 × 1.5`（留余量）。
- **省**：视实测，remote/imu/chassis 若峰值 1-2K 可各降 2-3K，合计 ~8-10K。
- **风险**：低（有实验依据）。

### P4：关第二路 CAN（~5 KB，条件项）
- **依据**：`hpm_mcan_data` 每路 5 KB + msg_buf 2.5 KB。若只用一路 CAN 可关。
- **省**：~5 KB。**风险**：取决于是否用第二路。

### P5：USB DCD 10 KB（最大单项，硬成本）
- **结论已查实**：`dcd_data_t` = QHD 2K + QTD 8K = 10KB；2048 对齐无损；必须 RAM 可写。
- **选项**：① 暂不用 USB → 关 `DUST_COM_USB` 省 ~14 KB；② 缩 `USB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT`（8→2，省 6KB，动 HPM SDK soc 头）。
- **建议**：不作首选项；USB 测试完可关。

## 四、栈使用检测实验（P2/P3 的前置）

### 4.1 原理
`CONFIG_INIT_STACKS=y` 用 0xA5 填充栈；运行时 `k_thread_stack_space_get(k_current_get())` 返回当前线程未用栈字节数。各线程循环里周期性打印，收集"最低余量"= 峰值使用。

### 4.2 改动（临时实验代码，测完删）
1. `project/prj.conf` 加 `CONFIG_INIT_STACKS=y` **和 `CONFIG_THREAD_STACK_INFO=y`**（`k_thread_stack_space_get` 由两者共同门禁，见 [thread.c:964](E:/Zephyr/zephyr/kernel/thread.c#L964)）。
2. 各线程 Task（remote/imu/chassis/shell/gpio/pc/test/can）循环里每 5s 打印：

```cpp
// 各线程 Task 里临时加（测完删）
static uint32_t s_last_ms = 0;
const uint32_t now = k_uptime_get_32();
if (now - s_last_ms >= 5000) {
    s_last_ms = now;
    size_t unused = 0;
    if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
        DUST_LOG_INF("[stack] %s free %u B", "<线程名>", (unsigned)unused);
    }
}
```

3. `System_Startup` 末尾（AppThread 后）打主栈余量（同 K_current_get）。

### 4.3 数据采集与定档
- 跑典型工况 30s（含遥控/底盘跑动/IMU 预热/Shell 交互），让每个线程跑满。
- 记录各线程 `free` 最低值 → 峰值 = `栈大小 - 最低 free`。
- **降档公式**：新栈 = `峰值 × 1.5` 向上取整到 256 倍数。
- 例：remote 5K 栈若最低 free 3.5K（峰值 1.5K）→ 新栈 2048。

### 4.4 验收
降档后烧录，重复 30s 工况，确认最低 free 仍 >0 且各功能正常。

## 五、执行阶段

| 阶段 | 内容 | 验证 |
|------|------|------|
| 阶段 0 | 栈检测实验（§四）：开 INIT_STACKS + 加打印 | 烧录 30s 收集各栈峰值表 |
| 阶段 1 | 系统堆 8192→0（P0） | 编译 + 烧录启动正常 |
| 阶段 2 | 评估 PMP_STACK_GUARD（P1）——由用户权衡关/留 | 若关，烧录确认无异常 |
| 阶段 3 | 按峰值降主栈 + 各线程栈（P2/P3） | 编译 + 烧录 30s 复测无溢出 |
| 阶段 4 | 评估关第二路 CAN（P4）/ USB（P5） | 按实际需求决定 |
| 阶段 5 | 终验：RAM 占用复查 + 全功能回归 | RAM < 75%，功能完整 |

## 六、执行清单

- [ ] 1. `prj.conf` 加 `CONFIG_INIT_STACKS=y` + `CONFIG_THREAD_STACK_INFO=y`（栈使用率检测开启）
- [ ] 2. 各线程 Task 加栈余量周期打印（remote/imu/chassis/shell/gpio/pc/test/can）
- [ ] 3. `System_Startup` 末尾打主栈余量
- [ ] 4. 烧录跑 30s 典型工况，记录各线程最低 free
- [ ] 5. `prj.conf` `CONFIG_HEAP_MEM_POOL_SIZE=8192` → `0`（P0）
- [ ] 6. **PMP_STACK_GUARD 关/留由用户拍板**（P1，省 ~12KB）
- [ ] 7. 按 `峰值×1.5` 定档，降 `CONFIG_MAIN_STACK_SIZE` 和各 `Thread<StackSize>`
- [ ] 8. 删除临时栈打印代码，编译烧录
- [ ] 9. 30s 复测确认无栈溢出、功能正常
- [ ] 10. 评估 P4（关第二路 CAN）/ P5（USB）
- [ ] 11. 终验：RAM 占用率复查（目标 <75%）
