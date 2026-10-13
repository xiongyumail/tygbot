/* SPDX-License-Identifier: Apache-2.0 */
#include "tyg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <ctype.h>
#include "freertos/event_groups.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"

#define WS_UP BIT0
#define WS_DONE BIT1
#define WS_FAIL BIT2
static esp_websocket_client_handle_t ws;
static EventGroupHandle_t events;
static SemaphoreHandle_t rx_lock;
static char *rx;
static size_t rx_len, rx_cap, frame_len;
static bool waiting;
static char expected[32];
static tg_room room;
static tg_door doors[TG_MAX_DOORS];
static size_t door_count;
static int64_t refreshed, discovered;
static char uums[256], cps[256], vdcs[256];
static char pending_phone[12];
static int pending_stage;
static int64_t pending_expiry;

static void error(char *detail, const char *message) { tg_copy(detail, TG_DETAIL, message); }
static void ws_close(void) {
    if (ws) { esp_websocket_client_stop(ws); esp_websocket_client_destroy(ws); ws = NULL; }
    xEventGroupClearBits(events, WS_UP | WS_DONE | WS_FAIL);
}
static void invalidate(void) { memset(&room, 0, sizeof(room)); door_count = 0; refreshed = 0; ws_close(); }
/* Keep the stored session: the next check re-validates it, so a misread reply costs no SMS login. */
static void session_rejected(void) { invalidate(); tg_login_note(0, false); }
/* The worker owns protocol operations. Callback never stops/destroys its own client. */
static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WEBSOCKET_EVENT_CONNECTED) { xEventGroupSetBits(events, WS_UP); return; }
    if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_CLOSED) {
        xEventGroupClearBits(events, WS_UP); xEventGroupSetBits(events, WS_FAIL); return;
    }
    if (id != WEBSOCKET_EVENT_DATA) return;
    esp_websocket_event_data_t *e = data;
    if (e->op_code != 1 && e->op_code != 0) return;
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    if (!waiting || !rx) { xSemaphoreGive(rx_lock); return; }
    if (!e->payload_offset) {
        frame_len = 0;
        if (e->op_code == 1) rx_len = 0;
    }
    if (e->data_len < 0 || e->payload_len < 0 || e->payload_offset != (int)frame_len || rx_len + e->data_len > TG_MAX_JSON) {
        waiting = false; xSemaphoreGive(rx_lock); xEventGroupSetBits(events, WS_FAIL); return;
    }
    size_t need = rx_len + e->data_len + 1;
    if (need > rx_cap) {
        size_t capacity = rx_cap;
        while (capacity < need && capacity < TG_MAX_JSON + 1) capacity = capacity > (TG_MAX_JSON + 1)/2 ? TG_MAX_JSON + 1 : capacity * 2;
        char *grown = realloc(rx, capacity);
        if (!grown) { waiting = false; xSemaphoreGive(rx_lock); xEventGroupSetBits(events, WS_FAIL); return; }
        rx = grown; rx_cap = capacity;
    }
    memcpy(rx + rx_len, e->data_ptr, e->data_len); rx_len += e->data_len; frame_len += e->data_len;
    rx[rx_len] = 0;
    if (e->fin && frame_len == (size_t)e->payload_len) {
        cJSON *value = tg_parse_json(rx);
        bool match = value && !strcmp(tg_string(value, "msgType"), expected);
        cJSON_Delete(value);
        if (match) { waiting = false; xEventGroupSetBits(events, WS_DONE); }
        else rx_len = frame_len = 0;
    }
    xSemaphoreGive(rx_lock);
}
void tg_protocol_init(void) {
    events = xEventGroupCreate(); rx_lock = xSemaphoreCreateMutex();
    /* Neither upstream library may log request URLs containing token or account data. */
    esp_log_level_set("websocket_client", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_NONE);
    esp_log_level_set("TRANSPORT_WS", ESP_LOG_NONE);
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
}
static char *http_request(const char *url, const char *payload, bool image, char *detail, size_t *length) {
    if (!tg_time_ready()) { error(detail, "等待网络校时，尚未发送请求"); return NULL; }
    if (!tg_url_allowed(url, image, false)) { error(detail, "拒绝不安全或非官方上游地址"); return NULL; }
    /* C3: release WSS TLS resources before allocating an HTTPS connection. */
    ws_close();
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true, .buffer_size = 1024, .buffer_size_tx = 1024, .keep_alive_enable = false};
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) { error(detail, "HTTPS 内存不足"); return NULL; }
    if (payload) { esp_http_client_set_method(h, HTTP_METHOD_POST); esp_http_client_set_header(h, "Content-Type", "application/json"); }
    size_t send_len = payload ? strlen(payload) : 0;
    esp_err_t rc = esp_http_client_open(h, send_len);
    if (rc == ESP_OK && payload) {
        int n = esp_http_client_write(h, payload, send_len);
        if (n != (int)send_len) rc = ESP_FAIL;
    }
    if (rc == ESP_OK && esp_http_client_fetch_headers(h) < 0) rc = ESP_FAIL;
    int status = esp_http_client_get_status_code(h);
    char *body = NULL;
    if (rc == ESP_OK && status == 200) {
        int64_t declared = esp_http_client_get_content_length(h);
        size_t capacity = declared > 0 && declared < TG_MAX_JSON ? declared + 1 : 2048;
        if (declared >= TG_MAX_JSON) { esp_http_client_close(h); esp_http_client_cleanup(h); error(detail, "上游响应过大"); return NULL; }
        body = malloc(capacity);
        if (body) {
            size_t used = 0;
            while (used < TG_MAX_JSON) {
                if (used + 1 == capacity) {
                    if (declared > 0 && used == (size_t)declared && esp_http_client_is_complete_data_received(h)) break;
                    size_t next = capacity > (TG_MAX_JSON + 1)/2 ? TG_MAX_JSON + 1 : capacity * 2;
                    char *grown = realloc(body, next);
                    if (!grown) { rc = ESP_ERR_NO_MEM; break; }
                    body = grown; capacity = next;
                }
                int n = esp_http_client_read(h, body + used, capacity - used - 1);
                if (n < 0) { rc = ESP_FAIL; break; }
                if (!n) break;
                used += n;
            }
            if (used == TG_MAX_JSON || !esp_http_client_is_complete_data_received(h)) rc = ESP_FAIL;
            body[used] = 0; if (length) *length = used;
        } else rc = ESP_ERR_NO_MEM;
    } else rc = ESP_FAIL;
    esp_http_client_close(h); esp_http_client_cleanup(h);
    if (rc != ESP_OK) { free(body); error(detail, "HTTPS 请求失败、响应过大或服务器拒绝；未重试"); return NULL; }
    return body;
}
static char *make_request(cJSON *body, const char *token, const char *kind) {
    cJSON *root = cJSON_CreateObject(), *header = cJSON_CreateObject();
    if (!root || !header || !body) { cJSON_Delete(root); cJSON_Delete(header); cJSON_Delete(body); return NULL; }
    char stamp[24], anonymous[33]; tg_timestamp(stamp, sizeof(stamp), true);
    if (!token) {
        struct timeval now; gettimeofday(&now, NULL);
        char seed[80]; snprintf(seed, sizeof(seed), "EA3vWPmfxhWUit2s%lld", (long long)now.tv_sec * 1000 + now.tv_usec / 1000);
        unsigned char hash[16]; mbedtls_md5((const unsigned char *)seed, strlen(seed), hash);
        for (int i = 0; i < 16; i++) snprintf(anonymous + i * 2, 3, "%02X", hash[i]);
        token = anonymous;
    }
    cJSON_AddStringToObject(header, "time_stamp", stamp); cJSON_AddStringToObject(header, "token", token);
    if (kind) cJSON_AddStringToObject(header, "msgType", kind);
    cJSON_AddItemToObject(root, "header", header); cJSON_AddItemToObject(root, "body", body);
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root); return text;
}
static cJSON *post(const char *base, const char *path, cJSON *body, const char *token, char *detail) {
    char url[512];
    const char *start = strstr(base, "://"), *end = start ? strchr(start + 3, '/') : NULL;
    size_t len = end ? (size_t)(end - base) : strlen(base);
    if (len + strlen(path) >= sizeof(url)) { cJSON_Delete(body); error(detail, "上游地址过长"); return NULL; }
    memcpy(url, base, len); strcpy(url + len, path);
    char *payload = make_request(body, token, NULL);
    if (!payload) { error(detail, "请求内存不足"); return NULL; }
    char *raw = http_request(url, payload, false, detail, NULL); free(payload);
    if (!raw) return NULL;
    cJSON *value = tg_parse_json(raw); free(raw);
    if (!cJSON_IsObject(value)) { cJSON_Delete(value); error(detail, "上游响应不是 JSON 对象"); return NULL; }
    if (token) {
        const char *reason = tg_string(value, "reason"), *message = tg_string(value, "message");
        if (strstr(reason, "未登录") || strstr(reason, "重新登录") || strstr(reason, "token失效") || strstr(reason, "登录失效")
            || strstr(message, "未登录") || strstr(message, "重新登录") || strstr(message, "token失效") || strstr(message, "登录失效")) {
            cJSON_Delete(value); session_rejected(); error(detail, TG_LOGGED_OUT); return NULL;
        }
    }
    return value;
}
static bool discover(char *detail) {
    if (*uums && esp_timer_get_time() - discovered < 3600000000LL) return true;
    cJSON *v = post("https://uums.taoyuangu.com:8443", "/uums/servlet/getAllConfigsServlet", cJSON_CreateObject(), NULL, detail);
    bool ok = tg_uums_ok(v, NULL);
    char next_uums[256] = "", next_cps[256] = "", next_vdcs[256] = "";
    cJSON *configs = cJSON_GetObjectItemCaseSensitive(v, "serverConfigs"), *item;
    if (!cJSON_IsArray(configs)) ok = false;
    cJSON_ArrayForEach(item, configs) {
        const char *key = tg_string(item, "key"), *url = tg_string(item, "value");
        char *target = !strcmp(key, "uums_server") ? next_uums : !strcmp(key, "cpsApi_server") ? next_cps : !strcmp(key, "htmlvdcs_server") ? next_vdcs : NULL;
        if (target && (!tg_url_allowed(url, false, false) || !tg_copy(target, 256, url))) ok = false;
    }
    cJSON_Delete(v);
    if (!ok || !*next_uums || !*next_cps || !*next_vdcs) { error(detail, "官方服务发现失败"); return false; }
    strcpy(uums, next_uums); strcpy(cps, next_cps); strcpy(vdcs, next_vdcs); discovered = esp_timer_get_time(); return true;
}
static cJSON *query_households(char *detail) {
    if (!*tg_cfg.user_token) { error(detail, TG_LOGGED_OUT); tg_login_note(0, false); return NULL; }
    if (!discover(detail)) return NULL;
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "type", "1"); cJSON_AddStringToObject(body, "openCode", "");
    cJSON *v = post(cps, "/cpsApi/servlet/queryOpenHouseholdList/v2", body, tg_cfg.user_token, detail);
    if (!v) return NULL;
    if (!tg_cps_ok(v)) { error(detail, "授权住房查询被服务器拒绝"); cJSON_Delete(v); return NULL; }
    return v;
}
static cJSON *exchange(const char *kind, cJSON *body, char *detail, double *reply_wait) {
    if (!ws || !esp_websocket_client_is_connected(ws)) { cJSON_Delete(body); error(detail, "门禁连接未就绪"); return NULL; }
    char *payload = make_request(body, room.token, kind);
    if (!payload) { error(detail, "门禁请求内存不足"); return NULL; }
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    rx_cap = 512; rx = calloc(1, rx_cap); rx_len = frame_len = 0; waiting = rx != NULL; tg_copy(expected, sizeof(expected), kind);
    xEventGroupClearBits(events, WS_DONE | WS_FAIL); xSemaphoreGive(rx_lock);
    if (!rx) { free(payload); error(detail, "门禁接收内存不足"); return NULL; }
    int64_t t0 = esp_timer_get_time();
    int sent = esp_websocket_client_send_text(ws, payload, strlen(payload), pdMS_TO_TICKS(5000));
    bool send_ok = sent == (int)strlen(payload); free(payload);
    EventBits_t bits = send_ok ? xEventGroupWaitBits(events, WS_DONE | WS_FAIL, false, false, pdMS_TO_TICKS(15000)) : 0;
    if (reply_wait) *reply_wait = (esp_timer_get_time() - t0) / 1000000.0;
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    waiting = false;
    cJSON *v = (bits & WS_DONE) ? tg_parse_json(rx) : NULL;
    free(rx); rx = NULL; xSemaphoreGive(rx_lock);
    if (!v) {
        invalidate();
        error(detail, !strcmp(kind, "OpenDoor") ? "开门结果未知（未收到确认），没有自动重发" : "门禁配置查询失败，未重试");
    }
    return v;
}
bool tg_refresh(bool force, char *detail) {
    if (!force && door_count && esp_timer_get_time() - refreshed < 240000000LL && ws && esp_websocket_client_is_connected(ws)) return true;
    invalidate();
    cJSON *v = query_households(detail);
    if (!v) return false;
    bool ok = tg_select_room(v, &tg_cfg, &room, detail); cJSON_Delete(v);
    if (!ok) return false;
    /* Returned door token is hex-like; refuse reserved characters rather than putting them in a URL. */
    for (const char *p = room.token; *p; p++) if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') { error(detail, "门禁 token 格式错误"); invalidate(); return false; }
    char url[768]; size_t n = strlen(vdcs);
    snprintf(url, sizeof(url), "wss://%.*s%swsserver/websocket?token=%s", (int)(n - 8), vdcs + 8, n && vdcs[n-1] == '/' ? "" : "/", room.token);
    if (!tg_url_allowed(url, false, true)) { error(detail, "门禁地址验证失败"); invalidate(); return false; }
    esp_websocket_client_config_t cfg = {.uri = url, .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_reconnect = true,
        .buffer_size = 1024, .task_stack = 4096, .network_timeout_ms = 15000, .ping_interval_sec = 15};
    ws = esp_websocket_client_init(&cfg);
    if (!ws) { error(detail, "WSS 内存不足"); invalidate(); return false; }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(ws) != ESP_OK || !(xEventGroupWaitBits(events, WS_UP | WS_FAIL, false, false, pdMS_TO_TICKS(16000)) & WS_UP)) {
        error(detail, "门禁 WSS 连接失败"); invalidate(); return false;
    }
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "mobile", room.mobile); cJSON_AddStringToObject(body, "organizationSeq", room.organization);
    v = exchange("MobileDownloadConfig", body, detail, NULL);
    ok = v && tg_parse_doors(v, doors, &door_count, detail); cJSON_Delete(v);
    if (!ok) { invalidate(); return false; }
    refreshed = esp_timer_get_time(); tg_login_note(1, false); error(detail, "登录和门禁连接正常"); return true;
}
bool tg_open(bool dry, char *detail, double *reply_wait) {
    if (!tg_refresh(dry, detail)) return false;
    size_t selected = 0;
    if (!tg_select_door(&tg_cfg, doors, door_count, &selected)) { error(detail, "门禁不唯一，请配置 door_code/door_number"); return false; }
    if (dry) { error(detail, "登录、授权和门禁连接已验证；没有发送开门指令"); return true; }
    const tg_door *d = &doors[selected];
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "clientType", "2"); cJSON_AddStringToObject(body, "code", d->code);
    cJSON_AddStringToObject(body, "number", d->number); cJSON_AddStringToObject(body, "mobile", room.mobile);
    cJSON_AddStringToObject(body, "householdSerial", room.household);
    if (!body || cJSON_GetArraySize(body) != 5) { cJSON_Delete(body); error(detail, "开门请求内存不足，未发送"); return false; }
    cJSON *reply = exchange("OpenDoor", body, detail, reply_wait);
    if (!reply) return false;
    bool ok = tg_open_ok(reply); cJSON_Delete(reply);
    error(detail, ok ? "开门成功（服务器已确认）" : "服务器未确认开门成功；没有自动重发"); return ok;
}
static bool captcha_fetch(const char *phone, char **out, char *detail) {
    if (!discover(detail)) return false;
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "phone", phone);
    cJSON *v = post(uums, "/uums/servlet/getPicRandomServlet", body, NULL, detail);
    char url[768]; bool ok = tg_uums_ok(v, NULL) && tg_copy(url, sizeof(url), tg_string(v, "picRandomUrl")); cJSON_Delete(v);
    if (!ok) { error(detail, "验证码获取被服务器拒绝"); return false; }
    size_t length = 0;
    char *bytes = http_request(url, NULL, true, detail, &length);
    if (!bytes) return false;
    /* No decoder or Pillow on MCU; browser displays original PNG/JPEG directly. */
    const char *type = length >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) ? "image/png" : length >= 3 && (unsigned char)bytes[0] == 0xff && (unsigned char)bytes[1] == 0xd8 ? "image/jpeg" : NULL;
    if (!type || length > 10000) { free(bytes); error(detail, "验证码图片无效或过大"); return false; }
    size_t capacity = 32 + 4 * ((length + 2) / 3) + 1; char *image = malloc(capacity);
    if (!image) { free(bytes); error(detail, "验证码内存不足"); return false; }
    int prefix = snprintf(image, capacity, "data:%s;base64,", type); size_t encoded = 0;
    int rc = mbedtls_base64_encode((unsigned char *)image + prefix, capacity - prefix, &encoded, (unsigned char *)bytes, length);
    free(bytes); if (rc) { free(image); error(detail, "验证码编码失败"); return false; }
    image[prefix + encoded] = 0; *out = image; return true;
}
bool tg_login_send(const char *phone, char **captcha, char *detail) {
    pending_stage = 0;
    if (!tg_phone_valid(phone)) { error(detail, "请输入 11 位手机号"); return false; }
    if (!captcha_fetch(phone, captcha, detail)) return false;
    strcpy(pending_phone, phone); pending_stage = 1; pending_expiry = esp_timer_get_time() + 600000000LL; return true;
}
bool tg_login_captcha(const char *code, bool *sent, char **captcha, char *detail) {
    *sent = false;
    if (pending_stage != 1 || esp_timer_get_time() > pending_expiry) { error(detail, "登录步骤已过期，请重新开始"); return false; }
    if (!tg_code_valid(code, true)) { error(detail, "验证码格式错误"); return false; }
    if (!*code) return captcha_fetch(pending_phone, captcha, detail);
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "phone", pending_phone);
    cJSON_AddStringToObject(body, "picRandom", code); cJSON_AddStringToObject(body, "sourceClientCode", "tyg_u");
    cJSON *v = post(uums, "/uums/servlet/getRandomCodeForSmsLogin", body, NULL, detail);
    if (!v) return false;
    bool ok = tg_uums_ok(v, "1"), wrong = tg_uums_ok(v, "2");
    if (!ok && !wrong) error(detail, tg_is_code(v, "code", "3") ? "当日短信请求已超过 5 次" : tg_is_code(v, "code", "-1") ? "请求过多，请 12 小时后重试" : "服务器没有发送短信");
    cJSON_Delete(v);
    if (ok) { *sent = true; pending_stage = 2; pending_expiry = esp_timer_get_time() + 600000000LL; return true; }
    if (wrong) return captcha_fetch(pending_phone, captcha, detail);
    return false;
}
bool tg_login_sms(const char *code, bool *linked, char *detail) {
    *linked = false;
    if (pending_stage != 2 || esp_timer_get_time() > pending_expiry) { error(detail, "登录步骤已过期，请重新开始"); return false; }
    if (!tg_code_valid(code, false)) { error(detail, "短信验证码格式错误"); return false; }
    cJSON *body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "mobile", pending_phone); cJSON_AddStringToObject(body, "smsVerifyCode", code);
    cJSON_AddStringToObject(body, "clientCode", "tyg_u"); cJSON_AddStringToObject(body, "lastLoginOs", "4");
    cJSON_AddStringToObject(body, "changeDeviceValidate", "1"); cJSON_AddStringToObject(body, "lastLoginDeviceToken", "");
    cJSON_AddStringToObject(body, "deviceUniqueId", tg_cfg.device_id);
    cJSON *v = post(uums, "/uums/servlet/smsUserLogin", body, NULL, detail);
    if (!v) return false;
    bool ok = tg_uums_ok(v, "0") && *tg_string(v, "token") && tg_copy(tg_cfg.user_token, sizeof(tg_cfg.user_token), tg_string(v, "token"));
    bool wrong = tg_uums_ok(v, "2"); cJSON_Delete(v);
    if (!ok) { error(detail, wrong ? "短信验证码错误" : "短信登录被服务器拒绝"); return false; }
    if (!tg_storage_save()) { error(detail, "登录会话存储失败"); return false; }
    pending_stage = 0; invalidate(); tg_login_note(1, true);
    *linked = tg_refresh(true, detail); return true;
}
cJSON *tg_households(char *detail) {
    invalidate(); cJSON *v = query_households(detail);
    if (!v) return NULL;
    cJSON *result = cJSON_CreateArray(), *area, *r;
    cJSON *areas = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(v, "data"), "areaList");
    if (!cJSON_IsArray(areas) || !result) { cJSON_Delete(v); cJSON_Delete(result); error(detail, "住房格式错误或内存不足"); return NULL; }
    cJSON_ArrayForEach(area, areas) {
        cJSON *rooms = cJSON_GetObjectItemCaseSensitive(area, "list");
        if (!cJSON_IsArray(rooms)) continue;
        cJSON_ArrayForEach(r, rooms) if (tg_is_code(r, "valid", "1")) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "organization", tg_string(area, "organizationSeq"));
            cJSON_AddStringToObject(item, "household", tg_string(r, "householdSerial"));
            cJSON_AddStringToObject(item, "area", tg_string(area, "areaName")); cJSON_AddItemToArray(result, item);
        }
    }
    cJSON_Delete(v); tg_login_note(1, false); return result;
}
cJSON *tg_doors(char *detail) {
    if (!tg_refresh(true, detail)) return NULL;
    cJSON *result = cJSON_CreateArray();
    for (size_t i = 0; i < door_count; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "code", doors[i].code); cJSON_AddStringToObject(d, "number", doors[i].number);
        cJSON_AddStringToObject(d, "name", doors[i].name); cJSON_AddStringToObject(d, "id", doors[i].id); cJSON_AddStringToObject(d, "group", doors[i].group);
        cJSON_AddItemToArray(result, d);
    }
    return result;
}
