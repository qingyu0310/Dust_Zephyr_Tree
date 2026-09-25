# STM32G431CBTx Custom Board

## 注意

此板级定义由 gongxun 自定义创建，非 Zephyr 官方支持的板。

- **芯片**: STM32G431CBT6 (48-pin LQFP, 128KB Flash / 32KB RAM)
- **用途**: gongxun 板卡
- **创建日期**: 2026-09-21

## 时钟

- HSE 24MHz → PLL（div-m=6, mul-n=85, div-p=2）→ SYSCLK 170MHz
- USB 48MHz 时钟源 = HSI48（RCC_CCIPR.CLK48SEL 复位值 00，无需改选择位）

## 所用引脚

| 功能 | 引脚 | 备注 |
|------|------|------|
| USART1 TX/RX | PA9 / PA10 | 调试串口（板卡基线 115200，工程 overlay 改 921600 + DMA） |
| USB DM/DP | PA11 / PA12 | dustusb 设备 |
