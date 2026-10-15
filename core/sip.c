/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal SIP registration and incoming-call notification for the door station.
 * Audio/video are out of scope: an INVITE is answered with 180 Ringing and surfaced
 * as an event, never picked up. Runs on its own task and UDP socket, independent of
 * the door-open path (HTTPS/WSS), so a SIP failure never blocks opening the door.
 * The SIP config is cached in NVS, so a normal boot registers without any TLS request. */
#include "tyg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"

#define SIP_EXPIRES 3600
/* Only headers are parsed; a longer datagram (SDP body) is truncated harmlessly. */
#define SIP_RX 1536
#define SIP_TX 1280
/* Session and config fetch never share the stack: TLS gets about 7 KB. */
#define SIP_STACK 8192
#define SIP_BACKOFF_MIN 60
#define SIP_BACKOFF_MAX 1800
#define SIP_TIMEOUTS_BEFORE_RESTART 3
#define NOINLINE __attribute__((noinline))

static const char *const SIP_TAG = "tygbot.sip";
static tg_sip_config config;
static bool have_config, config_cached;
/* Generation 1 is the boot request; only it may be served from the NVS cache. */
static uint32_t request_generation = 1;
static bool cache_allowed = true, kick;
static bool task_ready;
static SemaphoreHandle_t config_lock;
/* Reported to HA; guarded by tg_state_lock (shared with web.c). */
static bool registered, in_call;
static char caller[64], sip_state[TG_DETAIL] = "未启动", last_end[16];
static uint32_t call_sequence, active_generation, decline_generation;
static uint32_t stack_min_free = UINT32_MAX;
/* Prepared from the ringing INVITE, so a later decline allocates neither heap nor SIP stack. */
static char final_response[SIP_TX];
static size_t final_response_length;

static void set_state(bool reg, const char *text) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    registered = reg; tg_copy(sip_state, sizeof(sip_state), text);
    xSemaphoreGive(tg_state_lock);
}
static void call_event(bool ringing, const char *who, const char *ended_by) {
    tg_target_state target; tg_target_state_get(&target);
    char source[sizeof(target.door_name)];
    bool matched = tg_call_source(&target, ringing ? who : NULL, source, sizeof(source));
    cJSON *v = cJSON_CreateObject();
    cJSON_AddBoolToObject(v, "ringing", ringing);
    cJSON_AddStringToObject(v, "source", source);
    cJSON_AddBoolToObject(v, "matched", matched);
    if (!ringing && ended_by && *ended_by) cJSON_AddStringToObject(v, "ended_by", ended_by);
    cJSON_AddNumberToObject(v, "ts", esp_timer_get_time() / 1000000);
    tg_event("call", v); cJSON_Delete(v);
}
static uint32_t begin_call(const char *who) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (++call_sequence == 0) call_sequence = 1;
    active_generation = call_sequence; decline_generation = 0; in_call = true; last_end[0] = 0;
    tg_copy(caller, sizeof(caller), who ? who : "");
    uint32_t generation = active_generation;
    xSemaphoreGive(tg_state_lock);
    call_event(true, who, NULL);
    tg_submit_call_open(generation, who);
    return generation;
}
static void end_call(uint32_t generation, const char *ended_by) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool changed = in_call && (!generation || generation == active_generation);
    if (changed) {
        /* Retain the last caller and sequence for HA after a short call has ended. */
        in_call = false; active_generation = 0; decline_generation = 0;
        tg_copy(last_end, sizeof(last_end), ended_by ? ended_by : "");
    }
    xSemaphoreGive(tg_state_lock);
    if (changed) call_event(false, NULL, ended_by);
}
static bool decline_requested(uint32_t generation) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool requested = in_call && active_generation == generation && decline_generation == generation;
    xSemaphoreGive(tg_state_lock); return requested;
}
static void note_stack(void) {
    uint32_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    if (free_bytes < stack_min_free) stack_min_free = free_bytes;
    xSemaphoreGive(tg_state_lock);
}
void tg_sip_state(tg_call_state *out) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    out->registered = registered; out->ringing = in_call;
    tg_copy(out->caller, sizeof(out->caller), caller); tg_copy(out->detail, sizeof(out->detail), sip_state);
    tg_copy(out->ended_by, sizeof(out->ended_by), last_end);
    out->sequence = call_sequence; out->generation = in_call ? active_generation : 0; out->stack_min_free = stack_min_free;
    xSemaphoreGive(tg_state_lock);
}
bool tg_sip_call_matches(uint32_t generation, const char *who) {
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool matches = generation && who && *who && in_call && active_generation == generation && !strcmp(caller, who);
    xSemaphoreGive(tg_state_lock); return matches;
}
bool tg_sip_decline(uint32_t generation) {
    if (!generation) return false;
    xSemaphoreTake(tg_state_lock, portMAX_DELAY);
    bool accepted = in_call && active_generation == generation;
    if (accepted) decline_generation = generation;
    xSemaphoreGive(tg_state_lock); return accepted;
}

