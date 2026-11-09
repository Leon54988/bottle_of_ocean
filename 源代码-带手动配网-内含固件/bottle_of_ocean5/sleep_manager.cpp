#include "sleep_manager.h"
#include "rgb.h"
#include "time_sync.h"
#include "gravity.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#define KEY_PIN_DEFAULT 4
#define IDLE_TIMEOUT_MS_DEFAULT 15000
#define BRIGHTNESS_FADE_STEP 5
#define BRIGHTNESS_FADE_DELAY_MS 20
#define BRIGHTNESS_MAX 255


static uint8_t s_key_pin = KEY_PIN_DEFAULT;
static uint32_t s_idle_timeout_ms = IDLE_TIMEOUT_MS_DEFAULT;
static uint32_t s_last_activity_time = 0;
static uint32_t s_last_fade_time = 0;
static bool s_initialized = false;
static bool s_sleeping = false;
static bool s_fading = false;
static bool s_preparing_sleep = false;  // 是否正在准备睡眠（降低亮度）
static uint8_t s_target_brightness = BRIGHTNESS_MAX;
static bool s_last_key_pressed = false;  // 上一次扫描时按键是否为按下状态
static uint32_t s_last_key_release_time = 0;  // 上一次检测到按键“松开”（高电平）的时间
static sleep_manager_key_cb_t s_key_press_cb = nullptr;

// 检查按键是否被按下（低电平）
static bool is_key_pressed(void) {
  return digitalRead(s_key_pin) == LOW;
}

void sleep_manager_set_key_press_callback(sleep_manager_key_cb_t cb) {
  s_key_press_cb = cb;
}

// 阻塞渐变亮度（用于唤醒时，此时不需要检测按键）
static void fade_brightness_blocking(uint8_t target) {
  uint8_t current = rgb_get_brightness();
  
  if (current == target) {
    return;
  }
  
  int step = (target > current) ? BRIGHTNESS_FADE_STEP : -BRIGHTNESS_FADE_STEP;
  
  while (current != target) {
    current += step;
    if ((step > 0 && current >= target) || (step < 0 && current <= target)) {
      current = target;
    }
    rgb_set_brightness(current);
    delay(BRIGHTNESS_FADE_DELAY_MS);
  }
}

// 非阻塞渐变亮度（每次调用只执行一步）
// 返回true表示渐变完成，false表示还在渐变中
static bool fade_brightness_step(void) {
  uint8_t current = rgb_get_brightness();
  
  if (current == s_target_brightness) {
    s_fading = false;
    return true;  // 渐变完成
  }
  
  // 检查是否到了该执行下一步的时间
  uint32_t now = millis();
  if (now - s_last_fade_time < BRIGHTNESS_FADE_DELAY_MS) {
    return false;  // 还没到时间，继续等待
  }
  
  s_last_fade_time = now;
  s_fading = true;
  
  int step = (s_target_brightness > current) ? BRIGHTNESS_FADE_STEP : -BRIGHTNESS_FADE_STEP;
  current += step;
  
  // 确保不超过目标值
  if ((step > 0 && current >= s_target_brightness) || (step < 0 && current <= s_target_brightness)) {
    current = s_target_brightness;
  }
  
  rgb_set_brightness(current);
  
  if (current == s_target_brightness) {
    s_fading = false;
    return true;  // 渐变完成
  }
  
  return false;  // 还在渐变中
}

// 进入深度睡眠
static void enter_deep_sleep(void) {
  Serial.println("Entering deep sleep mode...");

  // 此时点阵屏亮度已经渐变到 0（熄灭），在芯片进入深度睡眠前，
  // 先让 MPU6050 进入睡眠模式，降低待机功耗
  gravity_sensor_sleep();
  
  //关断灯珠电源
  digitalWrite(PANEL_POWER_PIN, HIGH);
  
  // 配置GPIO2为唤醒源（低电平触发）
  esp_sleep_enable_ext0_wakeup((gpio_num_t)s_key_pin, 0); // 0 = LOW level

  delay(25);
  
  // 进入深度睡眠
  esp_deep_sleep_start();
  // 不会返回，除非被唤醒
}

int sleep_manager_init(uint8_t key_pin, uint32_t idle_timeout_ms) {
  if (s_initialized) {
    return -1;
  }
  
  s_key_pin = key_pin;
  s_idle_timeout_ms = idle_timeout_ms;
  
  // 配置按键引脚为输入，上拉
  pinMode(s_key_pin, INPUT_PULLUP);
  
  // 记录初始化时间
  s_last_activity_time = millis();
  s_last_fade_time = 0;
  s_initialized = true;
  s_sleeping = false;
  s_fading = false;
  s_preparing_sleep = false;
  s_target_brightness = BRIGHTNESS_MAX;
  s_last_key_pressed = is_key_pressed();
  if (!s_last_key_pressed) {
    // 上电时如果按键已经是“松开”（高电平）状态，则从现在开始计时
    s_last_key_release_time = millis();
  } else {
    // 按下状态下不计“松开时间”
    s_last_key_release_time = 0;
  }
  
  Serial.printf("Sleep manager initialized: key_pin=%d, timeout=%lu ms\n", 
                s_key_pin, s_idle_timeout_ms);
  
  return 0;
}

