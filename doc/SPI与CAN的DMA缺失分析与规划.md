# SPI 与 CAN 的 DMA 缺失分析与规划

> 目标文件：`framework/drivers/communication/spi/spi.{hpp,cpp}`、`framework/drivers/communication/can/can.{hpp,cpp}`、`framework/drivers/Kconfig`
> 板级文件：`projects/*/project/boards/hpm/*/*.overlay`（DT `dmas`/`dma-names`）
> 底层参考：`E:\Zephyr_HPMicro\sdk_glue\drivers\spi\spi_hpmicro.c`、`E:\Zephyr_HPMicro\sdk_glue\drivers\can\mcan_hpmicro.c`
>
> 目标：回答「为什么 UART 有 DMA 版，SPI 和 CAN 没有」，并给出各自要不要补、补什么、怎么补的规划。
>
> 日期：2026-10-09
>
> 前置：框架 `drivers` 六模块已稳定（`DUST_COM_UART_DMA` / `DUST_COM_SPI` / `DUST_COM_CAN` 三条门禁各自独立）。

---

## 1. 结论

**两个外设「没有 DMA」是两件完全不同的事，不能一起答。**

| 外设 | 「没有 DMA」的实质 | 该不该补 |
|------|------------------|---------|
| **UART** | 有 DMA 版（`DUST_COM_UART_DMA`，`UartDma` 双缓冲 + BipBuffer） | 对照基准，不动 |
| **CAN** | **不是缺 DMA，是经典 CAN 本来就没有 DMA 这个数据形态** —— 控制器自带 32 个 TX 邮箱 + 16 元素 RX FIFO，帧 ≤ 8B，硬件就是队列 | 不补 DMA；补的是 **TX 满帧丢帧无感知** 和 **RX 回调跑在 ISR 上下文** |
| **SPI** | **底层驱动本来就有 DMA**（`CONFIG_SPI_HPM_SPI_DMA`），只是板级没开、DT 没配 `dmas` —— 而且框架同步 API 与 DMA/非 DMA 是同一条调用路径，**开关 DMA 框架层一行不用改** | 按负载决定：小事务（IMU）不需要，大块（flash）才值 |

**一句话**：CAN 的答案在「硬件已有队列，DMA 无意义，短板在软件队列」；SPI 的答案在「DMA 是开关不是实现，值不值得开取决于谁挂在总线上」。

---

## 2. 现状走读

### 2.1 对照组：UART 为什么需要 DMA

`framework/drivers/communication/stream/uart/uart.hpp`：

- UART 是**字节流**——起始时间未知、帧长未知、可能连续不断来数据。CPU 无法预知「这次要收多少」，只能：
  1. 硬件 DMA 双缓冲（`dma_buf_[2][kMaxBufSize]`，`uart.hpp:92`）轮转收；
  2. 回调把数据攒进软件队列（`BipBuffer<kMaxBufSize * 2>`，`uart.hpp:96`）；
  3. 业务线程按自己的节奏 `Read()` 取走。
- 没有 DMA 就必须中断逐字节收（`DUST_COM_UART` = `UART_INTERRUPT_DRIVEN`），高速下中断风暴。
- 结论：**UART 的 DMA 是「未知长度的流需要硬件兜底」的必然结果。**

### 2.2 SPI 现状

框架 `Spi`（`spi.hpp:74`）头注释写死了定位：

> 基于 Zephyr SPI API，支持设备树初始化、半双工读写和全双工收发。**所有操作同步阻塞，不依赖中断或 DMA。**

实现只有四个同步调用：`spi_transceive_dt` / `spi_write_dt` / `spi_read_dt`（`spi.cpp:47,59,71`）。

**但底层 HPM 驱动本来就有两套**（`sdk_glue/drivers/spi/spi_hpmicro.c`）：

| Kconfig | 路径 | 行为 |
|---------|------|------|
| `CONFIG_SPI_HPM_SPI_DMA=n`（默认） | `spi_hpm_transceive` → `transceive()`（`:635`） | 中断逐 FIFO 搬运 + `spi_context_wait_for_completion()` 信号量阻塞 |
| `CONFIG_SPI_HPM_SPI_DMA=y` | `spi_hpm_transceive` → `transceive_dma()`（`:441`） | `dma_start()` + DMA 完成回调 + **同一个信号量阻塞** |
| `CONFIG_SPI_ASYNC=y` | `.transceive_async`（`:647`） | 异步回调版（对应 Zephyr `spi_transceive_cb`） |

