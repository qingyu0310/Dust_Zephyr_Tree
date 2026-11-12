/**
 * @file encoder.h
 * @author qingyu
 * @brief STM32 定时器编码器设备 — 硬件正交解码（X4）+ 周期测速（C 驱动接口）
 * @version 0.7
 * @date 2026-11-11
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 编码器设备 API
 *
 * 由驱动 encoder.c 实现（实现名带驱动前缀 encoder_stm32_*，与 Zephyr 原生驱动同构）。
 */
struct encoder_driver_api
{
    void     (*update)(const struct device* dev);             	// 采一次 CNT：算增量 / 累计 / 测速 / 方向
    int32_t  (*get_count)(const struct device* dev);          	// 累计计数（可正可负）
    float    (*get_counts_per_sec)(const struct device* dev);	// 计数/秒
    float    (*get_speed)(const struct device* dev);          	// 圈/秒 = 计数/秒 ÷ (ppr × 4)
    int8_t   (*get_direction)(const struct device* dev);      	// 1 = 正转，-1 = 反转，0 = 静止
    void     (*reset)(const struct device* dev);              	// 计数与速度清零（不动硬件 CNT）
    uint16_t (*get_counts_per_rev)(const struct device* dev);	// 每圈脉冲数（dts 的 st,counts-per-revolution）
};

/**
 * @brief 周期调用：采 CNT → 算增量/速度/方向
 * @param dev 编码器设备
 */
static inline void encoder_update(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    api->update(dev);
}

/**
 * @brief 累计计数（可正可负）
 */
static inline int32_t encoder_get_count(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    return api->get_count(dev);
}

/**
 * @brief 原始计数速度（计数/秒）
 */
static inline float encoder_get_counts_per_sec(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    return api->get_counts_per_sec(dev);
}

/**
 * @brief 换算速度（圈/秒 = 计数/秒 ÷ (ppr × 4)）
 */
static inline float encoder_get_speed(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    return api->get_speed(dev);
}

/**
 * @brief 方向：1 = 正转，-1 = 反转，0 = 静止
 */
static inline int8_t encoder_get_direction(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    return api->get_direction(dev);
}

/**
 * @brief 计数与速度清零（不动硬件 CNT）
 */
static inline void encoder_reset(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    api->reset(dev);
}

/**
 * @brief 每圈脉冲数（dts 的 st,counts-per-revolution）
 */
static inline uint16_t encoder_get_counts_per_rev(const struct device* dev)
{
    const struct encoder_driver_api* api = (const struct encoder_driver_api*)dev->api;
    return api->get_counts_per_rev(dev);
}

#ifdef __cplusplus
}
#endif
