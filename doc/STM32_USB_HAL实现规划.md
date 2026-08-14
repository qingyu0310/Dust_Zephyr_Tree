# STM32 USB HAL 实现规划与完成记录（UsbHalStm32）

> 2026-08-13 规划，**2026-08-14 实现完成 + 回环验证通过**。
> **目标**：参照 `temp/Dust_SentinelRobot_L_Game`（STM32F407 + STM32Cube USB_DEVICE CDC 虚拟串口）的 USB 设置，编写 STM32F407 OTG FS 的 `UsbHalStm32`，与现有 `UsbHalHpm` 同构，复用同一套 framework 中间层（`UsbHal` 接口 + `UsbDevPort`/`UsbCdcAcm` 协议栈 + `Usb` Stream 适配）。
> **中间层一行不改**；新增的只有子树 drivers 下的板卡底层（HAL 文件 + Kconfig + CMake）+ 目标 STM32 板卡 overlay。
> **⚠️ 调试中发现并修复一个根因**：F4 slave（非 DMA）模式下 HAL 的 `xfer_buff` 在 RXFLVL 处理时已前移到数据末尾，DataOut 回调把它当数据指针 → **长度对、内容错**（`[err]` 空、回传垃圾）。修法见 §3.8。**修复后回环验证通过**（trd_test.cpp：PC 发 ASCII → 收到 → 原样回传）。

---

## 0. 背景与目标

### 现状分层

```text
framework/drivers/communication/stream/usb/
├── usb.cpp / usb.hpp              Usb — 顶层 Stream 适配（业务侧接口）      ← 中间层，不动
├── usb_rx_queue.hpp               接收队列                                  ← 中间层，不动
├── interface/usb_hal.hpp          UsbHal 纯虚接口（芯片无关）                ← 中间层，不动
├── interface/usb_types.hpp        类型定义                                  ← 中间层，不动
├── core/                          UsbDevPort / UsbCdcAcm / descriptor 协议栈  ← 中间层，不动
└── Kconfig                        DUST_USB_DEVICE_STACK（HAL select 入口）  ← 只加一行 select

zephyr_user/drivers/               ← 板卡底层归子树（与 fsmc、usb_hal_hpm 同层）
├── usb/hal/hpm/usb_hal_hpm.{hpp,cpp}   HPM5361 EHCI HAL（已下沉，2026-08-09）
└── usb/hal/stm32/usb_hal_stm32.{hpp,cpp}  STM32 OTG FS HAL（2026-08-13 新增）
```

### 为什么这次只写 STM32 HAL

- USB 协议栈（描述符/标准请求/CDC 请求/EP0 状态机）已在 `UsbDevPort`/`UsbCdcAcm` 实现，`UsbHal` 只暴露 14 个硬件操作原语 + 事件回调。
- HPM 已跑通整条链路。把 `UsbHalStm32` 按 `UsbHal` 接口补齐，中间层零改动即可在 STM32 板卡上复用。
- STM32 底层选择**STM32Cube HAL PCD**（`stm32f4xx_hal_pcd.c`），与参考项目 `usbd_conf.c` 同一底层；Zephyr 官方 `usb_dc_stm32.c` 也是基于 `HAL_PCD_*`，可对照参考。

### 边界（本次不动）

`UsbHal` 接口、`UsbDevPort`/`UsbCdcAcm`/descriptor 协议栈、`usb.cpp`、`interface/`——全部留在 framework。framework 只改一行 Kconfig（新增 HAL_STM32 的 select）。

---

## 1. 参考项目 USB 设置盘点（证据，逐项对照）

参考项目 `temp/Dust_SentinelRobot_L_Game`（STM32F407IGH6 + STM32CubeMX 生成 USB_DEVICE CDC）：

| 项 | 参考项目设置 | 证据（文件:行） |
| --- | --- | --- |
| 控制器 | USB OTG FS（全速 12 Mbps） | `USB_DEVICE/Target/usbd_conf.c:337` `hpcd_USB_OTG_FS.Instance = USB_OTG_FS` |
| 引脚 | PA12→OTG_FS_DP、PA11→OTG_FS_DM、PA10→OTG_FS_ID，AF10 | `usbd_conf.c:78-89` |
| 中断 | `OTG_FS_IRQn`，优先级 5 | `usbd_conf.c:95-96` |
| PCD 参数 | `dev_endpoints=4, speed=PCD_SPEED_FULL, dma_enable=DISABLE, phy_itface=PCD_PHY_EMBEDDED, Sof_enable=DISABLE, low_power_enable=DISABLE, lpm_enable=DISABLE, vbus_sensing_enable=DISABLE, use_dedicated_ep1=DISABLE` | `usbd_conf.c:338-346` |
| FIFO | `RX=0x80(128 words/512B)`, `TX0=0x40(64 words/256B)`, `TX1=0x80(128 words/512B)` | `usbd_conf.c:367-369` |
| 端点 | EP0 控制 64B；CDC 数据 IN=0x81/OUT=0x01(Bulk 64B)；CDC 通知=0x82(Int 8B) | `usbd_cdc.h` 标准 ST 定义 |
| 描述符 | 设备类 0x02/子类 0x02（CDC），VID=1155, PID=22336 | `USB_DEVICE/App/usbd_desc.c:167-168, 65-68` |
| 设备枚举 | `MX_USB_DEVICE_Init` → USBD_Init/RegisterClass(CDC)/Start | `usbd_desc.c:169-181`、`usb_device.c:64-91` |
| 时钟使能 | `__HAL_RCC_USB_OTG_FS_CLK_ENABLE()` + GPIOA 时钟 | `usbd_conf.c:78, 92` |
| 传输模式 | **无 DMA**（`dma_enable=DISABLE`），HAL 中断逐包搬运 FIFO | `usbd_conf.c:340` |

