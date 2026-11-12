/**
 * @file esf_dump_riscv.cpp
 * @author qingyu
 * @brief HPM RISC-V 异常栈帧转储
 * @version 0.3
 * @date 2026-10-21
 *
 * @copyright Copyright (c) 2026
 *
 */

#include <cstdint>
#include <cstdio>
#include "esf_dump.hpp"

/**
 * @brief 转储 RISC-V 异常栈帧
 * @param esf 异常栈帧（可能为 nullptr）
 *
 * mcause 位域：最低位 1=中断 / 0=异常；其余为异常码（5=Load access fault，
 * 7=Store access fault，常见于空指针 / 越界）。mtval 记录出错地址。
 *
 * 输出走 FatalOut 轮询直发：handler 内中断已关且随后永久自旋，DMA 与异步
 * 日志队列都没有机会续发。
 */
void DumpEsf(const struct arch_esf* esf)
{
    unsigned long mcause = 0;
    unsigned long mtval  = 0;

    __asm__ volatile("csrr %0, mcause" : "=r" (mcause));
    __asm__ volatile("csrr %0, mtval"  : "=r" (mtval));

    char line[128];

    if (esf == nullptr) {
        snprintf(line, sizeof(line), "[fatal] mcause=%08lx mtval=%08lx esf=null", mcause, mtval);
        FatalOut(line);
        return;
    }

    snprintf(line, sizeof(line), "[fatal] mcause=%08lx mtval=%08lx mepc=%08lx mstatus=%08lx", mcause, mtval, esf->mepc, esf->mstatus);
    FatalOut(line);

    // SP 不在 esf 里单独保存：异常发生时栈帧压在栈顶，sp = esf + sizeof(esf)
    const uintptr_t sp = reinterpret_cast<uintptr_t>(esf) + sizeof(struct arch_esf);

    snprintf(line, sizeof(line), "[fatal] sp=%08lx ra=%08lx a0=%08lx a1=%08lx", static_cast<unsigned long>(sp), esf->ra, esf->a0, esf->a1);
    FatalOut(line);
}
