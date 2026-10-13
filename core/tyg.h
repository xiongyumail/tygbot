#pragma once
#include "core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_http_server.h"

#define TG_HTTP_PORT 80 /* Served by ESPHome's shared web server. */
#define TG_MAX_BODY 1024

extern tg_config tg_cfg;
extern SemaphoreHandle_t tg_state_lock;
extern SemaphoreHandle_t tg_action_lock;
extern bool tg_http_auth_required;
bool tg_password_hash(const char *password, const unsigned char *salt, unsigned char *out);
bool tg_storage_save(void);
bool tg_time_ready(void);
void tg_timestamp(char *out, size_t capacity, bool compact);
void tg_login_note(int state, bool relogin);
void tg_event(const char *kind, const cJSON *value);
cJSON *tg_snapshot(void);
void tg_set_busy(bool busy);
void tg_door_note(bool ok, const char *detail, double seconds, double reply_wait, const char *source);

void tg_protocol_init(void);
bool tg_refresh(bool force, char *detail);
bool tg_open(bool dry, char *detail, double *reply_wait);
bool tg_login_send(const char *phone, char **captcha, char *detail);
bool tg_login_captcha(const char *code, bool *sent, char **captcha, char *detail);
bool tg_login_sms(const char *code, bool *linked, char *detail);
cJSON *tg_households(char *detail);
cJSON *tg_doors(char *detail);
bool tg_web_start(void);
typedef enum { TG_ROUTE_NONE, TG_ROUTE_API, TG_ROUTE_SETUP, TG_ROUTE_DENY } tg_route;
/* Called on the shared server task for every request. */
tg_route tg_web_route(httpd_req_t *req);
esp_err_t tg_web_setup_page(httpd_req_t *req);
esp_err_t tg_web_deny(httpd_req_t *req);
/* Hands a request to the TYGBot web task. body is NUL-terminated or NULL. */
bool tg_web_submit(httpd_req_t *req, const char *body);
void tg_web_set_captive_portal(bool active, const char *ap_ip);
/* Queued on the same worker as HTTP; never performs networking on the caller. */
bool tg_submit_open(bool dry);