关键点：**两条同步路径对调用方完全等价** —— `spi_transceive_dt()` 内部就是 `spi_transceive()`，驱动按 Kconfig 选择走哪条。**框架 `Spi` 不需要为 DMA 改任何代码。**

**硬约束**（`spi_hpmicro.c:717` `SPI_DMA_CHANNELS(n)`）：

```c
.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(n, tx)),
```

`DT_INST_DMAS_CTLR_BY_NAME` 是 devicetree 宏 —— **一旦 `CONFIG_SPI_HPM_SPI_DMA=y`，DT 里每一个 `hpmicro,hpm-spi` 节点都必须有 `dmas` + `dma-names`**，缺一个就是编译期 devicetree 报错（不是运行期降级）。同时 `spi_hpm_init`（`:684`）还会检查 DMA 控制器 `device_is_ready`。

**当前用户**（以 `projects/qingyu` 为例）：

| 设备 | 总线 | DT 节点 | `spi-max-frequency` | 单次事务 | 发起节奏 |
|------|------|---------|---------------------|----------|----------|
| ICM42688P IMU | spi2 | `icm42688p@0` | **100 kHz** | 14 B（`icm42688p.cpp:134-139`，2 次事务） | 1 ms 轮询（`imu.cpp:119`） |
| W25Q128 Flash | spi1 | `w25q128@0` | 50 MHz | 读任意长度（`w25q128.cpp:230` `malloc(4+len)`）；页写 260 B（`:316-321`） | 初始化读校准 + 按需 |

**DMA 资源**（HPM5361）：`hdma` 8 通道（`hpm53xx.dtsi:346`），已占 2/3（uart4）、4/5（uart3），**剩 0/1/6/7**。请求号见 `hpm_dmamux_src.h`：

```
SPI1_RX = 0x2A, SPI1_TX = 0x2B
SPI2_RX = 0x2C, SPI2_TX = 0x2D
```

### 2.3 CAN 现状

框架 `Can`（`can.hpp:87`）：中断接收回调 + 阻塞发送。

```cpp
// can.cpp:76
int ret = can_send(dev_, frame, K_NO_WAIT, tx_callback, this);
return ret == 0;
```

底层 HPM MCAN 驱动（`sdk_glue/drivers/can/mcan_hpmicro.c`）：

- **TX**：`HPM_MCAN_NUM_TX_BUF_ELEMENTS = 32`（`:31`），`k_sem_init(&data->tx_sem, 32, 32)`（`:320`），`hpm_mcan_send` 先 `k_sem_take(&data->tx_sem, timeout)`（`:537`）——**32 个硬件邮箱，满了就等**。
- **RX**：硬件 RXFIFO0 / RXFIFO1，16 元素（`HPM_MCAN_NUM_RX_BUF_ELEMENTS`），中断触发后经 `can_rx_callback` 分发（`:275-280`）。
- **全文 0 处 DMA 引用**；Zephyr CAN 子系统 Kconfig（`zephyr/drivers/can/Kconfig`）也**没有任何 DMA / ASYNC 选项**。

**已埋但没人用的伏笔**：`framework/topic/to_can_tx/` 定义了

```cpp
// to_can_tx.cpp:17
K_MSGQ_DEFINE(user_can1_msgq, sizeof(topic::to_can_tx::Message), 16, 4);
```

全仓库 grep 只命中定义处，**没有任何生产者或消费者**。

---

## 3. 为什么 SPI / CAN 没有 DMA

### 3.1 CAN：不是「没有」，是「不需要」

