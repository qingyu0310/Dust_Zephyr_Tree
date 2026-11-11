/**
 * @file encoder.c
 * @author qingyu
 * @brief STM32 定时器编码器设备实现（TIM 编码器模式，硬件正交解码 X4）
 * @version 0.5
 * @date 2026-11-11
 *
 * @copyright Copyright (c) 2026
 *
 */

#define DT_DRV_COMPAT st_stm32_encoder

#include "encoder.h"

#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <soc.h>
#include <stm32_ll_tim.h>

#pragma message "Compiling Drivers/Device Encoder"

/**
 * @brief 设备配置：全部来自 devicetree
 */
struct encoder_config
{
    TIM_TypeDef*                     tim;     // 定时器实例（父节点 reg 地址）
    const struct pinctrl_dev_config* pcfg;    // A/B 相引脚
    const struct stm32_pclken*       pclken;  // 定时器时钟闸门
    uint16_t                         ppr;     // 每圈脉冲数（X4 → 实际 ×4）
};

/**
 * @brief 设备运行态
 */
struct encoder_data
{
    uint16_t last_cnt;       // 上次采到的 CNT（回绕由差值展开处理）
    int32_t  count;          // 累计计数
    float    counts_per_sec; // 计数/秒
    float    speed;          // 圈/秒
    int64_t  last_ms;        // 上次 Update 时刻
    int8_t   direction;      // 1 = 正转，-1 = 反转，0 = 静止
};

/**
 * @brief 初始化：开时钟 → apply pinctrl → 配 TIM 编码器模式 → 使能计数
 * @param dev Zephyr 设备实例
 * @return 0 成功，负值失败
 */
static int encoder_init(const struct device* dev)
{
    const struct encoder_config* cfg = (const struct encoder_config*)dev->config;
    struct encoder_data*         dat = (struct encoder_data*)dev->data;

    // 开定时器时钟闸门（编码器只用计数，不需要频率）
    const struct device* clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
    if (!device_is_ready(clk)) {
        return -ENODEV;
    }
    if (clock_control_on(clk, (clock_control_subsys_t)&cfg->pclken[0]) < 0) {
        return -EIO;
    }

    // 配置 A/B 相引脚复用
    if (pinctrl_apply_state(cfg->pcfg, PINCTRL_STATE_DEFAULT) < 0) {
        return -EIO;
    }

    TIM_TypeDef* tim = cfg->tim;

    // 时基：编码器模式只做计数，不分频（PSC=0、ARR 满量程 → 16 位自然回绕）
    LL_TIM_SetPrescaler(tim, 0);
    LL_TIM_SetAutoReload(tim, 0xFFFF);
    LL_TIM_SetCounter(tim, 0);

    // 输入通道：TI1/TI2 直连、上升沿、轻滤波（FDIV1_N2）、不分频
    LL_TIM_IC_SetActiveInput(tim, LL_TIM_CHANNEL_CH1, LL_TIM_ACTIVEINPUT_DIRECTTI);
    LL_TIM_IC_SetPolarity(tim, LL_TIM_CHANNEL_CH1, LL_TIM_IC_POLARITY_RISING);
    LL_TIM_IC_SetFilter(tim, LL_TIM_CHANNEL_CH1, LL_TIM_IC_FILTER_FDIV1_N2);
    LL_TIM_IC_SetPrescaler(tim, LL_TIM_CHANNEL_CH1, LL_TIM_ICPSC_DIV1);

    LL_TIM_IC_SetActiveInput(tim, LL_TIM_CHANNEL_CH2, LL_TIM_ACTIVEINPUT_DIRECTTI);
    LL_TIM_IC_SetPolarity(tim, LL_TIM_CHANNEL_CH2, LL_TIM_IC_POLARITY_RISING);
    LL_TIM_IC_SetFilter(tim, LL_TIM_CHANNEL_CH2, LL_TIM_IC_FILTER_FDIV1_N2);
    LL_TIM_IC_SetPrescaler(tim, LL_TIM_CHANNEL_CH2, LL_TIM_ICPSC_DIV1);

    // X4：TI1 + TI2 双边沿都计数 → 1 个脉冲 = 4 计数
    LL_TIM_SetEncoderMode(tim, LL_TIM_ENCODERMODE_X4_TI12);

    // 使能捕获通道（CC1E/CC2E）：不开这一步 TI1FP1/TI2FP2 不接入计数器时钟，CNT 恒为 0
    LL_TIM_CC_EnableChannel(tim, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH2);

    LL_TIM_EnableCounter(tim);

    dat->last_cnt       = (uint16_t)LL_TIM_GetCounter(tim);
    dat->last_ms        = k_uptime_get();
    dat->count          = 0;
    dat->counts_per_sec = 0.0f;
    dat->speed          = 0.0f;
    dat->direction      = 0;

    return 0;
}

