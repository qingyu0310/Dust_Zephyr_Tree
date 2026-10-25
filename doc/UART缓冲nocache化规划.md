# UART 缓冲 nocache 化规划（D-Cache 与 DMA 一致性）

> 2026-10-25。背景：`uhpm6e00evk` 上开启 D-Cache（默认 `CONFIG_DCACHE=y`）后，串口发送**长度正确、内容全 0**。原因是 `UartDma` 的发送缓冲位于普通可缓存 RAM，CPU 写入后未写回内存，DMA 从物理内存读到的是旧值。
>
> **拍板（用户 2026-10-25）**：采用**方案 A（缓冲放 nocache 段）**，不使用方案 B（每次 DMA 前后手工 flush/invalidate）。

---

## 1 问题

### 1.1 现象

`dust flash` 后串口收到：

```text
b'\x00\x00\x00\x00\x00\x00\x00'          ← 长度 7，全 0
b'\x00' × 700+                            ← 长度对，内容全 0
```

长度由 DMA 描述符决定（正确），内容由内存决定（全 0）——**两者分裂 = 内存侧数据没写回**。

### 1.2 根因

`hpm5361icb_defconfig` 里有 `CONFIG_DCACHE=n`，`uhpm6e00evk_defconfig` 漏了这行，默认 `CONFIG_DCACHE=y`。

开启 D-Cache 后：

```text
CPU:  memcpy(tx_data_, data, len)   → 写进 cache，标记 dirty
DMA:  从 tx_data_ 的物理地址读       → 读到 cache 之前的旧内容（全 0）
```

### 1.3 为什么现在没有自动维护

HPM 的 DMA 驱动（`dmav2_hpmicro.c`）和 UART 驱动（`uart_hpmicro.c`）**全文没有 `cache` 相关代码**——不 clean、不 invalidate、不支持 `__sparse_cache` 注解。一致性完全由调用方负责。

---

## 2 方案

### 2.1 A（采用）：缓冲放 nocache 段

把 `UartDma` 里所有被 DMA 直接读写的缓冲加 `__nocache` 属性，链接到 `.nocache` 段，由 HPM 的 PMA 配置成 non-cacheable 区。

**优点**：
- 一次声明，永久生效，**零调用方改动**
- 不需要在每个 `Send` / `Read` 前后插维护代码
- 不需要关心 cache line 对齐——整块区域都不缓存

**代价**：
- 占用 `.nocache` 段（当前 `_nocache_ram_size = 0`，还没有任何东西在里面）
- 这些缓冲从 RAM 普通区移到 nocache 区，**不增加总 RAM 用量**，只是换位置

### 2.2 B（不采用）：每次 DMA 前后手工维护

```cpp
sys_cache_data_flush_range(tx_buf_, len);         // TX 前：写回
sys_cache_data_invalidate_range(rx_buf_, len);    // RX 后：作废
```

**不采用的理由**：
- 维护点分散（`Send`、`Read`、ISR 回调的 `Reserve`/`Commit` 各一处），漏一处就是全 0 或旧数据
- 必须按 cache line 对齐处理首尾，否则误伤相邻数据
- 本项目 `CONFIG_CACHE_MANAGEMENT` **未开启**（`.config:47`），Zephyr 的 `sys_cache_*` 系列不可用，要直接调 HPM SDK 的 `l1c_dc_*`，与框架解耦目标冲突

---

## 3 设计

### 3.1 需要改的缓冲

`framework/drivers/communication/stream/uart/uart.hpp` 的私有成员，凡是被 DMA 直接读写的都要加 `__nocache`：

| 成员 | 大小 | DMA 方向 | 说明 |
|------|------|----------|------|
| `dma_buf_[2][kMaxBufSize]` | 2 × 256 = 512 B | **RX 写** | 硬件 DMA 双缓冲，`uart_rx_enable` / `uart_rx_buf_rsp` 的缓冲 |
| `tx_data_[kMaxBufSize]` | 256 B | **TX 读** | `uart_tx` 的源缓冲 |
| `rx_bip_` 内部的 `buffer_[Capacity]` | 512 B（`kMaxBufSize * 2`） | 否 | **CPU 侧软件队列，DMA 不直接访问**，不需要 nocache |

