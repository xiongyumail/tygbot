"""用模拟时钟和套接字运行 SIP 任务，验证失败状态与刷新竞态。"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_talk_runtime import ROOT, SHIM

SIP_SHIM = SHIM + r'''
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGE(...) ((void)0)
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xTaskCreate(void (*task)(void *), const char *, int, void *, int, void *);
void vTaskDelay(int);
uint32_t uxTaskGetStackHighWaterMark(void *);
int64_t esp_timer_get_time(void);
unsigned esp_random(void);
void tg_event(const char *, const cJSON *);
void tg_target_state_get(tg_target_state *);
bool tg_sip_cache_load(tg_sip_cache *);
bool tg_sip_cache_save(const tg_sip_cache *);
void tg_sip_cache_clear(void);
void tg_sip_state(tg_call_state *);
bool tg_submit_call_open(uint32_t, const char *);
'''

HARNESS = r'''
#define _POSIX_C_SOURCE 200112L
#include "tyg.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "sip.c"

SemaphoreHandle_t tg_state_lock;
tg_talk_auth tg_talk = {"13800000000", "e10adc3949ba59abbe56e057f20f883e"};
static int mode, reads, closes, fetches, waits, sends, sessions, clears, saves, registers, call_events, declined_events, call_submissions, send_mark;
static unsigned random_counter;
static uint32_t first_call, submitted_generation;
static char submitted_caller[64];
static int64_t clock_us;
static jmp_buf stop;
static char sent[8][SIP_TX];
static bool cache_present, fetch_ok;
static tg_sip_cache cache_store;
static tg_sip_config fixture = {"8001", "password", "192.0.2.7", "192.0.2.7", 6000};
int xSemaphoreTake(SemaphoreHandle_t m, int t) { (void)m; (void)t; return 1; }
int xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return 1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
int xTaskCreate(void (*task)(void *), const char *name, int stack, void *arg, int priority, void *handle) {
 (void)task; (void)arg; (void)priority; (void)handle; assert(!strcmp(name, "tyg-sip") && stack == 8192); return pdPASS;
}
uint32_t uxTaskGetStackHighWaterMark(void *task) { (void)task; return 4096; }
void tg_event(const char *kind, const cJSON *v) {
 if (strcmp(kind, "call")) return;
 call_events++; assert(!cJSON_GetObjectItemCaseSensitive(v, "caller"));
 const cJSON *ended = cJSON_GetObjectItemCaseSensitive(v, "ended_by");
 if (cJSON_IsString(ended) && !strcmp(ended->valuestring, "declined")) declined_events++;
}
void tg_target_state_get(tg_target_state *out) { memset(out, 0, sizeof(*out)); }
bool tg_submit_call_open(uint32_t generation, const char *who) {
 call_submissions++; submitted_generation = generation; strcpy(submitted_caller, who); return true;
}
int64_t esp_timer_get_time(void) { return clock_us; }
unsigned esp_random(void) { return random_counter++; }
bool tg_sip_cache_load(tg_sip_cache *out) { if (!cache_present) return false; *out = cache_store; return true; }
bool tg_sip_cache_save(const tg_sip_cache *c) { cache_store = *c; cache_present = true; saves++; return true; }
void tg_sip_cache_clear(void) { cache_present = false; clears++; }
bool tg_talk_fetch(tg_sip_config *cfg, char *detail, int *retry_s) {
 fetches++; *retry_s = 300;
 if (fetch_ok) { *cfg = fixture; return true; }
 memset(cfg, 0, sizeof(*cfg)); strcpy(detail, "下载 SIP 配置失败：平台错误"); return false;
}
static const char *last_sent(const char *prefix) {
 for (int i = sends - 1; i >= 0 && i >= sends - 8; i--) if (!strncmp(sent[i % 8], prefix, strlen(prefix))) return sent[i % 8];
 return NULL;
}
static void to_tag(const char *msg, char *out) {
 const char *to = strstr(msg, "\r\nTo: "); assert(to); const char *tag = strstr(to, ";tag="); assert(tag);
 sscanf(tag + 5, "%16[0-9a-f]", out);
}
void vTaskDelay(int delay) {
 clock_us += (int64_t)delay * 1000; waits++;
 if (mode == 23 && waits < 3) assert(fetches == 1);
 else if (mode == 23 && waits == 3) { assert(fetches == 1 && strstr(sip_state, "下载 SIP 配置失败")); tg_sip_kick(); }
 else if (mode == 23 && waits == 4) { assert(fetches == 2); longjmp(stop, 1); }
 else if (mode == 24 && waits == 1) { assert(fetches == 1); tg_talk_request(); }
 else if (mode == 24) { assert(fetches == 2 && clears == 1 && clock_us < 300000000); longjmp(stop, 1); }
 else if (mode >= 20) assert(0);
}
int socket(int domain, int type, int protocol) { (void)domain; (void)type; (void)protocol; sessions++; return 99; }
int connect(int fd, const struct sockaddr *addr, socklen_t n) { (void)addr; (void)n; assert(fd == 99); return 0; }
int getsockname(int fd, struct sockaddr *addr, socklen_t *n) {
 assert(fd == 99); struct sockaddr_in local = {.sin_family = AF_INET, .sin_port = htons(5070)};
 inet_pton(AF_INET, "192.0.2.20", &local.sin_addr); memcpy(addr, &local, sizeof(local)); *n = sizeof(local); return 0;
}
int setsockopt(int fd, int level, int option, const void *v, socklen_t n) {
 (void)level; (void)option; (void)v; (void)n; assert(fd == 99); return 0;
}
ssize_t send(int fd, const void *data, size_t n, int flags) {
 (void)flags; assert(fd == 99 && n < SIP_TX);
 memcpy(sent[sends % 8], data, n); sent[sends % 8][n] = 0;
 if (!strncmp(data, "REGISTER ", 9)) registers++;
 sends++; return (ssize_t)n;
}
int close(int fd) { assert(fd == 99); closes++; return 0; }
static ssize_t reply(void *out, size_t cap, const char *text) {
 assert(strlen(text) < cap); memcpy(out, text, strlen(text)); return (ssize_t)strlen(text);
}
#define CHALLENGE(code, header) "SIP/2.0 " code "\r\nCSeq: 1 REGISTER\r\n" header ": Digest realm=\"r\", nonce=\"n\", qop=\"auth\"\r\n\r\n"
#define INVITE "INVITE sip:8001@192.0.2.20:5070 SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK1\r\n" \
 "From: <sip:9001@192.0.2.7>;tag=abc\r\nTo: <sip:8001@192.0.2.7>\r\nCall-ID: call-1\r\nCSeq: 1 INVITE\r\nContent-Length: 0\r\n\r\n"
#define CANCEL "CANCEL sip:8001@192.0.2.20:5070 SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK1\r\n" \
 "From: <sip:9001@192.0.2.7>;tag=abc\r\nTo: <sip:8001@192.0.2.7>\r\nCall-ID: call-1\r\nCSeq: 1 CANCEL\r\nContent-Length: 0\r\n\r\n"
#define ACK "ACK sip:8001@192.0.2.20:5070 SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK1\r\n" \
 "From: <sip:9001@192.0.2.7>;tag=abc\r\nTo: <sip:8001@192.0.2.7>\r\nCall-ID: call-1\r\nCSeq: 1 ACK\r\nContent-Length: 0\r\n\r\n"
#define BYE "BYE sip:8001@192.0.2.20:5070 SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK3\r\n" \
 "From: <sip:9001@192.0.2.7>;tag=abc\r\nTo: <sip:8001@192.0.2.7>\r\nCall-ID: call-1\r\nCSeq: 2 BYE\r\nContent-Length: 0\r\n\r\n"
#define INVITE2 "INVITE sip:8001@192.0.2.20:5070 SIP/2.0\r\nVia: SIP/2.0/UDP 192.0.2.7:6000;branch=z9hG4bK2\r\n" \
 "From: <sip:9002@192.0.2.7>;tag=def\r\nTo: <sip:8001@192.0.2.7>\r\nCall-ID: call-2\r\nCSeq: 2 INVITE\r\nContent-Length: 0\r\n\r\n"
ssize_t recv(int fd, void *out, size_t cap, int flags) {
 assert(fd == 99); (void)flags; reads++; clock_us += 1000000;
 static char tag[17];
 switch (mode) {
 case 1: if (reads == 20) assert(!registered && strstr(sip_state, "SIP 注册超时")); break;
 case 2:
  if (reads == 1) return reply(out, cap, CHALLENGE("401 Unauthorized", "WWW-Authenticate"));
  if (reads == 2) { assert(strstr(last_sent("REGISTER"), "\r\nAuthorization: Digest username=\"8001\"")); assert(strstr(last_sent("REGISTER"), "qop=auth"));
   return reply(out, cap, "SIP/2.0 200 OK\r\nCSeq: 2 REGISTER\r\nContact: <sip:8001@192.0.2.20:5070>;expires=120\r\n\r\n"); }
  if (reads == 3) assert(registered && !strcmp(sip_state, "对讲已注册") && registers == 2);
  if (reads == 97) assert(registers == 2);
  if (reads == 99) { assert(registers == 3); tg_talk_request(); }
  break;
 case 3: if (reads == 1) { tg_sip_config next = fixture; strcpy(next.password, "new-password"); assert(store_config(&next, generation(), false)); } break;
 case 5:
  if (reads == 1) return reply(out, cap, CHALLENGE("407 Proxy Authentication Required", "Proxy-Authenticate"));
  if (reads == 2) { assert(strstr(last_sent("REGISTER"), "\r\nProxy-Authorization: Digest")); return reply(out, cap, "SIP/2.0 200 OK\r\nCSeq: 2 REGISTER\r\n\r\n"); }
  if (reads == 3) { assert(registered); tg_talk_request(); }
  break;
 case 6: case 7: case 22:
  if (mode == 22 && sessions == 2) { assert(fetches == 1 && clears == 1 && saves == 1); longjmp(stop, 1); }
  if (reads == 1 || reads == 2) return reply(out, cap, reads == 1 ? CHALLENGE("401 Unauthorized", "WWW-Authenticate") : "SIP/2.0 401 Unauthorized\r\nCSeq: 2 REGISTER\r\nWWW-Authenticate: Digest realm=\"r\", nonce=\"n2\"\r\n\r\n");
  if (mode == 7 && reads == 5) { assert(strstr(sip_state, "可能有误") && registers == 2); tg_talk_request(); }
  break;
 case 8:
  if (reads == 1 || reads == 2) {
   if (reads == 2) { assert(in_call && !strcmp(caller, "9001") && call_submissions == 1); to_tag(last_sent("SIP/2.0 180"), tag); assert(strstr(last_sent("SIP/2.0 100"), "\r\nTo: <sip:8001@192.0.2.7>\r\n")); }
   return reply(out, cap, INVITE);
  }
  if (reads == 3) { char again[17]; assert(call_submissions == 1); to_tag(last_sent("SIP/2.0 180"), again); assert(!strcmp(tag, again)); return reply(out, cap, CANCEL); }
  if (reads == 4) {
   const char *terminated = last_sent("SIP/2.0 487"); char final_tag[17]; assert(terminated && strstr(terminated, "\r\nCSeq: 1 INVITE\r\n"));
   to_tag(terminated, final_tag); assert(!strcmp(tag, final_tag)); assert(strstr(last_sent("SIP/2.0 200"), "\r\nCSeq: 1 CANCEL\r\n"));
   assert(!in_call); tg_talk_request();
  }
  break;
 case 9:
  if (reads == 1) return reply(out, cap, INVITE);
  if (reads == 2) {
   tg_call_state state; tg_sip_state(&state); assert(state.ringing && state.generation && call_events == 1 && call_submissions == 1);
   assert(submitted_generation == state.generation && !strcmp(submitted_caller, "9001"));
   first_call = state.generation; assert(tg_sip_decline(first_call)); return -1;
  }
  if (reads == 3) {
   const char *declined = last_sent("SIP/2.0 603 Decline");
   assert(declined && strstr(declined, "\r\nCSeq: 1 INVITE\r\n") && strstr(declined, "\r\nCall-ID: call-1\r\n"));
   assert(!in_call && active_generation == 0 && call_events == 2 && declined_events == 1); send_mark = sends; return reply(out, cap, INVITE);
  }
  if (reads == 4) { assert(sends == send_mark + 1 && last_sent("SIP/2.0 603 Decline") && call_events == 2); return reply(out, cap, ACK); }
  if (reads == 5) { assert(!tg_sip_decline(first_call)); tg_talk_request(); }
  break;
 case 10:
  if (reads == 1) return reply(out, cap, INVITE);
  if (reads == 2) return reply(out, cap, INVITE2);
  if (reads == 3) {
   assert(last_sent("SIP/2.0 486 Busy Here") && in_call && !strcmp(caller, "9001") && call_events == 1 && call_submissions == 1);
   return reply(out, cap, CANCEL);
  }
  if (reads == 4) { assert(!in_call && call_events == 2); tg_talk_request(); }
  break;
 case 11:
  if (reads == 1) return reply(out, cap, INVITE);
  if (reads == 2) { tg_call_state state; tg_sip_state(&state); first_call = state.generation; return reply(out, cap, CANCEL); }
  if (reads == 3) { assert(!tg_sip_decline(first_call)); return reply(out, cap, ACK); }
  if (reads == 4) return reply(out, cap, INVITE2);
  if (reads == 5) {
   tg_call_state state; tg_sip_state(&state); assert(state.ringing && state.generation != first_call && call_submissions == 2);
   assert(!tg_sip_decline(first_call) && tg_sip_decline(state.generation)); return -1;
  }
  if (reads == 6) { assert(last_sent("SIP/2.0 603 Decline") && !in_call && call_events == 4 && declined_events == 1); tg_talk_request(); }
  break;
 case 12:
  if (reads == 1) return reply(out, cap, INVITE);
  if (reads == 2) return reply(out, cap, BYE);
  if (reads == 3) { assert(!in_call && call_events == 2 && final_response_length == 0); tg_talk_request(); }
  break;
 case 20: assert(sessions == 1 && fetches == 0 && saves == 0 && !strcmp(sip_state, "对讲注册中")); longjmp(stop, 1);
 case 21: assert(sessions == 1 && fetches == 1 && saves == 1 && !strcmp(cache_store.owner, tg_talk.account)); longjmp(stop, 1);
 }
 return -1;
}
static void reset(int m) {
 mode = m; clock_us = 0; reads = closes = fetches = waits = sends = sessions = clears = saves = registers = call_events = declined_events = call_submissions = send_mark = 0;
 request_generation = 1; cache_allowed = true; kick = false; have_config = false; registered = in_call = false;
 call_sequence = active_generation = decline_generation = first_call = 0; final_response_length = 0;
 memset(&config, 0, sizeof(config)); memset(last_end, 0, sizeof(last_end)); memset(submitted_caller, 0, sizeof(submitted_caller)); submitted_generation = 0; cache_present = false; fetch_ok = true;
}
static void cache_for(const char *owner) {
 memset(&cache_store, 0, sizeof(cache_store)); strcpy(cache_store.owner, owner); cache_store.sip = fixture; cache_present = true;
}
int main(void) {
 tg_sip_init(); assert(task_ready);
 reset(1); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 assert(closes == 1 && registers == 3 && strstr(sip_state, "连续超时"));
 reset(2); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART); assert(closes == 1);
 reset(3); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART); assert(reads == 1 && closes == 1);
 reset(5); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(6); assert(store_config(&fixture, generation(), true)); assert(run_session(&fixture, true) == SIP_REJECTED); assert(strstr(sip_state, "重新获取") && closes == 1);
 reset(7); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(8); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(9); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(10); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(11); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 reset(12); assert(store_config(&fixture, generation(), false)); assert(run_session(&fixture, false) == SIP_RESTART);
 uint32_t stale = generation(); tg_talk_request(); assert(!store_config(&fixture, stale, false)); assert(!have_config);
 reset(20); cache_for(tg_talk.account); if (!setjmp(stop)) sip_task(NULL);
 reset(21); cache_for("13900000000"); if (!setjmp(stop)) sip_task(NULL);
 reset(22); cache_for(tg_talk.account); if (!setjmp(stop)) sip_task(NULL);
 reset(23); fetch_ok = false; if (!setjmp(stop)) sip_task(NULL);
 reset(24); fetch_ok = false; if (!setjmp(stop)) sip_task(NULL);
 tg_call_state state; tg_sip_state(&state); assert(state.stack_min_free == 4096);
 puts("SIP runtime passed: timeouts, digest, expires, proxy auth, rejection, ringing, cache, retry");
 return 0;
}
'''


class SipRuntime(unittest.TestCase):
    def test_real_sip_task(self):
        with tempfile.TemporaryDirectory(prefix="tygbot-sip-test-") as name:
            temp = Path(name)
            (temp / "tyg.h").write_text(SIP_SHIM)
            for header in ("freertos/FreeRTOS.h", "freertos/task.h", "esp_timer.h", "esp_random.h", "esp_log.h"):
                path = temp / header
                path.parent.mkdir(exist_ok=True)
                path.write_text('#include "tyg.h"\n')
            for header, native in (("sockets.h", "sys/socket.h"), ("netdb.h", "netdb.h"), ("inet.h", "arpa/inet.h")):
                path = temp / "lwip" / header
                path.parent.mkdir(exist_ok=True)
                path.write_text(f'#include <{native}>\n#include <sys/time.h>\n#include <unistd.h>\n')
            shutil.copy(ROOT / "core/sip.c", temp / "sip.c")
            (temp / "test.c").write_text(HARNESS)
            binary = temp / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-const-variable", "-Wno-deprecated-declarations",  # 见 test.sh
                "-g", "-fsanitize=address,undefined", "-I", str(temp), "-I", str(ROOT / "core"),
                "-I", "/usr/include/cjson", str(temp / "test.c"), str(ROOT / "core/core.c"),
                "-lcjson", "-lmbedcrypto", "-o", str(binary),
            ], check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("SIP runtime passed", result.stdout)