**关键结论**：参考项目 = STM32F407 OTG FS 全速设备模式，无 DMA、内置 FIFO、中断驱动。UsbHalStm32 照此实现（已落地）。

---

## 2. 现状盘点（代码库证据）

### 2.1 UsbHal 接口（framework `interface/usb_hal.hpp`）

14 个纯虚方法 + 1 个 `GetDefaultHal()`：

```cpp
bool Init(const Config& cfg, EventCallback cb, void* ctx);
bool Connect();  void Disconnect();
bool SetAddress(uint8_t addr);  Speed GetSpeed() const;
bool EpOpen(const EndpointConfig&);  bool EpClose(uint8_t ep);
bool EpStall(uint8_t ep, bool stall);
bool EpStartRx(uint8_t ep, uint16_t len);  bool EpStartTx(uint8_t ep, const uint8_t* data, uint16_t len);
bool Ep0StartIn(const uint8_t* data, uint16_t len);  bool Ep0StartOut(uint8_t* data, uint16_t len);
bool Ep0StatusIn();  bool Ep0StatusOut();
```
`UsbHal::Config`：`{ busid, reg_base, irq_num, irq_priority }`。
`UsbHal::Event`：`{ type(Reset/SetupReceived/TransferComplete/Connected/Disconnected), endpoint, data, length, error, setup[8], speed }`。
`EventCallback = void(*)(void* ctx, const Event&)`。

### 2.2 中间层调用约定（framework `core/usb_dev_port.cpp`）

- `UsbDevPort::Start()` → `hal_->Connect()`（`usb_dev_port.hpp:50`）。
- SETUP 阶段：HAL 上报 `SetupReceived`（`event.setup` 8 字节），端口层解析标准/类请求。
- EP0 状态机：端口层按 `DataIn → StatusOut`、`DataOut → StatusIn`、`StatusIn/Out → Idle` 推进（`usb_dev_port.cpp:342-407`），**HAL 只上报 TransferComplete，不参与协议**。
- 批量 OUT：端口层每次完成后调 `hal_->EpStartRx` 重提（`RequeueRx`），**HAL 不自动重提**。
- 批量 IN 错误：端口层 `EpStall(false) + EpClose + EpOpen` 重建（`usb_dev_port.cpp:390-401`）。
- 自研 CDC 端点（`core/usb_cdc_config.hpp:40-44`）：通知=**0x83**（Int IN 8B）、Bulk OUT=0x01、Bulk IN=0x81。**注意：通知端点是 EP3，不是参考项目的 EP2**。

### 2.3 UsbHalHpm 参考实现（zephyr_user/drivers/usb/hal/hpm/usb_hal_hpm.cpp）

文件级结构（UsbHalStm32 照此骨架）：
- 文件级 `static` 缓冲（HPM 需 `.nocache`，STM32 不需要）。
- `extern "C" void usb_isr_entry(const void* arg)` trampoline → `static_cast<UsbHalHpm*>(arg)->Isr()`。
- `UsbHal& GetDefaultHal()` 返回 `static UsbHalHpm`。
- `Init`：时钟/PHY → `irq_connect_dynamic` → 控制器 init → 记录 `callback_/context_/ready_`。
- `Isr`：读中断状态 → 分发 Reset/PortChange/Setup/TransferComplete → `callback_(context_, ev)`。

### 2.4 业务接入（已用 trd_test.cpp 验证）

规划原指向 `project/thread/pc/trd_pc.cpp`，但 **2026-08-13 主干瘦身后 trd_pc.cpp 已从主干删除**（迁用户区），STM32 USB 验证实际由 `project/thread/test/trd_test.cpp` 承载：

```cpp
UsbHal::Config cfg {};
cfg.reg_base     = DT_REG_ADDR(DT_NODELABEL(usbotg_fs));
cfg.irq_num      = DT_IRQN(DT_NODELABEL(usbotg_fs));
cfg.irq_priority = 5;   // 参考项目 OTG_FS_IRQn 优先级 5
while (!usb_.Init(cfg)) { k_msleep(100); }
```

