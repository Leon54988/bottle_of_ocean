#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_POWER_PIN 4

typedef void (*sleep_manager_key_cb_t)(void);

// 初始化睡眠管理器
// key_pin: 按键引脚（GPIO2）
// idle_timeout_ms: 无操作超时时间（毫秒），默认15000
int sleep_manager_init(uint8_t key_pin, uint32_t idle_timeout_ms);

// 启动睡眠管理器（需要在主循环中定期调用）
int sleep_manager_start(void);

// 设置按键“按下沿”(松开->按下, 高->低)回调
// 注意：回调在 sleep_manager_update() 的上下文中执行，应尽量短小且非阻塞
void sleep_manager_set_key_press_callback(sleep_manager_key_cb_t cb);

// 更新按键状态（检测到按键按下时调用，重置计时器）
void sleep_manager_reset_timer(void);

// 处理睡眠管理逻辑（在主循环中调用）
void sleep_manager_update(void);

// 获取当前是否处于睡眠状态
bool sleep_manager_is_sleeping(void);

#ifdef __cplusplus
}
#endif