void tg_talk_request(void) {
    if (!task_ready) return;
    xSemaphoreTake(config_lock, portMAX_DELAY);
    request_generation++; cache_allowed = false; have_config = false; memset(&config, 0, sizeof(config));
    xSemaphoreGive(config_lock);
}
void tg_sip_kick(void) {
    if (!task_ready) return;
    xSemaphoreTake(config_lock, portMAX_DELAY); kick = true; xSemaphoreGive(config_lock);
}
static uint32_t generation(void) {
    xSemaphoreTake(config_lock, portMAX_DELAY); uint32_t value = request_generation; xSemaphoreGive(config_lock); return value;
}
static bool store_config(const tg_sip_config *cfg, uint32_t requested, bool cached) {
    xSemaphoreTake(config_lock, portMAX_DELAY);
    bool current = requested == request_generation;
    if (current) { config = *cfg; have_config = true; config_cached = cached; }
    xSemaphoreGive(config_lock);
    return current;
}
static bool load_config(tg_sip_config *out, bool *cached) {
    xSemaphoreTake(config_lock, portMAX_DELAY);
    bool ok = have_config; if (ok) { *out = config; *cached = config_cached; }
    xSemaphoreGive(config_lock);
    return ok;
}
static bool config_changed(const tg_sip_config *cfg) {
    xSemaphoreTake(config_lock, portMAX_DELAY);
    bool changed = !have_config || memcmp(&config, cfg, sizeof(config));
    xSemaphoreGive(config_lock);
    return changed;
}
/* The cache is used only when it was fetched for the account that is logged in now. */
static NOINLINE bool cached_config(tg_sip_config *out) {
    tg_sip_cache cache; char owner[sizeof(tg_talk.account)];
    xSemaphoreTake(tg_state_lock, portMAX_DELAY); tg_copy(owner, sizeof(owner), tg_talk.account); xSemaphoreGive(tg_state_lock);
    bool ok = *owner && tg_sip_cache_load(&cache) && !strcmp(cache.owner, owner) && *cache.sip.account;
    if (ok) *out = cache.sip;
    memset(&cache, 0, sizeof(cache)); return ok;
}
static NOINLINE void save_cache(const tg_sip_config *cfg) {
    tg_sip_cache cache = {0};
    xSemaphoreTake(tg_state_lock, portMAX_DELAY); tg_copy(cache.owner, sizeof(cache.owner), tg_talk.account); xSemaphoreGive(tg_state_lock);
    cache.sip = *cfg;
    if (!tg_sip_cache_save(&cache)) ESP_LOGE(SIP_TAG, "SIP config cache not saved");
    memset(&cache, 0, sizeof(cache));
}
static void hex(char *out, size_t bytes) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; i++) { unsigned v = esp_random(); out[i * 2] = digits[(v >> 4) & 15]; out[i * 2 + 1] = digits[v & 15]; }
    out[bytes * 2] = 0;
}

typedef struct {
    int fd;
    char local_ip[INET_ADDRSTRLEN];
    int local_port;
    char call_id[64], from_tag[17];
    /* One unanswered INVITE transaction; final replies remain cached until ACK. */
    char invite_call[128], to_tag[17];
    unsigned cseq, invite_cseq;
    uint32_t invite_generation;
    int final_status;
    int64_t final_until;
} sip_ctx;
typedef enum { SIP_RESTART, SIP_REJECTED } sip_end;