**只改前两个，`rx_bip_` 不动** —— DMA 只写 `dma_buf_`，数据由 CPU 在 `uart_dma_callback` 里 `memcpy` 进 `rx_bip_`，那是 CPU-to-CPU 拷贝，cache 一致。

### 3.2 具体改法

> ⚠️ **2026-10-25 实测修正**：`__nocache` 展开为 `__attribute__((section(...)))`，**只能用于函数或全局变量，不能用于类成员**。直接写在 `UartDma` 的成员声明上编译报错：
>
> ```text
> error: 'section' attribute only applies to functions, global variables,
>        Objective-C methods, and Objective-C properties
> ```
>
> 所以**不能**用"给成员加 `__nocache`"的方式实现。

#### 可行形态：整类实例放 `nocache_load` 段

`__nocache` 只能标在**全局/静态变量**上。`UartDma` 的实例全部是 `static` 局部变量，所以标在**实例**上：

```cpp
// shell.cpp / trd_remote.cpp 等调用点
__nocache_load static UartDma rx {};
```

> ⚠️ **必须用 `__nocache_load` 而不是 `__nocache`（2026-10-25 踩坑）**
>
> `__nocache` 映射到 `.nocache`（**noload** 段，`zephyr.map` 里 `nocache 0x1200000 0x5c0` 无 load address），初值**不会被从 ROM 复制到 RAM**。`UartDma` 带初值成员：
>
> - `uint8_t* tx_buf_ = tx_data_;` → 野指针
> - `bool buf_free_[2] = {true, true};` → 垃圾
> - `atomic_t tx_busy_ / ready_ = ATOMIC_INIT(0);` → 垃圾
>
> 结果：`Send()` 里 `uart_tx(dev_, tx_buf_, len, 0)` 传野指针 → **`KERNEL_PANIC (reason=4)`**。
>
> `__nocache_load` 映射到 `.nocache_load`（**有 load address**，`nocache_load 0x12005c0 0x240 load address 0x80013090`），启动时从 ROM 拷初值，与普通 `.data` 同机制。

这样整个对象落在 nocache 区。**副作用**：`rx_bip_`（512 B）也变成 non-cacheable，CPU 读写它稍慢，但换来零维护、绝对正确。

#### 另一种形态：缓冲改成指针 + 外部 nocache 存储

```cpp
// uart.hpp
uint8_t* dma_buf_[2] = {nullptr, nullptr};
uint8_t* tx_data_ = nullptr;

// uart.cpp 文件作用域
__nocache static uint8_t g_dma_buf_storage[2][UartDma::kMaxBufSize];
__nocache static uint8_t g_tx_storage[UartDma::kMaxBufSize];

// Init() 里绑定
dma_buf_[0] = g_dma_buf_storage[0];
dma_buf_[1] = g_dma_buf_storage[1];
tx_data_    = g_tx_storage;
tx_buf_     = tx_data_;
```

**代价**：多个 `UartDma` 实例（当前 shell 一个、remote 一个）会**共用同一块存储**，必须改成每实例一份（按实例数乘以 `static` 数组，或数组索引进 `dma_buf_[i]`）。改动面大、易错。

#### 结论

**优先选"整类实例放 nocache_load 段"**（形态一）：一处声明、零内部改动、每实例独立，且初值正确加载。代价只是多浪费 512 B 的 non-cacheable RAM。

### 3.3 前置条件（已在当前工程满足）

| 条件 | 状态 | 证据 |
|------|------|------|
| `CONFIG_NOCACHE_MEMORY=y` | ✅ 已开 | `project/build/zephyr/.config:104` |
| `CONFIG_ARCH_HAS_NOCACHE_MEMORY_SUPPORT=y` | ✅ 已开 | `.config:485` |
| `.nocache` 段存在 | ✅ 已存在 | `zephyr.map:4706` `nocache 0x1200000 0x0` |
| PMA 已配置 nocache 区 | ✅ `soc_init_pma()` | `sdk_glue/soc/hpmicro/HPM6E00/soc.c:106-124`，`SYS_INIT` 在 `PRE_KERNEL_1` 执行 |
| 其他板卡（5361）不受影响 | ✅ `CONFIG_DCACHE=n` 时 `__nocache` 展开为空 | `section_tags.h:56` |

