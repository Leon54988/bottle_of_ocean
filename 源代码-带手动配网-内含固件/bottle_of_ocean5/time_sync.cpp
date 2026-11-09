#include "time_sync.h"
#include "wifi_credentials.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>
#include <math.h>

static TaskHandle_t s_time_sync_task = nullptr;

// ------------------ 潮汐相关配置（可以按需修改）------------------
// 假设每天有两个完整潮汐周期，每个周期：
// - 从高潮到下一次低潮约 6 小时
// - 从低潮到下一次高潮约 6 小时
// 这里用一个 12 小时的简化模型，并用 8 段“八分算潮”近似每个 6 小时过程。
//
// 参考：我们用一个 8 段的累计高度表 s_tide_cumulative[0..8]，
// 表示从“起点潮位”到“终点潮位”的累计比例（0.0~1.0），
// 在每一小段内部再做线性插值。
//
// 你可以根据自己的八分算潮经验把这个数组改掉，比如：
//   - 低潮到高潮：前期涨得慢、中间快、后期再慢
//   - 数组必须单调不减，首元素为 0.0，最后一个元素为 1.0
static const float s_tide_cumulative[9] = {
    0.0f,  // 第 0 段起点
    0.05f, // 第 1 段末
    0.15f, // 第 2 段末
    0.35f, // 第 3 段末
    0.65f, // 第 4 段末
    0.85f, // 第 5 段末
    0.95f, // 第 6 段末
    0.99f, // 第 7 段末
    1.0f   // 终点
};

// 一个潮汐周期（从高潮到下一次高潮）的时长（秒）
// 这里简化为 12 小时，你可以略微调整（例如真实平均 12h25m）
static const float TIDE_CYCLE_SECONDS = 12.0f * 3600.0f;

// 参考高潮时刻（本地时间，单位：小时）
// 我们假定当天 00:00 左右是一个高潮时间，后面按 12 小时间隔重复。
// 如果你知道自己所在地某一个“基准高潮”的时刻，可以把这里改成那个时间。
static const float TIDE_REF_HIGH_HOUR = 0.0f;

// 将 [0,1] 范围内的“阶段进度”用 8 段线性插值成潮位高度（0~1）
static float tide_curve_eighths(float phase)
{
    if (phase <= 0.0f) return 0.0f;
    if (phase >= 1.0f) return 1.0f;

    float pos = phase * 8.0f;   // 落在 [0,8)
    int idx = (int)pos;         // 段索引 0..7
    float t = pos - (float)idx; // 段内插值 0..1

    float h0 = s_tide_cumulative[idx];
    float h1 = s_tide_cumulative[idx + 1];
    return h0 + (h1 - h0) * t;
}

