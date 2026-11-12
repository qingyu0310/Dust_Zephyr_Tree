/* CMSIS core header for Cortex-M — shadows Zephyr's empty stub */
#if defined(CONFIG_CPU_CORTEX_M)
/* CMSIS v5.7+ renamed SHPR to SHP; Zephyr v4.3 still uses old name */
#define SHPR SHP
#include <soc.h>
/* core_cmX.h 由 soc.h 按芯片系列带进来（F1 → core_cm3.h / F4 → core_cm4.h），
   这里不能再写死 core_cm4.h —— M3 上会和 soc.h 已引入的 core_cm3.h 撞类型定义 */
#elif defined(CONFIG_CPU_AARCH32_CORTEX_A) || defined(CONFIG_CPU_AARCH32_CORTEX_R)
/* Zephyr's internal module handles these */
#endif