**关键点**：`__nocache` 在 `CONFIG_NOCACHE_MEMORY=n` 时**展开为空宏**（`section_tags.h:56`），所以这个改动对关闭 D-Cache 的板卡（如 5361）**零影响**，不需要 Kconfig 开关。

### 3.4 PMA 约束

`soc_init_pma()` 里有两条断言：

```c
assert((length & (length - 1U)) == 0U);         // 长度必须是 2 的幂
assert((start_addr & (length - 1U)) == 0U);     // 起始地址必须按长度对齐
```

链接脚本里 `.nocache` 区已用 `ALIGN(1 << LOG2CEIL(_nocache_ram_size))` 对齐（`zephyr.map:4708`），会自动满足。**但要注意**：加入约 768 B 后 `_nocache_ram_size` 从 0 变成 1024（向上取到 2 的幂），PMA 会预留 1 KB。RAM 多占 1 KB（普通 RAM 那边少 768 B，净增约 256 B）。

---

## 4 执行步表

> **拆成 5 步，每次只执行一步。**
> 用户说"开始第 N 步"= 只改表格里那一个文件，改完停下报告，等"继续"。

| 步 | 文件 | 动作 | 依赖前置 | 连带 |
|----|------|------|----------|------|
| **1** | `framework/shell/shell.cpp` | `static UartDma rx {};` 前加 `__nocache_load`；文件加 `#include <zephyr/linker/section_tags.h>` | 无 | 无（未开 NOCACHE_MEMORY 时展开为空） |
| **2** | `project/thread/remote/trd_remote.cpp` | `static UartDma rx {};` 前加 `__nocache_load` | 步 1 | 无 |
| **3** | `uhpm6e00evk_defconfig` | 删除 `CONFIG_DCACHE=n`（恢复默认 `y`） | 步 1、2 | 无 |
| **4** | 全量检查 | `grep -rn "__nocache_load" framework project` 应有 2 处（shell / trd_remote）；**确认没有裸 `__nocache`** | 步 1~3 | 无 |
| **5** | 文档回填 | `framework/drivers/README.md` 说明"UartDma 实例必须 `__nocache`（开了 D-Cache 的板卡）" | 步 4 | 无 |

**编译状态**：全程可编译。

**编译是用户的动作，AI 不代跑。**

---

## 5 验证清单

| # | 场景 | 预期 |
|---|------|------|
| 1 | `dust build uhpm6e00evk -p` 后看 map | `.nocache_load` 段**有 load address**（如 `load address 0x80013090`）；`_nocache_ram_size` 是 2 的幂 |
| 2 | `dust flash` + 串口接收 | 收到 `teee\r\n` 等真实文本，**不再是全 0** |
| 3 | 连续发送 1 分钟 | 无丢帧、无错乱（验证 tx 缓冲写入被 DMA 正确读到） |
| 4 | 接收 PC 发来的数据（remote/回环） | `rx_bip_` 内容正确（验证 `dma_buf_` → `rx_bip_` 的 CPU 拷贝正常） |
| 5 | `hpm5361icb` 构建 | 照常通过（`__nocache` 展开为空，行为不变） |

---

## 6 待确认

1. **是否同时给 `BipBuffer` 的 `buffer_` 加 `alignas(64)` 以外的处理** —— 本规划判定不需要（DMA 不直接访问）。若后续改成 DMA 直接写 `rx_bip_`，需重新评估。
2. **其他驱动（SPI / CAN / USB）是否也有同类问题** —— 本规划只覆盖 `UartDma`。SPI 若开 DMA（`CONFIG_SPI_HPM_SPI_DMA`）会有同样问题，需单独评估。
