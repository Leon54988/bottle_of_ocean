#include "wifi_provisioning.h"

#include "panel_config.h"
#include "rgb.h"
#include "sim_manager.h"
#include "wifi_credentials.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

// ---------- 配网 AP 配置 ----------
static const char* kApSsid = "BottleOfOcean-Setup";
static const char* kApPass = ""; // 开放热点，便于手机连接
static const uint16_t kDnsPort = 53;

static DNSServer s_dns;
static WebServer s_web(80);

static bool s_active = false;
static uint32_t s_started_ms = 0;

static String s_last_error;
static String s_last_ok;

static void draw_provisioning_icon(void) {
  rgb_clear();

  // 配网图案：竖向显示（按 8x32 的画布直接绘制到物理 8x32 面板）。
  //
  // 说明：当前面板配置为 8(W) x 32(H)，因此直接使用 (x,y) = (0..7, 0..31)。
  // 注意：主仿真（water_sim.cpp）在输出到面板时做了 180° 旋转。
  // 这里按你的需求将配网图案“再旋转 180°”（也就是不再在此处做输出翻转），
  // 让配网图标的朝向与肉眼观感一致（点在下、弧在上）。
  constexpr int CANVAS_W = PANEL_WIDTH;   // 8
  constexpr int CANVAS_H = PANEL_HEIGHT; // 32

  auto set_px = [](int lx, int ly, uint8_t r, uint8_t g, uint8_t b) {
    if (lx < 0 || lx >= CANVAS_W || ly < 0 || ly >= CANVAS_H) return;

    // 竖向画布 -> 物理面板坐标（8x32）
    int x = lx; // [0..7]
    int y = ly; // [0..31]
    if (x < 0 || x >= PANEL_WIDTH || y < 0 || y >= PANEL_HEIGHT) return;

    // 按需求：此处不再做 180° 输出翻转
    int idx = panel_led_index(x, y);  // panel_led_index 内部已处理蛇形走线
    rgb_set_fast((uint32_t)idx, r, g, b);
  };

  // 你指定的配网颜色
  const uint8_t R = 50, G = 0, B = 10;

  // --- Wi‑Fi 图标（竖向放在屏幕最中心，更像弧形）---
  // 分辨率只有 8x32：用“由窄到宽”的弧形带来逼近圆弧观感，并保留弧线间的留白。
  //
  // 图案整体 y 范围约 11..20，中心约在 y=15.5（屏幕中心 y=15.5），保证居中。

  // 约束：点用 2x2 像素；弧线厚度为 1 像素（不做带状填充）。

  // Outer arc (1px thick)
  for (int x = 1; x <= 6; x++) set_px(x, 11, R, G, B);
  set_px(0, 12, R, G, B);
  set_px(7, 12, R, G, B);

  // Middle arc (1px thick) - moved up 1px to tighten spacing
  for (int x = 2; x <= 5; x++) set_px(x, 13, R, G, B);
  set_px(1, 14, R, G, B);
  set_px(6, 14, R, G, B);

  // Inner arc (1px thick) - moved up 2px to tighten spacing
  for (int x = 3; x <= 4; x++) set_px(x, 15, R, G, B);
  set_px(2, 16, R, G, B);
  set_px(5, 16, R, G, B);

  // Dot (bottom, 2x2)
  set_px(3, 18, R, G, B);
  set_px(4, 18, R, G, B);
  set_px(3, 19, R, G, B);
  set_px(4, 19, R, G, B);

  rgb_show();
  delay(50);
}

static String html_escape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c; break;
    }
  }
  return out;
}

static void handle_root() {
  String page;
  page.reserve(1500);
  page += F("<!doctype html><html><head><meta charset='utf-8'/>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
            "<title>BottleOfOcean 配网</title>"
            "<style>"
            "body{font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial;margin:24px;}"
            ".card{max-width:520px;padding:16px 18px;border:1px solid #ddd;border-radius:12px;}"
            "label{display:block;margin:12px 0 6px;font-weight:600;}"
            "input{width:100%;padding:10px;border:1px solid #ccc;border-radius:10px;font-size:16px;}"
            "button{margin-top:14px;width:100%;padding:10px 12px;border:0;border-radius:10px;font-size:16px;"
            "background:#1677ff;color:#fff;font-weight:700;}"
            ".hint{color:#666;font-size:13px;line-height:1.35;margin-top:10px;}"
            ".msg-ok{margin-top:10px;color:#0a7a2f;font-weight:700;}"
            ".msg-err{margin-top:10px;color:#b00020;font-weight:700;}"
            "</style></head><body><div class='card'>"
            "<h2 style='margin:0 0 6px'>BottleOfOcean 配网</h2>"
            "<div class='hint'>连接到热点 <b>");
  page += html_escape(String(kApSsid));
  page += F("</b> 后，在此填写家里 Wi‑Fi。</div>");

  if (s_last_ok.length()) {
    page += F("<div class='msg-ok'>");
    page += html_escape(s_last_ok);
    page += F("</div>");
  } else if (s_last_error.length()) {
    page += F("<div class='msg-err'>");
    page += html_escape(s_last_error);
    page += F("</div>");
  }

  page += F("<form method='POST' action='/save'>"
            "<label>Wi‑Fi 名称 (SSID)</label>"
            "<input name='ssid' maxlength='63' required />"
            "<label>Wi‑Fi 密码</label>"
            "<input name='pass' type='password' maxlength='63' />"
            "<button type='submit'>保存并连接</button>"
            "</form>"
            "<div class='hint'>保存成功后设备会自动退出配网并回到流体模拟。</div>"
            "</div></body></html>");

  s_web.send(200, "text/html; charset=utf-8", page);
}

