#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 从 NVS 读取 WiFi 凭据；返回 true 表示存在且非空
bool wifi_credentials_load(char* ssid_out, size_t ssid_out_len,
                           char* pass_out, size_t pass_out_len);

// 保存 WiFi 凭据到 NVS；ssid 不能为空
bool wifi_credentials_save(const char* ssid, const char* pass);

// 清除 WiFi 凭据
void wifi_credentials_clear(void);

#ifdef __cplusplus
}
#endif