/* Append the digest line answering the 401/407 in `reply`; returns the length written or -1. */
static NOINLINE int build_auth(const tg_sip_config *cfg, const char *reply, int status, const char *uri, char *out, size_t cap) {
    char challenge[320], realm[96], nonce[128], qop[24], opaque[96], response[33], cnonce[17];
    bool proxy = status == 407;
    if (!tg_sip_header(reply, proxy ? "Proxy-Authenticate" : "WWW-Authenticate", challenge, sizeof(challenge))) return -1;
    if (!tg_sip_param(challenge, "realm", realm, sizeof(realm)) || !tg_sip_param(challenge, "nonce", nonce, sizeof(nonce))) return -1;
    bool use_qop = tg_sip_param(challenge, "qop", qop, sizeof(qop)) && strstr(qop, "auth");
    bool has_opaque = tg_sip_param(challenge, "opaque", opaque, sizeof(opaque));
    hex(cnonce, 8);
    tg_sip_digest(cfg->account, realm, cfg->password, "REGISTER", uri, nonce, use_qop ? "auth" : "", "00000001", cnonce, response);
    int n = snprintf(out, cap, "%s: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", response=\"%s\", algorithm=MD5",
                     proxy ? "Proxy-Authorization" : "Authorization", cfg->account, realm, nonce, uri, response);
    if (n < 0 || (size_t)n >= cap) return -1;
    if (use_qop) { int m = snprintf(out + n, cap - n, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce); if (m < 0 || (size_t)m >= cap - n) return -1; n += m; }
    if (has_opaque) { int m = snprintf(out + n, cap - n, ", opaque=\"%s\"", opaque); if (m < 0 || (size_t)m >= cap - n) return -1; n += m; }
    if ((size_t)n + 2 >= cap) return -1;
    memcpy(out + n, "\r\n", 3); return n + 2;
}
/* REGISTER, answering the 401/407 in `reply` when given. */
static NOINLINE bool send_register(sip_ctx *c, const tg_sip_config *cfg, const char *reply, int status) {
    char msg[SIP_TX], branch[25] = "z9hG4bK", uri[72];
    hex(branch + 7, 8);
    snprintf(uri, sizeof(uri), "sip:%s", cfg->domain);
    c->cseq++;
    int n = snprintf(msg, sizeof(msg),
        "REGISTER %s SIP/2.0\r\n"
        "Via: SIP/2.0/UDP %s:%d;branch=%s;rport\r\n"
        "Max-Forwards: 70\r\n"
        "From: <sip:%s@%s>;tag=%s\r\n"
        "To: <sip:%s@%s>\r\n"
        "Call-ID: %s\r\n"
        "CSeq: %u REGISTER\r\n"
        "Contact: <sip:%s@%s:%d>\r\n"
        "Expires: %d\r\n"
        "User-Agent: TYGBot\r\n",
        uri, c->local_ip, c->local_port, branch, cfg->account, cfg->domain, c->from_tag,
        cfg->account, cfg->domain, c->call_id, c->cseq, cfg->account, c->local_ip, c->local_port, SIP_EXPIRES);
    if (n < 0 || (size_t)n >= sizeof(msg)) return false;
    if (reply) {
        int m = build_auth(cfg, reply, status, uri, msg + n, sizeof(msg) - n);
        if (m < 0) return false;
        n += m;
    }
    int m = snprintf(msg + n, sizeof(msg) - n, "Content-Length: 0\r\n\r\n");
    if (m < 0 || (size_t)m >= sizeof(msg) - n) return false;
    return send(c->fd, msg, n + m, 0) >= 0;
}
/* Build a response by copying the server transaction headers from the request. */
static NOINLINE bool build_response(sip_ctx *c, const char *request, int status, const char *reason, bool add_to_tag,
                                    const char *cseq_override, char *msg, size_t cap, size_t *length) {
    int first = snprintf(msg, cap, "SIP/2.0 %d %s\r\n", status, reason);
    if (first < 0 || (size_t)first >= cap) return false;
    size_t n = (size_t)first;
    if (!tg_sip_headers(request, "Via", msg + n, cap - n)) return false;
    n += strlen(msg + n);
    if (!tg_sip_headers(request, "From", msg + n, cap - n)) return false;
    n += strlen(msg + n);
    if (!tg_sip_headers(request, "To", msg + n, cap - n)) return false;
    size_t to_len = strlen(msg + n);
    /* A To tag already present (an in-dialog request) is kept. */
    if (add_to_tag && !strstr(msg + n, ";tag=") && *c->to_tag) {
        n += to_len - 2;
        int m = snprintf(msg + n, cap - n, ";tag=%s\r\n", c->to_tag);
        if (m < 0 || (size_t)m >= cap - n) return false;
        n += (size_t)m;
    } else n += to_len;
    if (!tg_sip_headers(request, "Call-ID", msg + n, cap - n)) return false;
    n += strlen(msg + n);
    int m = cseq_override ? snprintf(msg + n, cap - n, "CSeq: %s\r\n", cseq_override) : 0;
    if (m < 0 || (size_t)m >= cap - n) return false;
    if (!cseq_override && !tg_sip_headers(request, "CSeq", msg + n, cap - n)) return false;
    n += cseq_override ? (size_t)m : strlen(msg + n);
    m = snprintf(msg + n, cap - n, "Contact: <sip:tygbot@%s:%d>\r\nUser-Agent: TYGBot\r\nContent-Length: 0\r\n\r\n",
                 c->local_ip, c->local_port);
    if (m < 0 || (size_t)m >= cap - n) return false;
    *length = n + (size_t)m; return true;
}
static NOINLINE bool respond(sip_ctx *c, const char *request, int status, const char *reason, bool add_to_tag, const char *cseq_override) {
    char msg[SIP_TX]; size_t length;
    return build_response(c, request, status, reason, add_to_tag, cseq_override, msg, sizeof(msg), &length)
        && send(c->fd, msg, length, 0) == (ssize_t)length;
}
static void clear_invite(sip_ctx *c) {
    c->invite_call[0] = 0; c->to_tag[0] = 0; c->invite_cseq = 0; c->invite_generation = 0;
    c->final_status = 0; c->final_until = 0; final_response_length = 0;
}
static bool request_key(const char *buf, char *call, size_t capacity, unsigned *number) {
    char cseq[64];
    return tg_sip_header(buf, "Call-ID", call, capacity) && *call
        && tg_sip_header(buf, "CSeq", cseq, sizeof(cseq)) && sscanf(cseq, "%u", number) == 1;
}
static bool same_invite(const sip_ctx *c, const char *call, unsigned number) {
    return *c->invite_call && number == c->invite_cseq && !strcmp(call, c->invite_call);
}
static bool same_call(const sip_ctx *c, const char *call) {
    return *c->invite_call && *call && !strcmp(call, c->invite_call);
}
static bool send_prepared(sip_ctx *c) {
    return final_response_length && send(c->fd, final_response, final_response_length, 0) == (ssize_t)final_response_length;
}
static void apply_decline(sip_ctx *c, int64_t now) {
    if (!*c->invite_call || c->final_status || !decline_requested(c->invite_generation)) return;
    if (send_prepared(c)) {
        c->final_status = 603; c->final_until = now + 32000000LL;
        end_call(c->invite_generation, "declined");
    }
}
static NOINLINE void handle_request(sip_ctx *c, const char *buf, const char *method, int64_t now) {
    char call[128] = ""; unsigned number = 0;
    bool has_key = request_key(buf, call, sizeof(call), &number);
    bool same = has_key && same_invite(c, call, number);
    if (!strcmp(method, "INVITE")) {
        if (!has_key) { respond(c, buf, 400, "Bad Request", false, NULL); return; }
        if (same) {
            if (c->final_status) send_prepared(c);
            else { respond(c, buf, 100, "Trying", false, NULL); respond(c, buf, 180, "Ringing", true, NULL); }
            return;
        }
        /* Only one unanswered door call is supported. Keep its transaction intact. */
        if (*c->invite_call && !c->final_status) {
            char old_tag[sizeof(c->to_tag)]; strcpy(old_tag, c->to_tag); hex(c->to_tag, 8);
            respond(c, buf, 486, "Busy Here", true, NULL); strcpy(c->to_tag, old_tag); return;
        }
        if (*c->invite_call) clear_invite(c);  /* A completed call must not block the next new INVITE. */
        char from[256], who[64] = "";
        if (tg_sip_header(buf, "From", from, sizeof(from))) tg_sip_user(from, who, sizeof(who));
        tg_copy(c->invite_call, sizeof(c->invite_call), call); c->invite_cseq = number; hex(c->to_tag, 8);
        char invite_cseq[72]; snprintf(invite_cseq, sizeof(invite_cseq), "%u INVITE", number);
        if (!build_response(c, buf, 603, "Decline", true, invite_cseq, final_response, sizeof(final_response), &final_response_length)) {
            respond(c, buf, 500, "Server Internal Error", true, NULL); clear_invite(c); return;
        }
        c->invite_generation = begin_call(who);
        respond(c, buf, 100, "Trying", false, NULL);
        respond(c, buf, 180, "Ringing", true, NULL);
    } else if (!strcmp(method, "CANCEL")) {
        respond(c, buf, 200, "OK", false, NULL);
        if (same && !c->final_status) {
            char invite_cseq[72]; snprintf(invite_cseq, sizeof(invite_cseq), "%u INVITE", number);
            if (build_response(c, buf, 487, "Request Terminated", true, invite_cseq,
                               final_response, sizeof(final_response), &final_response_length)) {
                c->final_status = 487; c->final_until = now + 32000000LL; send_prepared(c);
            }
            end_call(c->invite_generation, "remote");
        }
    } else if (!strcmp(method, "ACK")) {
        if (same && c->final_status) clear_invite(c);
    } else if (!strcmp(method, "BYE")) {
        respond(c, buf, 200, "OK", false, NULL);
        /* BYE is a dialog request and normally advances CSeq, so bind it by Call-ID. */
        if (has_key && same_call(c, call)) { end_call(c->invite_generation, "remote"); clear_invite(c); }
    } else if (!strcmp(method, "OPTIONS")) {
        respond(c, buf, 200, "OK", false, NULL);
    } else {
        respond(c, buf, 405, "Method Not Allowed", false, NULL);
    }
}