/**
 * @brief 周期采样：算增量 → 累计计数 → 测速 → 判方向
 * @param dev Zephyr 设备实例
 *
 * 采样周期由调用方保证（控制线程里按固定周期调）；测速用 k_uptime 的实际间隔算，
 * 所以调用方抖动不会带偏速度值。
 */
static void encoder_stm32_update(const struct device* dev)
{
    const struct encoder_config* cfg = (const struct encoder_config*)dev->config;
    struct encoder_data*         dat = (struct encoder_data*)dev->data;

    const uint16_t cnt   = (uint16_t)LL_TIM_GetCounter(cfg->tim);
    const int32_t  delta = (int16_t)(uint16_t)(cnt - dat->last_cnt); // 无符号相减后按 16 位展开，回绕安全

    dat->last_cnt = cnt;
    dat->count   += delta;

    const int64_t now   = k_uptime_get();
    const int64_t dt_ms = now - dat->last_ms;

    if (dt_ms > 0) {
        dat->last_ms        = now;
        dat->counts_per_sec = (float)delta * 1000.0f / (float)dt_ms;
        dat->speed          = dat->counts_per_sec / ((float)cfg->ppr * 4.0f);
    }

    dat->direction = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
}

static int32_t encoder_stm32_get_count(const struct device* dev)
{
    return ((struct encoder_data*)dev->data)->count;
}

static float encoder_stm32_get_counts_per_sec(const struct device* dev)
{
    return ((struct encoder_data*)dev->data)->counts_per_sec;
}

static float encoder_stm32_get_speed(const struct device* dev)
{
    return ((struct encoder_data*)dev->data)->speed;
}

static int8_t encoder_stm32_get_direction(const struct device* dev)
{
    return ((struct encoder_data*)dev->data)->direction;
}

/**
 * @brief 计数与速度清零
 * @param dev Zephyr 设备实例
 *
 * 不动 CNT、不动 last_cnt：硬件计数连续，清了 CNT 反而会丢一份增量。
 */
static void encoder_stm32_reset(const struct device* dev)
{
    struct encoder_data* dat = (struct encoder_data*)dev->data;
    dat->count          = 0;
    dat->counts_per_sec = 0.0f;
    dat->speed          = 0.0f;
    dat->direction      = 0;
}

static uint16_t encoder_stm32_get_counts_per_rev(const struct device* dev)
{
    return ((const struct encoder_config*)dev->config)->ppr;
}

static const struct encoder_driver_api encoder_api = {
    .update               = encoder_stm32_update,
    .get_count            = encoder_stm32_get_count,
    .get_counts_per_sec   = encoder_stm32_get_counts_per_sec,
    .get_speed            = encoder_stm32_get_speed,
    .get_direction        = encoder_stm32_get_direction,
    .reset                = encoder_stm32_reset,
    .get_counts_per_rev   = encoder_stm32_get_counts_per_rev,
};

#define ENCODER(index) DT_INST_PARENT(index)

/**
 * @brief 从 devicetree 填充 config
 */
#define ENCODER_CONFIG(index)                                                              \
{                                                                                          \
    (TIM_TypeDef*)DT_REG_ADDR(ENCODER(index)),                                             \
    PINCTRL_DT_INST_DEV_CONFIG_GET(index),                                                 \
    encoder_pclken_##index,                                                                \
    (uint16_t)DT_INST_PROP(index, st_counts_per_revolution),                               \
}

#define ENCODER_DEFINE(index)                                                              \
    static const struct stm32_pclken encoder_pclken_##index[] = STM32_DT_CLOCKS(ENCODER(index)); \
    PINCTRL_DT_INST_DEFINE(index);                                                         \
    static const struct encoder_config encoder_config_##index = ENCODER_CONFIG(index);     \
    static struct encoder_data encoder_data_##index;                                       \
    DEVICE_DT_INST_DEFINE(index, encoder_init, NULL, &encoder_data_##index,                \
                          &encoder_config_##index, POST_KERNEL,                            \
                          CONFIG_DUST_ENCODER_INIT_PRIORITY, &encoder_api);

DT_INST_FOREACH_STATUS_OKAY(ENCODER_DEFINE)
