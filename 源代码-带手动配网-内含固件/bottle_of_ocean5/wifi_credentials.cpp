#include "wifi_credentials.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static const char* kPrefsNs = "bocean";
static const char* kKeySsid = "ssid";
static const char* kKeyPass = "pass";

bool wifi_credentials_load(char* ssid_out, size_t ssid_out_len,
                           char* pass_out, size_t pass_out_len) {
  if (!ssid_out || ssid_out_len == 0 || !pass_out || pass_out_len == 0) {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, true)) {
    return false;
  }

  String ssid = prefs.getString(kKeySsid, "");
  String pass = prefs.getString(kKeyPass, "");
  prefs.end();

  if (ssid.length() == 0) {
    ssid_out[0] = '\0';
    pass_out[0] = '\0';
    return false;
  }

  strlcpy(ssid_out, ssid.c_str(), ssid_out_len);
  strlcpy(pass_out, pass.c_str(), pass_out_len);
  return true;
}

bool wifi_credentials_save(const char* ssid, const char* pass) {
  if (!ssid || ssid[0] == '\0') {
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return false;
  }

  bool ok = true;
  ok = ok && prefs.putString(kKeySsid, ssid) > 0;
  ok = ok && prefs.putString(kKeyPass, pass ? pass : "") > 0;
  prefs.end();
  return ok;
}

void wifi_credentials_clear(void) {
  Preferences prefs;
  if (!prefs.begin(kPrefsNs, false)) {
    return;
  }
  prefs.remove(kKeySsid);
  prefs.remove(kKeyPass);
  prefs.end();
}


