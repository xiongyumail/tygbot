// SPDX-License-Identifier: Apache-2.0
#include "tygbot.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "nvs.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <cstdio>
#include <ctime>
extern "C" {
#include "tyg.h"
// Stored in NVS as a raw blob; a different size is rejected at boot, so changing it needs a migration.
static_assert(sizeof(tg_config) == 936, "tg_config layout is persisted");
tg_config tg_cfg;
SemaphoreHandle_t tg_state_lock;
SemaphoreHandle_t tg_action_lock;
static nvs_handle_t store;
bool tg_storage_save(void) {
  return nvs_set_blob(store, "config", &tg_cfg, sizeof(tg_cfg)) == ESP_OK && nvs_commit(store) == ESP_OK;
}
bool tg_time_ready(void) { return time(nullptr) > 1700000000; }
void tg_timestamp(char *out, size_t cap, bool compact) {
  // Server requires Shanghai time, independent of HA / ESPHome's global TZ.
  time_t now = time(nullptr) + 8 * 3600;
  struct tm local; gmtime_r(&now, &local);
  strftime(out, cap, compact ? "%Y%m%d%H%M%S" : "%Y-%m-%d %H:%M:%S", &local);
}
}

namespace esphome::tygbot {
static const char *const TAG = "tygbot";
static bool unhex(const std::string &value, unsigned char *out, size_t size) {
  if (value.size() != size * 2) return false;
  for (size_t i = 0; i < size; i++) {
    unsigned n;
    if (sscanf(value.c_str() + i * 2, "%2x", &n) != 1) return false;
    out[i] = n;
  }
  return true;
}
bool WebHandler::canHandle(AsyncWebServerRequest *request) const {
  this->route_ = tg_web_route(*request);
  return this->route_ != TG_ROUTE_NONE;
}
void WebHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  // Requests are dispatched sequentially on the server task.
  if (index == 0) { this->body_.clear(); this->body_too_large_ = total > TG_MAX_BODY; }
  if (!this->body_too_large_) this->body_.append(reinterpret_cast<const char *>(data), len);
}
void WebHandler::handleRequest(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  if (this->route_ == TG_ROUTE_SETUP) tg_web_setup_page(req);
  else if (this->route_ == TG_ROUTE_DENY) tg_web_deny(req);
  // Oversized bodies are rejected by tg_web_handle from Content-Length.
  else tg_web_submit(req, req->content_len > 0 && !this->body_too_large_ ? this->body_.c_str() : nullptr);
  this->body_.clear(); this->body_too_large_ = false;
}
void TYGBot::register_web() {
  tg_state_lock = xSemaphoreCreateMutex(); tg_action_lock = xSemaphoreCreateMutex();
  if (tg_state_lock) web_server_base::global_web_server_base->add_handler_without_auth(&web_handler_);
}
void TYGBot::setup() {
  if (!tg_state_lock || !tg_action_lock || nvs_open("tygc3", NVS_READWRITE, &store) != ESP_OK) {
    status_ = "存储初始化失败，OTA仍可用"; mark_failed(); return;
  }
  size_t length = sizeof(tg_cfg);
  esp_err_t err = nvs_get_blob(store, "config", &tg_cfg, &length);
  if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || (err == ESP_OK && length != sizeof(tg_cfg))) {
    status_ = "存储格式不匹配，保留数据"; mark_failed(); return;
  }
  // ESPHome alone owns Wi-Fi; do not copy secrets or initialise its stack.
  if (!tg_copy(tg_cfg.host, sizeof(tg_cfg.host), host_.c_str()) ||
      !tg_copy(tg_cfg.organization, sizeof(tg_cfg.organization), organization_.c_str()) ||
      !tg_copy(tg_cfg.household, sizeof(tg_cfg.household), household_.c_str()) ||
      !tg_copy(tg_cfg.door_code, sizeof(tg_cfg.door_code), door_code_.c_str()) ||
      !tg_copy(tg_cfg.door_number, sizeof(tg_cfg.door_number), door_number_.c_str()) ||
      (auth_required_ && (!unhex(salt_, tg_cfg.salt, sizeof(tg_cfg.salt)) || !unhex(hash_, tg_cfg.password_hash, sizeof(tg_cfg.password_hash))))) {
    status_ = "配置无效"; mark_failed(); return;
  }
  tg_cfg.password_set = auth_required_; tg_http_auth_required = auth_required_;
  if (!auth_required_) {
    memset(tg_cfg.salt, 0, sizeof(tg_cfg.salt)); memset(tg_cfg.password_hash, 0, sizeof(tg_cfg.password_hash));
  }
  salt_.clear(); hash_.clear();
  // RF is active when loop starts: only then generate cookie/UUID entropy.
}
void TYGBot::loop() {
  if (is_failed()) return;
#ifdef USE_CAPTIVE_PORTAL
  bool active = captive_portal::global_captive_portal && captive_portal::global_captive_portal->is_active();
  if (active != portal_active_) {
    portal_active_ = active;
    char ap_ip[network::IP_ADDRESS_BUFFER_SIZE] = {};
    if (active) wifi::global_wifi_component->wifi_soft_ap_ip().str_to(ap_ip);
    tg_web_set_captive_portal(active, ap_ip);
  }
#endif
  if (started_ || (!network::is_connected() && !portal_active_)) return;
  started_ = true;
  esp_fill_random(tg_cfg.cookie_key, sizeof(tg_cfg.cookie_key));
  if (!*tg_cfg.device_id) {
    unsigned char id[16]; esp_fill_random(id, sizeof(id));
    id[6] = (id[6] & 15) | 0x40; id[8] = (id[8] & 63) | 0x80;
    snprintf(tg_cfg.device_id, sizeof(tg_cfg.device_id),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      id[0],id[1],id[2],id[3],id[4],id[5],id[6],id[7],id[8],id[9],id[10],id[11],id[12],id[13],id[14],id[15]);
  }
  if (!tg_storage_save()) { status_ = "会话存储失败，OTA仍可用"; mark_failed(); return; }
  tg_protocol_init();
  if (!tg_web_start()) { status_ = "服务内存不足，OTA仍可用"; mark_failed(); return; }
  // Hold a reference so the listener stays up after the captive portal ends.
  web_server_base::global_web_server_base->init();
  ready_ = true;
  ESP_LOGI(TAG, "Web service ready on port %d", TG_HTTP_PORT);
}
void TYGBot::update() {
  if (!ready_) return;
  cJSON *snapshot = tg_snapshot();
  if (!snapshot) return;
  busy_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(snapshot, "busy"));
  cJSON *login = cJSON_GetObjectItemCaseSensitive(snapshot, "login");
  logged_in_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(login, "ok"));
  cJSON *last = cJSON_GetObjectItemCaseSensitive(snapshot, "last_door");
  if (cJSON_IsObject(last)) {
    last_result_ = tg_string(last, "detail");
    last_result_ += " · "; last_result_ += tg_string(last, "at");
  }
  cJSON *warm = cJSON_GetObjectItemCaseSensitive(snapshot, "last_warm_up");
  status_ = !tg_time_ready() ? "等待网络校时" : busy_ ? "正在请求开门" :
      cJSON_IsObject(warm) ? tg_string(warm, "detail") : logged_in_ ? "已登录" : "请在设备网页短信登录";
  cJSON_Delete(snapshot);
}
bool TYGBot::open(bool dry) {
  if (!ready_ || !network::is_connected() || !tg_time_ready()) { status_ = "设备尚未就绪，未发送开门请求"; return false; }
  if (!tg_submit_open(dry)) { status_ = "服务忙，未发送请求"; return false; }
  return true;
}
void TYGBot::dump_config() {
  ESP_LOGCONFIG(TAG, "TYGBot: local web/API :%d, native ESPHome API and OTA", TG_HTTP_PORT);
}
}  // namespace esphome::tygbot