static void do_ntp_sync(void) {
    char ssid[64] = {0};
    char pass[64] = {0};
    if (!wifi_credentials_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        Serial.println("[time_sync] 未保存 Wi-Fi 凭据，跳过 NTP 同步");
        return;
    }

    Serial.println("[time_sync] 开始 NTP 同步...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    const uint32_t connect_timeout_ms = 8000;
    uint32_t start_ms = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start_ms) < connect_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[time_sync] Wi-Fi 连接失败");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return;
    }

    // 使用中国时区的 NTP 服务器
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
    
    
    // 等待时间同步完成
    const uint32_t ntp_timeout_ms = 5000;
    start_ms = millis();
    time_t now = 0;
    while ((now = time(nullptr)) < (time_t)(8 * 3600) && (millis() - start_ms) < ntp_timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (now >= (time_t)(8 * 3600)) {
        struct tm info;
        localtime_r(&now, &info);
        Serial.printf("[time_sync] NTP 同步成功，当前时间：%04d-%02d-%02d %02d:%02d:%02d\n",
                      info.tm_year + 1900,
                      info.tm_mon + 1,
                      info.tm_mday,
                      info.tm_hour,
                      info.tm_min,
                      info.tm_sec);

        // 同步成功后，计算并打印一次当前蓝色值，方便在串口监视器中观察
        uint8_t blue = time_sync_get_blue();
        Serial.printf("[time_sync] 当前蓝色值：%u\n", blue);
    } else {
        Serial.println("[time_sync] NTP 同步超时");
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

static void time_sync_task(void* arg) {
    (void)arg;

    for (;;) {
        // 等待手动唤醒发来的通知
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 收到通知后执行一次 NTP 同步
        do_ntp_sync();
    }
}

void time_sync_init(void) {
    // 设置时区为中国（UTC+8）
    setenv("TZ", "CST-8", 1);
    //setenv("TZ", "UTCm2+2", 1);//比中国晚十小时
    tzset();

    if (!s_time_sync_task) {
        xTaskCreatePinnedToCore(
            time_sync_task,
            "time_sync_task",
            4096,
            nullptr,
            1,
            &s_time_sync_task,
            1  // 放在 APP CPU 上运行
        );
    }

    // 开机后立即触发一次 NTP 同步（如果任务已创建），
    // 避免长时间处于默认时间导致蓝色值不随真实时间变化。
//    if (s_time_sync_task) {
//        xTaskNotifyGive(s_time_sync_task);
//    }
}

void time_sync_on_manual_wakeup(void) {
    if (s_time_sync_task) {
        xTaskNotifyGive(s_time_sync_task);
    }
}

uint8_t time_sync_get_blue(void) {
    time_t now = time(nullptr);
    if (now <= 0) {
        // 如果时间还没同步，就按午夜处理（蓝色最小）
        return 0;
    }

    struct tm info;
    localtime_r(&now, &info);

    // 使用带有分钟的“浮点小时”，让蓝色强度随时间连续变化
    float hour_f = (float)info.tm_hour + (float)info.tm_min / 60.0f + (float)info.tm_sec / 3600.0f;  // [0,24)
    if (hour_f < 0.0f || hour_f >= 24.0f) {
        return 0;
    }

    float t;
    if (hour_f < 12.0f) {
        // 0:00 -> 12:00  从 0 增到 1
        t = hour_f / 12.0f;
    } else {
        // 12:00 -> 24:00 从 1 降到 0
        t = (24.0f - hour_f) / 12.0f;
    }

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    int blue = (int)lrintf(255.0f * t);
    if (blue < 0) blue = 0;
    if (blue > 255) blue = 255;

    return (uint8_t)blue;
}

float time_sync_get_tide_level(void) {
    time_t now = time(nullptr);
    if (now <= 0) {
        // 时间尚未同步时，返回中等潮位，避免一开始就极端高潮/低潮
        return 0.5f;
    }

    struct tm info;
    localtime_r(&now, &info);

    // 当前本地时间（以秒计）
    float hour_f = (float)info.tm_hour +
                   (float)info.tm_min / 60.0f +
                   (float)info.tm_sec / 3600.0f;  // [0,24)
    if (hour_f < 0.0f || hour_f >= 24.0f) {
        return 0.5f;
    }

    // 以某个“参考高潮时刻”为 0，相对于这个高潮的秒数
    float ref_high_sec = TIDE_REF_HIGH_HOUR * 3600.0f;
    float day_sec = hour_f * 3600.0f;
    float rel_sec = day_sec - ref_high_sec;

    // 折算到一个 12 小时潮汐周期内 [0, TIDE_CYCLE_SECONDS)
    float cycle = TIDE_CYCLE_SECONDS;
    // 做一个浮点 fmod，结果落在 [-cycle, cycle)
    float phase_sec = fmodf(rel_sec, cycle);
    if (phase_sec < 0.0f) {
        phase_sec += cycle;
    }

    // [0,1) 表示从“参考高潮”开始的相位
    float cycle_phase = phase_sec / cycle; // 0.0 -> 1.0

    // 0.0~0.5: 从高潮往低潮的 6 小时
    // 0.5~1.0: 从低潮往下一次高潮的 6 小时
    float tide_level;
    if (cycle_phase < 0.5f) {
        // 第一半个周期：从 1.0 下降到 0.0
        float phase_down = cycle_phase / 0.5f; // 0..1
        float h = tide_curve_eighths(phase_down);
        tide_level = 1.0f - h;
    } else {
        // 第二半个周期：从 0.0 上升到 1.0
        float phase_up = (cycle_phase - 0.5f) / 0.5f; // 0..1
        tide_level = tide_curve_eighths(phase_up);
    }

    if (tide_level < 0.0f) tide_level = 0.0f;
    if (tide_level > 1.0f) tide_level = 1.0f;
    return tide_level;
}
