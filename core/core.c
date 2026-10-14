#include "core.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"

bool tg_on_captive_interface(bool active, const char *local_ip, const char *ap_ip) {
    return active && local_ip && ap_ip && *ap_ip && !strcmp(local_ip, ap_ip);
}

cJSON *tg_parse_json(const char *text) {
    if (!text || strlen(text) > TG_MAX_JSON) return NULL;
    /* Bound recursive cJSON stack use before entering its parser. */
    bool quoted = false, escaped = false; int depth = 0;
    for (const char *p = text; *p; p++) {
        if (quoted) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') quoted = false;
        } else if (*p == '"') quoted = true;
        else if (*p == '{' || *p == '[') { if (++depth > 32) return NULL; }
        else if (*p == '}' || *p == ']') { if (--depth < 0) return NULL; }
    }
    if (quoted || depth) return NULL;
    return cJSON_ParseWithOpts(text, NULL, true);
}

const char *tg_string(const cJSON *value, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(value, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}
bool tg_is_code(const cJSON *value, const char *key, const char *code) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(value, key);
    if (cJSON_IsString(item)) return strcmp(item->valuestring, code) == 0;
    if (!cJSON_IsNumber(item)) return false;
    char number[32];
    if (item->valuedouble != item->valueint) return false;
    snprintf(number, sizeof(number), "%d", item->valueint);
    return strcmp(number, code) == 0;
}
bool tg_copy(char *dest, size_t cap, const char *value) {
    if (!value || strlen(value) >= cap) return false;
    memcpy(dest, value, strlen(value) + 1);
    return true;
}
bool tg_call_source(const tg_target_state *target, const char *caller, char *source, size_t cap) {
    if (!source || !cap) return false;
    *source = 0;
    if (!caller || !*caller) return false;
    bool matched = target && target->door_selected && target->caller_unique && !strcmp(caller, target->door_code);
    tg_copy(source, cap, matched ? target->door_name : "未知门禁");
    return matched;
}
bool tg_caller_target_unique(size_t household_count, const tg_door *doors, size_t count, size_t selected) {
    if (household_count != 1 || !doors || selected >= count || !*doors[selected].code) return false;
    size_t matches = 0;
    for (size_t i = 0; i < count; i++) if (!strcmp(doors[i].code, doors[selected].code)) matches++;
    return matches == 1;
}
bool tg_phone_valid(const char *v) {
    if (!v || strlen(v) != 11 || v[0] != '1') return false;
    for (int i = 0; i < 11; i++) if (v[i] < '0' || v[i] > '9') return false;
    return true;
}
bool tg_code_valid(const char *v, bool empty) {
    if (!v || strlen(v) > 12 || (!empty && !*v)) return false;
    for (; *v; v++) if (!((*v >= '0' && *v <= '9') || (*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z'))) return false;
    return true;
}
bool tg_url_allowed(const char *url, bool image, bool ws) {
    const char *prefix = ws ? "wss://" : "https://";
    if (!url || strncmp(url, prefix, strlen(prefix)) || strlen(url) >= 768) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) if (*p <= 32 || *p >= 127 || *p == '#' || *p == '\\') return false;
    const char *start = url + strlen(prefix), *end = strchr(start, '/');
    if (!end) end = url + strlen(url);
    const char *query = strchr(start, '?');
    if (query && query < end) end = query;
    char host[96];
    size_t len = (size_t)(end - start);
    if (!len || len >= sizeof(host)) return false;
    memcpy(host, start, len); host[len] = 0;
    char *port = strchr(host, ':');
    if (port) { *port++ = 0; if (strcmp(port, "443") && strcmp(port, "8443")) return false; }
    if (image) {
        size_t n = strlen(host), suffix = strlen(".taoyuangu.com");
        return !strcmp(host, "taoyuangu.com") || (n > suffix && !strcmp(host + n - suffix, ".taoyuangu.com"));
    }
    return !strcmp(host, "uums.taoyuangu.com") || !strcmp(host, "sso.taoyuangu.com") || !strcmp(host, "lxj.taoyuangu.com");
}
bool tg_uums_ok(const cJSON *v, const char *code) {
    return tg_is_code(v, "result", "0") && (!code || tg_is_code(v, "code", code));
}
bool tg_cps_ok(const cJSON *v) { return tg_is_code(v, "code", "0"); }
bool tg_open_ok(const cJSON *v) {
    return tg_uums_ok(v, "0") && !strcmp(tg_string(v, "data"), "开门成功");
}
bool tg_select_room(const cJSON *v, const tg_config *cfg, tg_room *out, char *detail) {
    memset(out, 0, sizeof(*out));
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(v, "data");
    const cJSON *areas = cJSON_GetObjectItemCaseSensitive(data, "areaList");
    if (!tg_cps_ok(v) || !cJSON_IsArray(areas)) { tg_copy(detail, TG_DETAIL, "授权住房查询被服务器拒绝或格式错误"); return false; }
    unsigned found = 0;
    const cJSON *area, *room;
    cJSON_ArrayForEach(area, areas) {
        const char *org = tg_string(area, "organizationSeq");
        if (*cfg->organization && strcmp(org, cfg->organization)) continue;
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(area, "list");
        if (!cJSON_IsArray(list)) continue;
        cJSON_ArrayForEach(room, list) {
            const char *house = tg_string(room, "householdSerial"), *token = tg_string(room, "token"), *mobile = tg_string(room, "mobile");
            if (*cfg->household && strcmp(house, cfg->household)) continue;
            if (!tg_is_code(room, "valid", "1") || !*token || !*house || !*org || !tg_phone_valid(mobile)) continue;
            found++;
            if (!tg_copy(out->household, sizeof(out->household), house) || !tg_copy(out->organization, sizeof(out->organization), org)
                || !tg_copy(out->token, sizeof(out->token), token) || !tg_copy(out->mobile, sizeof(out->mobile), mobile)
                || !tg_copy(out->area, sizeof(out->area), tg_string(area, "areaName"))) {
                tg_copy(detail, TG_DETAIL, "授权字段过长"); memset(out, 0, sizeof(*out)); return false;
            }
        }
    }
    if (found != 1) {
        memset(out, 0, sizeof(*out));
        tg_copy(detail, TG_DETAIL, found ? "多个授权住房，请在设备网页选择当前住房" : "没有有效授权门禁");
        return false;
    }
    return true;
}
static cJSON *as_container(const cJSON *v) {
    if (cJSON_IsString(v)) return tg_parse_json(v->valuestring);
    return cJSON_Duplicate(v, true);
}
static bool append_lock(const cJSON *entry, const cJSON *lock, bool grouped, const char *group,
                        tg_door *doors, size_t *count) {
    if (!tg_is_code(lock, "Show", "1")) return true;
    const char *code = tg_string(grouped ? lock : entry, grouped ? "DeviceCode" : "Code");
    if (!*code) return true;
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(lock, "Number");
    char number[16];
    if (cJSON_IsString(n)) { if (!tg_copy(number, sizeof(number), n->valuestring)) return false; }
    else if (cJSON_IsNumber(n) && n->valuedouble == n->valueint && n->valueint >= 0) snprintf(number, sizeof(number), "%d", n->valueint);
    else return true;
    if (!*number) return true;
    for (const char *p = number; *p; p++) if (*p < '0' || *p > '9') return true;
    if (*count >= TG_MAX_DOORS) return false;
    tg_door *d = &doors[*count];
    if (!tg_copy(d->code, sizeof(d->code), code) || !tg_copy(d->number, sizeof(d->number), number)
        || !tg_copy(d->name, sizeof(d->name), tg_string(lock, "Name")) || !tg_copy(d->id, sizeof(d->id), tg_string(lock, "Id"))
        || !tg_copy(d->group, sizeof(d->group), group)) return false;
    (*count)++; return true;
}
static bool locks(const cJSON *entry, bool grouped, const char *group, tg_door *doors, size_t *count) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, "Lock");
    cJSON *container = as_container(v);
    if (!container) return false;
    bool ok = true;
    if (cJSON_IsObject(container)) ok = append_lock(entry, container, grouped, group, doors, count);
    else if (cJSON_IsArray(container)) {
        const cJSON *lock; cJSON_ArrayForEach(lock, container) if (!append_lock(entry, lock, grouped, group, doors, count)) { ok = false; break; }
    } else ok = false;
    cJSON_Delete(container); return ok;
}
bool tg_parse_doors(const cJSON *v, tg_door *doors, size_t *count, char *detail) {
    *count = 0;
    if (!tg_uums_ok(v, NULL)) goto error;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(v, "data"), *control = cJSON_GetObjectItemCaseSensitive(data, "DOOR_CONTROL");
    const cJSON *plain = cJSON_GetObjectItemCaseSensitive(control, "NO_GROUP_LIST"), *groups = cJSON_GetObjectItemCaseSensitive(control, "DOOR_GROUP_LIST"), *entry;
    if (!cJSON_IsArray(plain) || !cJSON_IsArray(groups)) goto error;
    cJSON_ArrayForEach(entry, plain) if (!locks(entry, false, "", doors, count)) goto error;
    cJSON_ArrayForEach(entry, groups) {
        cJSON *subgroups = as_container(cJSON_GetObjectItemCaseSensitive(entry, "LockGroup"));
        if (!subgroups) goto error;
        if (cJSON_IsObject(subgroups)) {
            cJSON *list = cJSON_CreateArray();
            if (!list) { cJSON_Delete(subgroups); goto error; }
            cJSON_AddItemToArray(list, subgroups); subgroups = list;
        }
        const cJSON *sub;
        bool ok = true;
        if (!cJSON_IsArray(subgroups)) { cJSON_Delete(subgroups); goto error; }
        cJSON_ArrayForEach(sub, subgroups) if (tg_is_code(sub, "Show", "1") && !locks(sub, true, tg_string(entry, "Name"), doors, count)) { ok = false; break; }
        cJSON_Delete(subgroups); if (!ok) goto error;
    }
    if (*count) return true;
error:
    *count = 0; tg_copy(detail, TG_DETAIL, "门禁配置无效、过大或没有可显示的门禁"); return false;
}
bool tg_select_door(const tg_config *cfg, const tg_door *doors, size_t count, size_t *index) {
    size_t found = 0;
    for (size_t i = 0; i < count; i++) {
        if (*cfg->door_code && strcmp(cfg->door_code, doors[i].code)) continue;
        if (*cfg->door_number && strcmp(cfg->door_number, doors[i].number)) continue;
        *index = i; found++;
    }
    return found == 1;
}