### 2.5 构建链现状

- 子树 `zephyr_user/drivers/Kconfig`：`DUST_USB_DEVICE_HAL_HPM` 定义（Kconfig:14-16）。
- 子树 `zephyr_user/drivers/CMakeLists.txt`：`if(CONFIG_DUST_USB_DEVICE_HAL_HPM)` 编译块（CMakeLists:11-21）。
- framework `communication/stream/usb/Kconfig`：`DUST_USB_DEVICE_STACK` → `select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_DUSTUSB_ENABLED`（注意 binding 现名 `dustusb`，不是规划时的 `qingyusb`）。
- **STM32 HAL 源装配（关键）**：`stm32f4xx_hal_pcd.c` 由 `CONFIG_USE_STM32_HAL_PCD` 控制（`e:/Zephyr/modules/hal/stm32/stm32cube/stm32f4xx/CMakeLists.txt:53-54` `zephyr_library_sources_ifdef(CONFIG_USE_STM32_HAL_PCD ...)`）；`stm32f4xx_hal.c` 无条件编译（该文件第 7 行）。`stm32f4xx_ll_usb.c` 由 `CONFIG_USE_STM32_LL_USB` 控制（HAL_PCD 内部依赖 `USB_*` 底层函数，**必须一并 select**）。Zephyr 的 usb_dc_stm32 正是 `select USE_STM32_HAL_PCD`（`zephyr/drivers/usb/device/Kconfig:55-56`）。

### 2.6 目标 STM32 板卡

`project/boards/st/` 下两块 F407 板卡（OTG FS 完全相同）：
- `board_rm_c/stm32f407igh6`（RM 板，有 can/spi/bmi088，最接近参考项目"哨兵"）。
- `puzhong/stm32f4_disco`（普中板，usart1 占 PA9/PA10）。

**F407 OTG FS 节点**（`zephyr/dts/arm/st/f4/stm32f4.dtsi:314`）：

```dts
usbotg_fs: usb@50000000 {
    compatible = "st,stm32-otgfs";
    reg = <0x50000000 0x40000>;
    interrupts = <67 0>;
    num-bidir-endpoints = <4>;
    ram-size = <1280>;
    maximum-speed = "full-speed";
    clocks = <&rcc STM32_CLOCK(AHB2, 7)>, <&rcc STM32_SRC_PLL_Q NO_SEL>;
    status = "disabled";
};
```

**OTG 引脚 pinctrl**（hal_stm32 模块自带，`/omit-if-no-ref/`）：`usb_otg_fs_dm_pa11`、`usb_otg_fs_dp_pa12`（见 `e:/Zephyr/modules/hal/stm32/dts/st/f4/*pinctrl.dtsi`）。

**Zephyr 板卡范例**（`zephyr/boards/adafruit/feather_stm32f405/adafruit_feather_stm32f405.dts:113-117`）：
```dts
zephyr_udc0: &usbotg_fs {
    pinctrl-0 = <&usb_otg_fs_dm_pa11 &usb_otg_fs_dp_pa12>;
    pinctrl-names = "default";
    status = "okay";
};
```

---

## 3. UsbHalStm32 设计

### 3.1 接口映射表（UsbHal → STM32 HAL PCD）

| UsbHal 方法 | STM32 实现 |
| --- | --- |
| `Init` | 时钟 `__HAL_RCC_USB_OTG_FS_CLK_ENABLE()` → `pinctrl_apply_state` → `HAL_PCD_Init`（参数见 3.2）→ FIFO（3.3）→ 存回调/ctx → `irq_connect_dynamic` + `irq_enable` |
| `Connect` / `Disconnect` | `HAL_PCD_Start(&s_pcd)` / `HAL_PCD_Stop(&s_pcd)` |
| `SetAddress` | `HAL_PCD_SetAddress(&s_pcd, address)` |
| `GetSpeed` | 返回 `Speed::Full`（FS 设备固定，参考项目 PCD_SPEED_FULL） |
| `EpOpen` | `HAL_PCD_EP_Open(&s_pcd, cfg.address, cfg.max_packet_size, type)`；type 映射见 3.4 |
| `EpClose` | `HAL_PCD_EP_Close(&s_pcd, ep)` |
| `EpStall(ep, true/false)` | `HAL_PCD_EP_SetStall(&s_pcd, ep)` / `HAL_PCD_EP_ClrStall(&s_pcd, ep)` |
| `EpStartRx` | `irq_lock` → `rx_buf_[rx_ping_]` 轮换 → `out_ep_[idx].buf=...` → `HAL_PCD_EP_Receive(&s_pcd, ep, buf, len)` |
| `EpStartTx` | `irq_lock`（防 DIEPEMPMSK RMW 丢更新）→ `memcpy(tx_buf_, data, len)` → `HAL_PCD_EP_Transmit(&s_pcd, ep, tx_buf_, len)` |
| `Ep0StartIn/Out` | 同 `EpStartTx`/`EpStartRx` 作用在 ep0（0x80 / 0x00），EP0 IN 超 64B 分包续传（`ep0_tx_rem_`） |
| `Ep0StatusIn/Out` | `HAL_PCD_EP_Transmit(&s_pcd, 0x80, nullptr, 0)` / `HAL_PCD_EP_Receive(&s_pcd, 0x00, nullptr, 0)` |
| `Isr` | `HAL_PCD_IRQHandler(&s_pcd)`，由 HAL 回调转 Event（3.5） |