int sleep_manager_start(void) {
  if (!s_initialized) {
    return -1;
  }
  
  // 检查是否从深度睡眠唤醒
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("Woke up from deep sleep by key press");

    // 这是一次“手动唤醒”，先使用 RTC 当前时间渲染，
    // 同时在后台任务中尝试进行一次 NTP 同步校时。
    time_sync_on_manual_wakeup();

    s_sleeping = false;
    s_last_activity_time = millis();
    
    // 等待RGB模块初始化完成（后台任务中的rgb_init会设置亮度为0）
    // 需要等待足够的时间让后台任务启动并调用rgb_init
    delay(200);
    
    // 确保亮度为0（rgb_init应该已经设置了，但这里再次确认）
    rgb_set_brightness(0);
    
    // 从0逐渐恢复亮度（使用阻塞版本，因为此时系统刚启动）
    fade_brightness_blocking(BRIGHTNESS_MAX);
    s_target_brightness = BRIGHTNESS_MAX;
    s_preparing_sleep = false;
  } else {
    // 正常启动，设置最大亮度（FastLED会保存这个值，即使RGB还没初始化）
    rgb_set_brightness(BRIGHTNESS_MAX);
    s_target_brightness = BRIGHTNESS_MAX;
  }
  
  return 0;
}

void sleep_manager_reset_timer(void) {
  if (!s_initialized) return;
  
  s_last_activity_time = millis();
  
  // 如果正在准备睡眠（降低亮度），立即停止并恢复亮度
  if (s_preparing_sleep) {
    s_preparing_sleep = false;
    s_target_brightness = BRIGHTNESS_MAX;
    s_last_fade_time = 0;  // 重置渐变时间，立即开始增加亮度
    Serial.println("Sleep preparation interrupted, restoring brightness");
  }
  
  // 如果正在睡眠或亮度降低，恢复亮度
  if (s_sleeping || rgb_get_brightness() < BRIGHTNESS_MAX) {
    s_sleeping = false;
    s_target_brightness = BRIGHTNESS_MAX;
    s_last_fade_time = 0;  // 重置渐变时间，立即开始增加亮度
  }
}

void sleep_manager_update(void) {
  if (!s_initialized || s_sleeping) {
    return;
  }
  
  uint32_t now = millis();
  bool pressed = is_key_pressed();  // 低电平为按下，高电平为松开

  // 1. 先根据当前按键电平更新“松开时间”和亮度状态
  if (pressed) {
    // 松开->按下沿：触发一次回调（用于连击/交互等）
    if (!s_last_key_pressed && s_key_press_cb) {
      s_key_press_cb();
    }
    // 按键被按下（低电平）——认为用户正在操作：
    // - 不累计“松开高电平时间”
    // - 如果正在做休眠准备（渐暗），立即打断并恢复亮度
    if (s_preparing_sleep) {
      s_preparing_sleep = false;
      s_target_brightness = BRIGHTNESS_MAX;
      s_last_fade_time = 0;
      Serial.println("Sleep preparation interrupted by key press, restoring brightness");
    }
    s_last_key_pressed = true;
    // 按下期间直接返回，不会进入休眠计时
    return;
  } else {
    // 当前为高电平 = “松开”状态
    if (s_last_key_pressed) {
      // 刚从按下（低电平）变为松开（高电平）：从现在开始重新计 15 秒
      s_last_key_release_time = now;
    }
    s_last_key_pressed = false;
  }
  
  // 如果正在准备睡眠（降低亮度），执行渐变步骤并检查是否完成
  if (s_preparing_sleep) {
    // 执行一步渐变
    bool fade_complete = fade_brightness_step();
    
    if (fade_complete) {
      // 渐变完成，检查是否降到0
      if (rgb_get_brightness() == 0) {
        s_preparing_sleep = false;
        s_sleeping = true;
        enter_deep_sleep();
        return;
      }
    }
    
    // 渐变还在进行中，继续等待
    return;
  }
  
  // 如果不在准备睡眠状态，但目标亮度不是当前亮度，继续渐变
  if (rgb_get_brightness() != s_target_brightness) {
    fade_brightness_step();
  }
  
  // 检查是否超时
  // 只关心“松开”（高电平）持续的时间：
  // 要求：如果按键被释放（IO2 为高电平）持续 15 秒，则进入休眠；
  // 其间如果 IO2 被拉低，则在再次变为高电平时重新开始计时。
  if (s_last_key_release_time == 0) {
    // 还没有记录过“松开时间”，不进行休眠判断
    return;
  }

  uint32_t elapsed = now - s_last_key_release_time;
  
  if (elapsed >= s_idle_timeout_ms) {
    // 开始准备睡眠：设置目标亮度为0，开始非阻塞渐变
    if (rgb_get_brightness() > 0) {
      s_preparing_sleep = true;
      s_target_brightness = 0;
      s_last_fade_time = 0;  // 立即开始渐变
      Serial.println("Starting sleep preparation: fading brightness to 0");
    } else {
      // 亮度已经是0，直接进入睡眠
      s_sleeping = true;
      enter_deep_sleep();
    }
    return;
  }
}

bool sleep_manager_is_sleeping(void) {
  return s_sleeping;
}
