#include "rgb.h"
#include "panel_config.h"
#include <FastLED.h>
#include <esp_sleep.h>

#define RGB_GPIO_PIN 48  // 修改为GPIO48
#define RGB_LED_COUNT PANEL_LED_COUNT

CRGB leds[RGB_LED_COUNT];

void rgb_init(void) {
  FastLED.addLeds<WS2812, RGB_GPIO_PIN, GRB>(leds, RGB_LED_COUNT);
  
  // 检查是否从深度睡眠唤醒，如果是则设置亮度为0（避免闪烁）
  // 否则设置最大亮度
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    // 从深度睡眠唤醒，设置亮度为0，等待后续渐变恢复
    FastLED.setBrightness(0);
  }// else {
    // 正常启动，设置最大亮度
  //FastLED.setBrightness(255);
  //}
  
  //FastLED.clear();
  FastLED.show();
}

void rgb_deinit(void) {
  // FastLED库不需要显式清理
}

void rgb_set(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= RGB_LED_COUNT) {
    return;
  }

  uint8_t m = r;
  if (g > m) {
    m = g;
  }
  if (b > m) {
    m = b;
  }

  if (m > PANEL_LED_VALUE_MAX) {
    r = (uint16_t)r * PANEL_LED_VALUE_MAX / m;
    g = (uint16_t)g * PANEL_LED_VALUE_MAX / m;
    b = (uint16_t)b * PANEL_LED_VALUE_MAX / m;
  }

  leds[index] = CRGB(r, g, b);
}

void rgb_set_fast(uint32_t index, uint8_t r, uint8_t g, uint8_t b) {
  if (index >= RGB_LED_COUNT) {
    return;
  }
  leds[index] = CRGB(r, g, b);
}

void rgb_clear(void) {
  FastLED.clear();
}

void rgb_set_hsv(uint32_t index, uint16_t hue, uint16_t light) {
  if (index >= RGB_LED_COUNT) {
    return;
  }

  hue %= 360;
  light = (light > PANEL_LED_VALUE_MAX) ? PANEL_LED_VALUE_MAX : light;
  
  CHSV hsv(hue, 255, light);
  CRGB rgb;
  hsv2rgb_spectrum(hsv, rgb);
  leds[index] = rgb;
}

void rgb_show(void) {
  FastLED.show();
  delayMicroseconds(80);
}

void rgb_set_brightness(uint8_t brightness) {
  FastLED.setBrightness(brightness);
}

uint8_t rgb_get_brightness(void) {
  return FastLED.getBrightness();
}