> 注：PCD 句柄为文件级 `static PCD_HandleTypeDef s_pcd`（同 HPM s_handle 模式），缓冲为类成员 `rx_buf_[2][256]`/`tx_buf_[256]`（非规划期的 s_rx_buf/s_tx_buf 文件级写法）。

### 3.2 PCD 参数（old=参考项目 USBD_LL_Init，定死不改）

```c
s_pcd.Instance                        = USB_OTG_FS;
s_pcd.Init.dev_endpoints              = 4;              // num-bidir-endpoints
s_pcd.Init.speed                      = PCD_SPEED_FULL; // FS 全速
s_pcd.Init.dma_enable                 = DISABLE;        // 无 DMA，中断搬运
s_pcd.Init.phy_itface                 = PCD_PHY_EMBEDDED;
s_pcd.Init.Sof_enable                 = DISABLE;
s_pcd.Init.low_power_enable           = DISABLE;
s_pcd.Init.lpm_enable                 = DISABLE;
s_pcd.Init.vbus_sensing_enable        = DISABLE;        // 不检测 VBUS（设备模式自供电）
s_pcd.Init.use_dedicated_ep1          = DISABLE;
// s_pcd.pData = this;（Init 末尾，供 HAL 弱回调反查实例）
```

### 3.3 FIFO 分配（默认按参考项目 + 补 EP3 风险说明）

```c
HAL_PCDEx_SetRxFiFo(&s_pcd, 0x80);   // RX = 128 words (512B)，所有 OUT 共用
HAL_PCDEx_SetTxFiFo(&s_pcd, 0, 0x40); // EP0 IN = 64 words (256B)
HAL_PCDEx_SetTxFiFo(&s_pcd, 1, 0x80); // EP1 IN = 128 words (512B) → CDC Bulk IN
```

**⚠️ 风险**：STM32 OTG 的 TX FIFO 按"每个实际使用的 IN 端点"分配。自研 CDC 通知端点是 **EP3 (0x83)**，参考项目只配到 EP1，未给通知端点分配 TX FIFO。
- 若通知端点**从不实际发送**（CDC 通知仅 DTR/RTS 变化等事件触发，`require_dtr=false` 时几乎不发送）→ 参考项目同款配置够用，枚举/收发正常。**当前验证即为此场景，通过。**
- 若通知端点**需要实际发送** → 补 `HAL_PCDEx_SetTxFiFo(&s_pcd, 2, 0x10)` + `HAL_PCDEx_SetTxFiFo(&s_pcd, 3, 0x10)`，并压缩 RX/TX1（OTG FS RAM 共 320 words：`RX=0x40(64) + TX0=0x10(16) + TX1=0x40(64) + TX2=0x10(16) + TX3=0x10(16) = 176 ✓`）。

### 3.4 端点类型映射

```cpp
uint8_t EpType(EndpointType t) {
    switch (t) {
        case EndpointType::Control:     return EP_TYPE_CTRL;
        case EndpointType::Isochronous: return EP_TYPE_ISOC;
        case EndpointType::Bulk:        return EP_TYPE_BULK;
        case EndpointType::Interrupt:   return EP_TYPE_INTR;
    }
    return EP_TYPE_BULK;
}
```

### 3.5 HAL 回调 → Event 映射（weak 弱函数覆盖，`USE_HAL_PCD_REGISTER_CALLBACKS=0`）

`PCD_HandleTypeDef.pData` 存 `this`；回调里 `static_cast<UsbHalStm32*>(hpcd->pData)` 取实例。

| HAL 弱回调 | 转发的 Event |
| --- | --- |
| `HAL_PCD_SetupStageCallback(hpcd)` | `Event{SetupReceived, setup=hpcd->Setup[8]}` |
| `HAL_PCD_DataInStageCallback(hpcd, epnum)` | `self->OnDataIn(epnum)`：EP0 超 64B 分包续传；批量 IN 满包在回调内**同步补发 ZLP**（见 doc/STM32_USB回传64B卡住诊断.md）；随后 `Event{TransferComplete, ep=epnum\|0x80, length=IN_ep.xfer_count}` |
| `HAL_PCD_DataOutStageCallback(hpcd, epnum)` | `Event{TransferComplete, ep=epnum, data=self->GetRxBufBase(epnum), length=HAL_PCD_EP_GetRxCount(...)}` —— **不能用 `xfer_buff`，见 §3.8** |
| `HAL_PCD_ResetCallback(hpcd)` | `HAL_PCD_EP_Open(0x00, 64, CTRL)` + `HAL_PCD_EP_Open(0x80, 64, CTRL)` → `Event{Reset}` |
| `HAL_PCD_ConnectCallback(hpcd)` | `Event{Connected, speed=Speed::Full}` |
| `HAL_PCD_DisconnectCallback(hpcd)` | `Event{Disconnected}` |
| `HAL_PCD_SOFCallback` / `Suspend` / `Resume` | 忽略（空实现） |

