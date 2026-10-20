/**
 * @file esf_dump_cortex_m.cpp
 * @author qingyu
 * @brief STM32 Cortex-M 异常栈帧转储
 * @version 0.1
 * @date 2026-10-20
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "esf_dump.hpp"
#include "log.hpp"

/**
 * @brief 转储 Cortex-M 异常栈帧
 * @param esf 异常栈帧（可能为 nullptr）
 *
 * basic 段由硬件自动压栈：pc 指向出错指令，lr 是返回地址，
 * xpsr 的 IPSR 字段是当前异常号。
 */
void DumpEsf(const struct arch_esf* esf)
{
    if (esf == nullptr) {
        DUST_LOG_ERR("[fatal] esf=null");
        return;
    }

    DUST_LOG_ERR("[fatal] pc=%08lx xpsr=%08lx lr=%08lx",
                 static_cast<unsigned long>(esf->basic.pc),
                 static_cast<unsigned long>(esf->basic.xpsr),
                 static_cast<unsigned long>(esf->basic.lr));
}
