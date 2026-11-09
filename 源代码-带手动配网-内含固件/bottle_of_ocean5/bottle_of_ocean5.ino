#include "panel_config.h"
#include "gravity.h"
#include "sim_manager.h"
#include "sleep_manager.h"
#include "time_sync.h"
#include "wifi_provisioning.h"

// 水流体仿真模式
static sim_mode_t g_boot_mode = SIM_MODE_WATER;

// 睡眠管理配置
#define SLEEP_KEY_PIN 2
#define SLEEP_IDLE_TIMEOUT_MS 15000  // 15秒


// ---------------- 配网连击触发（唤醒状态下 3 秒内按下 6 次）----------------
#define PROV_TAP_WINDOW_MS 3000
#define PROV_TAP_COUNT 6
static volatile uint32_t g_tap_first_ms = 0;
static volatile uint8_t g_tap_count = 0;
static volatile bool g_request_provisioning = false;

static void on_key_press_edge(void) {
  // 注意：该回调在 loop() 上下文中被调用（sleep_manager_update 内部），必须足够快
  uint32_t now = millis();

  if (g_tap_count == 0 || g_tap_first_ms == 0 || (now - g_tap_first_ms) > PROV_TAP_WINDOW_MS) {
    g_tap_first_ms = now;
    g_tap_count = 1;
  } else {
    g_tap_count++;
  }

  if (g_tap_count >= PROV_TAP_COUNT && (now - g_tap_first_ms) <= PROV_TAP_WINDOW_MS) {
    g_request_provisioning = true;
    g_tap_count = 0;
    g_tap_first_ms = 0;
  }
}

void setup() {
  // 初始化重力传感器
  gravity_init();
  delay(50);
  pinMode(PANEL_POWER_PIN, OUTPUT);
  digitalWrite(PANEL_POWER_PIN, LOW);
  
  Serial.begin(115200);
  delay(50);
//  Serial.println("Bottle of Ocean 3 - Water Simulation Only");

  // 初始化时间同步模块（创建后台任务、设置时区）
  time_sync_init();

  
  
  int err = gravity_sensor_start();
  if (err != 0) {
    Serial.printf("gravity_sensor_start failed: %d\n", err);
    return;
  }

  // 初始化仿真管理器
  sim_runtime_config_t cfg = {
    .core_id = 1,
    .stack_size = 8192,
    .priority = 5,
    .stop_timeout_ms = 1000,
  };

  err = sim_manager_init(&cfg);
  if (err != 0) {
    Serial.printf("sim_manager_init failed: %d\n", err);
    return;
  }

  // 启动水流体仿真模式
  err = sim_manager_start(g_boot_mode);
  if (err != 0) {
    Serial.printf("sim_manager_start failed: %d\n", err);
    return;
  }

  // 初始化睡眠管理器
  err = sleep_manager_init(SLEEP_KEY_PIN, SLEEP_IDLE_TIMEOUT_MS);
  if (err != 0) {
    Serial.printf("sleep_manager_init failed: %d\n", err);
    return;
  }

  // 注册按键“按下沿”回调，用于 6 连击进入配网模式
  sleep_manager_set_key_press_callback(on_key_press_edge);

  // 启动睡眠管理器（检测是否从深度睡眠唤醒）
  err = sleep_manager_start();
  if (err != 0) {
    Serial.printf("sleep_manager_start failed: %d\n", err);
    return;
  }

  Serial.println("System initialized successfully");
}

void loop() {
  // 配网模式下：停止休眠管理（避免误入深睡），专注处理 captive portal
  if (wifi_provisioning_is_active()) {
    wifi_provisioning_update();
    delay(5);
    return;
  }

  // 更新睡眠管理器（检测按键、超时、处理亮度渐变等）
  sleep_manager_update();

  if (g_request_provisioning) {
    g_request_provisioning = false;
    Serial.println("[prov] 6-tap detected: enter provisioning");
    wifi_provisioning_start();
  }
  
  // 短暂延时，避免CPU占用过高
  delay(25);
}