**无 DMA 的 DataIn/DataOut 语义**：`HAL_PCD_EP_Transmit/Receive` 后数据在 FIFO/缓冲中，HAL 中断搬运；完成回调时 `xfer_count` 有效。**每次 OUT 完成后必须由中间层 `RequeueRx` 重新 `HAL_PCD_EP_Receive`**（接口约定：HAL 不自动重提，符合 `usb_dev_port.cpp` 调用方式）。

### 3.6 端点与缓冲管理

- `kNumEps = 4`（`dev_endpoints=4`，EP0-3；自研 CDC 用 EP0/1/3 ✓）。
- `EpState` 数组记录 `{buf, len, mps, enable}`（同 HPM 的 in_ep_/out_ep_），索引 `ep & 0x0F`。`mps` 供批量 IN 满包 ZLP 判定。
- 缓冲（**普通 RAM，无需 nocache/cache 操作**，与 HPM 关键差异）：

```cpp
static constexpr uint16_t kBufSize = 256;   // 对齐 UsbHal::kTxBufSize/kRxBufSize
uint8_t rx_buf_[2][256] {};                 // OUT 双缓冲轮换
uint8_t tx_buf_[256] {};                    // IN 发送拷贝缓冲
```

- `Init` 时 `s_pcd.pData = this;`（供回调反查实例）。
- `GetRxBufBase(epnum)`：返回 `out_ep_[epnum&0x0F].buf`（EpStartRx 武装时记录的接收缓冲基址），**DataOut 回调取数据用**（§3.8）。

### 3.7 与 UsbHalHpm 关键差异对照

| 项 | UsbHalHpm（HPM5361） | UsbHalStm32（F407 OTG FS） |
| --- | --- | --- |
| 底层 SDK | `hpm_usb_device.c` + `hpm_usb_drv.c` | `stm32f4xx_hal_pcd.c` + `stm32f4xx_hal_pcd_ex.c` + `stm32f4xx_ll_usb.c` |
| 传输 | EHCI DMA（dcd_data/QHD·qTD） | **无 DMA**，HAL 中断搬运 FIFO |
| 缓冲 | `.nocache` 段 + `l1c_dc_invalidate/writeback` | 普通 RAM，无需 cache 操作 |
| 速度 | High/Full（PORTSC1） | 固定 `Speed::Full` |
| 端点表 | 16（USB_SOC_DCD_MAX_ENDPOINT_COUNT*2） | 4（dev_endpoints） |
| FIFO | 控制器外队列 | 内置 RX/TX FIFO（3.3 分配） |
| ISR 入口 | `usb_isr_entry` | `stm32_usb_isr`（独立命名，避免与 HPM 同名） |
| 中断 | `cfg.irq_num`（PLIC） | `OTG_FS_IRQn`=67（NVIC） |
| 传输完成长度 | 遍历 qTD 链计算 | `xfer_count` / `HAL_PCD_EP_GetRxCount` |

### 3.8 ⚠️ 调试发现与修复：F4 slave 模式 DataOut 数据指针偏移（2026-08-14）

**现象**：PC 发 47B ASCII（`jlkjlk...\n`），MCU 日志收 47B / 发 47B（`DataOut len=47`、`TC ep=0x01 len=47`、`TC ep=0x81 len=47`，长度全对），但线程 `[err] %s` 打印 rx_buf **为空**（= rx_buf[0]==0x00，log 的 `%s` 已验证正常），PC 回传也不是原样——**长度对、内容错**。

**根因（代码证据，F4 HAL 源码）**：

1. **RXFLVL 处理读包后把 `xfer_buff` 前移**（stm32f4xx_hal_pcd.c:1107-1116）：
   ```c
   if (((RegVal & USB_OTG_GRXSTSP_PKTSTS) >> 17) == STS_DATA_UPDT) {
       (void)USB_ReadPacket(USBx, ep->xfer_buff, ...);
       ep->xfer_buff += (RegVal & USB_OTG_GRXSTSP_BCNT) >> 4;   // ← 前移！
       ep->xfer_count += ...;
   }
   ```