1. **经典 CAN 的缓冲由控制器硬件提供**。CAN 帧 payload 最大 8 B（CAN FD 64 B），控制器内部有 TX 邮箱数组和 RX FIFO。DMA 的适用场景是「**大块连续内存 ↔ 外设寄存器**」，CAN 根本没有这种数据形态 —— 帧是硬件自行组装/拆分发送的，CPU 只负责「把帧塞进邮箱」。
2. **Zephyr CAN API 里没有 DMA 概念**。`can_send(dev, frame, timeout, cb, arg)` 的语义天然就是异步的（塞进邮箱就返回，完成走回调），没有「搬运字节数」这类参数，也不存在 DMA 版接口。
3. **所以「CAN 应该有 DMA」这个前提本身不成立** —— 就像问「为什么 CAN 没有波特率寄存器」一样。

### 3.2 SPI：底层有、板级没开、框架不需要

三层原因，逐层说清：

1. **框架层没有落点**。`Spi` 是「同步事务」抽象（发 N 字节、等回来），DMA 与否对它透明 —— `spi_transceive_dt()` 在 DMA 开与不开时是同一条调用。所以框架里**不会、也不应该**出现「`SpiDma` 类」，除非要的不是 DMA 而是**异步**（见 §4.3）。
2. **板级确实没开**。`CONFIG_SPI_HPM_SPI_DMA` 默认 `n`，且 overlay 里 spi1/spi2 都没有 `dmas` / `dma-names`（`hpm5361icb.overlay:99,114`）。
3. **当前两个用户都不在 DMA 的有效区**：
   - **IMU：14 B / 次。** DMA 的建链开销（写描述符、`dma_config`、`dma_start`）与「14 字节走 FIFO」同一个数量级，开了纯亏。**IMU 真正的瓶颈是 `spi-max-frequency = <100000>` 太低** —— 单样本 ≈ 18 B × 8 / 100 kHz ≈ **1.44 ms**，已经吃掉整个 1 ms 循环周期。ICM42688P 支持 24 MHz，把频率提上去比上 DMA 收益大两个数量级。
   - **Flash：大块。** 50 MHz 下 4 KB 读 = 655 µs；非 DMA 路径要按 FIFO 阈值打断几百次（`spi_hpm_master_transfer_callback` 每次搬 `transfer_len` 字节，`spi_hpmicro.c:78-133`），DMA 路径只要 1 次完成中断。**这才是 DMA 的真正受益者**，但当前 flash 只在 `PreInit` 读一次校准参数（几十字节），还没到痛的阈值。

### 3.3 真正该补的（不是 DMA）

| 外设 | 真正短板 | 证据 |
|------|---------|------|
| **CAN TX** | `K_NO_WAIT` + `return ret == 0` —— 32 个邮箱占满时**静默丢帧**，上层只拿到一个 `false`，无计数、无重试、无队列 | `can.cpp:76`；`mcan_hpmicro.c:537` |
| **CAN RX** | `rx_callback` 直接在 ISR 上下文调用户回调（`can.cpp:94-101`）—— 用户在 ISR 里做业务，慢一点就挤占中断/丢后续帧 | `can.cpp:94` |
| **SPI（IMU）** | `spi-max-frequency` 100 kHz 偏低，单次事务超循环周期 | `hpm5361icb.overlay:122` |
| **SPI（flash）** | 大块读 `malloc` 两次（`tx`+`rx`，`w25q128.cpp:232-233`）CPU 全程阻塞；页写用 260 B 栈缓冲（`:316`） | `w25q128.cpp` |

---

## 4. 目标设计

### 4.1 CAN 线：补软件队列，不碰 DMA

**TX 侧** —— 三档方案，改动量递增，**推荐 B**：

| 方案 | 改法 | 代价 | 效果 |
|------|------|------|------|
| A 最小 | `can.cpp:76` 的 `K_NO_WAIT` 改成可配 timeout（如 `K_MSEC(1)`） | 1 行 | 邮箱短暂满时等，不丢；但**阻塞调用线程** |
| **B 队列** | 启用已埋的 `user_can1_msgq`：发帧侧 `k_msgq_put(K_NO_WAIT)`，**一个 CAN TX 线程** `k_msgq_get(K_FOREVER)` → `Can::Send(K_MSEC(n))` | 新增 1 个薄线程 + topic 接线 | 发帧方永不阻塞、永不丢（队列满才丢且**可计数**）；邮箱满由 TX 线程等，不牵连业务线程 |
| C 只计数 | `Can::Send` 满时 `tx_drop_++`，暴露 `TxDropCount()` | 3 行 | 不解决丢帧，只让丢帧**可见** |

