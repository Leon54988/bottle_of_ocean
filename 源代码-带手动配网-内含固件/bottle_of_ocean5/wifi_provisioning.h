#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 开始配网模式：停止仿真、显示提示图案、开启热点与网页配网
// 返回 true 表示启动成功
bool wifi_provisioning_start(void);

// 在 loop() 中周期调用，驱动 DNS/WebServer
void wifi_provisioning_update(void);

// 是否处于配网模式
bool wifi_provisioning_is_active(void);

// 主动退出配网模式（无论是否成功）
void wifi_provisioning_stop(void);

#ifdef __cplusplus
}
#endif


