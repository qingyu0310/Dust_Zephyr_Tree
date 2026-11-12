# STM32F407IGH6 Custom Board

## 注意

此板级定义由项目 blinky 自定义创建，非 Zephyr 官方支持的板。

- **芯片**: STM32F407IGH6 (176-pin BGA)
- **用途**: 机器人底盘控制
- **创建日期**: 2026-05-13

## 与 stm32f4_disco 的差异

- 176 脚 BGA 封装（非 100 脚 LQFP）
- 移除了不匹配的外设（I2S、音频码、PWM LED 等）
- 增加了 IGH6 专用引脚定义（如 USART6 TX=PG14, RX=PG9）
- 引脚配置由自定义 pinctrl 文件管理，不依赖自动生成的 pinctrl

## 所用引脚

| 功能 | 引脚 | 备注 |
|------|------|------|
| USART1 TX/RX | PA9 / PB7 | 115200, 调试控制台 |
| USART3 RX | PC11 | 100000, 遥控器接收 |
| USART6 TX/RX | PG14 / PG9 | 115200, 预留 |
| CAN1 | PA11 / PA12 | 1Mbps, 电机总线 |
| CAN2 | PB5 / PB13 | 1Mbps, 电机总线 |
| LED | PH12 | GPIO_ACTIVE_LOW |
