# EtherCAT 从站当前状态说明（给主站侧）

> 日期：2026-10-29
> 从站硬件：HPM6E00EVK RevA（HPM6E80，片内 ESC）
> 固件：`zephyr_user` @ master，EtherCAT 模块已按 USB 风格重构完成
> 本文给主站侧（TwinCAT / SOEM / IgH EtherCAT Master）的 AI 或工程师参考

---

## 1. 一句话结论

从站已经**初始化完成、PHY 链路已 up、协议栈在跑**，具备被主站扫描和进入 OP 的条件。
**只接了 PORTA 一根网线，PORTB 空着**——这是合法的单从站拓扑，ESC 会在 PORTB 内部回环。

---

## 2. 硬件连接

| 项 | 值 |
|---|---|
| 从站设备 | HPM6E00EVK（HPM6E80，片内 ESC @ `0xf1700000`） |
| ECAT IN | **PORTA**（RJ45，标 `ECAT IN`），PHY = JL1111，MDIO 地址 **0** |
| ECAT OUT | PORTB（RJ45，标 `ECAT OUT`），PHY = JL1111，MDIO 地址 **1** |
| 当前接线 | 主站网口 → 网线 → **PORTA** |
| PORTB | **空着**（正常，ESC 内部回环） |
| 速率/双工 | 100BASE-TX 全双工，自协商 |
| PHY 复位 | PA10（两颗 PHY 共用） |

**主站侧网卡要求**：100M 全双工或自协商。**不能设成 10M**——从站只通告 100M 全双工。

---

## 3. 从站身份（主站扫描时会看到）

| 项 | 值 | 来源 |
|---|---|---|
| Vendor ID | `0x0048504D`（"HPM"） | SSC `ecat_def.h` `VENDOR_ID` |
| Product Code | `0x00000001` | SSC `ecat_def.h` `PRODUCT_CODE` |
| Revision No | `0x00000001` | SSC `ecat_def.h` `REVISION_NUMBER` |
| 设备名 | `digital_io` | SSC 对象字典 |
| 物理层 | YYY（3 端口能力，实际启用 2 个） | |
| 邮箱协议 | CoE（SdoInfo / CompleteAccess / SegmentedSdo 均支持） | |
| FoE / EoE | **不支持** | `FOE_SUPPORTED=0` / `EOE_SUPPORTED=0` |
| 分布式时钟 | **支持**（DC_SYNC，AssignActivate `#x300`） | |

> 注意：SSC 的 `eeprom.h` 里 `aEepromData` 是原始 ESI 生成的内容，
> 从站上电时如果板上 I2C EEPROM 校验不过会用它重刷。所以主站看到的身份
> 一定是上面这组值，**不要用 HPM SDK 的 `ECAT_IO.xml` 直接对**——
> 那份 ESI 是 PDO 只有 4 字节的版本，与当前固件的 32 字节不一致。

---

## 4. 过程数据（PDO）

### 4.1 大小

| 方向 | SM | 索引 | 字节数 |
|---|---|---|---|
| 主站 → 从站（RxPDO / Outputs） | SM2 @ `0x1100` | `0x1600` | **32 字节** |
| 从站 → 主站（TxPDO / Inputs） | SM3 @ `0x1400` | `0x1A00` | **32 字节** |

启动日志实测：`ecat: stack ready rx=32 tx=32`。

### 4.2 映射明细

`0x1600`（OutputCounter process data mapping，RxPDO）：

| 条目 | 对象 | 位长 | 类型 |
|---|---|---|---|
| 1~8 | `0x7010:01` ~ `0x7010:08` | 32 each | UDINT |

`0x1A00`（InputCounter process data mapping，TxPDO）：

| 条目 | 对象 | 位长 | 类型 |
|---|---|---|---|
| 1~8 | `0x6000:01` ~ `0x6000:08` | 32 each | UDINT |

即：**8 个 UINT32 出去，8 个 UINT32 回来**，都是小端序。

### 4.3 当前固件里的行为（重要）

`project/thread/test/trd_test.cpp` 目前是**测试代码**：

```cpp
// 主站 → 从站：只打日志，不解析
slave_.SetRxHandler([](const uint8_t* data, size_t len) {
    DUST_LOG_INF("ecat rx %u bytes", len);   // 串口会打印
});

// 从站 → 主站：填递增序列 0,1,2,...,31
slave_.SetTxProvider([](uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        data[i] = static_cast<uint8_t>(i);
    }
});
```

**主站读 TxPDO 应该看到字节序列 `00 01 02 ... 1F`**（32 字节递增）。
这是验证链路最直接的判据。

主站写 RxPDO 时，从站串口会打印 `ecat rx 32 bytes`。

---

## 5. 同步方式

支持两种（主站二选一）：

| 模式 | AssignActivate | 说明 |
|---|---|---|
| **SM-Synchron** | `0x0000` | 推荐先用这个，最简单 |
| DC-Synchron | `0x0300` | 从站支持，但当前没做 DC 调优 |

**建议主站先用 SM-Synchron**（Free Run / SM-Sync），确认能进 OP 之后再考虑 DC。