static void handle_save() {
  s_last_error = "";
  s_last_ok = "";

  String ssid = s_web.arg("ssid");
  String pass = s_web.arg("pass");
  ssid.trim();

  if (ssid.length() == 0) {
    s_last_error = "SSID 不能为空";
    handle_root();
    return;
  }

  Serial.printf("[prov] received ssid='%s' (pass len=%u)\n", ssid.c_str(), (unsigned)pass.length());

  // 保持 AP，额外开启 STA 连接测试
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  const uint32_t connect_timeout_ms = 12000;
  uint32_t start_ms = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start_ms) < connect_timeout_ms) {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED) {
    s_last_error = "连接失败：请检查 SSID/密码，或靠近路由器后重试";
    WiFi.disconnect(true, true);
    handle_root();
    return;
  }

  // 保存凭据
  if (!wifi_credentials_save(ssid.c_str(), pass.c_str())) {
    s_last_error = "已连接但保存失败（NVS 写入失败）";
    handle_root();
    return;
  }

  IPAddress ip = WiFi.localIP();
  s_last_ok = "连接成功，IP: " + ip.toString() + "，即将退出配网…";

  // 给浏览器回一个成功页，然后异步退出配网
  handle_root();
  delay(500);

  wifi_provisioning_stop();
}

static void handle_not_found() {
  // Captive portal: 任何未知路径都重定向到根页面
  s_web.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  s_web.send(302, "text/plain", "");
}

bool wifi_provisioning_start(void) {
  if (s_active) return true;

  Serial.println("[prov] starting provisioning mode...");

  // 1) 停止仿真，避免继续刷新点阵
  sim_manager_stop();

  // 2) 画提示图案
  draw_provisioning_icon();

  // 3) 开启 AP + DNS 劫持 + WebServer
  WiFi.mode(WIFI_AP);
  bool ap_ok = WiFi.softAP(kApSsid, kApPass);
  if (!ap_ok) {
    Serial.println("[prov] softAP start failed");
    // 回滚：清屏并恢复仿真
    rgb_clear();
    rgb_show();
    WiFi.mode(WIFI_OFF);
    sim_manager_start(SIM_MODE_WATER);
    return false;
  }

  IPAddress ap_ip = WiFi.softAPIP();
  Serial.printf("[prov] AP started: ssid=%s ip=%s\n", kApSsid, ap_ip.toString().c_str());

  s_dns.start(kDnsPort, "*", ap_ip);

  s_web.on("/", HTTP_GET, handle_root);
  s_web.on("/save", HTTP_POST, handle_save);
  s_web.onNotFound(handle_not_found);
  s_web.begin();

  s_last_error = "";
  s_last_ok = "";
  s_active = true;
  s_started_ms = millis();
  return true;
}

void wifi_provisioning_update(void) {
  if (!s_active) return;
  s_dns.processNextRequest();
  s_web.handleClient();
}

bool wifi_provisioning_is_active(void) {
  return s_active;
}

void wifi_provisioning_stop(void) {
  if (!s_active) {
    // 允许在 handle_save() 里调用 stop()：那时 s_active 仍然为 true
    // 若外部重复调用，也安全
  }

  Serial.println("[prov] stopping provisioning mode...");

  // 关闭 Web/DNS
  s_web.stop();
  s_dns.stop();

  // 关闭 WiFi（不保留 AP/STA）
  WiFi.disconnect(true, true);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  // 清屏，回到仿真
  rgb_clear();
  rgb_show();
  sim_manager_start(SIM_MODE_WATER);

  s_active = false;
  s_started_ms = 0;
  s_last_error = "";
  s_last_ok = "";
}