/* Hex without printf keeps the SIP task's deepest call chain short. */
static void hex_digest(const unsigned char hash[16], char out[33]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i * 2] = digits[hash[i] >> 4]; out[i * 2 + 1] = digits[hash[i] & 15]; }
    out[32] = 0;
}
void tg_md5_hex(const char *text, char out[33]) {
    unsigned char hash[16]; mbedtls_md5((const unsigned char *)text, strlen(text), hash);
    hex_digest(hash, out);
}
bool tg_talk_password(const char *encoded, char out[33]) {
    unsigned char plain[96]; size_t length = 0;
    *out = 0;
    if (!encoded || !*encoded || strlen(encoded) > 128) return false;
    if (mbedtls_base64_decode(plain, sizeof(plain) - 1, &length, (const unsigned char *)encoded, strlen(encoded)) || !length) return false;
    plain[length] = 0;
    bool ok = strlen((char *)plain) == length;
    if (ok) tg_md5_hex((char *)plain, out);
    memset(plain, 0, sizeof(plain)); return ok;
}
bool tg_xml_text(const char *xml, const char *tag, char *out, size_t cap) {
    char open[48], close[48]; *out = 0;
    if (!xml || snprintf(open, sizeof(open), "<%s>", tag) >= (int)sizeof(open)) return false;
    snprintf(close, sizeof(close), "</%s>", tag);
    const char *start = strstr(xml, open);
    if (!start) {
        snprintf(open, sizeof(open), "<%s/>", tag);
        return strstr(xml, open) != NULL;
    }
    start += strlen(open);
    const char *end = strstr(start, close);
    if (!end) return false;
    static const char *const names[] = {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"}, chars[] = "&<>\"'";
    size_t used = 0;
    for (const char *p = start; p < end; ) {
        char c = *p; size_t step = 1;
        if (c == '&') {
            unsigned i = 0;
            while (i < 5 && strncmp(p, names[i], strlen(names[i]))) i++;
            if (i == 5) return false;
            c = chars[i]; step = strlen(names[i]);
        }
        if (used + 1 >= cap) { *out = 0; return false; }
        out[used++] = c; p += step;
    }
    out[used] = 0; return true;
}
static bool host_valid(const char *v) {
    if (!*v) return false;
    for (; *v; v++) if (!isalnum((unsigned char)*v) && *v != '.' && *v != '-') return false;
    return true;
}
bool tg_talk_config(const char *xml, int default_port, tg_sip_config *out, char *detail) {
    char fault[16], port[8];
    memset(out, 0, sizeof(*out));
    if (tg_xml_text(xml, "faultCode", fault, sizeof(fault)) && *fault && strcmp(fault, "0")) {
        char text[96] = ""; tg_xml_text(xml, "faultText", text, sizeof(text));
        snprintf(detail, TG_DETAIL, "对讲配置被拒绝：%s %.80s", fault, text); return false;
    }
    if (!tg_xml_text(xml, "SIP_ACCOUNT", out->account, sizeof(out->account)) || !*out->account
        || !tg_xml_text(xml, "SIP_PASSWORD", out->password, sizeof(out->password)) || !*out->password
        || !tg_xml_text(xml, "SIP_DOMAIN", out->domain, sizeof(out->domain))) {
        tg_copy(detail, TG_DETAIL, "对讲配置缺少 SIP 账号"); memset(out, 0, sizeof(*out)); return false;
    }
    if (!tg_xml_text(xml, "SIP_PROXY", out->proxy, sizeof(out->proxy)) || !*out->proxy) strcpy(out->proxy, out->domain);
    if (!*out->domain) strcpy(out->domain, out->proxy);
    out->port = default_port;
    if (tg_xml_text(xml, "SIP_PROXY_PORT", port, sizeof(port)) && *port) {
        char *end; long n = strtol(port, &end, 10);
        if (*end || n < 1 || n > 65535) { tg_copy(detail, TG_DETAIL, "对讲端口无效"); memset(out, 0, sizeof(*out)); return false; }
        out->port = (int)n;
    }
    for (const char *p = out->account; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._-+", *p)) { tg_copy(detail, TG_DETAIL, "对讲账号格式无效"); memset(out, 0, sizeof(*out)); return false; }
    for (const char *p = out->password; *p; p++)
        if ((unsigned char)*p < 32 || *p == '"' || *p == '\\') { tg_copy(detail, TG_DETAIL, "对讲密码格式无效"); memset(out, 0, sizeof(*out)); return false; }
    if (!host_valid(out->domain) || !host_valid(out->proxy)) { tg_copy(detail, TG_DETAIL, "对讲服务器地址无效"); memset(out, 0, sizeof(*out)); return false; }
    return true;
}

int tg_sip_status(const char *m) {
    if (!m || strncmp(m, "SIP/2.0 ", 8) || !isdigit((unsigned char)m[8]) || !isdigit((unsigned char)m[9]) || !isdigit((unsigned char)m[10])) return 0;
    return (m[8] - '0') * 100 + (m[9] - '0') * 10 + (m[10] - '0');
}
bool tg_sip_method(const char *m, char *out, size_t cap) {
    size_t n = 0; *out = 0;
    if (!m) return false;
    while (m[n] >= 'A' && m[n] <= 'Z') n++;
    if (!n || m[n] != ' ' || n >= cap) return false;
    memcpy(out, m, n); out[n] = 0; return true;
}
static const char *compact_name(const char *name) {
    static const char *const map[][2] = {{"Via","v"},{"From","f"},{"To","t"},{"Call-ID","i"},{"Contact","m"},{"Content-Length","l"},{"Content-Type","c"}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(*map); i++) if (!strcasecmp(name, map[i][0])) return map[i][1];
    return NULL;
}
/* Calls visit(value, length) for each header line named `name`; stops at the blank line. */
static bool each_header(const char *m, const char *name, bool (*visit)(const char *, size_t, void *), void *context) {
    const char *alt = compact_name(name), *line = m ? strstr(m, "\r\n") : NULL;
    while (line) {
        line += 2;
        if (!strncmp(line, "\r\n", 2) || !*line) break;
        const char *eol = strstr(line, "\r\n"), *colon = memchr(line, ':', eol ? (size_t)(eol - line) : strlen(line));
        if (!eol || !colon) break;
        size_t n = (size_t)(colon - line);
        while (n && line[n - 1] == ' ') n--;
        if ((strlen(name) == n && !strncasecmp(line, name, n)) || (alt && n == 1 && tolower((unsigned char)*line) == *alt)) {
            const char *v = colon + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t len = (size_t)(eol - v);
            while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) len--;
            if (!visit(v, len, context)) return false;
        }
        line = eol;
    }
    return true;
}
typedef struct { char *out; size_t cap; bool found; } first_header;
static bool take_first(const char *v, size_t len, void *context) {
    first_header *f = context;
    if (len >= f->cap) return false;
    memcpy(f->out, v, len); f->out[len] = 0; f->found = true; return false;
}
bool tg_sip_header(const char *m, const char *name, char *out, size_t cap) {
    first_header f = {out, cap, false}; *out = 0;
    each_header(m, name, take_first, &f); return f.found;
}
typedef struct { const char *name; char *out; size_t cap, used; bool ok; } all_headers;
static bool append_header(const char *v, size_t len, void *context) {
    all_headers *a = context;
    int n = snprintf(a->out + a->used, a->cap - a->used, "%s: %.*s\r\n", a->name, (int)len, v);
    if (n < 0 || (size_t)n >= a->cap - a->used) { a->ok = false; return false; }
    a->used += (size_t)n; return true;
}
bool tg_sip_headers(const char *m, const char *name, char *out, size_t cap) {
    all_headers a = {name, out, cap, 0, true}; *out = 0;
    each_header(m, name, append_header, &a);
    if (!a.ok) *out = 0;
    return a.ok && a.used;
}
bool tg_sip_param(const char *v, const char *name, char *out, size_t cap) {
    size_t n = strlen(name); *out = 0;
    for (const char *p = v; p && *p; p++) {
        if (p != v && !strchr(";, \t", p[-1])) continue;
        if (strncasecmp(p, name, n) || (p[n] && !strchr("=;, \t>", p[n]))) continue;
        const char *value = p + n;
        if (*value != '=') return true;  /* flag parameter such as rport */
        value++;
        size_t len;
        if (*value == '"') { value++; const char *q = strchr(value, '"'); if (!q) return false; len = (size_t)(q - value); }
        else len = strcspn(value, ";, \t>");
        if (len >= cap) return false;
        memcpy(out, value, len); out[len] = 0; return true;
    }
    return false;
}
bool tg_sip_user(const char *v, char *out, size_t cap) {
    const char *p = v ? strstr(v, "sip:") : NULL; *out = 0;
    if (!p) return false;
    p += 4;
    size_t len = strcspn(p, "@;>: ");
    if (!len || len >= cap || p[len] != '@') return false;
    memcpy(out, p, len); out[len] = 0; return true;
}
int tg_sip_expires(const char *m, int fallback) {
    char value[256], number[16];
    if (tg_sip_header(m, "Contact", value, sizeof(value)) && tg_sip_param(value, "expires", number, sizeof(number)) && *number) {
        char *end; long n = strtol(number, &end, 10);
        if (!*end && n >= 0 && n <= 86400) return (int)n;
    }
    if (tg_sip_header(m, "Expires", number, sizeof(number)) && *number) {
        char *end; long n = strtol(number, &end, 10);
        if (!*end && n >= 0 && n <= 86400) return (int)n;
    }
    return fallback;
}
/* MD5 over "a:b:..." fed piece by piece, so no text buffer or printf is needed. */
static void md5_join(const char *const *parts, size_t count, char out[33]) {
    mbedtls_md5_context ctx; unsigned char hash[16];
    mbedtls_md5_init(&ctx); mbedtls_md5_starts(&ctx);
    for (size_t i = 0; i < count; i++) {
        if (i) mbedtls_md5_update(&ctx, (const unsigned char *)":", 1);
        mbedtls_md5_update(&ctx, (const unsigned char *)parts[i], strlen(parts[i]));
    }
    mbedtls_md5_finish(&ctx, hash); mbedtls_md5_free(&ctx);
    hex_digest(hash, out); memset(hash, 0, sizeof(hash));
}
void tg_sip_digest(const char *user, const char *realm, const char *password, const char *method, const char *uri,
                   const char *nonce, const char *qop, const char *nc, const char *cnonce, char out[33]) {
    char ha1[33], ha2[33];
    const char *a1[] = {user, realm, password}, *a2[] = {method, uri};
    md5_join(a1, 3, ha1); md5_join(a2, 2, ha2);
    const char *with_qop[] = {ha1, nonce, nc, cnonce, qop, ha2}, *plain[] = {ha1, nonce, ha2};
    if (qop && *qop) md5_join(with_qop, 6, out); else md5_join(plain, 3, out);
    memset(ha1, 0, sizeof(ha1));
}
