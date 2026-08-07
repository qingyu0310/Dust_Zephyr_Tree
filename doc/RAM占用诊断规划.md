# RAM 占用诊断规划（v3 · 带实验依据）

## 一、现状

`dust build hpm5361icb` 产物：

| 区域 | 使用 | 总大小 | 占用率 |
|------|------|--------|--------|
| ROM | 161616 B | 1 MB | 15.41% |
| **RAM** | **70940 B** | **128 KB** | **54.12%** |
| ITCM | 1838 B | 128 KB | 1.40% |
| AHB_SRAM | 2560 B | 32 KB | 7.81% |

目标：压到 <75% —— **已达成**。优化前 109828B/83.79% → 70940B/54.12%，**累计省 38888 B ≈ 38 KB**。

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

### 2.5 栈检测实测与降档（2026-08-07 已执行）

**检测方法**：`CONFIG_INIT_STACKS` 0xA5 填充 + 各线程循环 `k_thread_stack_space_get` 打最低 free。0xA5 用过不回填，最低 free = **历史峰值**（比瞬时值可信）。

| 线程 | 原栈 | 实测最低 free | 峰值使用 | 新栈 | 余量 |
|------|------|-------------|---------|------|------|
| main | 8192 | 6960* | ~2256 | **3072** | 1792 |
| imu | 4096 | 2448 | 1648 | **2048** | 400 |
| remote | 5120 | 3952 | 1168 | **2048** | 880 |
| gpio(output) | 2048 | 992（含检测块） | <300 | **1024** | ~700 |
| shell | 2048 | 880（var list 后） | 1168 | 2048（保持） | 880 |
| pc / test / can | 2048/2048/1024 | — | — | 未测 | — |

\* main 原 8192 时最低 free 6960（RunStage 内每阶段打）；降 3072 后 free 1792。

**guard 减半**（P1 的折中方案，非全关）：`CONFIG_PMP_STACK_GUARD_MIN_SIZE` 512→256，guard = `POW2_CEIL(esf~80 + 256)` = **512**（原 1024）。每栈省 512B × ~12 栈 ≈ **6 KB**。保护保留（进入即 fault），异常压栈空间 432B。详见 §3.1。

**配套：fatal handler**（排查中发现的输出黑洞）——`CONFIG_LOG`/`CONFIG_CONSOLE` 关闭导致 Zephyr fatal 输出被吞（LOG_ERR no-op + printk 无后端），栈溢出不可见。新增 [framework/cmd/fatal/fatal.cpp](framework/cmd/fatal/fatal.cpp) override `k_sys_fatal_error_handler`，用 DUST_LOG 上报 reason/mcause/mepc，无条件编译（无 Kconfig 门禁，防呆）。

## 三、优化方案（按优先级）

> **2026-08-07 执行状态**：P0 系统堆 8192→2048 / P1 guard 减半（非全关）/ P2 主栈 3072 / P3 imu·remote·gpio 降档 / P5 USB（QTD 8→2 + 缓冲 512→256）已执行；P4 CAN 未做。