2. **`HAL_PCD_EP_Receive` 只在下次武装时把 `xfer_buff` 复位到基址**（:1898 `ep->xfer_buff = pBuf`）。
3. **`PCD_EP_OutXfrComplete_int` 非 DMA 分支对非 EP0 不复位 xfer_buff** 就直接调 `DataOutStageCallback`（:2275-2297，只有 EP0 分支动 xfer_buff）。
4. 项目 `HAL_PCD_DataOutStageCallback` 用 `ev.data = hpcd->OUT_ep[epnum].xfer_buff` → **指向数据末尾**（rx_buf_ + 47）。
5. 中间层 `rx_queue_.Push(ev.data, 47)` 推的是错误偏移的内容（rx_buf_+47 起的 47B，全 0/陈旧）→ 线程读到首字节 0x00。

**为什么参考实现没踩**：STM32Cube `USBD_LL_DataOutStage` 对 bulk 直接 `UNUSED(pdata)`（usbd_core.c:593），CDC 类读**自己跟踪的 `hcdc->RxBuffer`**（usbd_cdc.c:592，经 `USBD_LL_PrepareReceive` 武装到缓冲基址）；CherryUSB 同样由类层持缓冲。HPM 板是 DMA 模式（数据由 DMA 写基址，xfer_buff 不复位问题不存在）。**这是"F4 slave 模式 + 直读 HAL xfer_buff"组合特有。**

**修复（已落地，回环验证通过）**：

- `usb_hal_stm32.hpp`：加 `GetRxBufBase(epnum)`，返回 `out_ep_[epnum & 0x0F].buf`（EpStartRx 武装时存入的接收缓冲基址，回调发生在 `RequeueRx` 下一次武装之前，此刻仍指向本次数据）。
- `usb_hal_stm32.cpp` DataOut 回调：`ev.data = self->GetRxBufBase(epnum)`，不再用已前移的 `xfer_buff`。
- `trd_test.cpp`：`Send(ktick, n)` → `Send(rx_buf, n)`（ktick 只有 6B，n=47 越界读 41B，UB；顺带修复）。

**验证**：PC 发 `jlkjlk...\n` → `[err] jlkjlk..., 47`、回传原样 47B。✓

**连带影响**：EP0 OUT 数据（如 SET_LINE_CODING 的 7B）同样走该回调，原实现也会读错偏移；`GetRxBufBase` 修复对 EP0 一并生效（`Ep0StartOut` 已把 `out_ep_[0].buf` 设为 control_buffer_）。

> 完整诊断见 `doc/STM32_USB回传内容错误诊断.md`。

---

## 4. 目标文件结构（已落地）

```text
zephyr_user/drivers/
├── Kconfig                        # +DUST_USB_DEVICE_HAL_STM32（select USE_STM32_HAL_PCD*/LL_USB）
├── CMakeLists.txt                 # +if(CONFIG_DUST_USB_DEVICE_HAL_STM32) 编译块
├── usb/hal/hpm/usb_hal_hpm.*      # 已有
└── usb/hal/stm32/
    ├── usb_hal_stm32.hpp          # 新增 ✓
    └── usb_hal_stm32.cpp          # 新增 ✓
```

framework `communication/stream/usb/Kconfig`：`DUST_USB_DEVICE_STACK` 增加一行 `select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_OTGFS_ENABLED`（✓ 已落地）。

---

## 5. 构建装配（old → new 定死，均已落地）

### 5.1 子树 `zephyr_user/drivers/Kconfig`

old（文件末尾，Kconfig:13-16）：
```kconfig
# USB 板卡底层 HAL（2026-08-09 从 framework/drivers/communication/stream/usb 下沉）
config DUST_USB_DEVICE_HAL_HPM
    bool "HPMicro EHCI USB HAL"
    default n
```

new（末尾追加，**实际实现含 USE_STM32_LL_USB，比规划多一条**）：
```kconfig
config DUST_USB_DEVICE_HAL_STM32
    bool "STM32 OTG FS USB HAL"
    default n
    select USE_STM32_HAL_PCD
    select USE_STM32_HAL_PCD_EX
    select USE_STM32_LL_USB
    help
      STM32F407 OTG FS 板卡底层 HAL（参考 STM32Cube USB_DEVICE CDC 设置，2026-08-13）。
      select USE_STM32_HAL_PCD* 触发 hal_stm32 模块编译 stm32f4xx_hal_pcd{,_ex}.c；
      USE_STM32_LL_USB 编译 stm32f4xx_ll_usb.c（HAL_PCD 依赖的 USB_* 底层函数）。
```

### 5.2 framework `communication/stream/usb/Kconfig`

old：
```kconfig
config DUST_USB_DEVICE_STACK
    bool
    select DUST_USB_DEVICE_CDC_ACM
    select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_QINGYUUSB_ENABLED
```
new（实际 binding 名 `dustusb`）：
```kconfig
config DUST_USB_DEVICE_STACK
    bool
    select DUST_USB_DEVICE_CDC_ACM
    select DUST_USB_DEVICE_HAL_HPM if DT_HAS_HPMICRO_HPM_DUSTUSB_ENABLED
    select DUST_USB_DEVICE_HAL_STM32 if DT_HAS_ST_STM32_OTGFS_ENABLED
```
（`st,stm32-otgfs` 是 Zephyr 现成 binding，`DT_HAS_ST_STM32_OTGFS_ENABLED` 宏由板卡 overlay 的 `&usbotg_fs { status="okay" }` 触发。）