**RX 侧** —— `rx_callback` 里只做 `k_msgq_put`（或 `k_fifo_put`）到软件队列，业务线程取。**ISR 内不做解析、不做业务**。这与 UART 的 `Dma 回调 → BipBuffer → 线程` 是同一个思想，只是 CAN 的「硬件缓冲」已经是现成的 FIFO，软件侧只需要一层解耦。

### 4.2 SPI 线：按需开底层 DMA（框架零改动）

**要不要开，取决于总线上挂什么**：

- **只有 IMU 这类小事务 → 不开。** 先解决 §3.2 的时钟问题（IMU `spi-max-frequency` 从 100 kHz 提到 1~8 MHz，实测边界后定）。
- **有 flash / 显示 / 大块传感器 → 开。** 开启步骤（三处，全在 HPM 侧）：

**① Kconfig**（板级 conf 或 `project/prj.conf`）：

```kconfig
CONFIG_SPI_HPM_SPI_DMA=y
```

**② DT：给 DT 里**每一个** `hpmicro,hpm-spi` 节点加 `dmas` + `dma-names`**（`CONFIG` 一开就是全局，漏一个编译期就炸）：

```dts
&spi1 {
    dmas = <&hdma 0 0x2A>, <&hdma 1 0x2B>;   /* rx, tx */
    dma-names = "rx", "tx";
};

&spi2 {
    dmas = <&hdma 6 0x2C>, <&hdma 7 0x2D>;   /* rx, tx */
    dma-names = "rx", "tx";
};
```

（通道 0/1/6/7 是 hdma 剩余通道；请求号见 §2.2。）

**③ 确认 `&hdma` 在 overlay 里 `status = "okay"`**（`hpm5361icb.overlay:37` 已有）。

**框架层：不动。** `Spi::Transceive/Send/Read` 一行不改。

### 4.3 SPI 线（可选，本次不做）：异步 API 封装

如果是「不想让调用线程等 655 µs」，那要的不是 DMA 而是**异步**：`CONFIG_SPI_ASYNC=y` + 框架加一个 `spi_transceive_cb` 回调版接口（参照 `Stream` 的 `RxCallback` 形态）。

**本次不建议做** —— 当前没有任何调用方需要「SPI 传输期间去干别的」，加了就是无人使用的抽象。**留作将来 flash 要后台写、或挂显示时的预案。**

---

## 5. 分阶段执行方案

> 顺序：CAN 先（当前有实际丢帧风险），SPI 后（等 flash 用法变重再做）。

### 阶段 1：CAN TX 队列（方案 B）

1. `framework/topic/to_can_tx/to_can_tx.hpp` —— 已有 `Message{tx_id, data[8]}` + `extern user_can1_msgq`，确认是否需要补一个 `Put(const Message&)` 便捷函数（若上层要跨编译单元用）。
2. 新增 CAN TX 线程（业务线程，放用户区 `project/thread/`，主干不放）：
   - 循环 `k_msgq_get(&user_can1_msgq, &msg, K_FOREVER)` → 填 `can_frame` → `Can::Send()`。
   - `Can::Send` 的 timeout 从 `K_NO_WAIT` 改成 `K_MSEC(N)`（N 按总线负载定，**待用户拍板**）。
3. 发帧方（电机控制等）从直接 `Can::Send` 改成 `k_msgq_put(K_NO_WAIT)`。

### 阶段 2：CAN RX 解耦

1. `framework/drivers/communication/can/can.cpp:94` 的 `rx_callback` 改为**只入队**（`k_msgq_put` / `k_fifo_put`，`K_NO_WAIT`，满则计数丢帧）。
2. 业务侧从「注册回调处理」改成「线程取队列解析」。
3. 队列深度、是否每路 CAN 一个队列 —— **待用户拍板**（现在只有 `user_can1_msgq` 一个，CAN2 没有）。

### 阶段 3：SPI DMA（**触发条件：flash/大块设备成为热路径**）

1. 按 §4.2 三处改：
   - 板级 conf 加 `CONFIG_SPI_HPM_SPI_DMA=y`；
   - overlay 给**每个** hpm-spi 节点加 `dmas`/`dma-names`；
   - 确认 `&hdma` okay。
