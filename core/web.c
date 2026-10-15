/* SPDX-License-Identifier: Apache-2.0 */
#include "tyg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "lwip/inet.h"

extern const unsigned char index_start[] asm("_binary_index_html_start");
extern const unsigned char index_end[] asm("_binary_index_html_end");
extern const unsigned char auth_start[] asm("_binary_login_html_start");
extern const unsigned char auth_end[] asm("_binary_login_html_end");
extern const unsigned char icon_start[] asm("_binary_icon_png_start");
extern const unsigned char icon_end[] asm("_binary_icon_png_end");
extern const unsigned char events_start[] asm("_binary_events_js_start");
extern const unsigned char events_end[] asm("_binary_events_js_end");
extern const unsigned char setup_start[] asm("_binary_setup_html_start");
extern const unsigned char setup_end[] asm("_binary_setup_html_end");
/* Measured peak about 3.4 KB (snapshot JSON); 3 KB once overflowed. */
#define TG_SSE_STACK_BYTES 6144
#define TG_WEB_VERSION "1.2.2"
typedef struct { uint32_t sequence; char kind[16], data[TG_EVENT_SIZE]; } event_record;
static event_record history[TG_EVENT_COUNT];
static uint32_t sequence;
static char boot[9], last_door[TG_EVENT_SIZE], last_door_detail[TG_DETAIL], last_door_at[24];
static int login = -1;
static time_t login_at;
static bool busy;
static uint32_t busy_sequence;
static char warm_detail[TG_DETAIL], warm_at[24];
static bool warm_ok;
static int64_t manual_until;
static SemaphoreHandle_t stream_slots;
static QueueHandle_t jobs, call_jobs, requests;
/* Set when a request's ownership moves to the worker or an SSE task. */
static bool handed_off;
static bool web_ready;
static char cached_basic[256];
static uint32_t sse_stack_min_free = UINT32_MAX;
static uint32_t worker_stack_min_free = UINT32_MAX;
static uint32_t sse_opened, sse_closed;
static uint32_t linked_success_generation;
static tg_target_state target;
bool tg_http_auth_required = true; /* Native defaults remain password-protected. */
static bool captive_active;
static char captive_ip[INET_ADDRSTRLEN];

void tg_web_set_captive_portal(bool active, const char *ap_ip) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    captive_active = active && tg_copy(captive_ip, sizeof(captive_ip), ap_ip ? ap_ip : "");
    xSemaphoreGive(tg_state_lock);
}