### 5.3 子树 `zephyr_user/drivers/CMakeLists.txt`

old（文件末尾，CMakeLists.txt:11-21）：
```cmake
# USB 板卡底层 HAL（HPM EHCI，2026-08-09 下沉自 framework）
if(CONFIG_DUST_USB_DEVICE_HAL_HPM)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm/usb_hal_hpm.cpp
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device/hpm_usb_device.c
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/src/hpm_usb_drv.c)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/hpm
        ${FW_ROOT}/drivers/communication/stream/usb/interface
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/components/usb/device
        ${SDK_GLUE_DIR}/../sdk_env/hpm_sdk/drivers/inc)
endif()
```

new（末尾追加）：
```cmake
# USB 板卡底层 HAL（STM32 OTG FS，2026-08-13）
if(CONFIG_DUST_USB_DEVICE_HAL_STM32)
    target_sources(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/stm32/usb_hal_stm32.cpp)
    target_include_directories(app PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/usb/hal/stm32
        ${FW_ROOT}/drivers/communication/stream/usb/interface)
endif()
```
说明：
- `stm32f4xx_hal_pcd.c/_ex.c`、`stm32f4xx_ll_usb.c` 由 5.1 的 `select USE_STM32_*` 经 hal_stm32 模块（zephyr_library）自动编译链接，**不需要也不应该**在子树 CMake 里加源。
- `log.hpp` 靠 app target 已有 `${FW_ROOT}/cmd` include。
- `usb_hal.hpp`/`usb_types.hpp` 靠 `${FW_ROOT}/drivers/communication/stream/usb/interface` include（同 HPM 块写法）。
- STM32 板卡（`stm32f407igh6`）的 `soc.h` 已 include 全部 `stm32f4xx_hal_*.h`，`PCD_HandleTypeDef`/`HAL_PCD_*` 可用。

### 5.4 关键：USE_STM32_HAL_PCD 装配链

```
DUST_USB_DEVICE_HAL_STM32 (子树 Kconfig)
  → select USE_STM32_HAL_PCD / USE_STM32_HAL_PCD_EX / USE_STM32_LL_USB
  → hal_stm32/stm32cube/stm32f4xx/CMakeLists.txt:53-54
     zephyr_library_sources_ifdef(...) 编译 stm32f4xx_hal_pcd.c / _ex.c / ll_usb.c
  → zephyr_library 链接进镜像，UsbHalStm32.cpp 里 HAL_PCD_* / USB_* 符号可解析
```

---

## 6. 板卡接入（目标板卡 board_rm_c，puzhong 同理）

### 6.1 overlay（`project/boards/st/board_rm_c/stm32f407igh6.overlay`）

old（现无 USB 节点）：
```dts
#include <dt-bindings/gpio/gpio.h>
#include <dt-bindings/pinctrl/stm32-pinctrl.h>
```
new（追加，参考 Zephyr feather 板卡 + 参考项目 PA11/PA12）：
```dts
&usbotg_fs {
    pinctrl-0 = <&usb_otg_fs_dm_pa11 &usb_otg_fs_dp_pa12>;
    pinctrl-names = "default";
    status = "okay";
};
```
- `usbotg_fs` 节点在 `stm32f4.dtsi:314`，`reg/interrupts/clocks` 已配，overlay 只补 pinctrl + enable。
- PA11(DP)/PA12(DM) 与板卡现有外设无冲突。PA10(ID) 设备模式可不接。
- **puzhong 备选**：`stm32f4_disco.overlay` 同样追加（其 usart1 用 PA9/PA10，与 OTG 的 DM/DP PA11/PA12 不冲突）。

### 6.2 业务接入（trd_test.cpp）

规划原为 trd_pc.cpp，主干瘦身后由 trd_test.cpp 承载（§2.4）：

```cpp
cfg.reg_base     = DT_REG_ADDR(DT_NODELABEL(usbotg_fs));
cfg.irq_num      = DT_IRQN(DT_NODELABEL(usbotg_fs));
cfg.irq_priority = 5;   // 参考项目 OTG_FS_IRQn 优先级 5
```
- `reg_base` 在 STM32 侧不直接使用（PCD 用 `USB_OTG_FS` 硬件宏），保留赋值保持接口一致。

---

## 7. 分阶段执行（全部完成）

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 1 | 新建 `usb_hal_stm32.{hpp,cpp}` 骨架 + 14 接口 + ISR + HAL 弱回调 + `GetDefaultHal` | ✅ |
| 2 | 构建装配：子树 Kconfig / framework usb Kconfig / 子树 CMakeLists 三处 | ✅ |
| 3 | 板卡 overlay `&usbotg_fs` + 业务 cfg 指向 `usbotg_fs` | ✅ |
| 4 | 编译 + 枚举 + 回环验证 | ✅（trd_test 回环通过） |
| 4.5 | 调试修复 DataOut 数据指针偏移（§3.8）+ trd_test Send 越界读 | ✅ |