2. 框架零改动。
3. **不满足触发条件就不做** —— 小事务上 DMA 是净损失。

### 阶段 4：SPI 时钟（与 DMA 无关，独立收益）

1. ICM42688P `spi-max-frequency` 100 kHz → 按芯片手册与实际信号质量上调（ICM42688P 最高 24 MHz，实际受走线限制）。
2. **上板实测**：调高后 IMU 采样是否还稳定（`WHO_AM_I` 读取、连续采样无错帧）。
3. 这条**收益最大、改动最小**（DT 一个数），建议**排在 SPI DMA 之前**。

---

## 6. 验证标准

| 阶段 | 验证项 | 方法 |
|------|-------|------|
| 1 | 队列模式下连续高频发帧不丢 | 上位机发 N 帧，对端计数 == N |
| 1 | 业务线程不被 CAN 阻塞 | 发帧侧线程周期抖动在预期内 |
| 1 | 队列满的行为符合预期 | 灌满队列，确认丢帧且有计数/日志 |
| 2 | ISR 内无业务耗时操作 | 看 ISR 里只有入队 |
| 2 | 高帧率下不丢 RX | 总线满负载跑，对端计数对比 |
| 3 | `CONFIG_SPI_HPM_SPI_DMA=y` 能编过 | **所有** hpm-spi 节点都配了 `dmas`（漏一个 devicetree 报错） |
| 3 | flash 读内容正确 + 大块读耗时下降 | 读回比对 + 计时对比开/关 DMA |
| 4 | 提频后 IMU 采样稳定 | `WHO_AM_I` 正确 + 长时间连续采样无错 |

> 编译/烧录是用户动作，AI 不代跑（见记忆 `verbatim-execution` 规则 8）。

---

## 7. 风险点

### 7.1 `CONFIG_SPI_HPM_SPI_DMA` 是全驱动级的

开了就必须**所有** hpm-spi 节点配 `dmas` —— 将来新增 SPI 设备忘了配，编译期直接炸，不会静默降级。这是好事（早暴露），但要在文档/注释里写明。

### 7.2 hdma 通道只剩 4 个

8 通道已占 4（uart3/uart4 各 2）。SPI1+SPI2 全开又要 4 个 —— **刚好用满**。将来再加 DMA 外设（ADC/I2C 等）会没通道，需要重新规划分配表。

### 7.3 CAN 队列深度与内存

`user_can1_msgq` 深度 16、`Message` 12 B → 192 B + 队列头。若每路 CAN 一个队列、每队列加深，**嵌入式 RAM 敏感，分配前问用户**（见记忆 `embedded-resource-sensitivity`）。

### 7.4 CAN TX 线程优先级

TX 线程优先级低于业务线程会让发帧延迟、高于会抢占控制周期 —— **优先级取值待用户拍板**。

### 7.5 SPI 提频的电气风险

50 MHz/24 MHz 请求值是否真能跑（走线、上拉、CS 建立时间）需示波器/实测确认，不能只看芯片手册上限。

---

## 8. 待用户拍板清单

1. **CAN TX**：选方案 A / **B（推荐）** / C？`Can::Send` 的 timeout 值？
2. **CAN RX**：要不要做解耦？每路 CAN 一个队列还是共用？
3. **CAN 队列**：深度多少（RAM 预算）？
4. **CAN TX 线程**：优先级？
5. **SPI DMA**：现在开还是等 flash 变重？（**建议：等**）
6. **SPI 时钟**：IMU 提频到多少？（**建议：先做这条**）

---

## 9. 执行清单（按序勾）

- [ ] 阶段 4：IMU `spi-max-frequency` 提频（DT 一行）+ 上板验证
- [ ] 阶段 1：CAN TX 队列（topic 接线 + TX 线程 + `Can::Send` timeout）
- [ ] 阶段 2：CAN RX 解耦（ISR 只入队 + 业务线程取）
- [ ] 阶段 3：SPI DMA（**仅当触发条件满足**：板级 conf + 全节点 `dmas` + `&hdma` okay）
- [ ] 更新 `framework/drivers/ARCHITECTURE.md` 与相关 README
