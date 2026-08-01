/**
 * @file thread.hpp
 * @author qingyu
 * @brief Zephyr 线程模板，封装线程创建、启动等样板代码
 * @version 0.1
 * @date 2026-04-12
 *
 * @copyright Copyright (c) 2026
 *
 */

#pragma once

#include <zephyr/kernel.h>
#include <cstdint>

/**
 * @brief 线程优先级（Zephyr 数值越小优先级越高）
 */
enum class ThreadPrio : uint8_t
{
    Highest     = 1,
    VeryHigh    = 2,
    High        = 3,
    Normal      = 4,
    Low         = 5,
    VeryLow     = 6,
    Lowest      = 7,
};

/**
 * @brief 线程模板，封装 k_thread 创建和初始化
 *
 * @tparam StackSize 线程栈大小
 */
template<uint32_t StackSize = 1024>
class Thread final
{
public:
    void Start(k_thread_entry_t entry, ThreadPrio prio = ThreadPrio::Normal, void* p1 = nullptr)
    {
        k_thread_create(&thread_, stack_, K_THREAD_STACK_SIZEOF(stack_),
                        entry, p1, nullptr, nullptr,
                        static_cast<uint8_t>(prio), 0, K_NO_WAIT);
    }

private:
    k_thread thread_ {};
    K_KERNEL_STACK_MEMBER(stack_, StackSize);
};