void tg_event(const char *kind, const cJSON *value) {
    char *text = cJSON_PrintUnformatted(value);
    if (!text) return;
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (strlen(text) < TG_EVENT_SIZE) {
        sequence++; event_record *e = &history[(sequence - 1) % TG_EVENT_COUNT];
        e->sequence = sequence; tg_copy(e->kind, sizeof(e->kind), kind); strcpy(e->data, text);
    }
    xSemaphoreGive(tg_state_lock); free(text);
}
void tg_login_note(int state, bool relogin) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool changed = state != login || (state == 1 && relogin);
    if (changed) { login = state; login_at = time(NULL); }
    xSemaphoreGive(tg_state_lock);
    if (!changed) return;
    cJSON *v = cJSON_CreateObject(); char at[24]; tg_timestamp(at, sizeof(at), false);
    cJSON_AddBoolToObject(v, "ok", state == 1); cJSON_AddNumberToObject(v, "ts", time(NULL)); cJSON_AddStringToObject(v, "at", at);
    tg_event("login", v); cJSON_Delete(v);
}
void tg_door_status_get(tg_door_status *out) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    out->busy = busy; out->busy_sequence = busy_sequence; out->login = login;
    out->has_door = *last_door_detail != 0;
    tg_copy(out->door_detail, sizeof(out->door_detail), last_door_detail); tg_copy(out->door_at, sizeof(out->door_at), last_door_at);
    out->has_warm = *warm_at != 0; tg_copy(out->warm_detail, sizeof(out->warm_detail), warm_detail);
    xSemaphoreGive(tg_state_lock);
}
void tg_target_state_get(tg_target_state *out) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    *out = target;
    xSemaphoreGive(tg_state_lock);
}
static cJSON *target_json(const tg_target_state *state) {
    cJSON *v = cJSON_CreateObject();
    cJSON_AddBoolToObject(v, "selected", state->door_selected);
    cJSON_AddBoolToObject(v, "automatic_supported", state->caller_unique);
    cJSON_AddStringToObject(v, "household", state->household_name);
    cJSON_AddStringToObject(v, "door", state->door_name);
    return v;
}
void tg_target_note(const tg_room *selected_room, const tg_door *selected_door, bool selected, bool caller_unique) {
    tg_target_state next = {0};
    bool has_household = selected_room && *selected_room->household;
    next.door_selected = has_household && selected && selected_door;
    next.caller_unique = next.door_selected && caller_unique;
    if (has_household) tg_copy(next.household_name, sizeof(next.household_name), selected_room->area);
    if (next.door_selected) {
        if (*selected_door->group && *selected_door->name)
            snprintf(next.door_name, sizeof(next.door_name), "%s · %s", selected_door->group, selected_door->name);
        else tg_copy(next.door_name, sizeof(next.door_name), *selected_door->name ? selected_door->name : "门禁");
        tg_copy(next.door_code, sizeof(next.door_code), selected_door->code);
    }
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool changed = memcmp(&target, &next, sizeof(next)) != 0;
    target = next;
    xSemaphoreGive(tg_state_lock);
    if (changed) { cJSON *v = target_json(&next); tg_event("target", v); cJSON_Delete(v); }
}
void tg_set_busy(bool value) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (value && !busy && ++busy_sequence == 0) busy_sequence = 1;
    busy = value;
    xSemaphoreGive(tg_state_lock);
}
void tg_door_note(bool ok, const char *detail, double seconds, double reply_wait, const char *source) {
    cJSON *v = cJSON_CreateObject(); char at[24]; tg_timestamp(at, sizeof(at), false);
    cJSON_AddBoolToObject(v, "ok", ok); cJSON_AddStringToObject(v, "detail", detail);
    cJSON_AddNumberToObject(v, "seconds", seconds); cJSON_AddNumberToObject(v, "reply_wait", reply_wait);
    cJSON_AddStringToObject(v, "at", at); cJSON_AddNumberToObject(v, "ts", time(NULL)); cJSON_AddStringToObject(v, "source", source);
    char *text = cJSON_PrintUnformatted(v);
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (text) tg_copy(last_door, sizeof(last_door), text);
    tg_copy(last_door_detail, sizeof(last_door_detail), detail); tg_copy(last_door_at, sizeof(last_door_at), at);
    xSemaphoreGive(tg_state_lock); free(text); tg_event("door", v); cJSON_Delete(v);
}
cJSON *tg_snapshot(void) {
    cJSON *v = cJSON_CreateObject();
    tg_target_state selected;
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(v, "ok", true); cJSON_AddBoolToObject(v, "busy", busy);
    cJSON_AddItemToObject(v, "last_door", *last_door ? cJSON_Parse(last_door) : cJSON_CreateNull());
    if (login < 0) cJSON_AddNullToObject(v, "login");
    else {
        cJSON *state = cJSON_CreateObject(); char at[24]; struct tm tm; localtime_r(&login_at, &tm); strftime(at, sizeof(at), "%Y-%m-%d %H:%M:%S", &tm);
        cJSON_AddBoolToObject(state, "ok", login == 1); cJSON_AddNumberToObject(state, "ts", login_at); cJSON_AddStringToObject(state, "at", at);
        cJSON_AddItemToObject(v, "login", state);
    }
    if (*warm_at) {
        cJSON *warm = cJSON_CreateObject(); cJSON_AddStringToObject(warm, "at", warm_at);
        cJSON_AddBoolToObject(warm, "ok", warm_ok); cJSON_AddStringToObject(warm, "detail", warm_detail); cJSON_AddItemToObject(v, "last_warm_up", warm);
    } else cJSON_AddNullToObject(v, "last_warm_up");
    cJSON_AddStringToObject(v, "version", TG_WEB_VERSION);
    cJSON_AddStringToObject(v, "commit", tg_build_commit);
    selected = target;
    xSemaphoreGive(tg_state_lock);
    cJSON_AddItemToObject(v, "target", target_json(&selected));
    cJSON_AddBoolToObject(v, "call_linked_open", tg_call_linked_open_get());
    tg_call_state state; tg_sip_state(&state);
    cJSON *call = cJSON_CreateObject();
    cJSON_AddBoolToObject(call, "registered", state.registered);
    cJSON_AddBoolToObject(call, "ringing", state.ringing);
    char source[sizeof(selected.door_name)];
    bool matched = tg_call_source(&selected, state.caller, source, sizeof(source));
    cJSON_AddStringToObject(call, "source", source);
    cJSON_AddBoolToObject(call, "matched", matched); cJSON_AddStringToObject(call, "detail", state.detail);
    cJSON_AddStringToObject(call, "ended_by", state.ended_by);
    cJSON_AddItemToObject(v, "call", call);
    return v;
}
static esp_err_t respond(httpd_req_t *req, int code, cJSON *v) {
    const char *status = code == 200 ? "200 OK" : code == 400 ? "400 Bad Request" : code == 401 ? "401 Unauthorized" : code == 403 ? "403 Forbidden" : code == 404 ? "404 Not Found" : code == 409 ? "409 Conflict" : code == 413 ? "413 Payload Too Large" : code == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
    httpd_resp_set_status(req, status); httpd_resp_set_type(req, "application/json; charset=utf-8"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *text = cJSON_PrintUnformatted(v); cJSON_Delete(v);
    esp_err_t rc = httpd_resp_send(req, text ? text : "{\"ok\":false,\"detail\":\"内存不足\"}", HTTPD_RESP_USE_STRLEN); free(text); return rc;
}
static void deny_framing(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "frame-ancestors 'none'");
}
static esp_err_t failure(httpd_req_t *req, int code, const char *detail) {
    cJSON *v = cJSON_CreateObject(); cJSON_AddBoolToObject(v, "ok", false); cJSON_AddStringToObject(v, "detail", detail); return respond(req, code, v);
}
static bool header(httpd_req_t *req, const char *key, char *out, size_t cap) {
    size_t n = httpd_req_get_hdr_value_len(req, key);
    if (!n) { *out = 0; return true; }
    return n < cap && httpd_req_get_hdr_value_str(req, key, out, cap) == ESP_OK;
}
static bool equal(const unsigned char *a, const unsigned char *b, size_t length) {
    unsigned char diff = 0; for (size_t i = 0; i < length; i++) diff |= a[i] ^ b[i]; return !diff;
}
static bool password_ok(const char *value) {
    unsigned char digest[32]; bool ok = tg_cfg.password_set && tg_password_hash(value, tg_cfg.salt, digest) && equal(digest, tg_cfg.password_hash, sizeof(digest));
    memset(digest, 0, sizeof(digest)); return ok;
}
static void signature(long long expires, char *out) {
    char seed[64]; unsigned char hash[32]; snprintf(seed, sizeof(seed), "tygbot:%lld", expires);
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), tg_cfg.cookie_key, 32, (const unsigned char *)seed, strlen(seed), hash);
    for (int i = 0; i < 32; i++) snprintf(out + 2*i, 3, "%02x", hash[i]);
}
static bool cookie_ok(const char *cookie) {
    const char *p = cookie;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, "tygbot_session=", 15)) {
            p += 15; char *end; errno = 0; long long expires = strtoll(p, &end, 10);
            if (errno || end == p || *end != '.' || expires < time(NULL) || expires > (long long)time(NULL) + 365LL*86400 + 60) return false;
            char expected_hash[65]; signature(expires, expected_hash); end++;
            if (strlen(end) < 64 || (end[64] && end[64] != ';' && end[64] != ' ')) return false;
            return equal((const unsigned char *)end, (const unsigned char *)expected_hash, 64);
        }
        p = strchr(p, ';'); if (p) p++;
    }
    return false;
}
static bool authorization(httpd_req_t *req) {
    char basic[256], cookie[384];
    if (!header(req, "Authorization", basic, sizeof(basic)) || !header(req, "Cookie", cookie, sizeof(cookie))) return false;
    if (*cookie && cookie_ok(cookie)) return true;
    if (strncmp(basic, "Basic ", 6)) return false;
    if (*cached_basic && !strcmp(basic, cached_basic)) return true;
    unsigned char decoded[192]; size_t length = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded)-1, &length, (const unsigned char *)basic + 6, strlen(basic + 6))) return false;
    decoded[length] = 0; char *colon = memchr(decoded, ':', length);
    bool ok = colon && strlen(colon + 1) <= 128 && password_ok(colon + 1);
    memset(decoded, 0, sizeof(decoded));
    if (ok) strcpy(cached_basic, basic); else vTaskDelay(pdMS_TO_TICKS(1000));
    return ok;
}
static bool host_guard(httpd_req_t *req, const char *path) {
    char host[128], origin[192], hostname[128], site[24], mode[24];
    if (!header(req, "Host", host, sizeof(host)) || !*host || !header(req, "Origin", origin, sizeof(origin))
        || !header(req, "Sec-Fetch-Site", site, sizeof(site)) || !header(req, "Sec-Fetch-Mode", mode, sizeof(mode))) return false;
    /* Other sites may only link to the pages; subresource GETs such as <img> must not reach the API. */
    if (*site && strcmp(site, "same-origin") && strcmp(site, "none")
        && (strcmp(mode, "navigate") || (strcmp(path, "/") && strcmp(path, "/auth")))) return false;
    if (req->method == HTTP_POST && *origin) {
        const char *base = !strncmp(origin, "http://", 7) ? origin + 7 : !strncmp(origin, "https://", 8) ? origin + 8 : NULL;
        if (!base || strcmp(base, host)) return false;
    }
    strcpy(hostname, host); char *port = strchr(hostname, ':'); if (port) *port = 0;
    struct in_addr ip;
    return inet_aton(hostname, &ip) != 0 || !strcmp(hostname, "localhost") || (*tg_cfg.host && !strcmp(hostname, tg_cfg.host));
}
/* The shared ESPHome server reads the request body before dispatch. */
static cJSON *read_json(const char *body) {
    cJSON *v = cJSON_ParseWithOpts(body && *body ? body : "{}", NULL, true);
    if (!cJSON_IsObject(v)) { cJSON_Delete(v); return NULL; } return v;
}
static esp_err_t auth_post(httpd_req_t *req, const char *body) {
    if (!tg_time_ready()) return failure(req, 503, "等待网络校时后再登录网页");
    cJSON *v = read_json(body);
    if (!v || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(v, "password"))) { cJSON_Delete(v); return failure(req, 400, "请求格式错误"); }
    const char *pw = tg_string(v, "password"); bool ok = strlen(pw) <= 128 && password_ok(pw); cJSON_Delete(v);
    if (!ok) { vTaskDelay(pdMS_TO_TICKS(1000)); return failure(req, 401, "密码错误"); }
    long long expires = time(NULL) + 365LL*86400; char hash[65], cookie[192]; signature(expires, hash);
    snprintf(cookie, sizeof(cookie), "tygbot_session=%lld.%s; Max-Age=31536000; Path=/; HttpOnly; SameSite=Lax", expires, hash);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie); v = cJSON_CreateObject(); cJSON_AddBoolToObject(v, "ok", true); return respond(req, 200, v);
}
static esp_err_t sse_send(httpd_req_t *req, const char *id, const char *kind, const char *text) {
    char head[96];
    int n = id ? snprintf(head, sizeof(head), "id: %s\nevent: %s\ndata: ", id, kind) : snprintf(head, sizeof(head), "event: %s\ndata: ", kind);
    if (httpd_resp_send_chunk(req, head, n) != ESP_OK || httpd_resp_send_chunk(req, text, strlen(text)) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, "\n\n", 2);
}
static bool peer_alive(int fd) {
    char byte;
    int result = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return result > 0 || (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}
static void note_sse_stack(void) {
    uint32_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (free_bytes < sse_stack_min_free) sse_stack_min_free = free_bytes;
    xSemaphoreGive(tg_state_lock);
}
static void sse_task(void *arg) {
    httpd_req_t *req = arg;
    const int fd = httpd_req_to_sockfd(req);
    char last[64] = ""; uint32_t after;
    if (!header(req, "Last-Event-ID", last, sizeof(last))) *last = 0;
    // A manually recreated browser EventSource cannot set custom headers.
    // Accept the same bounded replay cursor through the query string too.
    char query[128], diagnostic[4] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (!*last) httpd_query_key_value(query, "last_event_id", last, sizeof(last));
        httpd_query_key_value(query, "diagnostics", diagnostic, sizeof(diagnostic));
    }
    xSemaphoreTake(tg_state_lock, portMAX_DELAY); after = sequence;
    char prefix[16]; snprintf(prefix, sizeof(prefix), "%s-", boot);
    if (!strncmp(last, prefix, strlen(prefix))) {
        char *end; unsigned long n = strtoul(last + strlen(prefix), &end, 10);
        if (end != last + strlen(prefix) && !*end && n <= sequence) after = n;
    }
    xSemaphoreGive(tg_state_lock);
    httpd_resp_set_type(req, "text/event-stream; charset=utf-8"); httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    cJSON *snapshot = tg_snapshot(); char *text = cJSON_PrintUnformatted(snapshot); cJSON_Delete(snapshot);
    bool ok = text && sse_send(req, NULL, "snapshot", text) == ESP_OK; free(text);
    // Opt-in real transport telemetry exercises the same nested floating-point
    // JSON path as a door snapshot, without fabricating a door result or opening.
    if (ok && !strcmp(diagnostic, "1")) {
        cJSON *info = cJSON_CreateObject(), *transport = cJSON_CreateObject();
        if (!info || !transport) { cJSON_Delete(info); cJSON_Delete(transport); ok = false; }
        else {
            cJSON_AddNumberToObject(transport, "uptime_seconds", esp_timer_get_time() / 1000000.0);
            cJSON_AddItemToObject(info, "transport", transport);
            text = cJSON_PrintUnformatted(info); cJSON_Delete(info);
            ok = text && sse_send(req, NULL, "diagnostics", text) == ESP_OK; free(text);
        }
    }
    note_sse_stack();
    int64_t ping = esp_timer_get_time();
    while (ok) {
        // Detect a closed tab/foreground replacement promptly, rather than
        // retaining its slot until the old 15-second heartbeat fires.
        if (!peer_alive(fd)) break;
        event_record next = {0};
        xSemaphoreTake(tg_state_lock, portMAX_DELAY);
        uint32_t start = sequence >= TG_EVENT_COUNT ? sequence - TG_EVENT_COUNT + 1 : 1;
        if (after + 1 < start) after = start - 1;
        if (sequence > after) next = history[after % TG_EVENT_COUNT];
        xSemaphoreGive(tg_state_lock);
        if (next.sequence) {
            char id[40]; snprintf(id, sizeof(id), "%s-%lu", boot, (unsigned long)next.sequence);
            ok = sse_send(req, id, next.kind, next.data) == ESP_OK; after = next.sequence;
        } else if (esp_timer_get_time() - ping >= 2000000) {
            ok = httpd_resp_send_chunk(req, ": ping\n\n", 8) == ESP_OK; ping = esp_timer_get_time();
        } else vTaskDelay(pdMS_TO_TICKS(100));
        note_sse_stack();
    }
    // HTTPD owns close() and session reuse. Shutdown wakes its select(), then
    // complete the async request; do not queue closure before releasing req.
    shutdown(fd, SHUT_RDWR);
    httpd_req_async_handler_complete(req);
    xSemaphoreTake(tg_state_lock, portMAX_DELAY); sse_closed++; xSemaphoreGive(tg_state_lock);
    xSemaphoreGive(stream_slots); vTaskDelete(NULL);
}
typedef enum { JOB_OPEN, JOB_DRY, JOB_SEND, JOB_CAPTCHA, JOB_SMS, JOB_HOUSEHOLDS, JOB_DOORS,
               JOB_TARGET_GET, JOB_TARGET_SET, JOB_SETTING } job_kind;
