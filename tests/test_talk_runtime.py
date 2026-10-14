"""运行真实 talk.c，模拟 ESP HTTP 响应；不访问网络、不操作门禁。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SHIM = r'''
#pragma once
#include "core.h"
#include <stdint.h>
typedef int esp_err_t;
typedef void *SemaphoreHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 257
#define portMAX_DELAY 0
#define MALLOC_CAP_8BIT 1
#define HTTP_EVENT_ON_HEADER 1
#define HTTP_METHOD_POST 2
typedef struct { int event_id; char *header_key, *header_value; void *user_data; } esp_http_client_event_t;
typedef struct {
 const char *url; int timeout_ms, buffer_size, buffer_size_tx;
 int (*crt_bundle_attach)(void *); bool disable_auto_redirect, keep_alive_enable;
 esp_err_t (*event_handler)(esp_http_client_event_t *); void *user_data;
} esp_http_client_config_t;
typedef struct mock_http *esp_http_client_handle_t;
extern SemaphoreHandle_t tg_state_lock, tg_action_lock;
extern tg_talk_auth tg_talk;
int xSemaphoreTake(SemaphoreHandle_t, int);
int xSemaphoreGive(SemaphoreHandle_t);
void tg_sip_kick(void);
void tg_talk_endpoint(const char *, int);
bool tg_talk_fetch(tg_sip_config *, char *, int *);
bool tg_time_ready(void);
size_t heap_caps_get_free_size(int);
size_t heap_caps_get_largest_free_block(int);
int esp_crt_bundle_attach(void *);
const char *esp_err_to_name(esp_err_t);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
int esp_http_client_set_method(esp_http_client_handle_t, int);
int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
int esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
int esp_http_client_close(esp_http_client_handle_t);
int esp_http_client_cleanup(esp_http_client_handle_t);
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int *, int *);
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t);
'''

HARNESS = r'''
#include "tyg.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
SemaphoreHandle_t tg_state_lock, tg_action_lock = (void *)2;
static bool door_busy, action_held;
tg_talk_auth tg_talk = {"13800000000", "e10adc3949ba59abbe56e057f20f883e"};
struct mock_http { esp_http_client_config_t cfg; int step, status; bool connected; size_t offset; const char *body; char cookie[256]; } client;
static int inits, requests, connects, cleaned, scenario, drops, kicks;
static size_t heap = 200000, largest = 200000;
static char expanded_xml[5000];
static bool clock_ready = true;
int xSemaphoreTake(SemaphoreHandle_t m, int t) {
 if (m != tg_action_lock) return 1;
 assert(t == 0 && !action_held); if (door_busy) return 0; action_held = true; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t m) { if (m == tg_action_lock) { assert(action_held); action_held = false; } return 1; }
void tg_sip_kick(void) { kicks++; }
bool tg_time_ready(void) { return clock_ready; }
size_t heap_caps_get_free_size(int c) { (void)c; return heap; }
size_t heap_caps_get_largest_free_block(int c) { (void)c; return largest < heap ? largest : heap; }
int esp_crt_bundle_attach(void *p) { (void)p; return 0; }
const char *esp_err_to_name(esp_err_t e) { (void)e; return "ESP_FAIL"; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
 memset(&client, 0, sizeof(client)); client.cfg = *cfg; client.step = -1; inits++; heap -= 2048;
 assert(cfg->event_handler && cfg->user_data); return &client;
}
int esp_http_client_set_method(esp_http_client_handle_t h, int m) { (void)h; assert(m == HTTP_METHOD_POST); return 0; }
int esp_http_client_set_header(esp_http_client_handle_t h, const char *k, const char *v) {
 if (!strcmp(k, "Cookie")) snprintf(h->cookie, sizeof(h->cookie), "%s", v); return 0;
}
int esp_http_client_open(esp_http_client_handle_t h, int n) {
 (void)n;
 if ((scenario == 3 && requests == 1) || scenario == 11) return ESP_FAIL;
 /* 服务器已关闭复用的连接：请求未送出。 */
 if (scenario == 13 && requests == 2 && h->connected && !drops) { drops++; return ESP_FAIL; }
 /* 只有新建 TLS 时占用操作锁，复用连接不占用。 */
 assert(action_held == !h->connected);
 if (!h->connected) { h->connected = true; connects++; heap -= 32768; }
 h->step = requests++; h->status = 200; h->offset = 0; h->body = NULL; return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t h, const char *b, int n) {
 const char *types[] = {"AuthcodeRequest", "AuthenticationRequest", "ConfigInformRequest", "MobileDownloadConfigRequest"};
 assert(h->step < 4 && strstr(b, types[h->step]));
 if (h->step == 0) assert(!*h->cookie);
 else if (h->step == 1) assert(!strcmp(h->cookie, "JSESSIONID=first; route=node"));
 else assert(!strcmp(h->cookie, "JSESSIONID=second; route=node"));
 return n;
}
static void cookie(esp_http_client_handle_t h, char *value) {
 esp_http_client_event_t ev = {.event_id = HTTP_EVENT_ON_HEADER, .header_key = "set-cookie", .header_value = value, .user_data = h->cfg.user_data};
 assert(h->cfg.event_handler(&ev) == ESP_OK);
}
int esp_http_client_fetch_headers(esp_http_client_handle_t h) {
 if (h->step == 0) {
  if (scenario != 4) { cookie(h, "JSESSIONID=first; Path=/; HttpOnly"); cookie(h, "route=node; Path=/"); }
  if (scenario == 8) cookie(h, "bad\r\n=value; Path=/");
  h->body = scenario == 5 ? "<message><faultCode>1</faultCode></message>" : "<message><safeCode>challenge</safeCode></message>";
 } else if (h->step == 1) {
  cookie(h, "JSESSIONID=second; Path=/");
  h->body = scenario == 2 ? "<message><faultCode>3</faultCode></message>" : "<message><faultCode>0</faultCode></message>";
  if (scenario == 6) h->status = 503;
 } else if (h->step == 2) h->body = "<message><faultCode>0</faultCode></message>";
 else h->body = "<message><SIP_ACCOUNT>8001</SIP_ACCOUNT><SIP_PASSWORD>test-pass</SIP_PASSWORD><SIP_DOMAIN>192.0.2.7</SIP_DOMAIN><SIP_PROXY_PORT>6000</SIP_PROXY_PORT></message>";
 if (scenario == 9 && h->step == 3) {
  const char *config = h->body; size_t len = strlen(config) - strlen("</message>");
  memcpy(expanded_xml, config, len); memcpy(expanded_xml + len, "<padding>", 9); len += 9;
  memset(expanded_xml + len, 'x', 3000); len += 3000;
  strcpy(expanded_xml + len, "</padding></message>"); h->body = expanded_xml;
 }
 return (int)strlen(h->body);
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { return h->body && h->offset == strlen(h->body); }
int esp_http_client_read(esp_http_client_handle_t h, char *out, int cap) {
 if (scenario == 7 && h->step == 0) return 0;
 size_t n = strlen(h->body) - h->offset; if (n > 13) n = 13; if (n > (size_t)cap) n = cap;
 memcpy(out, h->body + h->offset, n); h->offset += n; return (int)n;
}
int esp_http_client_close(esp_http_client_handle_t h) { if (h->connected) { heap += 32768; h->connected = false; } return 0; }
int esp_http_client_cleanup(esp_http_client_handle_t h) { assert(!h->connected); heap += 2048; cleaned++; return 0; }
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t h) { (void)h; return scenario != 12; }
int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h, int *error, int *flags) {
 (void)h; *error = scenario == 11 ? -32512 : 0; *flags = 0; return 0;
}
static void reset(int s) { scenario = s; inits = requests = connects = cleaned = drops = 0; }
int main(void) {
 char detail[TG_DETAIL]; tg_sip_config cfg; int retry;
 tg_talk_endpoint("https://lxj.taoyuangu.com:8443/vdcs_hori/terminalHandle", 6000); assert(kicks == 1);
 tg_talk_endpoint("https://lxj.taoyuangu.com:8443/vdcs_hori/terminalHandle", 6000); assert(kicks == 1);
 /* 四步请求共用一个 HTTP 对象和一条 TLS 连接。 */
 reset(0); assert(tg_talk_fetch(&cfg, detail, &retry)); assert(inits == 1 && requests == 4 && connects == 1 && cleaned == 1); assert(!strcmp(cfg.account, "8001"));
 reset(2); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "账号认证失败")); assert(requests == 2 && cleaned == 1 && retry == 300); assert(!*cfg.password);
 reset(3); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "账号认证失败") && strstr(detail, "ESP_FAIL")); assert(cleaned == 1 && connects == 1);
 reset(4); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "Cookie")); assert(requests == 1 && cleaned == 1);
 reset(5); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "安全码")); assert(requests == 1);
 reset(6); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "HTTP 503")); assert(cleaned == 1);
 reset(7); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "网络请求失败")); assert(cleaned == 1);
 reset(8); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "Cookie 无效")); assert(cleaned == 1);
 reset(12); assert(tg_talk_fetch(&cfg, detail, &retry)); assert(requests == 4 && connects == 4 && cleaned == 1);
 reset(13); assert(tg_talk_fetch(&cfg, detail, &retry)); assert(requests == 4 && connects == 2 && drops == 1);
 /* 真机快照：61096 B 可用、49152 B 最大块。HTTP 对象和模拟 TLS 还会占用内存。 */
 heap = 61096; largest = 49152; reset(0); assert(tg_talk_fetch(&cfg, detail, &retry)); assert(requests == 4 && cleaned == 1 && heap == 61096);
 reset(9); assert(tg_talk_fetch(&cfg, detail, &retry)); assert(requests == 4 && cleaned == 1 && heap == 61096);
 reset(11); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "TLS -32512")); assert(cleaned == 1 && connects == 0 && heap == 61096);
 largest = 20000; reset(0); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "最大块 19 KB")); assert(requests == 0 && cleaned == 1 && retry == 60);
 largest = 200000;
 heap = 20000; reset(0); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "内存不足")); assert(requests == 0 && retry == 60); heap = 200000;
 door_busy = true; reset(0); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "门禁请求进行中") && requests == 0 && retry == 15 && cleaned == 1); door_busy = false;
 assert(!action_held);
 clock_ready = false; reset(0); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "校时")); assert(inits == 0 && retry == 5); clock_ready = true;
 memset(&tg_talk, 0, sizeof(tg_talk)); reset(0); assert(!tg_talk_fetch(&cfg, detail, &retry)); assert(strstr(detail, "重新登录")); assert(inits == 0 && retry == 300);
 puts("talk HTTP runtime passed: one connection, session, cookie rotation, reconnect, failures, cleanup, low memory");
 return 0;
}
'''


class TalkRuntime(unittest.TestCase):
    def test_real_http_flow(self):
        with tempfile.TemporaryDirectory(prefix="tygbot-talk-test-") as name:
            temp = Path(name)
            (temp / "tyg.h").write_text(SHIM)
            for header in ("esp_crt_bundle.h", "esp_http_client.h", "esp_heap_caps.h"):
                (temp / header).write_text('#include "tyg.h"\n')
            shutil.copy(ROOT / "core/talk.c", temp / "talk.c")
            (temp / "test.c").write_text(HARNESS)
            binary = temp / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                "-g", "-fsanitize=address,undefined", "-I", str(temp), "-I", str(ROOT / "core"),
                "-I", "/usr/include/cjson", str(temp / "talk.c"), str(temp / "test.c"),
                str(ROOT / "core/core.c"), "-lcjson", "-lmbedcrypto", "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("talk HTTP runtime passed", result.stdout)
