# youyeetoo R1 网卡（RTL8822CE WiFi）修复：设备树覆盖（DT overlay）操作手册

> 最后更新：2026-08-18（第 7 版）。本文档是给**板卡上的 AI 助手**看的自包含手册。
> **🎉 第 7 版：WiFi 修复成功（用户确认"通了的"）**——`r1-wifi-fix-overlay.dtbo` 装板后 PCIe 链路打通，RTL8822CE 被识别（lspci 10ec:c822 / wlan0 出现）。剩余：蓝牙验证 + WiFi 连接测试。
> **板型更正：板子是 R1 V2**（V3 兼容 V2；官方镜像 ImageV2V3 通用，GPIO4_A7 对 V2 同样适用）。
> **方向（第 5 版起，用户明确）**：本文的"网卡" = **RTL8822CE WiFi+BT 模块**（M.2 E-key 插槽），不是 RJ45 千兆以太网（eth0/gmac1）。
> **第 6 版关键进展（PCIe 链路实锤）**：RTL8822CE 是 **PCIe 接口**芯片（8822ce.ko alias `pci:10EC:C822`，不是 USB！）。模块走 `pcie@fe190000`（PCIE20_0），当前链路失败（LTSSM 0x3 无设备应答）根因已定位：**鲁班猫4 固件的 reset-gpios 配错**（GPIO3_D1，应为 GPIO4_A7）+ **多余 disable-gpios**（GPIO3_D2）。修复补丁 **`r1-wifi-fix-overlay.dtbo`（292B）已编译**。
> - 第 5 版依据：V3 原理图 WiFi 硬件连接 + 两板 dts 对比。

---

## 零、快速了解（AI 导读）

- **目标**：让 R1 板卡 M.2 E-key 插槽上的 **RTL8822CE（WiFi5 + BT5.0）** 能用。
- **现状**：系统 = Ubuntu 26.04 LTS（内核 6.1.118，LubanCat 内核），从 eMMC 启动正常；**WiFi 模块在鲁班猫4 固件上不工作**（原因见 §一）。
- **原因（实锤）**：鲁班猫4 固件的设备树**没有 WiFi（wireless-wlan / wireless-bluetooth）节点**，R1 硬件上 WiFi 模块的配置（host_wake 引脚等）完全没映射。
- **做法**：设备树覆盖补丁（overlay）补上 WiFi 节点，不重刷系统。
- **当前进度**：硬件图 + 两板 dts 对比已完成 → **下一步：板卡确认 WiFi 模块 USB 枚举状态（lsusb）→ 定补丁内容**（§四）。
- **安全**：overlay 失败自动回滚（boot.cmd 容错），不会砖。

---

## 一、前因后果（完整背景）

### 1.1 板卡与模块

| 项 | 值 |
|----|-----|
| 板卡 | youyeetoo R1 V3.0，SoC RK3588S，eMMC 32G，8G RAM |
| 目标模块 | **RTL8822CE（M.2 E-key 2230，WiFi5 8822 + BT5.0）**，R1 官方默认支持（README：Wi-Fi5+BT5.0 module RTL8822CE default driver） |
| 系统 | Ubuntu 26.04 LTS（内核 6.1.118，LubanCat 内核），eMMC 启动正常 |
| 网络现状 | eth1 = USB 共享/WiFi（开发 PC 侧）；**RTL8822CE 不工作** |

### 1.2 硬件连接（`youyeetoo R1 v3.pdf` 原理图实锤）

**R1 V3 有两个 M.2 插座：**
- **CON1 = M.2 M-KEY**（底部，NVMe SSD / SATA / 4G LTE）
- **CON2 = M.2 E-KEY**（顶部，WiFi/BT 模块 = RTL8822CE 插这里）

**CON2（E-key）关键引脚（原理图实锤）：**

| 信号 | 引脚 | 连接 |
|------|------|------|
| 电源 | 2/4/72/74 | **`+3VSB_WIFI_BT1`** —— 由 `VCC3V3_SYS` 经 0R 直连（**常供电，无独立开关控制**） |
| **PCIe 数据** | 33/35/39/41 | **PETp0/PETn0/PERp0/PERn0（PCIE20_0）**—— RTL8822CE（PCIe 接口）走这里 |
| **PERST0#** | 50 | **GPIO4_A7（PCIE20x1_2_PERSTn_M0，原理图 1706-1707 行实锤）** |
| SDIO | 6-15 | SDIO_CLK/CMD/DAT0-3（SDIO WiFi 用，8822CE 不用） |
| UART | 20-23 | UART_RX/TX、UART_Wake#、SDIO_Wake#（host_wake） |
| USB | 3/5 | DM3/DP3（E-key 预留，8822CE 是 PCIe 接口不用 USB） |

**电源开关电路**：GPIO0_C7 → Q13 → Q11（WNM2016 负载开关）→ **控制 CON1（M-key，SSD/4G）电源**，**不是** WiFi 使能。WiFi 电源是常供的。

