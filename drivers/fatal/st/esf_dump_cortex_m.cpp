/**
 * @file esf_dump_cortex_m.cpp
 * @author qingyu
 * @brief STM32 Cortex-M 异常栈帧转储
 * @version 0.3
 * @date 2026-10-21
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <cstdio>
#include "esf_dump.hpp"

/**
 * @brief 转储 Cortex-M 异常栈帧
 * @param esf 异常栈帧（可能为 nullptr）
 *
 * basic 段由硬件自动压栈：pc 指向出错指令，lr 是返回地址，
 * xpsr 的 IPSR 字段是当前异常号。
 *
 * 输出走 FatalOut 轮询直发：handler 内中断已关且随后永久自旋，DMA 与异步
 * 日志队列都没有机会续发。
 */
void DumpEsf(const struct arch_esf* esf)
{
    char line[128];

    if (esf == nullptr) {
        snprintf(line, sizeof(line), "[fatal] esf=null");
        FatalOut(line);
        return;
    }

    snprintf(line, sizeof(line), "[fatal] pc=%08lx xpsr=%08lx lr=%08lx",
             static_cast<unsigned long>(esf->basic.pc),
             static_cast<unsigned long>(esf->basic.xpsr),
             static_cast<unsigned long>(esf->basic.lr));
    FatalOut(line);
}