### P0：系统堆 8 KB → 2048（已执行，变体：非归零）
- **依据**：grep `k_malloc`/`k_heap`/`K_HEAP` 在 framework/ project/ **零命中**；zbus 通道用静态 msgq 不占堆（`HEAP_MEM_POOL_ADD_SIZE_ZBUS=0`）。
- **改动**：`prj.conf` `CONFIG_HEAP_MEM_POOL_SIZE=8192` → `2048`（用户拍板留 2KB 保险，不归零；覆盖板级默认 [hpm5361icb/Kconfig.defconfig:6-7](E:/Zephyr_HPMicro/sdk_glue_user/boards/hpmicro/hpm5361icb/Kconfig.defconfig#L6-L7)）。
- **省**：6 KB。**风险**：低（用户代码不碰堆；若某子系统运行时缺堆再降回）。

### P1：PMP_STACK_GUARD 评估（~12 KB，需用户权衡安全）——**已执行（减半方案）**
- **依据**：每栈 +1KB guard，~12 个栈 ≈ 12KB（§2.1 实测验证）。
- **改动**：`CONFIG_PMP_STACK_GUARD_MIN_SIZE` 512 → 256，guard = `POW2_CEIL(esf~80 + 256)` = **512**（[riscv/arch.h:51-53](E:/Zephyr/zephyr/include/zephyr/arch/riscv/arch.h#L51-L53)）。**不关保护**（全关 = 溢出静默破坏内存；fatal handler 已补上溢出可见性）。
- **省**：~6 KB（每栈 512B × ~12 栈）。
- **风险**：guard 减半，检测窗口缩短、异常压栈空间 1024→512（432B，够 ~6 层嵌套中断）。保护机制不变（进入即 fault）。

### P2：主栈 9 KB → 3072（已执行）
- **依据**：`main()` 只调 `System_Startup()`，调用链 = 各 `thread_init` 序列，实测峰值 ~2.3KB。
- **改动**：`prj.conf` `CONFIG_MAIN_STACK_SIZE=8192` → `3072`（用户拍板，比 4096 更省）。
- **省**：~5 KB（实际 9216→4096，含 guard 512）。**风险**：低，降后 free 1792B。

### P3：线程栈峰值实测降档（已部分执行）
- **对象**：remote(5120)、imu(4096)、chassis(4096)、shell/gpio×2/pc/test(2048)、can(1024)。
- **依据**：栈是 RAM 最大可控项；实测峰值远低于配置（§2.5）。
- **改动（已做）**：imu 4096→**2048**、remote 5120→**2048**、gpio(output) 2048→**1024**；shell 实测峰值 1168 保持 2048；chassis/pc/test/can **未测未降**。
- **省**：已降部分 ≈ 1+2+3 KB ≈ **6 KB**。
- **风险**：低（有实验依据）；gpio 1024 装不下 DUST_LOG 直打（~528B+），后续加日志须升回。

### P4：关第二路 CAN（~5 KB，条件项）
- **依据**：`hpm_mcan_data` 每路 5 KB + msg_buf 2.5 KB。若只用一路 CAN 可关。
- **省**：~5 KB。**风险**：取决于是否用第二路。

### P5：USB DCD 10 KB（最大单项，硬成本）——**已部分执行**
- **结论已查实**：`dcd_data_t` = QHD 2K + QTD 8K = 10KB；2048 对齐无损；必须 RAM 可写（USB 控制器 DMA 回写，flash 不可行）。
- **已做（QTD 8→2）**：board.cmake `add_compile_definitions(USB_SOC_DCD_QTD_COUNT_EACH_ENDPOINT=2)`，利用 [hpm_soc_feature.h:93](E:/Zephyr_HPMicro/sdk_env/hpm_sdk/soc/HPM5300/HPM5361/hpm_soc_feature.h#L93) 的 `#ifndef` 覆盖，**不改官方 SDK**。QTD 256→64 个（8K→2K），**省 6KB**。CDC 单发单收，每端点 2 深够。
- **已做（收发缓冲 512→256）**：`s_rx_buf[2][512]`/`s_tx_buf[512]` → 256（`UsbHal::kTxBufSize/kRxBufSize` 常量，抽象层定义，hal 数组与上层单次发送上限共用），**省 768B**。单次发送上限 512→256（工况冗余充足）。
- **未做**：关 USB 整段移除（省 ~14KB）。

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
| 阶段 0 | 栈检测实验（§四）：开 INIT_STACKS + 加打印 | ✅ 烧录采集各栈峰值表 |
| 阶段 1 | 系统堆 8192→2048（P0） | ✅ 已做（留 2KB 保险） |
| 阶段 2 | 评估 PMP_STACK_GUARD（P1） | ✅ 减半方案：MIN_SIZE 512→256 → guard 512，省 ~6KB |
| 阶段 3 | 按峰值降主栈 + 各线程栈（P2/P3） | ✅ main 3072 / imu 2048 / remote 2048 / gpio 1024 |
| 阶段 4 | 评估关第二路 CAN（P4）/ USB（P5） | ⬜ 未做 |
| 阶段 5 | 终验：RAM 占用复查 + 全功能回归 | ✅ RAM 65.45% < 75% |

## 六、执行清单

- [x] 1. `prj.conf` 加 `CONFIG_INIT_STACKS=y` + `CONFIG_THREAD_STACK_INFO=y`（栈使用率检测开启）
- [x] 2. 各线程 Task 加栈余量周期打印（main/imu/remote/gpio/shell 已测；pc/test/can 未测）
- [x] 3. `System_Startup` 末尾打主栈余量
- [x] 4. 烧录跑典型工况，记录各线程最低 free
- [x] 5. `prj.conf` `CONFIG_HEAP_MEM_POOL_SIZE=8192` → `2048`（P0 变体，留 2KB 保险）
- [x] 6. PMP_STACK_GUARD 评估 → **减半方案已做**（MIN_SIZE 256，guard 512，省 ~6KB）
- [x] 7. 降档：main 3072 / imu 2048 / remote 2048 / gpio 1024；shell 保持 2048
- [x] 8. 删除临时栈打印代码
- [x] 9. 复测确认无栈溢出（main/imu/remote/gpio/shell）
- [x] 10. P5 USB 已做（QTD 8→2 + 缓冲 512→256）；P4 关第二路 CAN 未做
- [x] 11. 终验：RAM 占用率 **65.45%**（目标 <75% 已达成）
