/* CMSIS core header for Cortex-M — shadows Zephyr's empty stub */
#if defined(CONFIG_CPU_CORTEX_M)
/* CMSIS v5.7+ renamed SHPR to SHP; Zephyr v4.3 still uses old name */
#define SHPR SHP
#include <soc.h>
#include <core_cm4.h>
#elif defined(CONFIG_CPU_AARCH32_CORTEX_A) || defined(CONFIG_CPU_AARCH32_CORTEX_R)
/* Zephyr's internal module handles these */
#endif