typedef struct {
    httpd_req_t *req; job_kind kind; char value[16], source[16]; int64_t submitted;
    uint32_t call_generation; bool automatic;
    char caller[64], organization[128], household[128], door_code[128], door_number[16];
} job;
typedef struct { uint32_t generation; int64_t submitted; char caller[64]; } call_job;
static uint32_t ringing_call(char *caller, size_t capacity) {
    tg_call_state call; tg_sip_state(&call);
    if (caller && capacity) tg_copy(caller, capacity, call.ringing ? call.caller : "");
    return call.ringing ? call.generation : 0;
}
static uint32_t ringing_generation(void) { return ringing_call(NULL, 0); }
bool tg_submit_call_open(uint32_t generation, const char *caller) {
    if (!call_jobs || !generation || !caller || !*caller || !tg_call_linked_open_get()) return false;
    /* A call from another door is shown as such, not recorded as a failed opening. Before the
     * first refresh the target is unknown, so the worker's own match check decides. */
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool other_door = target.door_selected && strcmp(caller, target.door_code);
    xSemaphoreGive(tg_state_lock);
    if (other_door) return false;
    call_job pending = {.generation = generation, .submitted = esp_timer_get_time()};
    if (!tg_copy(pending.caller, sizeof(pending.caller), caller)) return false;
    return xQueueOverwrite(call_jobs, &pending) == pdTRUE;
}
/* Latest connection check shown as the HA status. */
static void note_check(bool ok, const char *detail) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    warm_ok = ok; tg_copy(warm_detail, sizeof(warm_detail), detail); tg_timestamp(warm_at, sizeof(warm_at), false);
    xSemaphoreGive(tg_state_lock);
}
/* Returns the delay to the next check: inside the 240 s connection cache while healthy. */
static int64_t warm(void) {
    char detail[TG_DETAIL] = ""; bool ok = tg_refresh(true, detail); note_check(ok, detail);
    return ok ? 200000000LL : !strcmp(detail, TG_LOGGED_OUT) ? 600000000LL : 120000000LL;
}
static void worker(void *unused) {
    job j; int64_t next_warm = esp_timer_get_time() + 2000000;
    for (;;) {
        call_job pending;
        bool have_call = xQueueReceive(call_jobs, &pending, 0) == pdTRUE;
        bool have_job = have_call || xQueueReceive(jobs, &j, pdMS_TO_TICKS(200)) == pdTRUE;
        if (!have_job && xQueueReceive(call_jobs, &pending, 0) == pdTRUE) have_call = have_job = true;
        if (have_call) {
            memset(&j, 0, sizeof(j)); j.kind = JOB_OPEN; j.submitted = pending.submitted;
            j.call_generation = pending.generation; j.automatic = true;
            tg_copy(j.caller, sizeof(j.caller), pending.caller); strcpy(j.source, "来电联动");
        }
        if (!have_job) {
            if (esp_timer_get_time() >= next_warm && esp_timer_get_time() >= manual_until && tg_time_ready()) {
                int64_t delay = 1000000;
                if (xSemaphoreTake(tg_action_lock, 0)) { delay = warm(); xSemaphoreGive(tg_action_lock); }
                next_warm = esp_timer_get_time() + delay;
            }
            continue;
        }
        if (j.automatic && (!tg_call_linked_open_get() || !tg_sip_call_matches(j.call_generation, j.caller))) {
            memset(&j, 0, sizeof(j)); continue;
        }
        xSemaphoreTake(tg_action_lock, portMAX_DELAY);
        char detail[TG_DETAIL] = ""; cJSON *result = cJSON_CreateObject(); int code = 200; bool ok = false;
        int64_t begin = esp_timer_get_time();
        if (!j.automatic && begin - j.submitted > 10000000) { strcpy(detail, "排队已超时，未执行"); code = 409; }
        else if (j.automatic && (!tg_call_linked_open_get() || !tg_sip_call_matches(j.call_generation, j.caller))) {
            strcpy(detail, "来电已结束或设置已关闭，未开门"); code = 409;
        }
        else if (j.kind == JOB_OPEN || j.kind == JOB_DRY) {
            bool dry = j.kind == JOB_DRY; double reply_wait = 0;
            if (!dry && j.call_generation && linked_success_generation == j.call_generation) {
                strcpy(detail, "本次来电已经开门，未重复执行"); code = 409;
            } else if (!dry) {
                tg_set_busy(true); cJSON *start = cJSON_CreateObject(); cJSON_AddStringToObject(start, "source", j.source); cJSON_AddNumberToObject(start, "ts", time(NULL)); tg_event("door_start", start); cJSON_Delete(start);
                ok = j.automatic ? tg_open_call(j.caller, j.call_generation, detail, &reply_wait) : tg_open(false, detail, &reply_wait);
                double elapsed = (esp_timer_get_time() - j.submitted) / 1000000.0;
                if (ok && j.call_generation) {
                    linked_success_generation = j.call_generation;
                    if (tg_sip_decline(j.call_generation))
                        strncat(detail, "；正在发送 603 结束来电", TG_DETAIL - strlen(detail) - 1);
                    else
                        strncat(detail, "；来电已结束，无需发送 603", TG_DETAIL - strlen(detail) - 1);
                }
                tg_door_note(ok, detail, elapsed, reply_wait, j.source); tg_set_busy(false);
                cJSON_AddNumberToObject(result, "seconds", elapsed); code = ok ? 200 : 500;
            } else {
                ok = tg_open(true, detail, &reply_wait);
                double elapsed = (esp_timer_get_time() - j.submitted) / 1000000.0;
                note_check(ok, detail); cJSON_AddNumberToObject(result, "seconds", elapsed); code = ok ? 200 : 500;
            }
        } else if (j.kind == JOB_HOUSEHOLDS || j.kind == JOB_DOORS) {
            const char *key = j.kind == JOB_HOUSEHOLDS ? "households" : "doors";
            bool cached = false;
            cJSON *list = j.kind == JOB_HOUSEHOLDS ? tg_households(detail, &cached) : tg_doors(j.organization, j.household, detail, &cached);
            ok = list != NULL; cJSON_AddItemToObject(result, key, list ? list : cJSON_CreateArray()); code = ok ? 200 : 409;
            if (ok) { cJSON_AddBoolToObject(result, "cached", cached); strcpy(detail, cached ? "已加载最近授权列表" : "已更新授权列表"); }
        } else if (j.kind == JOB_TARGET_GET) {
            cJSON_AddItemToObject(result, "target", tg_target_config()); ok = true;
            strcpy(detail, "已获取当前门禁选择");
        } else if (j.kind == JOB_TARGET_SET) {
            ok = tg_target_set(j.organization, j.household, j.door_code, j.door_number, detail);
            code = ok ? 200 : 409;
            if (ok) {
                cJSON_AddItemToObject(result, "target", tg_target_config());
                tg_target_state selected; tg_target_state_get(&selected);
                cJSON_AddItemToObject(result, "state", target_json(&selected));
                cJSON_AddBoolToObject(result, "call_linked_open", tg_call_linked_open_get());
            }
        } else if (j.kind == JOB_SETTING) {
            bool enabled = !strcmp(j.value, "1");
            tg_target_state selected; tg_target_state_get(&selected);
            if (enabled && !selected.caller_unique) {
                strcpy(detail, selected.door_selected ? "当前授权范围无法唯一关联来电，不能开启自动模式" : "请先选择当前住房和门禁");
                code = 409;
            } else {
                ok = tg_call_linked_open_set(enabled); code = ok ? 200 : 500;
            }
            if (ok) {
                strcpy(detail, enabled ? "来电联动开门已开启，下次来电生效" : "来电联动开门已关闭");
                cJSON_AddBoolToObject(result, "call_linked_open", enabled);
            } else if (!*detail) strcpy(detail, "设置保存失败");
        } else {
            manual_until = esp_timer_get_time() + 300000000LL;
            char *captcha = NULL; const char *next = NULL;
            if (j.kind == JOB_SEND) { ok = tg_login_send(j.value, &captcha, detail); next = "captcha"; if (ok) strcpy(detail, "请输入图形验证码"); }
            if (j.kind == JOB_CAPTCHA) { bool sent = false; ok = tg_login_captcha(j.value, &sent, &captcha, detail); next = sent ? "sms" : "captcha"; if (ok) strcpy(detail, sent ? "短信验证码已发出" : "图形验证码已更换"); }
            if (j.kind == JOB_SMS) {
                bool linked = false; ok = tg_login_sms(j.value, &linked, detail); next = "done";
                if (ok) {
                    note_check(linked, detail); manual_until = 0;
                    if (linked) strcpy(detail, "登录成功");
                    else { char reason[TG_DETAIL]; tg_copy(reason, sizeof(reason), detail); snprintf(detail, TG_DETAIL, "登录成功，门禁连接失败：%.150s", reason); }
                }
            }
            code = ok ? 200 : 409;
            if (ok) cJSON_AddStringToObject(result, "next", next);
            if (captcha) cJSON_AddStringToObject(result, "captcha", captcha);
            free(captcha);
        }
        cJSON_AddBoolToObject(result, "ok", ok); cJSON_AddStringToObject(result, "detail", detail);
        if (j.req) { respond(j.req, code, result); httpd_req_async_handler_complete(j.req); }
        else cJSON_Delete(result);
        uint32_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
        xSemaphoreTake(tg_state_lock, portMAX_DELAY);
        if (free_bytes < worker_stack_min_free) worker_stack_min_free = free_bytes;
        xSemaphoreGive(tg_state_lock);
        xSemaphoreGive(tg_action_lock);
        memset(&j, 0, sizeof(j));
    }
}
bool tg_submit_open(bool dry) {
    if (!jobs) return false;
    job j = {.kind = dry ? JOB_DRY : JOB_OPEN, .submitted = esp_timer_get_time(),
             .call_generation = dry ? 0 : ringing_generation()};
    strcpy(j.source, "HA");
    return xQueueSend(jobs, &j, 0) == pdTRUE;
}
bool tg_submit_call_linked_open(bool enabled) {
    if (!jobs) return false;
    job j = {.kind = JOB_SETTING, .submitted = esp_timer_get_time()};
    strcpy(j.value, enabled ? "1" : "0");
    return xQueueSend(jobs, &j, 0) == pdTRUE;
}
static esp_err_t queue_job(httpd_req_t *req, job_kind kind, const char *value) {
    job j = {.kind = kind, .submitted = esp_timer_get_time()};
    if (kind == JOB_OPEN) j.call_generation = ringing_generation();
    if (value && !tg_copy(j.value, sizeof(j.value), value)) return failure(req, 400, "字段过长");
    strcpy(j.source, "HTTP"); char query[96], source[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && httpd_query_key_value(query, "from", source, sizeof(source)) == ESP_OK)
        strcpy(j.source, !strcmp(source, "ha") ? "HA" : !strcmp(source, "web") ? "网页" : "HTTP");
    j.req = req;
    if (!xQueueSend(jobs, &j, 0)) return failure(req, 409, "服务忙，请稍后再试");
    handed_off = true; return ESP_OK;
}
static esp_err_t queue_target(httpd_req_t *req, job_kind kind, const char *organization,
                              const char *household, const char *door_code, const char *door_number) {
    job j = {.req = req, .kind = kind, .submitted = esp_timer_get_time()};
    if ((organization && !tg_copy(j.organization, sizeof(j.organization), organization))
        || (household && !tg_copy(j.household, sizeof(j.household), household))
        || (door_code && !tg_copy(j.door_code, sizeof(j.door_code), door_code))
        || (door_number && !tg_copy(j.door_number, sizeof(j.door_number), door_number)))
        return failure(req, 400, "门禁选择字段过长");
    if (!xQueueSend(jobs, &j, 0)) return failure(req, 409, "服务忙，请稍后再试");
    handed_off = true; return ESP_OK;
}
static bool on_captive_interface(httpd_req_t *req) {
    char ap_ip[INET_ADDRSTRLEN], local_ip[INET_ADDRSTRLEN]; bool active;
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    active = captive_active; memcpy(ap_ip, captive_ip, sizeof(ap_ip));
    xSemaphoreGive(tg_state_lock);
    if (!active) return false;
    struct sockaddr_in local; socklen_t local_size = sizeof(local);
    return getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &local_size) == 0 &&
           local.sin_family == AF_INET && inet_ntop(AF_INET, &local.sin_addr, local_ip, sizeof(local_ip)) &&
           tg_on_captive_interface(active, local_ip, ap_ip);
}
static bool path_is(httpd_req_t *req, const char *path) {
    size_t n = strlen(path);
    return !strncmp(req->uri, path, n) && (req->uri[n] == 0 || req->uri[n] == '?');
}
tg_route tg_web_route(httpd_req_t *req) {
    if (!tg_state_lock) return TG_ROUTE_NONE;
    /* On the fallback AP the captive portal keeps its data endpoints and OTA;
     * only its page is replaced. */
    if (on_captive_interface(req)) {
        if (req->method != HTTP_GET || path_is(req, "/config.json") || path_is(req, "/wifisave")) return TG_ROUTE_NONE;
        return TG_ROUTE_SETUP;
    }
    if (!web_ready) return TG_ROUTE_NONE;
    /* ESPHome's firmware upload, only from this device's own page: same-origin, signed in if required. */
    if (path_is(req, "/update")) {
        bool allowed = req->method == HTTP_POST && httpd_req_get_hdr_value_len(req, "Origin") && host_guard(req, "/update")
            && (!tg_http_auth_required || authorization(req));
        return allowed ? TG_ROUTE_NONE : TG_ROUTE_DENY;
    }
    return TG_ROUTE_API;
}
esp_err_t tg_web_deny(httpd_req_t *req) { return failure(req, 403, "固件升级只能从设备网页发起"); }
esp_err_t tg_web_setup_page(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (path_is(req, "/icon.png")) { httpd_resp_set_type(req, "image/png"); return httpd_resp_send(req, (const char *)icon_start, icon_end-icon_start); }
    httpd_resp_set_type(req, "text/html; charset=utf-8"); deny_framing(req);
    /* The hotspot page shows the version and build commit, filled in when served. */
    static const char mark[] = "__TG_VERSION__";
    const char *page = (const char *)setup_start, *at = NULL;
    size_t size = setup_end-setup_start, mark_len = sizeof(mark) - 1;
    for (size_t i = 0; i + mark_len <= size && !at; i++) if (!memcmp(page + i, mark, mark_len)) at = page + i;
    if (!at) return httpd_resp_send(req, page, size);
    char version[40];
    snprintf(version, sizeof(version), *tg_build_commit ? "%s (%s)" : "%s", TG_WEB_VERSION, tg_build_commit);
    if (httpd_resp_send_chunk(req, page, at - page) != ESP_OK || httpd_resp_send_chunk(req, version, strlen(version)) != ESP_OK
        || httpd_resp_send_chunk(req, at + mark_len, page + size - at - mark_len) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t handle(httpd_req_t *req, const char *body) {
    char path[64]; const char *question = strchr(req->uri, '?'); size_t len = question ? (size_t)(question - req->uri) : strlen(req->uri);
    if (len >= sizeof(path)) return failure(req, 404, "未知接口");
    memcpy(path, req->uri, len); path[len] = 0;
    if (!host_guard(req, path)) return failure(req, 403, "不允许的 Host 或跨站请求");
    bool public = !strcmp(path, "/health") || !strcmp(path, "/auth") || !strcmp(path, "/icon.png");
    if (!public && tg_http_auth_required && !authorization(req)) {
        char accept[128]; header(req, "Accept", accept, sizeof(accept));
        if (req->method == HTTP_GET && strstr(accept, "text/html")) { httpd_resp_set_status(req, "302 Found"); httpd_resp_set_hdr(req, "Location", "/auth"); return httpd_resp_send(req, NULL, 0); }
        return failure(req, 401, "需要网页密码");
    }
    if (!tg_http_auth_required && !strcmp(path, "/auth")) {
        if (req->method == HTTP_GET) {
            httpd_resp_set_status(req, "302 Found"); httpd_resp_set_hdr(req, "Location", "/");
            return httpd_resp_send(req, NULL, 0);
        }
        cJSON *v = cJSON_CreateObject(); cJSON_AddBoolToObject(v, "ok", true);
        cJSON_AddStringToObject(v, "detail", "此设备无需网页密码"); return respond(req, 200, v);
    }
    if (req->method == HTTP_GET) {
        if (!strcmp(path, "/status")) return respond(req, 200, tg_snapshot());
        if (!strcmp(path, "/health")) {
            cJSON *v = cJSON_CreateObject(); cJSON_AddBoolToObject(v, "ok", true);
            cJSON_AddStringToObject(v, "version", TG_WEB_VERSION); cJSON_AddStringToObject(v, "commit", tg_build_commit);
            return respond(req, 200, v);
        }
        if (!strcmp(path, "/diagnostics")) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "target", "esp32c3"); cJSON_AddStringToObject(v, "version", TG_WEB_VERSION);
            cJSON_AddStringToObject(v, "web_version", TG_WEB_VERSION);
            cJSON_AddStringToObject(v, "commit", tg_build_commit);
            cJSON_AddNumberToObject(v, "http_port", TG_HTTP_PORT);
            cJSON_AddNumberToObject(v, "uptime_seconds", esp_timer_get_time() / 1000000.0);
            cJSON_AddNumberToObject(v, "reset_reason", esp_reset_reason());
            cJSON_AddNumberToObject(v, "sse_stack_bytes", TG_SSE_STACK_BYTES);
            cJSON_AddNumberToObject(v, "http_stack_min_free", uxTaskGetStackHighWaterMark(NULL));
            xSemaphoreTake(tg_state_lock, portMAX_DELAY);
            if (sse_stack_min_free == UINT32_MAX) cJSON_AddNullToObject(v, "sse_stack_min_free");
            else cJSON_AddNumberToObject(v, "sse_stack_min_free", sse_stack_min_free);
            if (worker_stack_min_free == UINT32_MAX) cJSON_AddNullToObject(v, "worker_stack_min_free");
            else cJSON_AddNumberToObject(v, "worker_stack_min_free", worker_stack_min_free);
            cJSON_AddNumberToObject(v, "sse_opened", sse_opened); cJSON_AddNumberToObject(v, "sse_closed", sse_closed);
            cJSON_AddNumberToObject(v, "sse_active", sse_opened - sse_closed);
            xSemaphoreGive(tg_state_lock);
            tg_call_state call; tg_sip_state(&call);
            if (call.stack_min_free == UINT32_MAX) cJSON_AddNullToObject(v, "sip_stack_min_free");
            else cJSON_AddNumberToObject(v, "sip_stack_min_free", call.stack_min_free);
            cJSON_AddBoolToObject(v, "time_ready", tg_time_ready());
            cJSON_AddNumberToObject(v, "heap_free", heap_caps_get_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(v, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            cJSON_AddNumberToObject(v, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
            return respond(req, 200, v);
        }
        if (!strcmp(path, "/") || !strcmp(path, "/auth")) {
            httpd_resp_set_type(req, "text/html; charset=utf-8"); httpd_resp_set_hdr(req, "Cache-Control", "no-store"); deny_framing(req);
            return !strcmp(path, "/") ? httpd_resp_send(req, (const char *)index_start, index_end-index_start) : httpd_resp_send(req, (const char *)auth_start, auth_end-auth_start);
        }
        if (!strcmp(path, "/icon.png")) { httpd_resp_set_type(req, "image/png"); return httpd_resp_send(req, (const char *)icon_start, icon_end-icon_start); }
        if (!strcmp(path, "/events.js")) {
            httpd_resp_set_type(req, "application/javascript; charset=utf-8");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, (const char *)events_start, events_end-events_start);
        }
        if (!strcmp(path, "/households")) return queue_job(req, JOB_HOUSEHOLDS, NULL);
        if (!strcmp(path, "/doors")) {
            char query[384] = "", organization[128] = "", household[128] = "";
            if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
                httpd_query_key_value(query, "organization", organization, sizeof(organization));
                httpd_query_key_value(query, "household", household, sizeof(household));
            }
            return queue_target(req, JOB_DOORS, organization, household, NULL, NULL);
        }
        if (!strcmp(path, "/target")) return queue_target(req, JOB_TARGET_GET, NULL, NULL, NULL, NULL);
        if (!strcmp(path, "/events")) {
            if (!xSemaphoreTake(stream_slots, 0)) return failure(req, 503, "最多两路 SSE 订阅");
            xSemaphoreTake(tg_state_lock, portMAX_DELAY); sse_opened++; xSemaphoreGive(tg_state_lock);
            if (xTaskCreate(sse_task, "sse", TG_SSE_STACK_BYTES, req, 4, NULL) != pdPASS) {
                xSemaphoreTake(tg_state_lock, portMAX_DELAY); sse_opened--; xSemaphoreGive(tg_state_lock);
                xSemaphoreGive(stream_slots); return failure(req, 503, "内存不足");
            }
            handed_off = true; return ESP_OK;
        }
    } else if (req->method == HTTP_POST) {
        if (req->content_len > TG_MAX_BODY) return failure(req, 413, "请求体过大");
        if (!strcmp(path, "/auth")) return auth_post(req, body);
        if (!strcmp(path, "/door")) {
            char query[96], dry[8]; bool is_dry = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && httpd_query_key_value(query, "dry", dry, sizeof(dry)) == ESP_OK && !strcmp(dry, "1");
            return queue_job(req, is_dry ? JOB_DRY : JOB_OPEN, NULL);
        }
        if (!strcmp(path, "/settings")) {
            cJSON *json = read_json(body); cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "call_linked_open") : NULL;
            if (!cJSON_IsBool(item)) { cJSON_Delete(json); return failure(req, 400, "设置格式错误"); }
            const char *value = cJSON_IsTrue(item) ? "1" : "0"; esp_err_t rc = queue_job(req, JOB_SETTING, value);
            cJSON_Delete(json); return rc;
        }
        if (!strcmp(path, "/target")) {
            cJSON *json = read_json(body);
            const char *organization = tg_string(json, "organization"), *household = tg_string(json, "household");
            const char *door_code = tg_string(json, "door_code"), *door_number = tg_string(json, "door_number");
            if (!json || !*organization || !*household || !*door_code || !*door_number) {
                cJSON_Delete(json); return failure(req, 400, "请选择住房和门禁");
            }
            char org[128], house[128], code[128], number[16];
            bool copied = tg_copy(org, sizeof(org), organization) && tg_copy(house, sizeof(house), household)
                && tg_copy(code, sizeof(code), door_code) && tg_copy(number, sizeof(number), door_number);
            cJSON_Delete(json);
            return copied ? queue_target(req, JOB_TARGET_SET, org, house, code, number)
                          : failure(req, 400, "门禁选择字段过长");
        }
        job_kind kind;
        if (!strcmp(path, "/login/send")) kind = JOB_SEND;
        else if (!strcmp(path, "/login/captcha")) kind = JOB_CAPTCHA;
        else if (!strcmp(path, "/login/sms")) kind = JOB_SMS;
        else return failure(req, 404, "未知接口");
        cJSON *json = read_json(body); const char *value = tg_string(json, kind == JOB_SEND ? "phone" : "code");
        if (!json || (kind == JOB_SEND ? !tg_phone_valid(value) : !tg_code_valid(value, kind == JOB_CAPTCHA))) { cJSON_Delete(json); return failure(req, 400, "手机号或验证码格式错误"); }
        char copy[16]; strcpy(copy, value); cJSON_Delete(json); return queue_job(req, kind, copy);
    }
    return failure(req, 404, "未知接口");
}
typedef struct { httpd_req_t *req; char *body; } web_request;
/* Requests run here, off the shared server task and its small stack. */
static void web_task(void *unused) {
    web_request r;
    for (;;) {
        if (!xQueueReceive(requests, &r, portMAX_DELAY)) continue;
        handed_off = false;
        handle(r.req, r.body);
        if (r.body) { memset(r.body, 0, strlen(r.body)); free(r.body); }
        if (!handed_off) httpd_req_async_handler_complete(r.req);
    }
}
bool tg_web_submit(httpd_req_t *req, const char *body) {
    web_request r = {0};
    if (body && !(r.body = strdup(body))) { failure(req, 503, "内存不足"); return false; }
    if (httpd_req_async_handler_begin(req, &r.req) != ESP_OK) { free(r.body); failure(req, 503, "服务资源不足"); return false; }
    if (!xQueueSend(requests, &r, 0)) {
        failure(r.req, 503, "服务忙，请稍后再试"); httpd_req_async_handler_complete(r.req); free(r.body); return false;
    }
    return true;
}
bool tg_web_start(void) {
    snprintf(boot, sizeof(boot), "%08lx", (unsigned long)esp_random());
    stream_slots = xSemaphoreCreateCounting(2, 2); jobs = xQueueCreate(2, sizeof(job));
    call_jobs = xQueueCreate(1, sizeof(call_job)); requests = xQueueCreate(4, sizeof(web_request));
    /* On failure ESPHome networking and OTA stay up; nothing is torn down. */
    if (!stream_slots || !jobs || !call_jobs || !requests) return false;
    web_ready = xTaskCreate(web_task, "tyg-web", 6144, NULL, 5, NULL) == pdPASS
        && xTaskCreate(worker, "tyg-worker", 8192, NULL, 5, NULL) == pdPASS;
    return web_ready;
}
