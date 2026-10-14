#pragma once
#include "esphome/core/component.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include <string>

namespace esphome::tygbot {
// Serves the TYGBot web UI, HTTP API and Wi-Fi setup page on ESPHome's shared web server.
class WebHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;

 protected:
  std::string body_;
  bool body_too_large_{false};
  mutable int route_{0};  // Set by canHandle; requests are handled sequentially.
};

class TYGBot : public PollingComponent {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }
  // Called before component setup so the handler precedes the captive portal's.
  void register_web();
  bool open(bool dry = false);
  bool call_linked_open_enabled() const;
  bool set_call_linked_open(bool enabled);
  // Re-fetch the door-station call (SIP) config, e.g. after moving house.
  void refresh_call();
  bool ready() const { return ready_; }
  bool logged_in() const { return logged_in_; }
  bool busy() const { return busy_; }
  const std::string &last_result() const { return last_result_; }
  const std::string &status() const { return status_; }
  bool call_ringing() const { return call_ringing_; }
  bool call_registered() const { return call_registered_; }
  const std::string &caller() const { return caller_; }
  const std::string &caller_id() const { return caller_id_; }
  const std::string &call_status() const { return call_status_; }
  const std::string &household_name() const { return household_name_; }
  const std::string &door_name() const { return door_name_; }
  void set_password_verifier(const std::string &salt, const std::string &hash) { salt_ = salt; hash_ = hash; }
  void set_auth_required(bool value) { auth_required_ = value; }
  void set_host(const std::string &v) { host_ = v; }
 protected:
  WebHandler web_handler_;
  bool started_{false}, ready_{false}, logged_in_{false}, busy_{false}, auth_required_{true}, portal_active_{false};
  std::string salt_, hash_, host_;
  std::string status_{"等待网络和校时"}, last_result_{"尚未开门"};
  bool call_ringing_{false}, call_registered_{false}, sip_started_{false};
  std::string caller_, caller_id_, call_status_{"未启用"}, household_name_, door_name_;
};
}  // namespace esphome::tygbot