### 1.3 核心差异：fe190000（PCIE20_0）复位/禁用脚配置（第 6 版实锤）

**RTL8822CE = PCIe 接口**（`8822ce.ko` alias `pci:v000010ECd0000C822`，纯 PCIe 驱动；RTL8822CS=SDIO、RTL8822CE=PCIe、RTL8822BU=USB）。模块必须走 **`pcie@fe190000`（PCIE20_0，E-key 槽）**，板卡实测模块在 PCIe 和 USB 都认不到（`lspci` 空、`lsusb` 无 0bda），dmesg：`PCIe Link Fail, LTSSM is 0x3`（无设备应答）。

**fe190000 两板对比（R1 官方 dts vs 鲁班猫4 运行态实测）：**

| 属性 | R1 官方 dts | 鲁班猫4（当前系统） | 判定 |
|------|------------|--------------------|------|
| status | okay | okay | 同 |
| **reset-gpios** | **`<&gpio4 7 0>` = GPIO4_A7**（原理图实锤 = PCIE20x1_2_PERSTn_M0 = E-key 引脚 50 PERST0#） | `<&gpio3 25 0>` = **GPIO3_D1** | ❌ **配错脚**（拉错 GPIO，模块 PERST# 未被正确控制） |
| **disable-gpios** | **无** | `<&gpio3 22 0>` = GPIO3_D2 | ❌ **多余**（R1 无此脚，可能干扰无关信号） |
| vpcie3v3-supply | 有 | 有（enabled 3.3V） | 同 |
| phys | combphy idx2 | combphy@fee00000 idx2 | 同 |
| pinctrl-0 / prsnt-gpios | 无 | 无 | 同 |

**根因**：鲁班猫4 固件把 PERST# 复位脚配成了 GPIO3_D1（R1 上该脚不连模块），模块 PERST# 一直未正确释放 → PCIe 链路无设备应答（LTSSM 0x3）。

**顺带确认（R1 官方 dts 的 WiFi 节点）**：`wireless-wlan`（wlan-platdata，wifi_chip_type=rtl8852be 默认值、host_wake=GPIO0_PA0）、`wireless-bluetooth`（bluetooth-platdata）——鲁班猫4 固件完全没有；但 **8822CE 是 PCIe 驱动，不依赖 wlan-platdata**，先把 PCIe 链路修通是主线，WiFi/BT 节点补丁是后续。

---

## 二、overlay 加载机制（26.04 已确认）

| 项 | 值 |
|----|-----|
| 启动链路 | U-Boot `boot.scr` → `/boot/firmware/ubuntuEnv.txt`（`fdtfile=` + `overlays=`）→ 应用 overlay → 内核 |
| 配置文件 | `/boot/firmware/ubuntuEnv.txt`（eMMC boot 分区，FAT32） |
| overlay 目录 | `/boot/firmware/dtbs/rockchip/overlay/` |
| 启用方式 | `overlays=` 行末尾加 ` r1-wifi-fix-overlay` |
| 容错 | 应用失败自动恢复原设备树（不会砖） |

---

## 三、当前状态与诊断数据

### 3.1 已确认

- 系统 26.04 从 eMMC 正常启动；overlay 机制就绪（`overlays=` 已有 dp0-in-vp1 + hdmi0 + 旧 gmac 补丁条目）。
- RTL8822CE = PCIe 接口（8822ce.ko，纯 PCIe）；模块在 PCIe/USB 都认不到，链路失败 LTSSM 0x3。
- **根因实锤**：鲁班猫4 固件 fe190000 的 reset-gpios=GPIO3_D1（应为 GPIO4_A7）、disable-gpios=GPIO3_D2（多余）。
- 修复补丁 **`r1-wifi-fix-overlay.dtbo`（292B，md5 `19fa8783e10310ad2aa3b4d8ebd7175a`）已编译**并同步 `doc/`。

### 3.2 补丁内容（第 3 版：WiFi/PCIe 修复）

```dts
/dts-v1/;
/plugin/;

/ {
    fragment@0 {
        target-path = "/pcie@fe190000";   /* PCIE20_0, M.2 E-key WiFi 槽 */
        __overlay__ {
            /* R1: PERST# = GPIO4_A7 (PCIE20x1_2_PERSTn_M0, E-key 引脚 50) */
            reset-gpios = <&gpio4 7 0>;
            /* R1 无 disable 脚: 清空鲁班猫4 的 disable-gpios (GPIO3_D2)
               空属性 -> 驱动 gpiod_get_optional 返回 NULL -> 不拉该脚 */
            disable-gpios;
        };
    };
};
```

---

## 四、安装与验证（已完成 ✅）

> 2026-08-18：补丁已装板并验证成功——`lspci` 出现 10ec:c822（RTL8822CE），`wlan0` 出现，WiFi 可用。以下为过程留档 + 剩余项。