---

## 8. 验证标准（汇总）

- [x] `board_rm_c`（stm32f407igh6）编译通过：无 include 找不到、无 `HAL_PCD_*` undefined reference、无 `GetDefaultHal` 重复符号
- [x] `CONFIG_DUST_USB_DEVICE_HAL_STM32=y`（board_rm_c 编译时被 STACK select）
- [x] `stm32f4xx_hal_pcd.c` 进入编译（`USE_STM32_HAL_PCD=y`，含 `_ex.c`/`ll_usb.c`）
- [x] `DT_HAS_ST_STM32_OTGFS_ENABLED` 宏生成
- [x] PC 枚举 USB CDC 设备成功，trd_test 回环收发正常
- [x] 参考项目 USB 参数逐项对照（PCD 参数/FIFO/引脚 AF10）一致
- [x] **DataOut 数据指针修复验证**：PC 发 ASCII → `[err]` 打印原内容、回传原样（§3.8）

## 9. 风险与注意

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| **DataOut 数据指针偏移** | F4 slave 模式 HAL 把 xfer_buff 前移到数据末尾，直读拿错指针 → 长度对内容错 | **已修复**：用 `GetRxBufBase`（§3.8） |
| 通知端点 TX FIFO 未分配 | 自研 CDC 通知端点是 EP3(0x83)，参考项目只配到 EP1 | 首版按参考项目 FIFO（验证通过）；通知端点实际发送异常时用 §3.3 补充方案 |
| `dev_endpoints=4` 限制 | OTG FS 只有 4 个双向端点 | 自研 CDC 用 EP0/1/3，够用；不加更多端点 |
| `USE_STM32_HAL_PCD` 与 Zephyr usb 栈冲突 | 若某板卡同时开 CONFIG_USB_DC_STM32 会重复 select | 自研栈不依赖 Zephyr usb 栈；板卡 prj.conf 不设 USB_DC_STM32 |
| 中断优先级 | 参考项目 OTG_FS_IRQn=5 | trd_test 设 `irq_priority=5`；若与 Zephyr 调度冲突改 1 |
| 目标板卡未定 | board_rm_c / puzhong 皆 F407 OTG FS | overlay 均给出；代码层不依赖具体板卡 |
| `pinctrl_apply_state` 宏名 | 自研 HAL 非 Zephyr 驱动，无 DT_DRV_INST | 用 `PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(usbotg_fs))` |
| 时钟使能方式 | 参考项目 `__HAL_RCC_USB_OTG_FS_CLK_ENABLE()` | 采用 HAL RCC 宏（与参考项目一致） |
| 使用者工作区 | ai_imu/detected 等复制了 framework 子模块副本 | 本轮只动主仓库 + framework 子模块；使用者工作区后续各自 `git submodule update` |

## 10. 执行清单（逐条勾）

- [x] 阶段 1：建 `zephyr_user/drivers/usb/hal/stm32/`，写 `usb_hal_stm32.hpp`（类定义 + §3 映射）
- [x] 阶段 1：写 `usb_hal_stm32.cpp`（14 接口 + ISR + HAL 弱回调覆盖 + `GetDefaultHal`）
- [x] 阶段 2：`zephyr_user/drivers/Kconfig` 追加 `DUST_USB_DEVICE_HAL_STM32`（select USE_STM32_HAL_PCD* + USE_STM32_LL_USB）
- [x] 阶段 2：framework usb/Kconfig `DUST_USB_DEVICE_STACK` 加 `select HAL_STM32 if DT_HAS_ST_STM32_OTGFS_ENABLED`
- [x] 阶段 2：`zephyr_user/drivers/CMakeLists.txt` 追加 HAL_STM32 编译块
- [x] 阶段 3：`board_rm_c/stm32f407igh6.overlay` 加 `&usbotg_fs`（pinctrl + status）
- [x] 阶段 3：业务 cfg 指向 `usbotg_fs` + irq_priority=5（trd_test.cpp）
- [x] 阶段 4：编译 board_rm_c 验证（无 undefined reference / GetDefaultHal 单定义）
- [x] 阶段 4：烧录后 PC 枚举 USB CDC + trd_test 回环验证
- [x] 调试修复：DataOut 数据指针用 `GetRxBufBase`（§3.8）+ trd_test `Send(rx_buf, n)`
- [ ] 后续：framework/drivers 子模块提交 + 主仓库 PR（主仓库必须 PR + auto-merge，见 CI/CD 记忆；上传前查 dust_hpm_tree 对齐——本轮若改 STM32 树无关可跳过，但 framework Kconfig 改动要随子模块提交）
