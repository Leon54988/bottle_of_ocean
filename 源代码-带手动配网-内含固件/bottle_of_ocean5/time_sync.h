#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化时间同步模块（创建后台任务、设置时区等）
void time_sync_init(void);

// 在“手动唤醒”（按键从深度睡眠唤醒）时调用，触发后台 NTP 同步
void time_sync_on_manual_wakeup(void);

// 根据当前本地时间（时）计算蓝色通道的值：
// - 0:00 -> 12:00 逐小时从 0 增加到 255
// - 12:00 -> 24:00 逐小时从 255 降到 0
uint8_t time_sync_get_blue(void);

// 根据当前本地时间计算潮位因子（0.0 = 最低潮, 1.0 = 最高潮）
// 实现为“8 段分段线性近似”的潮差曲线，你可以在 time_sync.cpp 里修改
// s_tide_cumulative[] 数组来微调每一段的权重，实现你习惯的“八分算潮法”。
float time_sync_get_tide_level(void);

#ifdef __cplusplus
}
#endif