/* One registration session: resolve, bind, register, then service messages until it must restart. */
static NOINLINE sip_end run_session(const tg_sip_config *cfg, bool cached) {
    sip_ctx c = {0};
    final_response_length = 0;
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res = NULL;
    char port[8]; snprintf(port, sizeof(port), "%d", cfg->port);
    if (getaddrinfo(cfg->proxy, port, &hints, &res) != 0 || !res) { set_state(false, "对讲服务器解析失败"); return SIP_RESTART; }
    struct sockaddr_in server = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    c.fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (c.fd < 0) { set_state(false, "SIP 套接字创建失败"); return SIP_RESTART; }
    bool ok = connect(c.fd, (struct sockaddr *)&server, sizeof(server)) == 0;
    struct sockaddr_in local; socklen_t ll = sizeof(local);
    if (ok && getsockname(c.fd, (struct sockaddr *)&local, &ll) == 0) {
        inet_ntop(AF_INET, &local.sin_addr, c.local_ip, sizeof(c.local_ip)); c.local_port = ntohs(local.sin_port);
    } else ok = false;
    if (!ok) { close(c.fd); set_state(false, "SIP 连接失败"); return SIP_RESTART; }
    struct timeval tv = {.tv_usec = 250000}; setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    hex(c.call_id, 12); hex(c.from_tag, 8);

    send_register(&c, cfg, NULL, 0);
    int64_t now = esp_timer_get_time();
    int64_t next_register = now + SIP_BACKOFF_MIN * 1000000LL, next_keepalive = now + 30000000LL;
    int64_t register_deadline = now + 15000000LL;
    bool authed = false, awaiting_register = true;
    int backoff = SIP_BACKOFF_MIN, timeouts = 0;
    char buf[SIP_RX];

    for (;;) {
        /* A fresh config (re-login) or shutdown request ends the session so it restarts cleanly. */
        if (config_changed(cfg)) { close(c.fd); return SIP_RESTART; }
        int len = recv(c.fd, buf, sizeof(buf) - 1, 0);
        now = esp_timer_get_time();
        if (len > 0) {
            buf[len] = 0;
            int status = tg_sip_status(buf);
            char method[16];
            if (status) {
                char cseq[64], cseq_method[16] = "";
                if (tg_sip_header(buf, "CSeq", cseq, sizeof(cseq))) sscanf(cseq, "%*u %15s", cseq_method);
                if (strcmp(cseq_method, "REGISTER") || status < 200) continue;
                awaiting_register = false;
                if ((status == 401 || status == 407) && !authed) {
                    if (send_register(&c, cfg, buf, status)) { authed = true; awaiting_register = true; register_deadline = now + 15000000LL; }
                    else set_state(false, "SIP 认证构造失败");
                } else if (status == 401 || status == 407 || status == 403) {
                    authed = false;
                    /* A cached password may be stale: fetch the config once more instead of retrying it. */
                    if (cached) { close(c.fd); set_state(false, "SIP 认证被拒，正在重新获取对讲配置"); return SIP_REJECTED; }
                    set_state(false, "SIP 认证被拒，对讲账号或密码可能有误");
                    next_register = now + backoff * 1000000LL; backoff = backoff * 2 > SIP_BACKOFF_MAX ? SIP_BACKOFF_MAX : backoff * 2;
                } else if (status < 300) {
                    authed = false; timeouts = 0; backoff = SIP_BACKOFF_MIN;
                    int expires = tg_sip_expires(buf, SIP_EXPIRES);
                    if (expires > 0) {
                        int refresh = expires * 4 / 5 < 20 ? 20 : expires * 4 / 5;
                        set_state(true, "对讲已注册"); next_register = now + refresh * 1000000LL;
                    } else { set_state(false, "SIP 服务器未授予注册期限"); next_register = now + backoff * 1000000LL; }
                } else {
                    authed = false; char text[48]; snprintf(text, sizeof(text), "SIP 注册被拒绝：%d", status); set_state(false, text);
                    next_register = now + backoff * 1000000LL; backoff = backoff * 2 > SIP_BACKOFF_MAX ? SIP_BACKOFF_MAX : backoff * 2;
                }
            } else if (tg_sip_method(buf, method, sizeof(method))) handle_request(&c, buf, method, now);
        }
        apply_decline(&c, now);
        if (c.final_status && now >= c.final_until) clear_invite(&c);
        if (awaiting_register && now >= register_deadline) {
            awaiting_register = false; authed = false;
            /* Repeated silence usually means a changed address or route: rebuild the socket. */
            if (++timeouts >= SIP_TIMEOUTS_BEFORE_RESTART) { close(c.fd); set_state(false, "SIP 注册连续超时，重建连接"); return SIP_RESTART; }
            set_state(false, "SIP 注册超时，未收到服务器响应");
        }
        if (now >= next_register) {
            authed = false; send_register(&c, cfg, NULL, 0); awaiting_register = true;
            register_deadline = now + 15000000LL; next_register = now + backoff * 1000000LL;
        }
        if (now >= next_keepalive) { send(c.fd, "\r\n\r\n", 4, 0); next_keepalive = now + 30000000LL; }
    }
}
/* Serve the pending request from the cache or the talkback platform; returns the retry delay on failure. */
static NOINLINE void obtain_config(uint32_t requested, bool use_cache, uint32_t *handled, int64_t *retry_at) {
    tg_sip_config cfg; char detail[TG_DETAIL] = ""; int retry_s = 300;
    bool cached = use_cache && cached_config(&cfg), ok = cached;
    if (!ok) {
        if (!use_cache) tg_sip_cache_clear();
        set_state(false, "正在获取对讲配置");
        ok = tg_talk_fetch(&cfg, detail, &retry_s);
        note_stack();
    }
    if (requested != generation()) { memset(&cfg, 0, sizeof(cfg)); return; }
    if (ok) {
        if (store_config(&cfg, requested, cached)) { *handled = requested; if (!cached) save_cache(&cfg); }
        *retry_at = 0;
    } else {
        set_state(false, detail);
        /* 失败原因保留在状态中；限速重试，不占用门禁任务。 */
        *retry_at = esp_timer_get_time() + retry_s * 1000000LL;
    }
    memset(&cfg, 0, sizeof(cfg));
}
static void sip_task(void *unused) {
    (void) unused;
    uint32_t handled = 0, attempted = 0; int64_t retry_at = 0;
    for (;;) {
        xSemaphoreTake(config_lock, portMAX_DELAY);
        uint32_t requested = request_generation; bool use_cache = cache_allowed, kicked = kick; kick = false;
        xSemaphoreGive(config_lock);
        /* 新的登录、手动刷新或服务发现完成立即生效，无需等待旧的重试期限。 */
        if (requested != attempted || kicked) { retry_at = 0; attempted = requested; }
        if (requested != handled && esp_timer_get_time() >= retry_at) obtain_config(requested, use_cache, &handled, &retry_at);
        tg_sip_config cfg; bool cached = false;
        if (!load_config(&cfg, &cached) || !*cfg.account) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        set_state(false, "对讲注册中");
        end_call(0, "connection");
        sip_end end = run_session(&cfg, cached);
        end_call(0, "connection"); note_stack();
        memset(&cfg, 0, sizeof(cfg));
        if (end == SIP_REJECTED) tg_talk_request();
        else if (load_config(&cfg, &cached)) { memset(&cfg, 0, sizeof(cfg)); vTaskDelay(pdMS_TO_TICKS(5000)); }
    }
}
void tg_sip_init(void) {
    if (config_lock) return;
    config_lock = xSemaphoreCreateMutex();
    if (!config_lock || xTaskCreate(sip_task, "tyg-sip", SIP_STACK, NULL, 4, NULL) != pdPASS) {
        set_state(false, "对讲任务启动失败，内存不足");
        ESP_LOGE(SIP_TAG, "SIP task start failed");
    } else task_ready = true;
}