### 过程（已完成）

### 第 1 步：装补丁（U 盘拷入）

```bash
sudo cp 【U盘路径】/r1-wifi-fix-overlay.dtbo /boot/firmware/dtbs/rockchip/overlay/
md5sum /boot/firmware/dtbs/rockchip/overlay/r1-wifi-fix-overlay.dtbo
# 应为 19fa8783e10310ad2aa3b4d8ebd7175a
```

### 第 2 步：启用（ubuntuEnv.txt 先备份）

```bash
sudo cp /boot/firmware/ubuntuEnv.txt /boot/firmware/ubuntuEnv.txt.bak
sudo nano /boot/firmware/ubuntuEnv.txt
# overlays= 行末尾加: r1-wifi-fix-overlay
# （可选）去掉旧的 r1-gmac-fix-overlay 条目（那是修 eth0 的错误方向，与 WiFi 无关）
grep overlays= /boot/firmware/ubuntuEnv.txt
sync && sudo reboot
```

### 第 3 步：验证

```bash
lspci -nn                      # 应出现 10ec:c822 (Realtek RTL8822CE)
dmesg | grep -iE "pcie|rtw|8822|rtl" | tail -30   # 应无 "Link Fail"
ip link                         # 应出现 wlan0
rfkill list                     # 查是否软禁用
```

**验证结果（2026-08-18 用户确认）**：
- ✅ `lspci` 出现 `10ec:c822`（RTL8822CE）→ PCIe 链路通
- ✅ `wlan0` 出现 → 驱动加载
- ✅ **WiFi 修复成功**

**剩余验证项（下一步）**：
1. WiFi 连接测试：`nmcli device wifi list` / `nmcli dev wifi connect <SSID> password <pw>`（或 `iwctl`）
2. 蓝牙：`bluetoothctl list`（hci0）/ `bluetoothctl scan on`
3. 系统启动日志里的固有报错（VOP/PMIC/VDEC/DMC/RTC 等，鲁班猫4 DTB 跑 R1 的固有现象）**与 WiFi 无关，不影响使用，暂不处理**

**历史留档（lspci 曾空时）**：
1. 确认补丁生效：`cat /proc/device-tree/pcie@fe190000/reset-gpios | od -A x -t x1` 应为 `04 07 00`（gpio4 pin7）
2. dmesg 看 LTSSM 状态/报错
3. 硬件级：模块金手指/插槽接触、PERST0# 电平（万用表）
4. 若 reset 正确仍失败 → 查 combphy 配置/电源 → 下一轮调参

---

## 五、风险与注意事项

1. **不会砖**：overlay 失败自动回滚；撤补丁随时可逆。
2. **以实测为准**：R1 官方 dts 的硬件参数已有两处被实测推翻（GMAC phy@0、clock_in_out），WiFi 相关参数（GPIO0_PA0 host_wake 等）也以板卡实测为准。
3. **eMMC 优先启动**：BootROM 探测顺序 eMMC > SD。
4. **补丁与文档同步更新**：每次改补丁，`doc/` 下 dtbo + dts + 本文档一起更新（见 [[youyeetoo-patch-sync-discipline]]）。

---

## 附录：历史遗留（RJ45 eth0/gmac1 分析，当前不是主线）

之前版本误把"网卡"理解为 RJ45 千兆以太网，已完成的 GMAC 分析保留备查：
- 两板 gmac1 差异：复位脚（R1=GPIO3_PB7 高有效 vs 鲁班猫4=GPIO3_PB2 低有效）、delay（R1 tx=0x44/rx=0x18 vs 鲁班猫4 tx=0x1b）、PHY 地址（实测都是 1，R1 官方 dtb 的 phy@0 是错的）、clock_in_out（实测 R1 是 output，官方 dtb 的 input 导致 DMA reset 失败）。
- GMAC 补丁历史版本在 git/旧文档；若用户以后要修 eth0，参考 §附录的差异表重新做补丁（注意以实测为准）。
- **RTL8211F 以太网 PHY 在原理图**：3468 行（RTL8211F-CG），R1 板载千兆以太网芯片，与 WiFi 无关。

---

## 参考来源

- **V3 原理图**：`doc/youyeetoo R1 v3.pdf`（本机，WiFi 引脚/电源/USB 连接实锤来源）
- R1 官方 dts：`R1_UbuntuCamera_ImageV2V3.img` 内 dtb（`d00dfeed` @ `0x4eca26`）
- 鲁班猫4 dts：24.04 镜像 boot 分区 `0x1322000`
- youyeetoo R1 仓库（README/规格）：https://github.com/youyeetoo/R1
- 鲁班猫 RK3588 设备树/插件文档：https://doc.embedfire.com/linux/rk3588/quick_start/en/latest/quick_start/device_tree/device_tree.html