---

## 6. 主站侧操作建议

### 6.1 用 IgH EtherCAT Master（Linux）

```bash
# 扫描
ethercat slaves
# 期望：0  0:0  PREOP  +  digital_io

# 看 PDO 映射
ethercat cstruct
# 期望：RxPDO 0x1600 有 8 个 0x7010 条目，TxPDO 0x1A00 有 8 个 0x6000 条目

# 进 OP
ethercat states OP
ethercat slaves
# 期望：0  0:0  OP  +  digital_io

# 读 TxPDO（应该是 00 01 02 ... 1F）
ethercat upload --type uint32 0 0x6000 1
```

### 6.2 用 SOEM

```c
ec_config_init(FALSE);       // 期望找到 1 个从站
ec_config_map(&IOmap);       // IOmap 里 RxPDO 32B / TxPDO 32B
ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
ec_slave[0].state = EC_STATE_OPERATIONAL;
ec_writestate(0);
```

### 6.3 用 TwinCAT

1. 扫描设备，应该出现 `digital_io`（HPMicro，`0x0048504D`）
2. 确认 PDO 是 8×UDINT 出 / 8×UDINT 入
3. 先用 **SM-Synchron**，不要一上来勾 DC
4. 激活配置，看是否进 OP

---

## 7. 预期启动日志（从站串口，921600 8N1）

```
EEPROM loading successful, no checksum error.
JL1111@0: PHY ID=0x937c4024
JL1111@0: ANAR=0x0101
JL1111@0: BMCR=0x1000
JL1111@0: configured (autoneg, MII)
JL1111@1: PHY ID=0x937c4024
JL1111@1: ANAR=0x0101
JL1111@1: BMCR=0x1000
JL1111@1: configured (autoneg, MII)
ecat: hal ready, 2 phy
ecat: port0 (JL1111) link down      ← 上电时还没协商完
ecat: port1 (JL1111) link down      ← PORTB 空着，正常
ecat: stack ready rx=32 tx=32
...
ecat: port0 (JL1111) link up        ← 网线插好后
```

主站写数据时会周期性出现：

```
ecat rx 32 bytes
```

---

## 8. 已知限制 / 排查要点

| 现象 | 原因 | 处理 |
|---|---|---|
| `port1 link down` | PORTB 没插线 | **正常**，ESC 内部回环，不影响单从站通信 |
| 主站扫不到从站 | 网卡速率不是 100M / 网线插在 PORTB | 网卡设 100M 全双工；网线插 PORTA |
| 从站停在 PREOP 进不了 SAFE-OP | 主站 PDO 配置与从站对象字典不符 | 用 `ethercat cstruct` 核对：8×UDINT，32 字节 |
| 从站进 OP 但 TxPDO 全 0 | 主站没触发 `APPL_InputMapping` | 检查主站是否真的在发过程数据帧 |
| TxPDO 读到 `00 01 02...` 但长度不对 | 主站 PDO 配置成 4 字节（旧 ESI） | 按当前固件的 32 字节重新配 |

**没有 ESI/XML 文件**——当前项目没有生成 ESI。主站侧请用 `ethercat cstruct`
（IgH）或扫描结果（SOEM/TwinCAT）自动取 PDO 配置，不要依赖 ESI。

如果必须要 ESI：按本文 §3 的身份 + §4 的 PDO 映射，参考
`hpm_sdk/samples/ethercat/ecat_io/SSC/ESI/ECAT_IO.xml` 的结构改
（把 RxPdo/TxPdo 的 Entry 从 1 个扩到 8 个，`<Sm DefaultSize>` 改成 32）。

---

## 9. 从站固件侧代码位置（供排查用）

```
framework/drivers/communication/ethercat/
├── ethercat.hpp/.cpp            顶层（继承 Stream，业务只碰这里）
├── interface/
│   ├── ecat_hal.hpp             ESC 硬件抽象
│   ├── ecat_phy.hpp             PHY 驱动抽象
│   └── ecat_types.hpp           规范类型
├── core/
│   ├── ecat_stack.cpp           SSC 生命周期（MainInit / MainLoop / ISR 分发）
│   ├── ecat_pdo.cpp             PDO 长度计算（rx=32 / tx=32 出自这里）
│   └── ecat_eeprom.cpp          SII 钩子
├── appl/ecat_appl_hooks.cpp     12 个 APPL_* 钩子
└── ssc/                         倍福 SSC 5.13 原版（不改）

zephyr_user/drivers/ethercat/hal/
├── hpm/ecat_hal_hpm.cpp         ESC 寄存器 / MDIO / 中断 / 链路
└── ports/
    ├── hpm_ecat_hw.c            SSC 的 HW_* 端口层
    └── phy/
        ├── jl1111/              HPM6E00EVK 用的这颗
        ├── rtl8201f/            HPM5E31 板用的
        └── tja1101b/            HPM5E31 板用的
```

业务代码在 `project/thread/test/trd_test.cpp`——上面 §4.3 的两个回调就在这个文件里。
