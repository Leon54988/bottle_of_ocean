#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int water_sim_start(int core_id, uint32_t stack_size, int priority);
int water_sim_stop(uint32_t timeout_ms);
bool water_sim_is_running(void);

#ifdef __cplusplus
}
#endif

